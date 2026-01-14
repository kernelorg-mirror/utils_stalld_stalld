#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Common helper functions for stalld tests
#
# Copyright (C) 2025 Red Hat Inc

# Environment variables for test behavior:
#   STALLD_TEST_KEEP_DL_SERVER=1     - Skip DL-server disable (for debugging)
#   STALLD_TEST_KEEP_RT_THROTTLING=1 - Skip RT throttling disable (for debugging)
#   STALLD_TEST_BACKEND              - Backend to use (set by parse_test_options)
#   STALLD_TEST_THREADING_MODE       - Threading mode (set by parse_test_options)

# Test result tracking
TEST_NAME=""
TEST_PASSED=0
TEST_FAILED=0

# stalld PID
STALLD_PID=""

# Cleanup list
CLEANUP_PIDS=()
CLEANUP_FILES=()

# Color output
if [ -t 1 ]; then
	RED='\033[0;31m'
	GREEN='\033[0;32m'
	YELLOW='\033[1;33m'
	BLUE='\033[0;34m'
	NC='\033[0m' # No Color
else
	RED=''
	GREEN=''
	YELLOW=''
	BLUE=''
	NC=''
fi

# Logging function that writes to both stdout and journal
# This allows correlating test activities with stalld behavior in journalctl
log() {
	local timestamp="[$(date +'%H:%M:%S')]"
	local message="$*"

	# Echo to stdout with timestamp
	echo "${timestamp} ${message}"

	# Also send to journal with stalld tag for easy correlation
	# Strip ANSI color codes before sending to journal
	local clean_message=$(echo "${message}" | sed 's/\x1b\[[0-9;]*m//g')
	logger -t stalld "[TEST] ${clean_message}"
}

# Parse common test options
# Usage: parse_test_options "$@"
# This function should be called at the beginning of each test script
parse_test_options() {
	while [[ $# -gt 0 ]]; do
		case $1 in
			-b|--backend)
				export STALLD_TEST_BACKEND="$2"
				shift 2
				;;
			-m|--threading-mode)
				export STALLD_TEST_THREADING_MODE="$2"
				shift 2
				;;
			-h|--help)
				echo "Common test options:"
				echo "  -b, --backend <name>         Backend to use (sched_debug|S or queue_track|Q)"
				echo "  -m, --threading-mode <mode>  Threading mode (power|adaptive|aggressive)"
				echo "  -h, --help                   Show this help"
				return 1
				;;
			*)
				echo "Unknown option: $1"
				echo "Usage: $0 [-b|--backend <backend>] [-m|--threading-mode <mode>] [-h|--help]"
				return 1
				;;
		esac
	done
	return 0
}

# Start a test
start_test() {
	TEST_NAME=$1
	echo -e "${BLUE}=== Starting test: ${TEST_NAME} ===${NC}"
	# Log test start to journal for correlation
	logger -t stalld "[TEST] === Starting test: ${TEST_NAME} ==="
}

# End a test
end_test() {
	if [ ${TEST_FAILED} -eq 0 ]; then
		echo -e "${GREEN}=== Test ${TEST_NAME}: PASSED ===${NC}"
		logger -t stalld "[TEST] === Test ${TEST_NAME}: PASSED ==="
		return 0
	else
		echo -e "${RED}=== Test ${TEST_NAME}: FAILED ===${NC}"
		logger -t stalld "[TEST] === Test ${TEST_NAME}: FAILED ==="
		return 1
	fi
}

# Assert functions
assert_equals() {
	local expected=$1
	local actual=$2
	local message=${3:-""}

	if [ "${expected}" == "${actual}" ]; then
		echo -e "  ${GREEN}PASS${NC}: ${message}"
		TEST_PASSED=$((TEST_PASSED + 1))
		return 0
	else
		echo -e "  ${RED}FAIL${NC}: ${message}"
		echo "    Expected: ${expected}"
		echo "    Actual:   ${actual}"
		TEST_FAILED=$((TEST_FAILED + 1))
		return 1
	fi
}

assert_contains() {
	local haystack=$1
	local needle=$2
	local message=${3:-""}

	if echo "${haystack}" | grep -q "${needle}"; then
		echo -e "  ${GREEN}PASS${NC}: ${message}"
		TEST_PASSED=$((TEST_PASSED + 1))
		return 0
	else
		echo -e "  ${RED}FAIL${NC}: ${message}"
		echo "    String '${needle}' not found"
		TEST_FAILED=$((TEST_FAILED + 1))
		return 1
	fi
}

