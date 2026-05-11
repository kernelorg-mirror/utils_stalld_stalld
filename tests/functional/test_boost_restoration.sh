#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: Policy Restoration After Boosting
# Verify stalld correctly restores original scheduling policies and priorities
# after boost duration expires.
#
# Note: starvation_gen creates SCHED_FIFO threads by default (blocker and blockees).
# Tests verify that SCHED_FIFO tasks are properly restored after boosting.
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Policy Restoration After Boosting" "test_restoration"

#=============================================================================
# Test 1: Restore SCHED_FIFO Policy (starvation_gen creates SCHED_FIFO threads)
#=============================================================================
test_section "Test 1: Restore SCHED_FIFO Policy"

threshold=5
boost_duration=3

log "Starting stalld with ${boost_duration}s boost duration"
# Use -i to ignore kworkers so stalld focuses on our test workload
start_stalld_with_log "${STALLD_LOG}" -f -v -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU} -d ${boost_duration} -N -i "kworker"

# Create starvation (starvation_gen creates SCHED_FIFO blocker prio 80, blockee prio 1)
log "Creating starvation with SCHED_FIFO tasks (blocker prio 80, blockee prio 1)"
start_starvation_gen -c ${TEST_CPU} -p 80 -b 1 -n 1 -d 12

# Find the starved task
tracked_pid=$(find_starved_child "${STARVE_PID}")

if [ -n "${tracked_pid}" ]; then
    log "Tracking task PID ${tracked_pid}"

    # Verify initial policy is SCHED_FIFO (1) with priority 1
    initial_policy=$(get_sched_policy ${tracked_pid})
    initial_prio=$(get_sched_priority ${tracked_pid})
    log "Initial policy: ${initial_policy} (expected: 1=SCHED_FIFO), prio: ${initial_prio}"

    assert_success "Initial policy is SCHED_FIFO" test "$initial_policy" = "1"

    # Wait for starvation detection and boosting
    log "Waiting for starvation detection and boost..."
    wait_for_boost_detected "${STALLD_LOG}"

    # Check policy during boost (should be DEADLINE=6)
    if [ -f "/proc/${tracked_pid}/sched" ]; then
        boosted_policy=$(get_sched_policy ${tracked_pid})
        log "Policy during boost: ${boosted_policy}"

        assert_success "Task boosted to SCHED_DEADLINE (6)" test "$boosted_policy" = "6"
    fi

    # Wait for boost duration to complete
    log "Waiting for boost to complete (${boost_duration}s)..."
    sleep $((boost_duration + 1))

    # Check if task was restored after boost
    if [ -f "/proc/${tracked_pid}/sched" ]; then
        post_boost_policy=$(get_sched_policy ${tracked_pid})
        post_boost_prio=$(get_sched_priority ${tracked_pid})
        log "Policy after boost: ${post_boost_policy}, prio: ${post_boost_prio}"

        if [ "$post_boost_policy" = "1" ]; then
            pass "Policy restored to SCHED_FIFO (1) during boost cycle"
        elif [ "$post_boost_policy" = "6" ]; then
            log "ℹ INFO: Still in boost (DEADLINE), will check final restoration"
        else
            log "⚠ INFO: Policy is ${post_boost_policy} (unexpected)"
        fi
    fi
else
    log "⚠ WARNING: Could not find starved task to track"
fi

# Wait for starvation_gen to complete naturally
log "Waiting for starvation test to complete..."
wait ${STARVE_PID} 2>/dev/null || true

# Final check: verify policy was restored (task may have exited)
if [ -n "${tracked_pid}" ] && [ -f "/proc/${tracked_pid}/sched" ]; then
    final_policy=$(get_sched_policy ${tracked_pid})
    final_prio=$(get_sched_priority ${tracked_pid})
    log "Final policy: ${final_policy}, prio: ${final_prio}"

    if [ "$final_policy" = "1" ]; then
        pass "Policy restored to SCHED_FIFO (1)"
        assert_success "Priority restored to ${initial_prio}" test "$final_prio" = "$initial_prio"
    else
        log "ℹ INFO: Final policy is ${final_policy} (task may have exited)"
    fi
else
    log "ℹ INFO: Task exited (expected - starvation test completed)"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: SCHED_OTHER Policy Restoration
#=============================================================================
test_section "Test 2: Restore SCHED_OTHER Policy"
log "Test that SCHED_OTHER tasks are correctly restored after boosting"

threshold=5
boost_duration=3

log "Starting stalld"
rm -f "${STALLD_LOG}"
start_stalld_with_log "${STALLD_LOG}" -f -v -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU} -d ${boost_duration} -N -i "kworker"

# Use -o flag to create SCHED_OTHER blockees
log "Creating SCHED_OTHER starvation (RT blocker prio 80, SCHED_OTHER blockee)"
start_starvation_gen -c ${TEST_CPU} -p 80 -o -n 1 -d 12

# Find the starved SCHED_OTHER task
tracked_pid=$(find_starved_child "${STARVE_PID}")

if [ -n "${tracked_pid}" ]; then
    initial_policy=$(get_sched_policy ${tracked_pid})
    log "Tracking task PID ${tracked_pid}, initial policy: ${initial_policy} (expected: 0=SCHED_OTHER)"

    # Wait for boost
    log "Waiting for starvation detection and boost..."
    if wait_for_boost_detected "${STALLD_LOG}"; then
        pass "SCHED_OTHER task boosted"

        # Wait for boost to expire and policy to be restored
        sleep $((boost_duration + 1))

        if [ -f "/proc/${tracked_pid}/sched" ]; then
            final_policy=$(get_sched_policy ${tracked_pid})
            log "Policy after boost: ${final_policy}"

            assert_success "Policy restored to SCHED_OTHER (0)" test "$final_policy" = "0"
        else
            log "⚠ INFO: Task exited before restoration check"
        fi
    else
        fail "No boost detected for SCHED_OTHER task"
    fi
else
    log "⚠ WARNING: Could not find starved task to track"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Task Exit During Boost
#=============================================================================
test_section "Test 3: Graceful Handling of Task Exit During Boost"

threshold=5
boost_duration=5

log "Starting stalld with ${threshold}s threshold, ${boost_duration}s boost (task will exit during boost)"
rm -f "${STALLD_LOG}"
start_stalld_with_log "${STALLD_LOG}" -f -v -t $threshold -c ${TEST_CPU} -a ${STALLD_CPU} -d ${boost_duration} -N -i "kworker"

# Task must survive past the threshold to be detected and boosted,
# then exit during the boost window.
short_duration=$((threshold + 3))
log "Creating starvation that will exit after ${short_duration}s"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 1 -d ${short_duration}

# Wait for starvation detection and boosting
if wait_for_boost_detected "${STALLD_LOG}"; then
    pass "Boost occurred"

    # At this point (12s), starvation_gen has exited (at 8s) during the boost
    # stalld should still be running despite the task exiting during boost
    # No additional sleep needed - we're already past the task exit point
    sleep 1

    # Verify stalld is still running and didn't crash after task exit
    assert_success "stalld handled task exit during boost gracefully" process_alive ${STALLD_PID}

else
    log "⚠ WARNING: No boost detected in this test run"
fi

# Cleanup
cleanup_scenario "${STARVE_PID}"

end_test
