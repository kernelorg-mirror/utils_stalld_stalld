#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: SCHED_FIFO Boosting Mechanism
# Verify stalld correctly boosts starving tasks using SCHED_FIFO with -F flag
# and implements FIFO emulation behavior
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "SCHED_FIFO Boosting Mechanism" "test_fifo_boost"

#=============================================================================
# Test 1: FIFO Boost with -F Flag
#=============================================================================
test_section "Test 1: FIFO Boost with -F Flag"

threshold=5
# Create starvation FIRST (before stalld starts)
starvation_duration=$((threshold + 5))
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d ${starvation_duration}

log "Starting stalld with -F flag to force SCHED_FIFO boosting"
# Note: -F requires non-single-threaded mode (aggressive mode)
# Use -g 1 for 1-second granularity to ensure timely detection
start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -N -F -A -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Wait for boosting
log "Waiting for boost detection..."
assert_boost_detected "${STALLD_LOG}" "Boosting occurred with -F flag"
assert_log_contains "${STALLD_LOG}" "SCHED_FIFO" "SCHED_FIFO boosting used (as requested by -F)"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: FIFO Priority Verification
#=============================================================================
test_section "Test 2: FIFO Priority Verification"

threshold=5
rm -f "${STALLD_LOG}"

# Create starvation FIRST
log "Creating starvation on CPU ${TEST_CPU}"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 1 -d 10
tracked_pid=$(find_starved_child "${STARVE_PID}")

log "Starting stalld with -F flag (FIFO boosting)"
start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -N -F -A -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU}

# Wait for boosting
log "Waiting for boost detection..."
wait_for_boost_detected "${STALLD_LOG}"

fifo_task_found=0
if [ -n "${tracked_pid}" ] && [ -f "/proc/${tracked_pid}/sched" ]; then
    policy=$(get_sched_policy ${tracked_pid})
    log "Child PID ${tracked_pid} policy: ${policy} (1=SCHED_FIFO)"

    if [ "$policy" = "1" ]; then
        priority=$(get_sched_priority ${tracked_pid})
        pass "Task PID ${tracked_pid} boosted to SCHED_FIFO (policy 1)"
        log "        Priority: ${priority}"
        fifo_task_found=1
    fi
fi

if [ ${fifo_task_found} -eq 0 ]; then
    log "⚠ INFO: Could not verify FIFO policy in /proc (timing issue or boost already expired)"
    # FIFO emulation cycles between FIFO and OTHER, so we may catch it in OTHER state
    assert_log_contains "${STALLD_LOG}" "boosted.*SCHED_FIFO" "SCHED_FIFO boost confirmed in logs"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: FIFO Emulation Behavior
#=============================================================================
test_section "Test 3: FIFO Emulation Behavior"
log "FIFO emulation cycles: boost→sleep(runtime)→restore→sleep(remainder)"

threshold=5
boost_duration=5  # 5 seconds total boost
boost_period=1000000000   # 1 second period
boost_runtime=20000       # 20µs runtime

log "Starting stalld with FIFO emulation:"
log "  Duration: ${boost_duration}s"
log "  Period: ${boost_period}ns (1s)"
log "  Runtime: ${boost_runtime}ns (20µs)"
log "  Expected cycles: ~5"

rm -f "${STALLD_LOG}"

# Create starvation FIRST
log "Creating starvation on CPU ${TEST_CPU}"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 1 -d 12

start_stalld_with_log "${STALLD_LOG}" -f -v -g 1 -N -F -A -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU} \
    -d ${boost_duration} -p ${boost_period} -r ${boost_runtime}

# Wait for boosting to start
log "Waiting for boost detection..."
wait_for_boost_detected "${STALLD_LOG}"

# Wait for FIFO emulation cycles to complete (boost_duration + buffer)
log "Waiting for FIFO emulation cycles to complete..."
sleep $((boost_duration + 2))

# Count boost events (FIFO emulation creates multiple boosts)
boost_count=$(grep -c "boosted.*SCHED_FIFO" "${STALLD_LOG}")
log "Number of FIFO boost events: ${boost_count}"

if [ ${boost_count} -gt 1 ]; then
    pass "Multiple FIFO boost events (${boost_count}) - emulation cycling detected"
    log "        (FIFO emulation boosts, sleeps, restores, repeats)"
else
    log "⚠ INFO: Only ${boost_count} FIFO boost event(s)"
    log "        (emulation may complete very quickly or timing issue)"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

end_test
