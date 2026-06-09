#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: CPU selection (-c/--cpu option)
# Verify stalld only monitors specified CPUs
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

is_cpu_monitored() {
    grep -q "adding cpu ${1}$" "$STALLD_LOG"
}

start_test "CPU Selection (-c option)"

# Setup test environment
setup_test_environment

require_root
check_rt_throttling

# Setup log file for stalld output
STALLD_LOG="/tmp/stalld_cpu_selection_$$.log"
CLEANUP_FILES+=("${STALLD_LOG}")

# Get available CPUs
num_cpus=$(nproc)
if [ "$num_cpus" -lt 2 ]; then
    echo "SKIP: Test requires at least 2 CPUs (found: $num_cpus)"
    exit 77
fi

echo "System has $num_cpus CPUs"

# Test 1: Single CPU monitoring
test_section "Test 1: Single CPU monitoring (-c 0)"
rm -f "${STALLD_LOG}"
start_stalld_with_log "${STALLD_LOG}" -f -v -c 0 -l -t 5

# Check that stalld mentions CPU 0
if is_cpu_monitored 0; then
    pass "stalld monitoring CPU 0"
else
    fail "stalld not monitoring CPU 0"
fi

stop_stalld

# Test 2: CPU list (comma-separated)
if [ "$num_cpus" -ge 4 ]; then
    test_section "Test 2: CPU list monitoring (-c 0,2)"
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -c 0,2 -l -t 5

    # Check for CPU 0 and CPU 2 in output
    cpu0_found=0
    cpu2_found=0
    if is_cpu_monitored 0; then
        cpu0_found=1
    fi
    if is_cpu_monitored 2; then
        cpu2_found=1
    fi

    assert_success "stalld monitoring CPUs 0 and 2" test "$cpu0_found" -eq 1 -a "$cpu2_found" -eq 1

    stop_stalld
else
    echo "SKIP: Test 2 requires at least 4 CPUs"
fi

# Test 3: CPU range
if [ "$num_cpus" -ge 4 ]; then
    test_section "Test 3: CPU range monitoring (-c 0-2)"
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -c 0-2 -l -t 5

    # Check for CPUs 0, 1, 2 in output
    cpu0_found=0
    cpu1_found=0
    cpu2_found=0
    if is_cpu_monitored 0; then
        cpu0_found=1
    fi
    if is_cpu_monitored 1; then
        cpu1_found=1
    fi
    if is_cpu_monitored 2; then
        cpu2_found=1
    fi

    assert_success "stalld monitoring CPUs 0-2" test "$cpu0_found" -eq 1 -a "$cpu1_found" -eq 1 -a "$cpu2_found" -eq 1

    stop_stalld
else
    echo "SKIP: Test 3 requires at least 4 CPUs"
fi

# Test 4: Combined format (list and range)
if [ "$num_cpus" -ge 6 ]; then
    test_section "Test 4: Combined format (-c 0,2-4)"
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -c 0,2-4 -l -t 5

    # Should monitor CPUs 0, 2, 3, 4
    monitored_cpus=0
    for cpu in 0 2 3 4; do
        if is_cpu_monitored ${cpu}; then
            ((monitored_cpus++))
        fi
    done

    assert_success "stalld monitoring combined CPU specification (0,2-4)" test "$monitored_cpus" -eq 4

    stop_stalld
else
    echo "SKIP: Test 4 requires at least 6 CPUs"
fi

# Test 5: Invalid CPU number (should handle gracefully)
test_section "Test 5: Invalid CPU number (-c 999)"
invalid_cpu=999

assert_stalld_rejects "stalld rejected invalid CPU number" -f -v -c $invalid_cpu -l -t 5

# Test 6: Verify non-selected CPUs are NOT monitored
if [ "$num_cpus" -ge 2 ]; then
    test_section "Test 6: Verify non-selected CPUs not monitored (-c 0)"
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -c 0 -l -t 5

    # Check that CPU 1 is NOT being monitored
    assert_success --negate "stalld not monitoring non-selected CPU 1" is_cpu_monitored 1

    stop_stalld
fi

end_test
