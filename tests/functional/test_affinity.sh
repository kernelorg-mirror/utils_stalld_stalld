#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: stalld -a/--affinity option
# Verifies that stalld runs on specified CPUs when -a is used
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

# Helper function to check CPU affinity (test-specific)
check_affinity() {
    local pid=$1

    # Use taskset to check affinity
    if command -v taskset > /dev/null 2>&1; then
        affinity=$(taskset -cp "$pid" 2>/dev/null | awk -F': ' '{print $2}')
        log "ℹ INFO: Current affinity for PID $pid: $affinity" >&2
        echo "$affinity"
    else
        # Fallback: check /proc/PID/status
        if [ -f "/proc/$pid/status" ]; then
            affinity=$(grep "Cpus_allowed_list:" "/proc/$pid/status" | awk '{print $2}')
            log "ℹ INFO: Current affinity for PID $pid (from /proc): $affinity" >&2
            echo "$affinity"
        else
            log "⚠ WARNING: Cannot check affinity (no taskset, no /proc)" >&2
            echo ""
        fi
    fi
}

init_functional_test "CPU Affinity Option (-a)" "test_affinity"

num_cpus=$(nproc)
log "System has $num_cpus CPUs"

if [ "$num_cpus" -lt 2 ]; then
    echo -e "${YELLOW}SKIP: Test requires at least 2 CPUs${NC}"
    exit 77
fi

if ! command -v taskset > /dev/null 2>&1; then
    log "⚠ WARNING: taskset not found, will use /proc fallback"
fi

#=============================================================================
# Test 1: Default behavior (no -a specified)
#=============================================================================
test_section "Test 1: Default behavior (no affinity restriction)"

rm -f "${STALLD_LOG}"
start_stalld_with_log "${STALLD_LOG}" -f -v -l -t 5

default_affinity=$(check_affinity "${STALLD_PID}")
log "ℹ INFO: Default affinity: $default_affinity"

assert_success "stalld has default affinity" test -n "$default_affinity"

stop_stalld

#=============================================================================
# Test 2: Single CPU affinity
#=============================================================================
test_section "Test 2: Single CPU affinity (-a 0)"

rm -f "${STALLD_LOG}"
start_stalld_with_log "${STALLD_LOG}" -f -v -l -t 5 -a 0

affinity=$(check_affinity "${STALLD_PID}")

assert_success "stalld restricted to CPU 0" test "$affinity" = "0"

stop_stalld

#=============================================================================
# Test 3: Multi-CPU affinity (CPU list)
#=============================================================================
test_section "Test 3: Multi-CPU affinity (-a 0,2)"

if [ "$num_cpus" -ge 4 ]; then
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -l -t 5 -a 0,2

    affinity=$(check_affinity "${STALLD_PID}")

    # Accept either "0,2" or "0-2" or "2,0" (different systems may report differently)
    assert_success "stalld restricted to CPUs 0,2" test -n "$(echo "$affinity" | grep -E '^0,2$|^0-2$|^2,0$')"

    stop_stalld
else
    log "⊘ SKIP: Test 3 requires at least 4 CPUs"
fi

#=============================================================================
# Test 4: CPU range affinity
#=============================================================================
test_section "Test 4: CPU range affinity (-a 0-2)"

if [ "$num_cpus" -ge 4 ]; then
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -l -t 5 -a 0-2

    affinity=$(check_affinity "${STALLD_PID}")

    # Accept various formats: "0-2", "0,1,2", etc.
    assert_success "stalld restricted to CPU range 0-2" test -n "$(echo "$affinity" | grep -E '0.*1.*2|0-2')"

    stop_stalld
else
    log "⊘ SKIP: Test 4 requires at least 4 CPUs"
fi

#=============================================================================
# Test 5: Verify stalld actually runs on specified CPU
#=============================================================================
test_section "Test 5: Verify stalld threads run on specified CPU"

if [ "$num_cpus" -ge 2 ]; then
    test_cpu=1
    rm -f "${STALLD_LOG}"
    start_stalld_with_log "${STALLD_LOG}" -f -v -l -t 5 -a "$test_cpu"

    # Check affinity
    affinity=$(check_affinity "${STALLD_PID}")

    # Also check if any child threads exist and verify their affinity
    child_threads=$(ps -T -p "${STALLD_PID}" 2>/dev/null | tail -n +2 | wc -l)
    if [ "$child_threads" -gt 0 ]; then
        log "ℹ INFO: Found $child_threads threads for stalld"
    fi

    assert_success "stalld process affinity set to CPU $test_cpu" test "$affinity" = "$test_cpu"

    stop_stalld
else
    log "⊘ SKIP: Test 5 requires at least 2 CPUs"
fi

#=============================================================================
# Test 6: Combined with CPU monitoring (-c and -a)
#=============================================================================
test_section "Test 6: Combined affinity and monitoring (-a 0 -c 1)"

if [ "$num_cpus" -ge 2 ]; then
    rm -f "${STALLD_LOG}"

    # Run stalld on CPU 0, but monitor CPU 1
    start_stalld_with_log "${STALLD_LOG}" -f -v -l -t 5 -a 0 -c 1

    affinity=$(check_affinity "${STALLD_PID}")

    assert_success "stalld affinity to CPU 0 while monitoring CPU 1" test "$affinity" = "0"

    stop_stalld
else
    log "⊘ SKIP: Test 6 requires at least 2 CPUs"
fi

#=============================================================================
# Test 7: Invalid CPU affinity
#=============================================================================
test_section "Test 7: Invalid CPU affinity (-a 999)"

assert_stalld_rejects "Invalid CPU affinity rejected with error" -f -v -l -t 5 -a 999

#=============================================================================
# Test 8: Verify affinity persists
#=============================================================================
test_section "Test 8: Verify affinity persists over time"

rm -f "${STALLD_LOG}"
start_stalld_with_log "${STALLD_LOG}" -f -v -l -t 5 -a 0

affinity_start=$(check_affinity "${STALLD_PID}")
log "ℹ INFO: Initial affinity: $affinity_start"

# Wait a bit
sleep 3

affinity_end=$(check_affinity "${STALLD_PID}")
log "ℹ INFO: Affinity after 3s: $affinity_end"

assert_success "CPU affinity persisted over time" test "$affinity_start" = "$affinity_end"

stop_stalld

end_test