assert_not_contains() {
	local haystack=$1
	local needle=$2
	local message=${3:-""}

	if ! echo "${haystack}" | grep -q "${needle}"; then
		echo -e "  ${GREEN}PASS${NC}: ${message}"
		TEST_PASSED=$((TEST_PASSED + 1))
		return 0
	else
		echo -e "  ${RED}FAIL${NC}: ${message}"
		echo "    String '${needle}' found but should not be present"
		TEST_FAILED=$((TEST_FAILED + 1))
		return 1
	fi
}

assert_file_exists() {
	local file=$1
	local message=${2:-"File should exist: ${file}"}

	if [ -f "${file}" ]; then
		echo -e "  ${GREEN}PASS${NC}: ${message}"
		TEST_PASSED=$((TEST_PASSED + 1))
		return 0
	else
		echo -e "  ${RED}FAIL${NC}: ${message}"
		TEST_FAILED=$((TEST_FAILED + 1))
		return 1
	fi
}

assert_file_not_exists() {
	local file=$1
	local message=${2:-"File should not exist: ${file}"}

	if [ ! -f "${file}" ]; then
		echo -e "  ${GREEN}PASS${NC}: ${message}"
		TEST_PASSED=$((TEST_PASSED + 1))
		return 0
	else
		echo -e "  ${RED}FAIL${NC}: ${message}"
		TEST_FAILED=$((TEST_FAILED + 1))
		return 1
	fi
}

assert_process_running() {
	local pid=$1
	local message=${2:-"Process ${pid} should be running"}

	if kill -0 ${pid} 2>/dev/null; then
		echo -e "  ${GREEN}PASS${NC}: ${message}"
		TEST_PASSED=$((TEST_PASSED + 1))
		return 0
	else
		echo -e "  ${RED}FAIL${NC}: ${message}"
		TEST_FAILED=$((TEST_FAILED + 1))
		return 1
	fi
}

assert_process_not_running() {
	local pid=$1
	local message=${2:-"Process ${pid} should not be running"}

	if ! kill -0 ${pid} 2>/dev/null; then
		echo -e "  ${GREEN}PASS${NC}: ${message}"
		TEST_PASSED=$((TEST_PASSED + 1))
		return 0
	else
		echo -e "  ${RED}FAIL${NC}: ${message}"
		TEST_FAILED=$((TEST_FAILED + 1))
		return 1
	fi
}

