#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Main test runner for stalld test suite
#
# Copyright (C) 2025 Red Hat Inc

set -e

# Configuration
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STALLD_BIN="${TEST_ROOT}/../stalld"
RESULTS_DIR="${TEST_ROOT}/results"
LOG_FILE="${RESULTS_DIR}/test_run_$(date +%Y%m%d_%H%M%S).log"
BACKEND=""  # Backend to use for tests (empty = default)
BACKEND_MATRIX=1  # Test both backends by default
BACKENDS=("sched_debug" "queue_track")  # Backends to test
THREADING_MODE_MATRIX=0  # Threading mode matrix testing disabled by default (enable with --full-matrix)
THREADING_MODES=("power" "adaptive" "aggressive")  # Threading modes to test
THREADING_MODE=""  # Specific threading mode to use (empty = default)

# Source test helpers for RT throttling and DL-server management
source "${TEST_ROOT}/helpers/test_helpers.sh" 2>/dev/null || true

# RT throttling state
SAVED_RT_RUNTIME=""

# DL-server state
declare -A SAVED_DL_SERVER_RUNTIME

# Configuration flags
DISABLE_DL_SERVER=1

# Test categories
declare -a UNIT_TESTS
declare -a FUNC_TESTS
declare -a INTEG_TESTS

# Statistics
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0

# Per-backend statistics
declare -A BACKEND_TOTAL
declare -A BACKEND_PASSED
declare -A BACKEND_FAILED
declare -A BACKEND_SKIPPED

# Per-mode statistics (for full matrix testing)
declare -A MODE_TOTAL
declare -A MODE_PASSED
declare -A MODE_FAILED
declare -A MODE_SKIPPED

# Color output
if [ -t 1 ]; then
	RED='\033[0;31m'
	GREEN='\033[0;32m'
	YELLOW='\033[1;33m'
	BLUE='\033[0;34m'
	BOLD='\033[1m'
	NC='\033[0m' # No Color
else
	RED=''
	GREEN=''
	YELLOW=''
	BLUE=''
	BOLD=''
	NC=''
fi

# Print banner
print_banner() {
	echo -e "${BOLD}=========================================${NC}"
	echo -e "${BOLD}   stalld Test Suite - $(date)${NC}"
	if [ ${THREADING_MODE_MATRIX} -eq 1 ]; then
		echo -e "${BOLD}   Full Matrix: ${BACKENDS[*]} × ${THREADING_MODES[*]}${NC}"
	elif [ ${BACKEND_MATRIX} -eq 1 ]; then
		echo -e "${BOLD}   Testing backends: ${BACKENDS[*]}${NC}"
	elif [ -n "${BACKEND}" ]; then
		echo -e "${BOLD}   Backend: ${BACKEND}${NC}"
	fi
	if [ -n "${THREADING_MODE}" ] && [ ${THREADING_MODE_MATRIX} -eq 0 ]; then
		echo -e "${BOLD}   Threading mode: ${THREADING_MODE}${NC}"
	fi
	echo -e "${BOLD}=========================================${NC}"
}

# Save and disable RT throttling
save_and_disable_rt_throttling() {
	if [ -f /proc/sys/kernel/sched_rt_runtime_us ]; then
		SAVED_RT_RUNTIME=$(cat /proc/sys/kernel/sched_rt_runtime_us)
		echo -e "${BLUE}Saving RT throttling state: ${SAVED_RT_RUNTIME}${NC}" | tee -a "${LOG_FILE}"

		if [ "${SAVED_RT_RUNTIME}" != "-1" ]; then
			echo -e "${BLUE}Disabling RT throttling for test run...${NC}" | tee -a "${LOG_FILE}"
			echo -1 > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
			if [ $? -eq 0 ]; then
				echo -e "${GREEN}RT throttling disabled${NC}" | tee -a "${LOG_FILE}"
			else
				echo -e "${YELLOW}WARNING: Failed to disable RT throttling${NC}" | tee -a "${LOG_FILE}"
			fi
		fi
	fi
	echo "" | tee -a "${LOG_FILE}"
}

