#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test backend selection functionality
#
# This test verifies that stalld can be started with different backends
# and that backend detection works correctly.
#
# Copyright (C) 2025 Red Hat Inc

TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"
parse_test_options "$@" || exit $?

start_test "Backend Selection"

# Setup test environment
setup_test_environment

require_root

# Helper: start stalld with a specific backend flag, verify the expected
# backend message appears in the log. This test intentionally bypasses
# start_stalld_with_log() because it needs to control the -b flag
# directly rather than inheriting it from STALLD_TEST_BACKEND.
#
# Usage: test_backend_flag <backend_flag> <expected_message> <description>
test_backend_flag() {
	local backend_flag=$1
	local expected_msg=$2
	local description=$3
	local log_file="/tmp/stalld_backend_${backend_flag}_$$.log"

	CLEANUP_FILES+=("${log_file}")

	"${TEST_ROOT}/../stalld" -v -f -l -g 1 -b "${backend_flag}" -t 60 \
		> "${log_file}" 2>&1 &
	STALLD_PID=$!
	CLEANUP_PIDS+=("${STALLD_PID}")

	if ! wait_for_stalld_ready "${log_file}" 15; then
		fail "stalld failed to start (${description})"
	fi

	assert_log_contains "${log_file}" "${expected_msg}" "${description}"

	stop_stalld
}

# Test 1: sched_debug backend (full name)
test_section "Test 1: Starting stalld with sched_debug backend"
test_backend_flag "sched_debug" "using sched_debug backend" \
	"sched_debug backend selected"

# Test 2: queue_track backend (if available)
test_section "Test 2: Check queue_track (BPF) backend"
if is_backend_available "queue_track"; then
	test_backend_flag "queue_track" "using queue_track backend" \
		"queue_track backend selected"
else
	echo "  queue_track (BPF) backend not available"
	echo "  (This is expected on i686, powerpc, ppc64le, or kernels <=3.x)"
	TEST_PASSED=$((TEST_PASSED + 1))
fi

# Test 3: Short name 'S' for sched_debug
test_section "Test 3: Testing short name 'S' for sched_debug"
test_backend_flag "S" "using sched_debug backend" \
	"Short name 'S' works for sched_debug"

# Test 4: STALLD_TEST_BACKEND environment variable
if [ -n "${STALLD_TEST_BACKEND}" ]; then
	test_section "Test 4: Testing STALLD_TEST_BACKEND=${STALLD_TEST_BACKEND}"
	# Normalize short names for expected message
	BACKEND_NORMALIZED="${STALLD_TEST_BACKEND}"
	case "${STALLD_TEST_BACKEND}" in
		S) BACKEND_NORMALIZED="sched_debug" ;;
		Q) BACKEND_NORMALIZED="queue_track" ;;
	esac
	test_backend_flag "${STALLD_TEST_BACKEND}" \
		"using ${BACKEND_NORMALIZED} backend" \
		"STALLD_TEST_BACKEND environment variable respected"
else
	test_section "Test 4: Skipping (STALLD_TEST_BACKEND not set)"
	TEST_PASSED=$((TEST_PASSED + 1))
fi

end_test