# Start stalld in background
start_stalld() {
	local args="$@"

	# Find stalld binary - use TEST_ROOT if available, otherwise fall back to relative path
	local stalld_bin="${TEST_ROOT}/../stalld"
	if [ -z "${TEST_ROOT}" ]; then
		stalld_bin="../stalld"
	fi
	if [ ! -x "${stalld_bin}" ]; then
		echo -e "${RED}ERROR: stalld binary not found at ${stalld_bin}${NC}"
		return 1
	fi

	# Parse arguments to find pidfile if specified
	# Also detect if running in foreground mode
	# Use simple pattern matching instead of loop to avoid infinite loop bugs
	local pidfile=""
	local foreground_mode=0

	if [[ "$args" =~ --pidfile=([^\ ]+) ]]; then
		pidfile="${BASH_REMATCH[1]}"
	elif [[ "$args" =~ --pidfile[[:space:]]+([^\ ]+) ]]; then
		pidfile="${BASH_REMATCH[1]}"
	elif [[ "$args" =~ -P[[:space:]]+([^\ ]+) ]]; then
		pidfile="${BASH_REMATCH[1]}"
	fi

	# Check for foreground mode flags
	if [[ "$args" =~ -f([[:space:]]|$) ]] || [[ "$args" =~ --foreground([[:space:]]|$) ]] || [[ "$args" =~ -v([[:space:]]|$) ]]; then
		foreground_mode=1
	fi

	# Add backend option if STALLD_TEST_BACKEND is set
	if [ -n "${STALLD_TEST_BACKEND}" ]; then
		args="-b ${STALLD_TEST_BACKEND} ${args}"
		echo "Using backend: ${STALLD_TEST_BACKEND}"
	fi

	# Add threading mode flag if STALLD_TEST_THREADING_MODE is set
	if [ -n "${STALLD_TEST_THREADING_MODE}" ]; then
		case "${STALLD_TEST_THREADING_MODE}" in
			power)
				args="-O ${args}"
				echo "Using threading mode: power (single-threaded)"
				;;
			adaptive)
				args="-M ${args}"
				echo "Using threading mode: adaptive"
				;;
			aggressive)
				args="-A ${args}"
				echo "Using threading mode: aggressive"
				;;
			*)
				echo -e "${YELLOW}WARNING: Unknown threading mode '${STALLD_TEST_THREADING_MODE}', using default${NC}"
				;;
		esac
	fi

	${stalld_bin} ${args} &
	local shell_pid=$!

	# Strategy for finding the daemon PID depends on whether pidfile is specified
	if [ -n "$pidfile" ]; then
		# Wait for pidfile to be created (up to 15 seconds)
		# BPF initialization can take 10+ seconds on some architectures
		local timeout=15
		local elapsed=0
		while [ ! -f "$pidfile" ] && [ $elapsed -lt $timeout ]; do
			sleep 0.5
			elapsed=$((elapsed + 1))
		done

		if [ -f "$pidfile" ]; then
			STALLD_PID=$(cat "$pidfile" 2>/dev/null)
			if [ -z "${STALLD_PID}" ]; then
				echo -e "${RED}ERROR: pidfile exists but is empty${NC}"
				return 1
			fi
		else
			echo -e "${RED}ERROR: pidfile was not created within ${timeout} seconds${NC}"
			return 1
		fi
	else
		# No pidfile - use pgrep with retries
		# Strategy depends on foreground vs daemon mode
		local max_attempts=10
		local attempt=0
		STALLD_PID=""

		if [ $foreground_mode -eq 1 ]; then
			# Foreground mode: stalld doesn't daemonize, just find any stalld process
			sleep 1
			STALLD_PID=$(pgrep -n -x stalld 2>/dev/null)

			# If pgrep didn't find it, fall back to the shell PID
			if [ -z "${STALLD_PID}" ]; then
				if kill -0 ${shell_pid} 2>/dev/null; then
					STALLD_PID=${shell_pid}
				fi
			fi
		else
			# Daemon mode: parent forks, child becomes daemon (ppid=1), parent exits
			# We need to wait for and find the DAEMON child, not the exiting parent
			while [ $attempt -lt $max_attempts ]; do
				sleep 0.5

				# Get all stalld processes
				local pids=$(pgrep -x stalld 2>/dev/null)

				for pid in $pids; do
					if kill -0 $pid 2>/dev/null; then
						# Check parent PID - daemon should have ppid=1 (init) or ppid=2 (kthreadd)
						local ppid=$(ps -o ppid= -p $pid 2>/dev/null | tr -d ' ')

						# For daemonized processes, wait for ppid=1 or 2
						if [ "$ppid" = "1" ] || [ "$ppid" = "2" ]; then
							STALLD_PID=$pid
							# Wait a bit to ensure daemon is stable
							sleep 0.5
							# Verify it's still running
							if kill -0 $pid 2>/dev/null; then
								break 2  # Break out of both loops
							else
								# Daemon died, keep looking
								STALLD_PID=""
							fi
						fi
					fi
				done

				# If we found a daemonized process, we're done
				if [ -n "${STALLD_PID}" ]; then
					break
				fi

				attempt=$((attempt + 1))
			done

			# Last resort: use the shell PID if nothing else worked
			if [ -z "${STALLD_PID}" ]; then
				if kill -0 ${shell_pid} 2>/dev/null; then
					STALLD_PID=${shell_pid}
				fi
			fi
		fi
	fi

	# Verify we found a PID and it's running
	if [ -z "${STALLD_PID}" ]; then
		echo -e "${RED}ERROR: Could not determine stalld PID${NC}"
		return 1
	fi

	if ! kill -0 ${STALLD_PID} 2>/dev/null; then
		echo -e "${RED}ERROR: stalld PID ${STALLD_PID} is not running${NC}"
		return 1
	fi

	CLEANUP_PIDS+=("${STALLD_PID}")
	echo "stalld started with PID ${STALLD_PID}"
	return 0
}

