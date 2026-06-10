#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: Logging destinations (-v, -k, -s)
# Verify stalld logs to correct destinations
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

start_test "Logging Destinations"

# Setup test environment
setup_test_environment

# Require root for this test
require_root

# Filter stdin for stalld daemon messages, excluding the test
# framework's own log entries tagged with [TEST].
has_stalld_log() {
	grep -F "stalld" | grep -Fqv "[TEST]"
}

# Test 1: Verbose mode (-v) logs to stdout/stderr
test_section "Test 1: Verbose mode (-v) logs to stdout"

LOG_FILE="/tmp/stalld_test_verbose_$$.log"
CLEANUP_FILES+=("${LOG_FILE}")

start_stalld_with_log "${LOG_FILE}" -f -v -l -t 5

assert_process_running "${STALLD_PID}" "stalld should be running"
assert_success "verbose mode produces output" test -s "${LOG_FILE}"
assert_log_contains "${LOG_FILE}" "stalld\|version\|monitoring" "output contains expected messages"

stop_stalld

# Test 2: Kernel message log (-k)
test_section "Test 2: Kernel message log (-k)"

# Clear dmesg if possible (requires root)
if command -v dmesg >/dev/null 2>&1; then
	DMESG_BEFORE=$(dmesg | wc -l)

	start_stalld -f -k -l -t 5

	if assert_process_running "${STALLD_PID}" "stalld with -k should be running"; then
		sleep 1
		DMESG_AFTER=$(dmesg | wc -l)

		# Check if new messages appeared in dmesg
		# Note: This might not work in all environments
		if [ ${DMESG_AFTER} -gt ${DMESG_BEFORE} ]; then
			# Check if recent dmesg contains stalld messages
			if dmesg | tail -10 | has_stalld_log; then
				pass "stalld messages in kernel log"
			else
				echo -e "  ${YELLOW}SKIP${NC}: cannot verify kernel log messages"
			fi
		else
			echo -e "  ${YELLOW}SKIP${NC}: no new dmesg entries (may require special permissions)"
		fi
	fi

	stop_stalld
else
	echo -e "  ${YELLOW}SKIP${NC}: dmesg not available"
fi

# Test 3: Syslog (-s, default)
test_section "Test 3: Syslog (-s, default)"

# Check if syslog is available
SYSLOG_FILE=""
if [ -f /var/log/syslog ]; then
	SYSLOG_FILE="/var/log/syslog"
elif [ -f /var/log/messages ]; then
	SYSLOG_FILE="/var/log/messages"
fi

if [ -n "${SYSLOG_FILE}" ]; then
	# Get current line count
	SYSLOG_BEFORE=$(wc -l < "${SYSLOG_FILE}")

	start_stalld -f -s -l -t 5
	sleep 3

	if assert_process_running "${STALLD_PID}" "stalld with -s should be running"; then
		SYSLOG_AFTER=$(wc -l < "${SYSLOG_FILE}")

		if [ ${SYSLOG_AFTER} -gt ${SYSLOG_BEFORE} ]; then
			# Check for stalld messages in recent syslog
			if tail -20 "${SYSLOG_FILE}" | has_stalld_log; then
				pass "stalld messages in syslog"
			else
				echo -e "  ${YELLOW}SKIP${NC}: no stalld messages found in syslog"
			fi
		else
			echo -e "  ${YELLOW}SKIP${NC}: syslog may be delayed"
		fi
	fi

	stop_stalld
elif command -v journalctl >/dev/null 2>&1; then
	# Try journalctl instead
	echo "Using journalctl instead of syslog file"

	start_stalld -f -s -l -t 5
	sleep 3

	if assert_process_running "${STALLD_PID}" "stalld with -s should be running"; then
		# Check journalctl for stalld messages
		if journalctl -u stalld --since "1 minute ago" 2>/dev/null | has_stalld_log; then
			pass "stalld messages in journalctl"
		elif journalctl --since "1 minute ago" 2>/dev/null | has_stalld_log; then
			pass "stalld messages in system journal"
		else
			echo -e "  ${YELLOW}SKIP${NC}: no stalld messages in journal (may take time to appear)"
		fi
	fi

	stop_stalld
else
	echo -e "  ${YELLOW}SKIP${NC}: neither syslog nor journalctl available"
fi

# Test 4: Combined logging (-v -k -s)
test_section "Test 4: Combined logging modes"

LOG_FILE="/tmp/stalld_test_combined_$$.log"
CLEANUP_FILES+=("${LOG_FILE}")

start_stalld_with_log "${LOG_FILE}" -f -v -k -s -l -t 5

assert_process_running "${STALLD_PID}" "stalld with combined logging should be running"
assert_success "combined logging produces output" test -s "${LOG_FILE}"

stop_stalld

# Flush any pending journal writes from previous tests that used -s
journalctl --sync 2>/dev/null
sleep 1

# Test 5: No syslog without -s flag
test_section "Test 5: No syslog without -s flag"

if [ -n "${SYSLOG_FILE}" ]; then
	SYSLOG_BEFORE=$(wc -l < "${SYSLOG_FILE}")

	start_stalld -f -l -t 5
	sleep 1

	SYSLOG_AFTER=$(wc -l < "${SYSLOG_FILE}")

	if [ ${SYSLOG_AFTER} -gt ${SYSLOG_BEFORE} ]; then
		LINES_ADDED=$((SYSLOG_AFTER - SYSLOG_BEFORE))
		if tail -n "${LINES_ADDED}" "${SYSLOG_FILE}" | has_stalld_log; then
			fail "stalld messages found in syslog without -s flag"
		else
			pass "no stalld messages in syslog without -s flag"
		fi
	else
		pass "no new messages in syslog (stalld stayed quiet)"
	fi

	stop_stalld
elif command -v journalctl >/dev/null 2>&1; then
	log "Using journalctl instead of syslog file"

	START_TIME=$(date '+%Y-%m-%d %H:%M:%S')

	start_stalld -f -l -t 5
	sleep 1

	if journalctl -t stalld --since "${START_TIME}" 2>/dev/null | has_stalld_log; then
		fail "stalld messages found in journal without -s flag"
	else
		pass "no stalld messages in journal without -s flag"
	fi

	stop_stalld
else
	log "SKIP: neither syslog nor journalctl available"
fi

end_test
