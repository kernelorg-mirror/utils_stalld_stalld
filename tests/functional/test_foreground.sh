#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: Foreground mode (-f/--foreground)
# Verify stalld runs in foreground and doesn't daemonize
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

start_test "Foreground Mode"

# Setup test environment
setup_test_environment

# Require root for this test
require_root

# Test 1: Without -f flag, stalld should daemonize
test_section "Test 1: stalld daemonizes by default"
start_stalld -l -t 5

assert_process_running "${STALLD_PID}" "stalld should be running"

PARENT_PID=$(ps -o ppid= -p ${STALLD_PID} 2>/dev/null | tr -d ' ')
if [ "${PARENT_PID}" == "1" ] || [ "${PARENT_PID}" == "2" ]; then
	pass "stalld daemonized (parent is init/kthreadd)"
else
	assert_success "stalld daemonized (parent is not test shell)" test "${PARENT_PID}" != "$$"
fi

stop_stalld

# Test 2: With -f flag, stalld should stay in foreground
test_section "Test 2: stalld stays in foreground with -f"

# Start stalld in foreground but in background job
start_stalld -f -l -t 5

assert_process_running "${STALLD_PID}" "stalld should be running with -f"

PARENT_PID=$(ps -o ppid= -p ${STALLD_PID} 2>/dev/null | tr -d ' ')
assert_success "stalld did not daemonize with -f (parent is not init)" test "${PARENT_PID}" != "1"

stop_stalld

# Test 3: With -v flag, foreground mode should be implicit
test_section "Test 3: -v implies foreground mode"

start_stalld -v -l -t 5

assert_process_running "${STALLD_PID}" "stalld should be running with -v"

PARENT_PID=$(ps -o ppid= -p ${STALLD_PID} 2>/dev/null | tr -d ' ')
assert_success "-v implies foreground mode" test "${PARENT_PID}" != "1"

stop_stalld

end_test