# Stop stalld
stop_stalld() {
	if [ -n "${STALLD_PID}" ]; then
		if kill -0 ${STALLD_PID} 2>/dev/null; then
			# Try graceful shutdown first
			kill ${STALLD_PID} 2>/dev/null || true
			# Give it a moment to exit gracefully
			sleep 0.2
			# Force kill if still running
			if kill -0 ${STALLD_PID} 2>/dev/null; then
				kill -9 ${STALLD_PID} 2>/dev/null || true
			fi
			# Poll for process termination (don't use wait - might not be a child)
			local timeout=10
			local elapsed=0
			while kill -0 ${STALLD_PID} 2>/dev/null && [ ${elapsed} -lt ${timeout} ]; do
				sleep 0.1
				elapsed=$((elapsed + 1))
			done
		fi
		STALLD_PID=""
	fi
}

# Kill any existing stalld processes (cleanup from previous runs)
# This ensures a clean slate before starting tests
kill_existing_stalld() {
	local pids=$(pgrep -x stalld 2>/dev/null)
	if [ -n "${pids}" ]; then
		echo "Killing existing stalld processes: ${pids}"
		for pid in ${pids}; do
			# Try graceful shutdown first
			kill ${pid} 2>/dev/null || true
		done
		sleep 0.5
		# Force kill any remaining
		pids=$(pgrep -x stalld 2>/dev/null)
		if [ -n "${pids}" ]; then
			for pid in ${pids}; do
				kill -9 ${pid} 2>/dev/null || true
			done
			sleep 0.2
		fi
		# Verify all killed
		pids=$(pgrep -x stalld 2>/dev/null)
		if [ -n "${pids}" ]; then
			echo -e "${YELLOW}WARNING: Could not kill all stalld processes: ${pids}${NC}"
			return 1
		fi
		echo "All existing stalld processes killed"
	fi
	return 0
}

# Cleanup function (call in trap)
cleanup() {
	local exit_code=$?

	# Don't print cleanup messages if exiting normally (exit code 0)
	if [ ${exit_code} -ne 0 ]; then
		echo -e "${YELLOW}Cleaning up test resources...${NC}" >&2
	fi

	# Stop stalld
	stop_stalld

	# Kill any starvation generators first (these often have child processes)
	# Use pkill which handles process trees better
	pkill -9 -f starvation_gen 2>/dev/null || true

	# Small delay to let processes terminate
	sleep 0.2

	# Kill any tracked processes
	# Use SIGKILL (-9) and ignore EPERM errors (process may have different privileges)
	for pid in "${CLEANUP_PIDS[@]}"; do
		if [ -n "${pid}" ] && [ "${pid}" -gt 0 ] 2>/dev/null; then
			# Check if process exists
			if kill -0 ${pid} 2>/dev/null; then
				# Try gentle kill first
				kill ${pid} 2>/dev/null || true
				# Give it a moment
				sleep 0.1
				# Force kill if still running
				if kill -0 ${pid} 2>/dev/null; then
					kill -9 ${pid} 2>/dev/null || true
				fi
				# Poll for termination (don't use wait - might not be a child)
				local timeout=5
				local elapsed=0
				while kill -0 ${pid} 2>/dev/null && [ ${elapsed} -lt ${timeout} ]; do
					sleep 0.1
					elapsed=$((elapsed + 1))
				done
			fi
		fi
	done

	# Remove tracked files
	for file in "${CLEANUP_FILES[@]}"; do
		rm -f "${file}" 2>/dev/null
	done

	# Remove temp files
	rm -f /tmp/stalld_test_* 2>/dev/null || true

	# Restore DL-server if it was saved
	restore_dl_server || true

	# Restore RT throttling if it was saved
	restore_rt_throttling || true

	# Exit with the original exit code
	exit ${exit_code}
}

# Signal handler for interrupts
handle_signal() {
	echo ""
	echo -e "${YELLOW}Test interrupted by signal (Ctrl-C)${NC}" >&2
	exit 130  # Standard exit code for SIGINT
}

# Trap to ensure cleanup
trap cleanup EXIT
trap handle_signal INT TERM

