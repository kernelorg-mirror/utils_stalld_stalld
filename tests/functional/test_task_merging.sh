#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: Task Merging Logic
# Verify stalld's merge_tasks_info() correctly preserves starvation timestamps
# for tasks that make no progress (same PID + same context switches)
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Task Merging Logic" "test_merge"

#=============================================================================
# Test 1: Timestamp Preservation for Non-Progressing Tasks
#=============================================================================
test_section "Test 1: Timestamp Preservation Across Cycles"
log "Task merging: same PID + same ctxsw = preserved timestamp"

threshold=3
log "Starting stalld with ${threshold}s threshold (log-only, verbose)"
# Use -g 1 for 1-second granularity
start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Create long starvation to span multiple monitoring cycles
starvation_duration=18
log "Creating starvation for ${starvation_duration}s (multiple detection cycles)"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d ${starvation_duration}

# Wait for first detection
log "Waiting for first detection cycle..."
wait_for_starvation_detected "${STALLD_LOG}"

# Extract first starvation duration
if grep -q "starved.*for [0-9]" "${STALLD_LOG}"; then
    first_duration=$(grep "starved.*for [0-9]" "${STALLD_LOG}" | head -1 | grep -oE "for [0-9]+" | awk '{print $2}')
    log "First detection: task starved for ${first_duration}s"
    if [ -z "${first_duration}" ]; then
        first_duration=0
    fi
else
    log "⚠ WARNING: No starvation detected in first cycle"
    log "          This may indicate timing issues or system load"
    first_duration=0
fi

# Wait for second detection cycle
log "Waiting for second detection cycle..."
sleep 4

# Extract second starvation duration
second_duration=$(grep "starved.*for [0-9]" "${STALLD_LOG}" | tail -1 | grep -oE "for [0-9]+" | awk '{print $2}')
if [ -z "${second_duration}" ]; then
    second_duration=0
fi
log "Second detection: task starved for ${second_duration}s"

# Verify timestamp was preserved (duration increased)
if [ "${second_duration}" -gt "${first_duration}" ]; then
    delta=$((second_duration - first_duration))
    pass "Starvation duration increased by ${delta}s"
    log "        Timestamp preserved across monitoring cycles"
else
    fail "Duration did not increase (${first_duration}s -> ${second_duration}s)"
    log "        Timestamp may have been reset (task merging failed)"
fi

# Wait for third detection to confirm continued accumulation
log "Waiting for third detection cycle..."
sleep 4

third_duration=$(grep "starved.*for [0-9]" "${STALLD_LOG}" | tail -1 | grep -oE "for [0-9]+" | awk '{print $2}')
if [ -z "${third_duration}" ]; then
    third_duration=0
fi
log "Third detection: task starved for ${third_duration}s"

if [ "${third_duration}" -gt "${second_duration}" ]; then
    pass "Duration continues to accumulate (${third_duration}s total)"
else
    log "⚠ INFO: Duration did not increase in third cycle"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Same PID + Same Context Switches = Merged
#=============================================================================
test_section "Test 2: Merge Condition Verification"
log "Merging occurs when: PID matches AND context switches unchanged"

threshold=5
rm -f "${STALLD_LOG}"
start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -l -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Create starvation
log "Creating starvation"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 1 -d 12

# Wait for starvation detection
wait_for_starvation_detected "${STALLD_LOG}"

# Find the starved task PID
tracked_pid=$(find_starved_child "${STARVE_PID}")

