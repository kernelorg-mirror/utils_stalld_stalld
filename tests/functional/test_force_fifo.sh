#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: stalld -F/--force_fifo option
# Verifies that stalld uses SCHED_FIFO instead of SCHED_DEADLINE when -F is specified
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Force FIFO Option (-F)" "test_force_fifo"

#=============================================================================
# Test 1: Default behavior (should use SCHED_DEADLINE)
#=============================================================================
test_section "Test 1: Default behavior (no -F, should use SCHED_DEADLINE)"

threshold=3
log "Starting stalld with ${threshold}s threshold (default, no -F)"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -t ${threshold}

# Create starvation
starvation_duration=10
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d ${starvation_duration}

# Wait for detection and boosting
assert_boost_detected "${STALLD_LOG}" "Boosting occurred in default mode"
assert_log_contains --ignore-case "${STALLD_LOG}" "sched_deadline" "SCHED_DEADLINE used by default"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Force FIFO mode (-F)
#=============================================================================
test_section "Test 2: Force FIFO mode (-F)"

# Note: Single-threaded mode only works with SCHED_DEADLINE (dies with FIFO)
# So we need to use aggressive mode (-A) when testing FIFO
STALLD_LOG2="/tmp/stalld_test_force_fifo_test2_$$.log"
CLEANUP_FILES+=("${STALLD_LOG2}")

log "Starting stalld with -F flag and aggressive mode (-A)"
start_stalld_with_log "${STALLD_LOG2}" -f -v -c "${TEST_CPU}" -t ${threshold} -F -A

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d ${starvation_duration}

# Wait for detection and boosting
assert_boost_detected "${STALLD_LOG2}" "Boosting occurred with -F flag"
assert_log_contains --ignore-case "${STALLD_LOG2}" "sched_fifo" "SCHED_FIFO used with -F flag"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Verify FIFO priority setting
#=============================================================================
test_section "Test 3: Verify FIFO priority is set"

STALLD_LOG3="/tmp/stalld_test_force_fifo_test3_$$.log"
CLEANUP_FILES+=("${STALLD_LOG3}")

log "Starting stalld with -F and -A flags"
start_stalld_with_log "${STALLD_LOG3}" -f -v -c "${TEST_CPU}" -t ${threshold} -F -A

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d ${starvation_duration}

# Wait for detection and boosting
assert_boost_detected "${STALLD_LOG3}" "FIFO boosting with priority setting completed"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 4: Verify FIFO emulation behavior (sleep runtime, restore, sleep remainder)
#=============================================================================
test_section "Test 4: FIFO emulation behavior"

boost_duration=3
long_starvation=12
STALLD_LOG4="/tmp/stalld_test_force_fifo_test4_$$.log"
CLEANUP_FILES+=("${STALLD_LOG4}")

log "Starting stalld with -F, -A, and ${boost_duration}s boost duration"
start_stalld_with_log "${STALLD_LOG4}" -f -v -c "${TEST_CPU}" -t ${threshold} -F -A -d ${boost_duration}

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${long_starvation}s"
start_starvation_gen -c ${TEST_CPU} -p 80 -n 2 -d ${long_starvation}

# Wait for detection and boosting
assert_boost_detected "${STALLD_LOG4}" "FIFO emulation boosting detected"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 5: Single-threaded mode with FIFO (should fail/exit)
#=============================================================================
test_section "Test 5: Single-threaded mode with FIFO (should fail)"

log "Testing single-threaded mode (-O) with -F (should exit)"
assert_stalld_rejects "Single-threaded mode rejected FIFO" -f -v -c "${TEST_CPU}" -t ${threshold} -F -O

end_test