# Parse stalld log for specific message
wait_for_log_message() {
	local pattern=$1
	local timeout=${2:-10}
	local log_file=${3:-/var/log/syslog}

	# If log_file doesn't exist, try journalctl
	if [ ! -f "${log_file}" ]; then
		# Using journalctl instead
		local elapsed=0
		while [ ${elapsed} -lt ${timeout} ]; do
			if journalctl -u stalld --since "1 minute ago" 2>/dev/null | grep -q "${pattern}"; then
				return 0
			fi
			sleep 1
			elapsed=$((elapsed + 1))
		done
		return 1
	fi

	local elapsed=0
	while [ ${elapsed} -lt ${timeout} ]; do
		if grep -q "${pattern}" "${log_file}"; then
			return 0
		fi
		sleep 1
		elapsed=$((elapsed + 1))
	done

	return 1
}

# Get thread scheduling policy
get_thread_policy() {
	local pid=$1
	if [ -f /proc/${pid}/sched ]; then
		awk '/policy/ {print $3}' /proc/${pid}/sched 2>/dev/null
	else
		echo "unknown"
	fi
}

# Get thread priority
get_thread_priority() {
	local pid=$1
	if [ -f /proc/${pid}/sched ]; then
		awk '/prio/ {print $3}' /proc/${pid}/sched 2>/dev/null
	else
		echo "unknown"
	fi
}

# Create CPU load on specific CPU
create_cpu_load() {
	local cpu=$1
	local duration=${2:-60}

	taskset -c ${cpu} dd if=/dev/zero of=/dev/null bs=1M count=999999 &
	local pid=$!
	CLEANUP_PIDS+=("${pid}")
	echo ${pid}
}

# Check if running as root
require_root() {
	if [ $EUID -ne 0 ]; then
		echo -e "${YELLOW}SKIP: This test requires root privileges${NC}"
		exit 0
	fi
}

# Check if RT throttling is disabled
check_rt_throttling() {
	if [ -f /proc/sys/kernel/sched_rt_runtime_us ]; then
		local throttle=$(cat /proc/sys/kernel/sched_rt_runtime_us)
		if [ "${throttle}" != "-1" ]; then
			echo -e "${YELLOW}WARNING: RT throttling is enabled (${throttle})${NC}"
			return 1
		fi
	fi
	return 0
}

# RT throttling state management
SAVED_RT_RUNTIME=""

# Save current RT throttling state
save_rt_throttling() {
	# Check override - skip if user wants to keep RT throttling enabled
	if [ "${STALLD_TEST_KEEP_RT_THROTTLING}" = "1" ]; then
		echo "Skipping RT throttling save (STALLD_TEST_KEEP_RT_THROTTLING=1)"
		return 0
	fi

	if [ -f /proc/sys/kernel/sched_rt_runtime_us ]; then
		SAVED_RT_RUNTIME=$(cat /proc/sys/kernel/sched_rt_runtime_us)
		echo "Saved RT throttling state: ${SAVED_RT_RUNTIME}"
	else
		echo -e "${YELLOW}WARNING: /proc/sys/kernel/sched_rt_runtime_us not found${NC}"
		SAVED_RT_RUNTIME=""
	fi
}

# DL-server state management
declare -A SAVED_DL_SERVER_RUNTIME

# Save current DL-server state for all CPUs
save_dl_server() {
	# Check override - skip if user wants to keep DL-server enabled
	if [ "${STALLD_TEST_KEEP_DL_SERVER}" = "1" ]; then
		echo "Skipping DL-server save (STALLD_TEST_KEEP_DL_SERVER=1)"
		return 0
	fi

	local dl_server_dir="/sys/kernel/debug/sched/fair_server"

	if [ ! -d "${dl_server_dir}" ]; then
		return 0  # DL-server not present, nothing to save
	fi

	echo "Saving DL-server state for all CPUs..."
	local cpu_count=0

	for cpu_dir in "${dl_server_dir}"/cpu*; do
		if [ -d "${cpu_dir}" ]; then
			local cpu=$(basename "${cpu_dir}")
			local runtime_file="${cpu_dir}/runtime"

			if [ -f "${runtime_file}" ]; then
				SAVED_DL_SERVER_RUNTIME["${cpu}"]=$(cat "${runtime_file}" 2>/dev/null)
				if [ $? -eq 0 ]; then
					cpu_count=$((cpu_count + 1))
				fi
			fi
		fi
	done

	if [ ${cpu_count} -gt 0 ]; then
		echo "Saved DL-server state for ${cpu_count} CPUs"
		return 0
	else
		echo -e "${YELLOW}WARNING: No DL-server runtime files found${NC}"
		return 1
	fi
}

