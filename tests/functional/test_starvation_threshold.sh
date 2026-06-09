#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: stalld -t/--starving_threshold option
# Verifies that stalld detects starvation after the configured threshold
#
# IMPORTANT: stalld must run on a different CPU than the test CPU to avoid
# interference with the starvation scenario. This test uses CPU affinity (-a)
# to ensure stalld runs on a separate CPU.
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "Starvation Threshold Option (-t)" "test_threshold"

#=============================================================================
# Test 1: Custom threshold (5 seconds)
#=============================================================================
test_section "Test 1: Custom threshold of 5 seconds"

threshold=5

# Create starvation BEFORE starting stalld (avoid detecting kworker tasks)
starvation_duration=10
log "Creating starvation on CPU ${TEST_CPU} for ${starvation_duration}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

log "Starting stalld with ${threshold}s threshold"
# Use -i to ignore kernel workers that may starve before our test tasks
start_stalld_with_log "${STALLD_LOG}" -f -v -N -M -g 1 -i "ksoftirqd,kworker" -c "${TEST_CPU}" -a "${STALLD_CPU}" -t ${threshold}

# Wait for starvation detection
log "Waiting for detection (threshold: ${threshold}s)"

assert_starvation_detected "${STALLD_LOG}" "Starvation detected after ${threshold}s threshold"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 2: Verify no detection before threshold
#=============================================================================
test_section "Test 2: No detection before threshold"

threshold=10
rm -f "${STALLD_LOG}"

# Create starvation BEFORE starting stalld (avoid detecting kworker tasks)
# Create starvation that will last 6 seconds (less than threshold)
starvation_duration=6
log "Creating short starvation (${starvation_duration}s) with threshold of ${threshold}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

log "Starting stalld with ${threshold}s threshold"
start_stalld_with_log "${STALLD_LOG}" -f -v -N -M -g 1 -c "${TEST_CPU}" -a "${STALLD_CPU}" -t ${threshold}

# Wait for starvation generator to fully complete
wait "${STARVE_PID}" 2>/dev/null || true

# Give stalld time to process and log (if it were to detect)
sleep 1

# Check that starvation_gen was NOT detected (duration less than threshold)
assert_log_contains --negate "${STALLD_LOG}" \
    "starvation_gen.*starved on CPU ${TEST_CPU}\|starved on CPU ${TEST_CPU}.*starvation_gen" \
    "No starvation detected for duration less than threshold"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 3: Shorter threshold (3 seconds)
#=============================================================================
test_section "Test 3: Shorter threshold (3 seconds)"

threshold=3
rm -f "${STALLD_LOG}"

# Create starvation BEFORE starting stalld (avoid detecting kworker tasks)
# Create starvation for 8 seconds
starvation_duration=8
log "Creating starvation for ${starvation_duration}s with threshold of ${threshold}s"
start_starvation_gen -c "${TEST_CPU}" -p 80 -n 2 -d ${starvation_duration}

log "Starting stalld with ${threshold}s threshold"
# Use -i to ignore kernel workers that may starve before our test tasks
start_stalld_with_log "${STALLD_LOG}" -f -v -N -M -g 1 -i "ksoftirqd,kworker" -c "${TEST_CPU}" -a "${STALLD_CPU}" -t ${threshold}

# Wait for starvation detection
log "Waiting for detection (threshold: ${threshold}s)"

# Check if starvation_gen was detected
assert_starvation_detected "${STALLD_LOG}" "Starvation detected with ${threshold}s threshold"

# Cleanup
cleanup_scenario "${STARVE_PID}"

#=============================================================================
# Test 4: Invalid threshold values
#=============================================================================
test_section "Test 4: Invalid threshold values"

log "Testing with threshold = 0"
assert_stalld_rejects "Zero threshold rejected with error" -f -v -t 0

log "Testing with threshold = -5"
assert_stalld_rejects "Negative threshold rejected with error" -f -v -t -5

end_test