if [ -n "${tracked_pid}" ]; then
    log "Tracking starved task PID ${tracked_pid}"

    # Get context switch count
    ctxsw_before=$(get_ctxt_switches ${tracked_pid})
    log "Context switches: ${ctxsw_before}"

    # Wait for next detection cycle
    sleep 3

    # Verify context switches haven't changed (task is starved)
    if [ -f "/proc/${tracked_pid}/status" ]; then
        ctxsw_after=$(get_ctxt_switches ${tracked_pid})
        log "Context switches after 3s: ${ctxsw_after}"

        delta=$((ctxsw_after - ctxsw_before))
        if [ ${delta} -lt 5 ]; then
            pass "Context switches remained low (delta: ${delta})"
            log "        Task meeting merge criteria (same PID, same ctxsw)"
        else
            log "⚠ INFO: Context switches increased by ${delta}"
            log "        Task may be making some progress"
        fi

        # Verify timestamp was preserved in logs
        detections=$(grep -c "starved" "${STALLD_LOG}")
        log "Total starvation detections: ${detections}"

        if [ ${detections} -ge 2 ]; then
            pass "Multiple detections indicate task merging across cycles"
        fi
    else
        log "⚠ WARNING: Task exited before second check"
    fi
else
    log "⚠ WARNING: Could not track starved task PID"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Multiple CPUs with Independent Task Merging
#=============================================================================
test_section "Test 3: Per-CPU Independent Task Merging"

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

    threshold=3
    log "Testing task merging on CPU ${CPU0} and CPU ${CPU1} independently"

    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -l -t $threshold -c ${CPU0},${CPU1} -a ${STALLD_CPU}

    # Create starvation on both CPUs
    log "Creating starvation on CPU ${CPU0}"
    start_starvation_gen -c ${CPU0} -p 80 -n 1 -d 10
    STARVE_PID0=${STARVE_PID}

    log "Creating starvation on CPU ${CPU1}"
    start_starvation_gen -c ${CPU1} -p 80 -n 1 -d 10
    STARVE_PID1=${STARVE_PID}

    # Wait for starvation detection on both CPUs
    wait_for_starvation_detected "${STALLD_LOG}"
    sleep 4

    # Check CPU0 starvation accumulation
    cpu0_detections=$(grep "starved on CPU ${CPU0}" "${STALLD_LOG}" | wc -l)
    log "CPU ${CPU0} detections: ${cpu0_detections}"

    if [ ${cpu0_detections} -ge 2 ]; then
        # Check if duration increased
        cpu0_first=$(grep "starved on CPU ${CPU0}" "${STALLD_LOG}" | head -1 | grep -oE "for [0-9]+" | awk '{print $2}')
        cpu0_last=$(grep "starved on CPU ${CPU0}" "${STALLD_LOG}" | tail -1 | grep -oE "for [0-9]+" | awk '{print $2}')
        if [ -z "${cpu0_first}" ]; then cpu0_first=0; fi
        if [ -z "${cpu0_last}" ]; then cpu0_last=0; fi
        log "CPU ${CPU0}: ${cpu0_first}s -> ${cpu0_last}s"

        if [ "${cpu0_last}" -gt "${cpu0_first}" ]; then
            pass "CPU ${CPU0} task merging working (timestamp preserved)"
        fi
    fi

    # Check CPU1 starvation accumulation
    cpu1_detections=$(grep "starved on CPU ${CPU1}" "${STALLD_LOG}" | wc -l)
    log "CPU ${CPU1} detections: ${cpu1_detections}"

    if [ ${cpu1_detections} -ge 2 ]; then
        cpu1_first=$(grep "starved on CPU ${CPU1}" "${STALLD_LOG}" | head -1 | grep -oE "for [0-9]+" | awk '{print $2}')
        cpu1_last=$(grep "starved on CPU ${CPU1}" "${STALLD_LOG}" | tail -1 | grep -oE "for [0-9]+" | awk '{print $2}')
        if [ -z "${cpu1_first}" ]; then cpu1_first=0; fi
        if [ -z "${cpu1_last}" ]; then cpu1_last=0; fi
        log "CPU ${CPU1}: ${cpu1_first}s -> ${cpu1_last}s"

        if [ "${cpu1_last}" -gt "${cpu1_first}" ]; then
            pass "CPU ${CPU1} task merging working (timestamp preserved)"
        fi
    fi

    if [ ${cpu0_detections} -ge 2 ] && [ ${cpu1_detections} -ge 2 ]; then
        pass "Independent task merging on both CPUs"
    fi

    # Cleanup
    cleanup_scenario "${STARVE_PID0}" "${STARVE_PID1}"
fi

end_test
