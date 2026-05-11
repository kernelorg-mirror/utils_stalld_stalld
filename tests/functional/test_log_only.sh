#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: Log-only mode (-l/--log_only)
# Verify stalld detects starvation but doesn't boost with -l flag
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

start_test "Log-only Mode"

# Setup test environment
setup_test_environment

# Require root for this test
require_root

# Check RT throttling
if ! check_rt_throttling; then
	echo -e "${YELLOW}SKIP: RT throttling must be disabled for this test${NC}"
	exit 77  # autotools SKIP exit code
fi

# Pick a CPU for testing
TEST_CPU=$(pick_test_cpu)
echo "Using CPU ${TEST_CPU} for testing"

# Pick a different CPU for stalld to run on (avoid interference)
STALLD_CPU=0
if [ ${TEST_CPU} -eq 0 ]; then
    STALLD_CPU=1
fi
echo "Stalld will run on CPU ${STALLD_CPU}"

# Test: stalld with -l should log starvation but not boost

# Create a temp log file for stalld output
LOG_FILE="/tmp/stalld_test_log_only_$$.log"
CLEANUP_FILES+=("${LOG_FILE}")

echo "Creating starvation on CPU ${TEST_CPU} (will run for 10 seconds)"

# Start starvation generator BEFORE stalld to ensure CPU is busy from the start
start_starvation_gen -c ${TEST_CPU} -p 10 -n 1 -d 10
STARVGEN_PID=${STARVE_PID}

# Start stalld in log-only mode with verbose output to capture logs
echo "Starting stalld in log-only mode with 5 second threshold"
start_stalld_with_log "${LOG_FILE}" -f -v -l -t 5 -c ${TEST_CPU} -a ${STALLD_CPU}

echo "Starvation generator started (PID ${STARVGEN_PID})"
echo "Waiting for starvation detection..."

# Check if stalld detected the starvation (should log it)
assert_starvation_detected "${LOG_FILE}" "stalld detected and logged starvation"

# Check that stalld did NOT boost (should not see "boosted" message with -l)
assert_log_contains --negate "${LOG_FILE}" "boosted" "stalld did not boost in log-only mode"

# Cleanup
cleanup_scenario "${STARVGEN_PID}"

end_test