# Restore DL-server state for all CPUs
restore_dl_server() {
	local dl_server_dir="/sys/kernel/debug/sched/fair_server"

	if [ ! -d "${dl_server_dir}" ]; then
		return 0  # DL-server not present, nothing to restore
	fi

	if [ ${#SAVED_DL_SERVER_RUNTIME[@]} -eq 0 ]; then
		return 0  # Nothing was saved
	fi

	echo "Restoring DL-server state..."
	local cpu_count=0

	for cpu in "${!SAVED_DL_SERVER_RUNTIME[@]}"; do
		local runtime_file="${dl_server_dir}/${cpu}/runtime"
		local saved_value="${SAVED_DL_SERVER_RUNTIME[${cpu}]}"

		if [ -f "${runtime_file}" ]; then
			echo "${saved_value}" > "${runtime_file}" 2>/dev/null
			if [ $? -eq 0 ]; then
				cpu_count=$((cpu_count + 1))
			else
				echo -e "${YELLOW}WARNING: Failed to restore ${cpu}/runtime${NC}"
			fi
		fi
	done

	if [ ${cpu_count} -gt 0 ]; then
		echo "Restored DL-server state for ${cpu_count} CPUs"
	fi

	# Clear saved state
	unset SAVED_DL_SERVER_RUNTIME
	declare -gA SAVED_DL_SERVER_RUNTIME
}

# Disable DL-server for all CPUs (set runtime to 0)
disable_dl_server() {
	# Check override - skip if user wants to keep DL-server enabled
	if [ "${STALLD_TEST_KEEP_DL_SERVER}" = "1" ]; then
		echo "Skipping DL-server disable (STALLD_TEST_KEEP_DL_SERVER=1)"
		return 0
	fi

	local dl_server_dir="/sys/kernel/debug/sched/fair_server"

	if [ ! -d "${dl_server_dir}" ]; then
		return 0  # DL-server not present, nothing to disable
	fi

	echo "Disabling DL-server for all CPUs..."
	local cpu_count=0

	for cpu_dir in "${dl_server_dir}"/cpu*; do
		if [ -d "${cpu_dir}" ]; then
			local cpu=$(basename "${cpu_dir}")
			local runtime_file="${cpu_dir}/runtime"

			if [ -f "${runtime_file}" ]; then
				echo 0 > "${runtime_file}" 2>/dev/null
				if [ $? -eq 0 ]; then
					cpu_count=$((cpu_count + 1))
				else
					echo -e "${YELLOW}WARNING: Failed to disable ${cpu}/runtime${NC}"
				fi
			fi
		fi
	done

	if [ ${cpu_count} -gt 0 ]; then
		echo "Disabled DL-server for ${cpu_count} CPUs"
		return 0
	else
		echo -e "${RED}ERROR: Failed to disable DL-server${NC}"
		return 1
	fi
}

# Unified test environment setup
# Manages both DL-server and RT throttling in one call
# This is the recommended way to set up test isolation
setup_test_environment() {
	echo "Setting up test environment..."

	# Kill any existing stalld processes from previous runs
	kill_existing_stalld

	# Save and disable RT throttling
	save_rt_throttling
	disable_rt_throttling

	# Save and disable DL-server (if present)
	save_dl_server
	disable_dl_server
}

# Restore RT throttling state
restore_rt_throttling() {
	if [ -n "${SAVED_RT_RUNTIME}" ] && [ -f /proc/sys/kernel/sched_rt_runtime_us ]; then
		echo "${SAVED_RT_RUNTIME}" > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
		if [ $? -eq 0 ]; then
			echo "Restored RT throttling state: ${SAVED_RT_RUNTIME}"
		else
			echo -e "${YELLOW}WARNING: Failed to restore RT throttling state${NC}"
		fi
		SAVED_RT_RUNTIME=""
	fi
}

# Disable RT throttling (for tests that require it)
disable_rt_throttling() {
	# Check override - skip if user wants to keep RT throttling enabled
	if [ "${STALLD_TEST_KEEP_RT_THROTTLING}" = "1" ]; then
		echo "Skipping RT throttling disable (STALLD_TEST_KEEP_RT_THROTTLING=1)"
		return 0
	fi

	if [ -f /proc/sys/kernel/sched_rt_runtime_us ]; then
		echo -1 > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
		if [ $? -eq 0 ]; then
			echo "Disabled RT throttling"
			return 0
		else
			echo -e "${RED}ERROR: Failed to disable RT throttling (need root?)${NC}"
			return 1
		fi
	else
		echo -e "${YELLOW}WARNING: /proc/sys/kernel/sched_rt_runtime_us not found${NC}"
		return 1
	fi
}

# Get number of CPUs
get_num_cpus() {
	nproc
}

# Get a list of online CPUs
get_online_cpus() {
	local num_cpus=$(get_num_cpus)
	local cpus=""

	for ((i=0; i<num_cpus; i++)); do
		if [ -f /sys/devices/system/cpu/cpu${i}/online ]; then
			if [ "$(cat /sys/devices/system/cpu/cpu${i}/online)" == "1" ]; then
				cpus="${cpus} ${i}"
			fi
		else
			# CPU 0 doesn't have online file, it's always online
			cpus="${cpus} ${i}"
		fi
	done

	echo ${cpus}
}

# Pick a CPU for testing (prefer last CPU)
pick_test_cpu() {
	local cpus=$(get_online_cpus)
	echo ${cpus##* }  # Return last CPU
}

#
# Backend detection and helpers
#

# Detect which backend stalld was compiled with (default)
detect_default_backend() {
	local stalld_bin="../stalld"
	if [ ! -x "${stalld_bin}" ]; then
		echo "unknown"
		return 1
	fi

	# Check if stalld was built with BPF support
	# Look for queue_track symbols in the binary
	if command -v nm >/dev/null 2>&1; then
		if nm "${stalld_bin}" 2>/dev/null | grep -q "queue_track"; then
			echo "queue_track"
			return 0
		fi
	fi

	# Fall back to sched_debug
	echo "sched_debug"
	return 0
}

# Check if a specific backend is available
is_backend_available() {
	local backend=$1
	local stalld_bin="../stalld"

	if [ ! -x "${stalld_bin}" ]; then
		return 1
	fi

	case "${backend}" in
		"sched_debug"|"S")
			# sched_debug is always available
			return 0
			;;
		"queue_track"|"Q")
			# Check if BPF backend is available
			if command -v nm >/dev/null 2>&1; then
				if nm "${stalld_bin}" 2>/dev/null | grep -q "queue_track"; then
					return 0
				fi
			fi
			return 1
			;;
		*)
			return 1
			;;
	esac
}

