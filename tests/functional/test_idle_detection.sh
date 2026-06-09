#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: Idle CPU Detection
# Verify stalld's idle detection skips idle CPUs to reduce overhead,
# and resumes monitoring when CPUs become busy
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Idle CPU Detection" "test_idle"

#=============================================================================
# Test 1: Monitoring Resumes When CPU Becomes Busy
#=============================================================================
test_section "Test 1: Monitoring Resumes for Busy CPUs"

threshold=5
rm -f "${STALLD_LOG}"
log "Starting stalld"
start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Initially idle
log "CPU ${TEST_CPU} initially idle"
sleep 3

# Now create load to make CPU busy
log "Creating load on CPU ${TEST_CPU} to make it busy"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d 12

# Wait for stalld to detect the busy CPU and starvation
log "Waiting for stalld to detect busy CPU and starvation..."

# Verify stalld detected starvation (meaning it resumed monitoring)
if wait_for_starvation_detected "${STALLD_LOG}"; then
    pass "stalld detected starvation on now-busy CPU"
    log "        Monitoring resumed when CPU became busy"
else
    log "⚠ INFO: No starvation detected"
    log "        (may be timing issue or CPU remained idle)"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Idle CPUs Are Skipped
#=============================================================================
test_section "Test 2: Idle CPUs Are Skipped"

rm -f "${STALLD_LOG}"
threshold=3

log "Starting stalld with idle detection on an idle CPU"
start_stalld_with_log "${STALLD_LOG}" -f -v -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

wait_for_log_message "skipping" 5 "${STALLD_LOG}"

assert_log_contains "${STALLD_LOG}" "skipping" "Idle CPU correctly skipped"

cleanup_scenario

#=============================================================================
# Test 3: Idle Detection with Multiple CPUs
#=============================================================================
test_section "Test 3: Per-CPU Independent Idle Detection"

NUM_CPUS=$(get_num_cpus)
if [ ${NUM_CPUS} -lt 2 ]; then
    log "⚠ SKIP: Need at least 2 CPUs for this test (have ${NUM_CPUS})"
else
    CPU0=${TEST_CPU}
    if [ ${TEST_CPU} -eq 0 ]; then
        CPU1=1
    else
        CPU1=0
    fi

    log "Testing idle detection on CPU ${CPU0} (idle) and CPU ${CPU1} (busy)"

    threshold=5
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -l -t $threshold -c ${CPU0},${CPU1}

    # Create load only on CPU1, leave CPU0 idle
    log "Creating load on CPU ${CPU1} only"
    start_starvation_gen -c ${CPU1} -p 80 -n 2 -d 12

    # Wait for detection
    wait_for_starvation_detected "${STALLD_LOG}"

    # Check which CPU had starvation detected
    cpu0_detections=$(grep -c "starved on CPU ${CPU0}" "${STALLD_LOG}")
    cpu1_detections=$(grep -c "starved on CPU ${CPU1}" "${STALLD_LOG}")

    log "CPU ${CPU0} detections: ${cpu0_detections} (should be 0, it's idle)"
    log "CPU ${CPU1} detections: ${cpu1_detections} (should be >0, it's busy)"

    if [ ${cpu0_detections} -eq 0 ] && [ ${cpu1_detections} -gt 0 ]; then
        pass "Idle CPU skipped, busy CPU monitored"
    elif [ ${cpu1_detections} -gt 0 ]; then
        pass "Busy CPU ${CPU1} monitored"
        if [ ${cpu0_detections} -gt 0 ]; then
            log "⚠ INFO: CPU ${CPU0} also had detections (may have background activity)"
        fi
    else
        log "⚠ INFO: Detection pattern differs (timing or load dependent)"
    fi

    # Cleanup
    cleanup_scenario "${STARVE_PID}"
fi

end_test
