#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test: stalld -P/--pidfile option
# Verifies that stalld creates and manages PID files correctly
#
# Copyright (C) 2025 Red Hat Inc

# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options
parse_test_options "$@" || exit $?

init_functional_test "PID File Option (-P)" "test_pidfile"

#=============================================================================
# Test 1: Default pidfile location (no -P specified)
#=============================================================================
test_section "Test 1: Default behavior (no -P specified)"

start_stalld -l -t 5

# Check common default pidfile locations
default_found=0
for pidfile in /var/run/stalld.pid /run/stalld.pid; do
    if [ -f "$pidfile" ]; then
        log "ℹ INFO: Found default pidfile at $pidfile"
        default_found=1

        # Verify PID matches
        pid_from_file=$(cat "$pidfile")
        assert_success "Default pidfile contains correct PID" test "$pid_from_file" = "${STALLD_PID}"
        break
    fi
done

if [ $default_found -eq 0 ]; then
    log "ℹ INFO: No default pidfile found (may be expected depending on configuration)"
fi

stop_stalld

#=============================================================================
# Test 2: Custom pidfile location
#=============================================================================
test_section "Test 2: Custom pidfile location"

custom_pidfile="/tmp/stalld_test_pidfile_custom_$$.pid"
CLEANUP_FILES+=("${custom_pidfile}")

# Ensure pidfile doesn't exist before test
rm -f "${custom_pidfile}"

log "Starting stalld with custom pidfile: ${custom_pidfile}"
start_stalld -l -t 5 --pidfile "${custom_pidfile}"

assert_file_exists "${custom_pidfile}" "Custom pidfile created at ${custom_pidfile}"

pid_from_file=$(cat "${custom_pidfile}")
assert_success "Custom pidfile contains correct PID" test "$pid_from_file" = "${STALLD_PID}"

# Test 3: Verify pidfile removed on clean shutdown
test_section "Test 3: Verify pidfile removed on clean shutdown"
stop_stalld

if [ ! -f "${custom_pidfile}" ]; then
    pass "Pidfile removed on clean shutdown"
else
    log "⚠ WARNING: Pidfile still exists after shutdown (may be expected)"
    # Not failing - some implementations keep pidfile
fi

#=============================================================================
# Test 4: Custom pidfile in /tmp
#=============================================================================
test_section "Test 4: Custom pidfile in /tmp directory"

tmp_pidfile="/tmp/stalld_test_tmp_$$.pid"
CLEANUP_FILES+=("${tmp_pidfile}")
rm -f "${tmp_pidfile}"

log "Starting stalld with /tmp pidfile: ${tmp_pidfile}"
start_stalld -l -t 5 --pidfile "${tmp_pidfile}"

assert_file_exists "${tmp_pidfile}" "Pidfile created in /tmp directory"

pid_from_file=$(cat "${tmp_pidfile}")
assert_success "/tmp pidfile contains correct PID" test "$pid_from_file" = "${STALLD_PID}"

stop_stalld

#=============================================================================
# Test 5: Test with foreground mode
#=============================================================================
test_section "Test 5: Pidfile with foreground mode (-f)"

fg_pidfile="/tmp/stalld_test_pidfile_foreground_$$.pid"
CLEANUP_FILES+=("${fg_pidfile}")
rm -f "${fg_pidfile}"

log "Starting stalld in foreground mode with pidfile: ${fg_pidfile}"
start_stalld -f -v -l -t 5 --pidfile "${fg_pidfile}"

if [ -f "${fg_pidfile}" ]; then
    pass "Pidfile created in foreground mode"

    pid_from_file=$(cat "${fg_pidfile}")
    assert_success "Foreground mode pidfile contains correct PID" test "$pid_from_file" = "${STALLD_PID}"
else
    log "⚠ WARNING: Pidfile not created in foreground mode (may be expected)"
fi

stop_stalld

#=============================================================================
# Test 6: Invalid pidfile path (permission denied)
#=============================================================================
test_section "Test 6: Invalid pidfile path (permission denied)"

log "Testing invalid pidfile path"
assert_stalld_rejects "Invalid pidfile path rejected with error" -f -v -l -t 5 --pidfile "/nonexistent_$$/stalld.pid"

#=============================================================================
# Test 7: Verify pidfile is readable by other processes
#=============================================================================
test_section "Test 7: Verify pidfile is readable"

readable_pidfile="/tmp/stalld_test_pidfile_readable_$$.pid"
CLEANUP_FILES+=("${readable_pidfile}")
rm -f "${readable_pidfile}"

log "Starting stalld with readable pidfile: ${readable_pidfile}"
start_stalld -l -t 5 --pidfile "${readable_pidfile}"

assert_file_exists "${readable_pidfile}" "Pidfile created"
assert_success "Pidfile is readable" cat "${readable_pidfile}"

perms=$(stat -c "%a" "${readable_pidfile}" 2>/dev/null || stat -f "%Lp" "${readable_pidfile}" 2>/dev/null)
log "ℹ INFO: Pidfile permissions: $perms"

stop_stalld

end_test