# Get list of available backends
get_available_backends() {
	local backends=()

	# sched_debug is always available
	backends+=("sched_debug")

	# Check for queue_track (BPF)
	if is_backend_available "queue_track"; then
		backends+=("queue_track")
	fi

	echo "${backends[@]}"
}

# Start stalld with specific backend
start_stalld_with_backend() {
	local backend=$1
	shift  # Remove backend from args
	local extra_args="$@"

	if ! is_backend_available "${backend}"; then
		echo -e "${YELLOW}WARNING: Backend '${backend}' not available, skipping test${NC}"
		return 77  # Skip exit code
	fi

	echo "Starting stalld with backend: ${backend}"
	start_stalld -b "${backend}" ${extra_args}
	return $?
}

#
# Consolidated helper functions (previously duplicated across tests)
#

# Logging with timestamp (used by many functional tests)
log() {
	echo "[$(date +'%H:%M:%S')] $*"
}

# Get scheduling policy (0=OTHER, 1=FIFO, 2=RR, 6=DEADLINE)
get_sched_policy() {
	local pid=$1
	if [ -f "/proc/${pid}/sched" ]; then
		awk '/^policy/ {print $3}' /proc/${pid}/sched 2>/dev/null
	else
		echo "-1"
	fi
}

# Get scheduling priority
get_sched_priority() {
	local pid=$1
	if [ -f "/proc/${pid}/sched" ]; then
		awk '/^prio/ {print $3}' /proc/${pid}/sched 2>/dev/null
	else
		echo "-1"
	fi
}

# Get nice value (field 19 in /proc/pid/stat)
get_nice_value() {
	local pid=$1
	if [ -f "/proc/${pid}/stat" ]; then
		awk '{print $19}' /proc/${pid}/stat 2>/dev/null
	else
		echo "99"
	fi
}

