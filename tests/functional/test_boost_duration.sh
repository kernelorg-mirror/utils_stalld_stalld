#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: stalld -d/--boost_duration option
# Verifies that stalld boosts tasks for the specified duration
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Boost Duration Option (-d)" "test_boost_duration"

#=============================================================================
# Test 1: Default duration (should be 3 seconds)
#=============================================================================
test_section "Test 1: Default boost duration (no -d specified)"

threshold=3
log "Starting stalld with ${threshold}s threshold (default boost duration)"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold}

# Create starvation
starvation_duration=15
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with default duration"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Short duration (1 second)
#=============================================================================
test_section "Test 2: Short boost duration of 1 second"

short_duration=1
rm -f "${STALLD_LOG}"

log "Starting stalld with ${threshold}s threshold and ${short_duration}s boost duration"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold} -d ${short_duration}

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with ${short_duration}s duration"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Long duration (10 seconds)
#=============================================================================
test_section "Test 3: Long boost duration of 10 seconds"

long_duration=10
long_starvation=20
threshold=10
rm -f "${STALLD_LOG}"

log "Starting stalld with ${threshold}s threshold and ${long_duration}s boost duration"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold} -d ${long_duration}

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${long_starvation}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${long_starvation}

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with ${long_duration}s duration" 20

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 4: Verify task policy is restored after boost duration
#=============================================================================
test_section "Test 4: Verify policy restoration after boost duration"

threshold=3
duration=2
rm -f "${STALLD_LOG}"

log "Starting stalld with ${threshold}s threshold and ${duration}s boost duration"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold} -d ${duration}

# Create starvation with a specific task we can track
log "Creating starvation on CPU ${TEST_CPU} for 15s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 1 -d 15

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with ${duration}s boost duration"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 5: Invalid duration values
#=============================================================================
test_section "Test 5: Invalid duration values"

log "Testing with duration = 0"
assert_stalld_rejects "Zero duration rejected with error" -f -v -t ${threshold} -d 0

log "Testing with duration = -5"
assert_stalld_rejects "Negative duration rejected with error" -f -v -t ${threshold} -d -5

end_test
