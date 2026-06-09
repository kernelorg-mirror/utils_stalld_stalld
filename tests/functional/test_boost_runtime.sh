#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: stalld -r/--boost_runtime option
# Verifies that stalld uses the specified SCHED_DEADLINE runtime
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Boost Runtime Option (-r)" "test_boost_runtime"

#=============================================================================
# Test 1: Default runtime (should be 20,000 ns = 20 microseconds)
#=============================================================================
test_section "Test 1: Default runtime (no -r specified)"

threshold=3
log "Starting stalld with ${threshold}s threshold (default boost runtime)"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold}

# Create starvation
starvation_duration=10
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with default runtime"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Custom runtime (10,000 ns = 10 microseconds, less than default)
#=============================================================================
test_section "Test 2: Custom runtime of 10,000 ns (10μs)"

custom_runtime=10000
rm -f "${STALLD_LOG}"

log "Starting stalld with ${threshold}s threshold and ${custom_runtime}ns runtime"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold} -r ${custom_runtime}

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with custom runtime ${custom_runtime}ns"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Larger runtime (100,000 ns = 100 microseconds)
#=============================================================================
test_section "Test 3: Larger runtime of 100,000 ns (100μs)"

large_runtime=100000
rm -f "${STALLD_LOG}"

log "Starting stalld with ${threshold}s threshold and ${large_runtime}ns runtime"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold} -r ${large_runtime}

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with large runtime ${large_runtime}ns"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 4: Runtime < period (valid configuration)
#=============================================================================
test_section "Test 4: Runtime < period (valid)"

# Default period is 1,000,000,000 ns, so runtime of 500,000 ns should be valid
valid_runtime=500000
period=1000000000
rm -f "${STALLD_LOG}"

log "Starting stalld with runtime ${valid_runtime}ns < period ${period}ns"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t ${threshold} -r ${valid_runtime} -p ${period}

# Create starvation
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for actual boosting
assert_boost_detected "${STALLD_LOG}" "Boost with runtime ${valid_runtime}ns < period ${period}ns"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 5: Runtime > period (should error or be rejected)
#=============================================================================
test_section "Test 5: Runtime > period (invalid)"

invalid_runtime=2000000000
period=1000000000

log "Testing with runtime ${invalid_runtime}ns > period ${period}ns"
assert_stalld_rejects "Runtime > period rejected with error" -f -v -t ${threshold} -r ${invalid_runtime} -p ${period}

#=============================================================================
# Test 6: Invalid runtime (0)
#=============================================================================
test_section "Test 6: Invalid runtime value (0)"

log "Testing with runtime = 0"
assert_stalld_rejects "Zero runtime rejected with error" -f -v -t ${threshold} -r 0

#=============================================================================
# Test 7: Negative runtime
#=============================================================================
test_section "Test 7: Invalid runtime value (negative)"

log "Testing with runtime = -5000"
assert_stalld_rejects "Negative runtime rejected with error" -f -v -t ${threshold} -r -5000

end_test