# Get total context switch count (voluntary + nonvoluntary)
get_ctxt_switches() {
	local pid=$1
	if [ -f "/proc/${pid}/status" ]; then
		local vol=$(grep voluntary_ctxt_switches /proc/${pid}/status | awk '{print $2}')
		local nonvol=$(grep nonvoluntary_ctxt_switches /proc/${pid}/status | awk '{print $2}')
		echo $((vol + nonvol))
	else
		echo "0"
	fi
}

# Start stalld with output redirected to a log file
# Usage: start_stalld_with_log <log_file> [stalld_args...]
start_stalld_with_log() {
	local log_file="$1"
	shift
	local args="$@"

	# Build stalld command with backend option if specified
	# Also add -g 1 for 1-second granularity to ensure timely detection
	local stalld_args="-g 1 $args"
	if [ -n "${STALLD_TEST_BACKEND}" ]; then
		stalld_args="-b ${STALLD_TEST_BACKEND} ${stalld_args}"
		echo "Using backend: ${STALLD_TEST_BACKEND}"
	fi

	# Start stalld with output redirected
	${TEST_ROOT}/../stalld ${stalld_args} > "${log_file}" 2>&1 &
	STALLD_PID=$!
	CLEANUP_PIDS+=("${STALLD_PID}")
	sleep 1
}

# Wait for scheduling policy to change to expected value
# Usage: wait_for_policy_change <pid> <expected_policy> [timeout]
wait_for_policy_change() {
	local pid=$1
	local expected_policy=$2
	local timeout=${3:-10}
	local elapsed=0

	while [ $elapsed -lt $timeout ]; do
		local current_policy=$(get_sched_policy $pid)
		if [ "$current_policy" = "$expected_policy" ]; then
			return 0
		fi
		sleep 1
		elapsed=$((elapsed + 1))
	done
	return 1
}

#
# Test template functions
#

# Calculate wait time for starvation detection
# Formula: threshold + granularity + processing_buffer
# Usage: wait_time=$(calculate_detection_timeout <threshold>)
calculate_detection_timeout() {
	local threshold=$1
	# Default: granularity=1s, processing_buffer=3s
	# Will use timeout constants once Part 2 is implemented
	echo $((threshold + 1 + 3))
}

# Standard test initialization for functional tests
# Performs: start_test, setup_test_environment, require_root,
#           RT throttling check, CPU selection, path setup
# Usage: init_functional_test "test_name" "log_suffix"
init_functional_test() {
	local test_name=$1
	local log_suffix=${2:-"test"}

	start_test "${test_name}"
	setup_test_environment
	require_root

	# Check RT throttling
	if ! check_rt_throttling; then
		echo -e "${YELLOW}SKIP: RT throttling must be disabled for this test${NC}"
		exit 77
	fi

	# Pick a CPU for testing
	TEST_CPU=$(pick_test_cpu)
	log "Using CPU ${TEST_CPU} for testing"

	# Pick a different CPU for stalld to avoid interference
	STALLD_CPU=0
	if [ ${TEST_CPU} -eq 0 ]; then
		STALLD_CPU=1
	fi
	log "Stalld will run on CPU ${STALLD_CPU}"

	# Setup paths
	STARVE_GEN="${TEST_ROOT}/helpers/starvation_gen"
	STALLD_LOG="/tmp/stalld_${log_suffix}_$$.log"
	CLEANUP_FILES+=("${STALLD_LOG}")

	# Export variables for use in test
	export TEST_CPU STALLD_CPU STARVE_GEN STALLD_LOG
}

# Export functions for use in tests
export -f start_test end_test
export -f assert_equals assert_contains assert_not_contains
export -f assert_file_exists assert_file_not_exists
export -f assert_process_running assert_process_not_running
export -f start_stalld stop_stalld kill_existing_stalld cleanup
export -f wait_for_log_message
export -f get_thread_policy get_thread_priority
export -f create_cpu_load
export -f detect_default_backend is_backend_available get_available_backends start_stalld_with_backend
export -f require_root check_rt_throttling
export -f save_rt_throttling restore_rt_throttling disable_rt_throttling
export -f save_dl_server restore_dl_server disable_dl_server
export -f setup_test_environment
export -f get_num_cpus get_online_cpus pick_test_cpu
export -f log get_sched_policy get_sched_priority get_nice_value get_ctxt_switches
export -f start_stalld_with_log wait_for_policy_change
export -f calculate_detection_timeout init_functional_test