# Save and disable DL-server
save_and_disable_dl_server() {
	local dl_server_dir="/sys/kernel/debug/sched/fair_server"

	if [ ! -d "${dl_server_dir}" ]; then
		return 0  # DL-server not present
	fi

	echo -e "${BLUE}Saving DL-server state for all CPUs...${NC}" | tee -a "${LOG_FILE}"
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
		echo -e "${GREEN}Saved DL-server state for ${cpu_count} CPUs${NC}" | tee -a "${LOG_FILE}"

		# Now disable DL-server
		echo -e "${BLUE}Disabling DL-server for all CPUs...${NC}" | tee -a "${LOG_FILE}"
		local disabled_count=0

		for cpu_dir in "${dl_server_dir}"/cpu*; do
			if [ -d "${cpu_dir}" ]; then
				local cpu=$(basename "${cpu_dir}")
				local runtime_file="${cpu_dir}/runtime"

				if [ -f "${runtime_file}" ]; then
					echo 0 > "${runtime_file}" 2>/dev/null
					if [ $? -eq 0 ]; then
						disabled_count=$((disabled_count + 1))
					fi
				fi
			fi
		done

		if [ ${disabled_count} -gt 0 ]; then
			echo -e "${GREEN}Disabled DL-server for ${disabled_count} CPUs${NC}" | tee -a "${LOG_FILE}"
		else
			echo -e "${YELLOW}WARNING: Failed to disable DL-server${NC}" | tee -a "${LOG_FILE}"
		fi
	fi
	echo "" | tee -a "${LOG_FILE}"
}

# Restore RT throttling
restore_rt_throttling_state() {
	if [ -n "${SAVED_RT_RUNTIME}" ] && [ -f /proc/sys/kernel/sched_rt_runtime_us ]; then
		echo -e "\n${BLUE}Restoring RT throttling state: ${SAVED_RT_RUNTIME}${NC}"
		echo "${SAVED_RT_RUNTIME}" > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
		if [ $? -eq 0 ]; then
			echo -e "${GREEN}RT throttling restored${NC}"
		else
			echo -e "${YELLOW}WARNING: Failed to restore RT throttling state${NC}"
		fi
	fi
}

