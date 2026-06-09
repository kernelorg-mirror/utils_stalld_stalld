#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Test: stalld -p/--boost_period option
# Verifies that stalld uses the specified SCHED_DEADLINE period

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Boost Period Option (-p)" "test_boost_period"

#=============================================================================
# Test 1: Default period (should be 1,000,000,000 ns = 1 second)
#=============================================================================
test_section "Test 1: Default period (no -p specified)"

threshold=5
log "Starting stalld with default period"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t $threshold -N

# Create starvation
starvation_duration=$((threshold + 5))
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for starvation detection and boosting
assert_boost_detected "${STALLD_LOG}" "Boosting occurred with default period"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Custom period (500ms = 500,000,000 ns)
#=============================================================================
test_section "Test 2: Custom period of 500,000,000 ns (500ms)"

custom_period=500000000
rm -f "${STALLD_LOG}"
log "Starting stalld with custom period ${custom_period} ns"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t $threshold -p $custom_period -N

# Create starvation
log "Creating starvation on CPU ${TEST_CPU}"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for starvation detection and boosting
assert_boost_detected "${STALLD_LOG}" "Boosting occurred with custom period ${custom_period} ns"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Very short period (100ms = 100,000,000 ns)
#=============================================================================
test_section "Test 3: Short period of 200,000,000 ns (200ms)"

short_period=200000000
rm -f "${STALLD_LOG}"
log "Starting stalld with short period ${short_period} ns"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t $threshold -p $short_period -N

# Create starvation
log "Creating starvation on CPU ${TEST_CPU}"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for starvation detection and boosting
assert_boost_detected "${STALLD_LOG}" "Boosting occurred with short period ${short_period} ns"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 4: Very long period (10s = 10,000,000,000 ns)
#=============================================================================
test_section "Test 4: Long period of 3,000,000,000 ns (3s)"

long_period=3000000000
rm -f "${STALLD_LOG}"
log "Starting stalld with long period ${long_period} ns"
start_stalld_with_log "${STALLD_LOG}" -f -v -c "${TEST_CPU}" -a ${STALLD_CPU} -t $threshold -p $long_period -N

# Create starvation
log "Creating starvation on CPU ${TEST_CPU}"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

# Wait for starvation detection and boosting
assert_boost_detected "${STALLD_LOG}" "Boosting occurred with long period ${long_period} ns"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 5: Invalid period (0)
#=============================================================================
test_section "Test 5: Invalid period value (0)"

assert_stalld_rejects "Zero period rejected with error" -f -v -t $threshold -p 0

#=============================================================================
# Test 6: Negative period
#=============================================================================
test_section "Test 6: Invalid period value (negative)"

assert_stalld_rejects "Negative period rejected with error" -f -v -t $threshold -p -1000000

end_test
