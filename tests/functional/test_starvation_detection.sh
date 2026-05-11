#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: Starvation Detection Logic
# Verify stalld correctly detects starving tasks and tracks context switches
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Starvation Detection Logic" "test_detection"

# Get number of CPUs for multi-CPU tests
NUM_CPUS=$(get_num_cpus)

#=============================================================================
# Test 1: Basic Starvation Detection
#=============================================================================
test_section "Test 1: Basic Starvation Detection"

threshold=5

# Create starvation BEFORE starting stalld to avoid idle detection race
starvation_duration=$((threshold + 5))
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d ${starvation_duration}

log "Starting stalld with ${threshold}s threshold (log-only mode)"
start_stalld_with_log "${STALLD_LOG}" -f -v -N -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Wait for starvation detection
log "Waiting for starvation detection..."
assert_starvation_detected "${STALLD_LOG}" "Starvation detected"
assert_log_contains "${STALLD_LOG}" "starved on CPU ${TEST_CPU}" "Correct CPU ID logged (CPU ${TEST_CPU})"
assert_log_contains "${STALLD_LOG}" "starved on CPU ${TEST_CPU} for [0-9]" "Starvation duration logged"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Context Switch Count Tracking
#=============================================================================
test_section "Test 2: Context Switch Count Tracking"

rm -f "${STALLD_LOG}"
threshold=5

# Create starvation
log "Creating starvation on CPU ${TEST_CPU}"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 1 -d 15

log "Starting stalld with ${threshold}s threshold (log-only mode)"
start_stalld_with_log "${STALLD_LOG}" -f -v -N -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Wait for starvation detection
log "Waiting for starvation detection..."
wait_for_starvation_detected "${STALLD_LOG}"

# Try to find the starved task PID from starvation_gen children
tracked_pid=$(find_starved_child "${STARVE_PID}")
if [ -n "${tracked_pid}" ]; then
    ctxt_before=$(get_ctxt_switches ${tracked_pid})
    log "Found starved task PID ${tracked_pid}, context switches: ${ctxt_before}"

    sleep 2

    ctxt_after=$(get_ctxt_switches ${tracked_pid})
    log "Context switches after 2s: ${ctxt_after}"

    ctxt_delta=$((ctxt_after - ctxt_before))
    assert_success "Context switch count remained low" test ${ctxt_delta} -lt 5
else
    log "⚠ WARNING: Could not find starved task PIDs to verify context switches"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Multiple CPUs Detection
#=============================================================================
test_section "Test 3: Multiple CPUs Detection"

if [ ${NUM_CPUS} -lt 2 ]; then
    log "⚠ SKIP: Need at least 2 CPUs for this test (have ${NUM_CPUS})"
else
    CPU0=${TEST_CPU}
    # Pick a different CPU
    if [ ${TEST_CPU} -eq 0 ]; then
        CPU1=1
    else
        CPU1=0
    fi

    # Pick stalld CPU that avoids both test CPUs
    STALLD_CPU_MULTI=${STALLD_CPU}
    if [ ${STALLD_CPU} -eq ${CPU0} ] || [ ${STALLD_CPU} -eq ${CPU1} ]; then
        # Find a CPU that's not CPU0 or CPU1
        for cpu in $(get_online_cpus); do
            if [ $cpu -ne ${CPU0} ] && [ $cpu -ne ${CPU1} ]; then
                STALLD_CPU_MULTI=$cpu
                break
            fi
        done
        log "Adjusted stalld CPU from ${STALLD_CPU} to ${STALLD_CPU_MULTI} to avoid test CPUs"
    fi

    log "Testing starvation detection on CPU ${CPU0} and CPU ${CPU1}"
    log "Stalld will run on CPU ${STALLD_CPU_MULTI}"

    rm -f "${STALLD_LOG}"
    threshold=5

    # Create starvation on CPU0
    log "Creating starvation on CPU ${CPU0}"
    start_starvation_gen -c ${CPU0} -p 80 -n 1 -d 12
    STARVE_PID0=${STARVE_PID}

    # Create starvation on CPU1
    log "Creating starvation on CPU ${CPU1}"
    start_starvation_gen -c ${CPU1} -p 80 -n 1 -d 12
    STARVE_PID1=${STARVE_PID}

    start_stalld_with_log "${STALLD_LOG}" -f -v -N -l -t $threshold -c ${CPU0},${CPU1} -a ${STALLD_CPU_MULTI}

    # Wait for starvation detection on both CPUs
    log "Waiting for starvation detection on CPU ${CPU0}..."
    assert_starvation_detected "${STALLD_LOG}" "Starvation detected on CPU ${CPU0}" "30" "${CPU0}"

    log "Waiting for starvation detection on CPU ${CPU1}..."
    assert_starvation_detected "${STALLD_LOG}" "Starvation detected on CPU ${CPU1}" "30" "${CPU1}"

    # Cleanup
    cleanup_scenario "${STARVE_PID0}" "${STARVE_PID1}"
fi

#=============================================================================
# Test 4: No False Positives (Task Making Progress)
#=============================================================================
test_section "Test 4: No False Positives"

rm -f "${STALLD_LOG}"
threshold=5
log "Starting stalld with ${threshold}s threshold (log-only mode)"
start_stalld_with_log "${STALLD_LOG}" -f -v -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Create a task that gets CPU time but isn't starved
# Use a SCHED_OTHER task with lower priority that still gets scheduled
log "Creating a busy task that should NOT be starved"
taskset -c ${TEST_CPU} bash -c 'for i in {1..100}; do sleep 0.1; done' &
BUSY_PID=$!

# Wait beyond threshold
sleep $((threshold + 3))

# Verify this task was NOT reported as starved
# Since it's making progress, stalld shouldn't detect it
assert_log_contains --negate "${STALLD_LOG}" \
    "${BUSY_PID}.*starved" \
    "No false positive - progress-making task not reported as starved"

send_signal TERM ${BUSY_PID}
wait ${BUSY_PID} 2>/dev/null

stop_stalld

#=============================================================================
# Test 5: Edge Case - Task Exits During Monitoring
#=============================================================================
test_section "Test 5: Task Exits During Monitoring"

rm -f "${STALLD_LOG}"
threshold=10
log "Starting stalld with ${threshold}s threshold (log-only mode)"
start_stalld_with_log "${STALLD_LOG}" -f -v -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Create short-lived starvation that exits before threshold
log "Creating short-lived starvation (3s, less than ${threshold}s threshold)"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 1 -d 3

# Wait for task to exit
sleep 5

assert_process_running "${STALLD_PID}" "stalld still running after task exit"

# Check for error messages
assert_log_contains --negate --ignore-case "${STALLD_LOG}" "error\|segfault\|crash" "No error messages in log"

stop_stalld

end_test