# Restore DL-server
restore_dl_server_state() {
	local dl_server_dir="/sys/kernel/debug/sched/fair_server"

	if [ ! -d "${dl_server_dir}" ]; then
		return 0  # DL-server not present
	fi

	if [ ${#SAVED_DL_SERVER_RUNTIME[@]} -eq 0 ]; then
		return 0  # Nothing was saved
	fi

	echo -e "\n${BLUE}Restoring DL-server state...${NC}"
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
		echo -e "${GREEN}Restored DL-server state for ${cpu_count} CPUs${NC}"
	fi
}

# Cleanup function
cleanup_runner() {
	local exit_code=$?

	# If interrupted by signal, print message
	if [ ${exit_code} -gt 128 ]; then
		echo ""
		echo -e "${YELLOW}Test run interrupted by signal${NC}"
	fi

	if [ $EUID -eq 0 ]; then
		# Kill any remaining stalld processes
		local pids=$(pgrep -x stalld 2>/dev/null)
		if [ -n "${pids}" ]; then
			echo -e "${BLUE}Cleaning up remaining stalld processes: ${pids}${NC}"
			for pid in ${pids}; do
				kill -9 ${pid} 2>/dev/null || true
			done
		fi

		restore_dl_server_state
		restore_rt_throttling_state
	fi
}

# Signal handler for clean interrupts
handle_interrupt() {
	echo ""
	echo -e "${YELLOW}Caught interrupt signal (Ctrl-C), cleaning up...${NC}"
	exit 130  # Standard exit code for SIGINT
}

# Set up cleanup traps
trap cleanup_runner EXIT
trap handle_interrupt INT TERM

# Kill any existing stalld processes before tests
kill_existing_stalld_processes() {
	local pids=$(pgrep -x stalld 2>/dev/null)
	if [ -n "${pids}" ]; then
		echo -e "${BLUE}Killing existing stalld processes: ${pids}${NC}" | tee -a "${LOG_FILE}"
		for pid in ${pids}; do
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
		# Verify
		pids=$(pgrep -x stalld 2>/dev/null)
		if [ -n "${pids}" ]; then
			echo -e "${YELLOW}WARNING: Could not kill all stalld processes: ${pids}${NC}" | tee -a "${LOG_FILE}"
		else
			echo -e "${GREEN}All existing stalld processes killed${NC}" | tee -a "${LOG_FILE}"
		fi
		echo "" | tee -a "${LOG_FILE}"
	fi
}

# Initialize
init_tests() {
	mkdir -p "${RESULTS_DIR}"

	print_banner | tee "${LOG_FILE}"
	echo "" | tee -a "${LOG_FILE}"

	# Kill any existing stalld processes from previous runs
	if [ $EUID -eq 0 ]; then
		kill_existing_stalld_processes
	fi

	# Save and disable RT throttling if running as root
	if [ $EUID -eq 0 ]; then
		save_and_disable_rt_throttling

		# Save and disable DL-server if requested
		if [ ${DISABLE_DL_SERVER} -eq 1 ]; then
			save_and_disable_dl_server
		fi
	fi

	# Check prerequisites
	check_prerequisites
}

# Check prerequisites
check_prerequisites() {
	echo -e "${BLUE}Checking prerequisites...${NC}" | tee -a "${LOG_FILE}"

	# Check if running as root
	if [ $EUID -ne 0 ]; then
		echo -e "${YELLOW}WARNING: Not running as root. Some tests will be skipped.${NC}" | tee -a "${LOG_FILE}"
	fi

	# Check if stalld binary exists
	if [ ! -f "${STALLD_BIN}" ]; then
		echo -e "${RED}ERROR: stalld binary not found at ${STALLD_BIN}${NC}" | tee -a "${LOG_FILE}"
		echo "Please run 'make' in the project root first." | tee -a "${LOG_FILE}"
		exit 1
	fi

	# Check if stalld is executable
	if [ ! -x "${STALLD_BIN}" ]; then
		echo -e "${RED}ERROR: stalld binary is not executable${NC}" | tee -a "${LOG_FILE}"
		exit 1
	fi

	# Check kernel version for BPF support
	KERNEL_VER=$(uname -r | cut -d. -f1)
	if [ "${KERNEL_VER}" -lt 4 ]; then
		echo -e "${YELLOW}WARNING: Kernel < 4.x, BPF tests will be skipped${NC}" | tee -a "${LOG_FILE}"
	fi

	echo "" | tee -a "${LOG_FILE}"
}

# Check if test should be skipped for given threading mode
should_skip_test_for_mode() {
	local test_path=$1
	local mode=$2
	local test_name=$(basename "${test_path}" .sh)

	# Power mode only works with SCHED_DEADLINE, not FIFO
	if [ "${mode}" == "power" ]; then
		case "${test_name}" in
			test_force_fifo|test_fifo_boosting)
				return 0  # Should skip
				;;
		esac
	fi

	return 1  # Don't skip
}

# Discover tests
discover_tests() {
	# Find unit tests (C executables)
	if [ -d "${TEST_ROOT}/unit" ]; then
		while IFS= read -r test; do
			UNIT_TESTS+=("${test}")
		done < <(find "${TEST_ROOT}/unit" -type f -executable -name "test_*" 2>/dev/null)
	fi

	# Find functional tests (shell scripts)
	if [ -d "${TEST_ROOT}/functional" ]; then
		while IFS= read -r test; do
			FUNC_TESTS+=("${test}")
		done < <(find "${TEST_ROOT}/functional" -type f -name "test_*.sh" 2>/dev/null)
	fi

	# Find integration tests (shell scripts)
	if [ -d "${TEST_ROOT}/integration" ]; then
		while IFS= read -r test; do
			INTEG_TESTS+=("${test}")
		done < <(find "${TEST_ROOT}/integration" -type f -name "test_*.sh" 2>/dev/null)
	fi

	# Add legacy test wrapper
	if [ -x "${TEST_ROOT}/legacy/test01_wrapper.sh" ]; then
		UNIT_TESTS=("${TEST_ROOT}/legacy/test01_wrapper.sh" "${UNIT_TESTS[@]}")
	fi
}

# Run unit tests
run_unit_tests() {
	if [ ${#UNIT_TESTS[@]} -eq 0 ]; then
		echo -e "${YELLOW}No unit tests found${NC}" | tee -a "${LOG_FILE}"
		return
	fi

	echo -e "\n${BOLD}${GREEN}Running Unit Tests${NC}" | tee -a "${LOG_FILE}"
	echo "-------------------------------------------" | tee -a "${LOG_FILE}"

	if [ ${THREADING_MODE_MATRIX} -eq 1 ]; then
		# Full matrix: Test with all backends × all threading modes
		for backend in "${BACKENDS[@]}"; do
			export STALLD_TEST_BACKEND="${backend}"
			for mode in "${THREADING_MODES[@]}"; do
				export STALLD_TEST_THREADING_MODE="${mode}"
				for test in "${UNIT_TESTS[@]}"; do
					if should_skip_test_for_mode "${test}" "${mode}"; then
						continue  # Skip this test for this mode
					fi
					run_unit_test "${test}" "${backend}:${mode}"
				done
			done
		done
	elif [ ${BACKEND_MATRIX} -eq 1 ]; then
		# Test with all backends
		for backend in "${BACKENDS[@]}"; do
			export STALLD_TEST_BACKEND="${backend}"
			for test in "${UNIT_TESTS[@]}"; do
				run_unit_test "${test}" "${backend}"
			done
		done
	else
		# Test with specified backend/mode or default
		for test in "${UNIT_TESTS[@]}"; do
			run_unit_test "${test}"
		done
	fi
}

run_unit_test() {
	local test_path=$1
	local test_name=$(basename "${test_path}")
	local backend_mode="${2:-}"  # Optional "backend" or "backend:mode" parameter

	TOTAL_TESTS=$((TOTAL_TESTS + 1))

	# Parse backend and mode from parameter
	local backend=""
	local mode=""
	if [ -n "${backend_mode}" ]; then
		if [[ "${backend_mode}" == *":"* ]]; then
			backend="${backend_mode%%:*}"
			mode="${backend_mode##*:}"
			MODE_TOTAL["${mode}"]=$((MODE_TOTAL["${mode}"] + 1))
		else
			backend="${backend_mode}"
		fi
		BACKEND_TOTAL["${backend}"]=$((BACKEND_TOTAL["${backend}"] + 1))
	fi

	if [ ! -x "${test_path}" ]; then
		echo -e "${YELLOW}SKIP${NC}: ${test_name} (not executable)" | tee -a "${LOG_FILE}"
		SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
		if [ -n "${backend}" ]; then
			BACKEND_SKIPPED["${backend}"]=$((BACKEND_SKIPPED["${backend}"] + 1))
		fi
		if [ -n "${mode}" ]; then
			MODE_SKIPPED["${mode}"]=$((MODE_SKIPPED["${mode}"] + 1))
		fi
		return
	fi

	# Add backend/mode prefix to test name
	local display_name="${test_name}"
	if [ -n "${backend_mode}" ]; then
		display_name="[${backend_mode}] ${test_name}"
	fi

	echo -n "Running ${display_name}... " | tee -a "${LOG_FILE}"

	local test_log="${RESULTS_DIR}/${test_name}.log"
	if [ -n "${backend_mode}" ]; then
		# Replace : with _ for filename
		test_log="${RESULTS_DIR}/${backend_mode//:/_}_${test_name}.log"
	fi

	if "${test_path}" > "${test_log}" 2>&1; then
		echo -e "${GREEN}PASS${NC}" | tee -a "${LOG_FILE}"
		PASSED_TESTS=$((PASSED_TESTS + 1))
		if [ -n "${backend}" ]; then
			BACKEND_PASSED["${backend}"]=$((BACKEND_PASSED["${backend}"] + 1))
		fi
		if [ -n "${mode}" ]; then
			MODE_PASSED["${mode}"]=$((MODE_PASSED["${mode}"] + 1))
		fi
	else
		echo -e "${RED}FAIL${NC}" | tee -a "${LOG_FILE}"
		echo "  See ${test_log} for details" | tee -a "${LOG_FILE}"
		FAILED_TESTS=$((FAILED_TESTS + 1))
		if [ -n "${backend}" ]; then
			BACKEND_FAILED["${backend}"]=$((BACKEND_FAILED["${backend}"] + 1))
		fi
		if [ -n "${mode}" ]; then
			MODE_FAILED["${mode}"]=$((MODE_FAILED["${mode}"] + 1))
		fi
	fi
}

# Run functional tests
run_functional_tests() {
	if [ ${#FUNC_TESTS[@]} -eq 0 ]; then
		echo -e "${YELLOW}No functional tests found${NC}" | tee -a "${LOG_FILE}"
		return
	fi

	echo -e "\n${BOLD}${GREEN}Running Functional Tests${NC}" | tee -a "${LOG_FILE}"
	echo "-------------------------------------------" | tee -a "${LOG_FILE}"

	if [ ${THREADING_MODE_MATRIX} -eq 1 ]; then
		# Full matrix: Test with all backends × all threading modes
		for backend in "${BACKENDS[@]}"; do
			export STALLD_TEST_BACKEND="${backend}"
			for mode in "${THREADING_MODES[@]}"; do
				export STALLD_TEST_THREADING_MODE="${mode}"
				for test in "${FUNC_TESTS[@]}"; do
					if should_skip_test_for_mode "${test}" "${mode}"; then
						continue  # Skip this test for this mode
					fi
					run_shell_test "${test}" "${backend}:${mode}"
				done
			done
		done
	elif [ ${BACKEND_MATRIX} -eq 1 ]; then
		# Test with all backends
		for backend in "${BACKENDS[@]}"; do
			export STALLD_TEST_BACKEND="${backend}"
			for test in "${FUNC_TESTS[@]}"; do
				run_shell_test "${test}" "${backend}"
			done
		done
	else
		# Test with specified backend/mode or default
		for test in "${FUNC_TESTS[@]}"; do
			run_shell_test "${test}"
		done
	fi
}

# Run integration tests
run_integration_tests() {
	if [ ${#INTEG_TESTS[@]} -eq 0 ]; then
		echo -e "${YELLOW}No integration tests found${NC}" | tee -a "${LOG_FILE}"
		return
	fi

	echo -e "\n${BOLD}${GREEN}Running Integration Tests${NC}" | tee -a "${LOG_FILE}"
	echo "-------------------------------------------" | tee -a "${LOG_FILE}"

	if [ ${THREADING_MODE_MATRIX} -eq 1 ]; then
		# Full matrix: Test with all backends × all threading modes
		for backend in "${BACKENDS[@]}"; do
			export STALLD_TEST_BACKEND="${backend}"
			for mode in "${THREADING_MODES[@]}"; do
				export STALLD_TEST_THREADING_MODE="${mode}"
				for test in "${INTEG_TESTS[@]}"; do
					if should_skip_test_for_mode "${test}" "${mode}"; then
						continue  # Skip this test for this mode
					fi
					run_shell_test "${test}" "${backend}:${mode}"
				done
			done
		done
	elif [ ${BACKEND_MATRIX} -eq 1 ]; then
		# Test with all backends
		for backend in "${BACKENDS[@]}"; do
			export STALLD_TEST_BACKEND="${backend}"
			for test in "${INTEG_TESTS[@]}"; do
				run_shell_test "${test}" "${backend}"
			done
		done
	else
		# Test with specified backend/mode or default
		for test in "${INTEG_TESTS[@]}"; do
			run_shell_test "${test}"
		done
	fi
}

run_shell_test() {
	local test_path=$1
	local test_name=$(basename "${test_path}" .sh)
	local backend_mode="${2:-}"  # Optional "backend" or "backend:mode" parameter

	TOTAL_TESTS=$((TOTAL_TESTS + 1))

	# Parse backend and mode from parameter
	local backend=""
	local mode=""
	if [ -n "${backend_mode}" ]; then
		if [[ "${backend_mode}" == *":"* ]]; then
			backend="${backend_mode%%:*}"
			mode="${backend_mode##*:}"
			MODE_TOTAL["${mode}"]=$((MODE_TOTAL["${mode}"] + 1))
		else
			backend="${backend_mode}"
		fi
		BACKEND_TOTAL["${backend}"]=$((BACKEND_TOTAL["${backend}"] + 1))
	fi

	# Add backend/mode prefix to test name
	local display_name="${test_name}"
	if [ -n "${backend_mode}" ]; then
		display_name="[${backend_mode}] ${test_name}"
	fi

	echo -n "Running ${display_name}... " | tee -a "${LOG_FILE}"

	local test_log="${RESULTS_DIR}/${test_name}.log"
	if [ -n "${backend_mode}" ]; then
		# Replace : with _ for filename
		test_log="${RESULTS_DIR}/${backend_mode//:/_}_${test_name}.log"
	fi

	if bash "${test_path}" > "${test_log}" 2>&1; then
		echo -e "${GREEN}PASS${NC}" | tee -a "${LOG_FILE}"
		PASSED_TESTS=$((PASSED_TESTS + 1))
		if [ -n "${backend}" ]; then
			BACKEND_PASSED["${backend}"]=$((BACKEND_PASSED["${backend}"] + 1))
		fi
		if [ -n "${mode}" ]; then
			MODE_PASSED["${mode}"]=$((MODE_PASSED["${mode}"] + 1))
		fi
	else
		local exit_code=$?
		if [ ${exit_code} -eq 77 ]; then
			# Exit code 77 = SKIP (autotools convention)
			echo -e "${YELLOW}SKIP${NC}" | tee -a "${LOG_FILE}"
			SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
			if [ -n "${backend}" ]; then
				BACKEND_SKIPPED["${backend}"]=$((BACKEND_SKIPPED["${backend}"] + 1))
			fi
			if [ -n "${mode}" ]; then
				MODE_SKIPPED["${mode}"]=$((MODE_SKIPPED["${mode}"] + 1))
			fi
		else
			echo -e "${RED}FAIL${NC}" | tee -a "${LOG_FILE}"
			echo "  See ${test_log} for details" | tee -a "${LOG_FILE}"
			FAILED_TESTS=$((FAILED_TESTS + 1))
			if [ -n "${backend}" ]; then
				BACKEND_FAILED["${backend}"]=$((BACKEND_FAILED["${backend}"] + 1))
			fi
			if [ -n "${mode}" ]; then
				MODE_FAILED["${mode}"]=$((MODE_FAILED["${mode}"] + 1))
			fi
		fi
	fi
}

# Print summary
print_summary() {
	echo -e "\n${BOLD}=========================================== ${NC}" | tee -a "${LOG_FILE}"
	echo -e "${BOLD}Test Summary:${NC}" | tee -a "${LOG_FILE}"
	echo "  Total:   ${TOTAL_TESTS}" | tee -a "${LOG_FILE}"
	echo -e "  ${GREEN}Passed:  ${PASSED_TESTS}${NC}" | tee -a "${LOG_FILE}"
	echo -e "  ${RED}Failed:  ${FAILED_TESTS}${NC}" | tee -a "${LOG_FILE}"
	echo -e "  ${YELLOW}Skipped: ${SKIPPED_TESTS}${NC}" | tee -a "${LOG_FILE}"

	# Print per-backend statistics if matrix testing enabled
	if [ ${BACKEND_MATRIX} -eq 1 ] && [ ${#BACKEND_TOTAL[@]} -gt 0 ]; then
		echo "" | tee -a "${LOG_FILE}"
		echo -e "${BOLD}Per-Backend Results:${NC}" | tee -a "${LOG_FILE}"
		for backend in "${BACKENDS[@]}"; do
			local total=${BACKEND_TOTAL["${backend}"]:-0}
			local passed=${BACKEND_PASSED["${backend}"]:-0}
			local failed=${BACKEND_FAILED["${backend}"]:-0}
			local skipped=${BACKEND_SKIPPED["${backend}"]:-0}

			echo "" | tee -a "${LOG_FILE}"
			echo -e "  ${BOLD}Backend: ${backend}${NC}" | tee -a "${LOG_FILE}"
			echo "    Total:   ${total}" | tee -a "${LOG_FILE}"
			echo -e "    ${GREEN}Passed:  ${passed}${NC}" | tee -a "${LOG_FILE}"
			echo -e "    ${RED}Failed:  ${failed}${NC}" | tee -a "${LOG_FILE}"
			echo -e "    ${YELLOW}Skipped: ${skipped}${NC}" | tee -a "${LOG_FILE}"
		done
	fi

	# Print per-mode statistics if full matrix testing enabled
	if [ ${THREADING_MODE_MATRIX} -eq 1 ] && [ ${#MODE_TOTAL[@]} -gt 0 ]; then
		echo "" | tee -a "${LOG_FILE}"
		echo -e "${BOLD}Per-Threading-Mode Results:${NC}" | tee -a "${LOG_FILE}"
		for mode in "${THREADING_MODES[@]}"; do
			local total=${MODE_TOTAL["${mode}"]:-0}
			local passed=${MODE_PASSED["${mode}"]:-0}
			local failed=${MODE_FAILED["${mode}"]:-0}
			local skipped=${MODE_SKIPPED["${mode}"]:-0}

			echo "" | tee -a "${LOG_FILE}"
			echo -e "  ${BOLD}Mode: ${mode}${NC}" | tee -a "${LOG_FILE}"
			echo "    Total:   ${total}" | tee -a "${LOG_FILE}"
			echo -e "    ${GREEN}Passed:  ${passed}${NC}" | tee -a "${LOG_FILE}"
			echo -e "    ${RED}Failed:  ${failed}${NC}" | tee -a "${LOG_FILE}"
			echo -e "    ${YELLOW}Skipped: ${skipped}${NC}" | tee -a "${LOG_FILE}"
		done
	fi

	if [ ${FAILED_TESTS} -eq 0 ]; then
		echo -e "\n${BOLD}${GREEN}All tests passed!${NC}" | tee -a "${LOG_FILE}"
	else
		echo -e "\n${BOLD}${RED}Some tests failed.${NC}" | tee -a "${LOG_FILE}"
	fi

	echo -e "${BOLD}===========================================${NC}" | tee -a "${LOG_FILE}"
	echo "" | tee -a "${LOG_FILE}"
	echo "Full log: ${LOG_FILE}" | tee -a "${LOG_FILE}"

	# Exit with failure if any tests failed
	[ ${FAILED_TESTS} -eq 0 ]
}

# Parse command-line options
UNIT_ONLY=0
FUNCTIONAL_ONLY=0
INTEGRATION_ONLY=0
SPECIFIC_TEST=""  # Run specific test by name

while [[ $# -gt 0 ]]; do
	case $1 in
		--unit-only)
			UNIT_ONLY=1
			shift
			;;
		--functional-only)
			FUNCTIONAL_ONLY=1
			shift
			;;
		--integration-only)
			INTEGRATION_ONLY=1
			shift
			;;
		-t|--test)
			SPECIFIC_TEST="$2"
			shift 2
			;;
		--disable-dl-server)
			DISABLE_DL_SERVER=1
			shift
			;;
		--no-backend-matrix)
			BACKEND_MATRIX=0
			shift
			;;
		--full-matrix)
			BACKEND_MATRIX=1
			THREADING_MODE_MATRIX=1
			shift
			;;
		--quick)
			# Quick mode: single backend (sched_debug), adaptive mode only
			BACKEND_MATRIX=0
			THREADING_MODE_MATRIX=0
			BACKEND="sched_debug"
			THREADING_MODE="adaptive"
			shift
			;;
		--backend-only)
			# Backend-only mode: test both backends, adaptive mode only
			BACKEND_MATRIX=1
			THREADING_MODE_MATRIX=0
			THREADING_MODE="adaptive"
			shift
			;;
		-b|--backend)
			BACKEND="$2"
			BACKEND_MATRIX=0  # Disable matrix when specific backend requested
			shift 2
			;;
		-m|--threading-mode)
			THREADING_MODE="$2"
			THREADING_MODE_MATRIX=0  # Disable matrix when specific mode requested
			shift 2
			;;
		-h|--help)
			echo "Usage: $0 [OPTIONS]"
			echo ""
			echo "Test Selection:"
			echo "  --unit-only          Run only unit tests"
			echo "  --functional-only    Run only functional tests"
			echo "  --integration-only   Run only integration tests"
			echo "  -t, --test <name>    Run specific test by name (e.g., test_fifo_boosting)"
			echo ""
			echo "Matrix Testing Modes:"
			echo "  (default)            Backend matrix: test both backends (2× runtime)"
			echo "  --full-matrix        Full matrix: test all backends × threading modes (6× runtime)"
			echo "  --backend-only       Backend matrix only, adaptive mode (2× runtime)"
			echo "  --quick              Fast mode: sched_debug + adaptive only (1× runtime)"
			echo "  --no-backend-matrix  Disable matrix testing (test default/specified only)"
			echo ""
			echo "Specific Backend/Mode:"
			echo "  -b, --backend <name>        Backend to use:"
			echo "                                sched_debug (or S) - debugfs/procfs backend"
			echo "                                queue_track (or Q) - eBPF backend (default)"
			echo "  -m, --threading-mode <mode> Threading mode to use:"
			echo "                                power      - Single-threaded (-O)"
			echo "                                adaptive   - Adaptive/conservative (-M)"
			echo "                                aggressive - Aggressive (-A)"
			echo ""
			echo "Other Options:"
			echo "  --disable-dl-server  Disable DL-server before running tests"
			echo "  -h, --help           Show this help"
			echo ""
			echo "Matrix Testing Details:"
			echo "  - Backend matrix tests both sched_debug and queue_track backends"
			echo "  - Full matrix tests 2 backends × 3 threading modes = 6 combinations"
			echo "  - Power mode skips FIFO tests (incompatible with single-threaded mode)"
			echo "  - Use --quick for fast iteration during development"
			exit 0
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

# Run a specific test by name
run_specific_test() {
	local test_name="$1"
	local found=0
	local test_path=""

	# Search for the test in all test directories
	for dir in unit functional integration legacy; do
		if [ -f "${TEST_ROOT}/${dir}/${test_name}.sh" ]; then
			test_path="${TEST_ROOT}/${dir}/${test_name}.sh"
			found=1
			break
		elif [ -f "${TEST_ROOT}/${dir}/${test_name}" ]; then
			test_path="${TEST_ROOT}/${dir}/${test_name}"
			found=1
			break
		fi
	done

	if [ ${found} -eq 0 ]; then
		echo -e "${RED}Error: Test '${test_name}' not found${NC}"
		echo "Searched in: unit/, functional/, integration/, legacy/"
		exit 1
	fi

	echo -e "${BOLD}=========================================${NC}"
	echo -e "${BOLD}   Running Single Test: ${test_name}${NC}"
	if [ -n "${BACKEND}" ]; then
		echo -e "${BOLD}   Backend: ${BACKEND}${NC}"
	fi
	if [ -n "${THREADING_MODE}" ]; then
		echo -e "${BOLD}   Threading mode: ${THREADING_MODE}${NC}"
	fi
	echo -e "${BOLD}=========================================${NC}"
	echo ""

	# Export settings
	export STALLD_TEST_BACKEND="${BACKEND}"
	export STALLD_TEST_THREADING_MODE="${THREADING_MODE}"

	# Run the test
	init_tests

	# Determine test type and run appropriately
	if [[ "${test_path}" == */unit/* ]]; then
		run_unit_test "${test_path}"
	else
		run_shell_test "${test_path}"
	fi

	# Print result
	echo ""
	if [ ${FAILED_TESTS} -eq 0 ]; then
		echo -e "${GREEN}${BOLD}✓ Test PASSED${NC}"
		exit 0
	else
		echo -e "${RED}${BOLD}✗ Test FAILED${NC}"
		echo -e "  See ${RESULTS_DIR}/${test_name}.log for details"
		exit 1
	fi
}

# Main execution
main() {
	# Export BACKEND and THREADING_MODE for use by test scripts
	export STALLD_TEST_BACKEND="${BACKEND}"
	export STALLD_TEST_THREADING_MODE="${THREADING_MODE}"

	init_tests
	discover_tests

	# Run test suites based on options
	if [ -n "${SPECIFIC_TEST}" ]; then
		run_specific_test "${SPECIFIC_TEST}"
	elif [ ${UNIT_ONLY} -eq 1 ]; then
		run_unit_tests
	elif [ ${FUNCTIONAL_ONLY} -eq 1 ]; then
		run_functional_tests
	elif [ ${INTEGRATION_ONLY} -eq 1 ]; then
		run_integration_tests
	else
		# Run all tests
		run_unit_tests
		run_functional_tests
		run_integration_tests
	fi

	# Print summary and exit
	print_summary
}

main "$@"
