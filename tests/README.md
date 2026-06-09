# stalld Test Suite

Comprehensive test suite for the stalld (starvation daemon) project.

## Overview

This test suite validates all aspects of stalld functionality:
- Command-line option handling
- Starvation detection logic
- Thread boosting behavior
- Backend functionality (eBPF and procfs)
- Edge cases and integration scenarios

## Quick Start

### Prerequisites

- Root privileges (most tests require root)
- RT throttling disabled: `echo -1 > /proc/sys/kernel/sched_rt_runtime_us`
- stalld built: run `make` in project root

### Test Environment Management

All tests should manage DL-server and RT throttling state to ensure proper test isolation. The test framework provides the `setup_test_environment()` function for this purpose:

```bash
start_test "My Test"

# Setup test environment (disables RT throttling and DL-server)
setup_test_environment

require_root
# ... test code ...
end_test
```

The framework automatically restores state on cleanup (normal exit or interrupt).

#### Environment Variable Overrides

For debugging and special testing scenarios, you can skip the automatic disable:

- `STALLD_TEST_KEEP_DL_SERVER=1` - Skip DL-server disable (keep DL-server enabled)
- `STALLD_TEST_KEEP_RT_THROTTLING=1` - Skip RT throttling disable (keep throttling enabled)

Example usage:
```bash
# Keep DL-server enabled for debugging
STALLD_TEST_KEEP_DL_SERVER=1 ./functional/test_starvation_detection.sh

# Keep both enabled
STALLD_TEST_KEEP_DL_SERVER=1 STALLD_TEST_KEEP_RT_THROTTLING=1 ./functional/test_log_only.sh
```

#### Writing DL-server Conflict Tests

To test stalld and DL-server interaction (future tests), skip the environment setup to leave DL-server enabled:

```bash
start_test "DL-server Conflict Test"

# Do NOT call setup_test_environment()
# DL-server remains enabled for this test

require_root
# ... test stalld behavior with DL-server active ...
end_test
```

### Running Tests

```bash
# Run all tests
make test

# Run specific test categories
make test-functional     # Functional tests only
make test-integration    # Integration tests only

# Run test runner directly
./run_tests.sh
./run_tests.sh --functional-only

# Test with specific backend (applies to ALL tests)
./run_tests.sh --backend sched_debug    # Force procfs backend
./run_tests.sh --backend queue_track    # Force eBPF backend
./run_tests.sh -b S                     # Short form for sched_debug
./run_tests.sh -b Q                     # Short form for queue_track

# Disable DL-server for starvation testing
./run_tests.sh --disable-dl-server

# Run individual test with backend selection
cd functional
sudo ./test_log_only.sh                 # Uses default backend
sudo ./test_log_only.sh -b sched_debug  # Uses procfs backend
sudo ./test_log_only.sh -b Q            # Uses eBPF backend
```

## Test Organization

```
tests/
├── run_tests.sh              # Main test runner
├── Makefile                  # Build system
├── helpers/
│   ├── test_helpers.sh       # Common helper functions
│   └── starvation_gen.c      # Configurable starvation generator
├── functional/               # Functional tests (shell scripts)
│   ├── test_foreground.sh
│   ├── test_log_only.sh
│   └── test_logging_destinations.sh
├── integration/              # Integration tests (shell scripts)
├── fixtures/                 # Test data and configurations
└── results/                  # Test output logs (gitignored)
```

## Writing Tests

### Functional Test Template (Shell)

```bash
#!/bin/bash
# Load test helpers
TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${TEST_ROOT}/helpers/test_helpers.sh"

# Parse command-line options (supports -b/--backend)
parse_test_options "$@" || exit $?

start_test "Your Test Name"

# Setup test environment (disable RT throttling and DL-server)
setup_test_environment

# Require root if needed
require_root

# Your test logic here
# start_stalld automatically uses STALLD_TEST_BACKEND if set
start_stalld -f -v -l -t 5
assert_process_running "${STALLD_PID}" "stalld should be running"

# Cleanup handled automatically
stop_stalld

end_test
```

**Note**: If your test directly invokes stalld (not using `start_stalld`), check `STALLD_TEST_BACKEND` and add `-b ${STALLD_TEST_BACKEND}` to the command line. See `test_log_only.sh` for an example.

### Test Exit Codes

- `0`: Test passed
- `1`: Test failed
- `77`: Test skipped (autotools convention)

## Helper Functions

### Test Assertions

```bash
assert_equals expected actual "message"
assert_contains haystack needle "message"
assert_file_exists "/path/to/file"
assert_process_running $PID
```

### stalld Management

```bash
start_stalld [args...]      # Start stalld in background
stop_stalld                 # Stop stalld gracefully
```

### System Helpers

```bash
require_root                # Skip test if not root
check_rt_throttling         # Check if RT throttling is disabled
pick_test_cpu               # Pick a CPU for testing
wait_for_log_message "pattern" timeout
```

### Backend Selection

```bash
# Detect which backend stalld was compiled with
detect_default_backend      # Returns "queue_track" or "sched_debug"

# Check if a backend is available
is_backend_available "sched_debug"    # Always returns true
is_backend_available "queue_track"    # Returns true if BPF backend available

# Get list of all available backends
get_available_backends      # Returns space-separated list

# Start stalld with specific backend
start_stalld_with_backend "sched_debug" -f -v -t 60
start_stalld_with_backend "queue_track" -f -v -t 60
start_stalld_with_backend "S" -f -v    # Short name for sched_debug
start_stalld_with_backend "Q" -f -v    # Short name for queue_track

# Or use start_stalld directly with -b option
start_stalld -b sched_debug -f -v -t 60
start_stalld -b queue_track -f -v -t 60
```

**Backend Names:**
- `sched_debug` or `S` - Procfs backend (always available)
- `queue_track` or `Q` - eBPF backend (available on x86_64, aarch64, s390x with modern kernels)

### Starvation Generator

```bash
# Create controlled starvation
../helpers/starvation_gen -c CPU -p priority -n num_threads -d duration -v

# Example: CPU 2, priority 10, 3 blockee threads, 60 seconds
../helpers/starvation_gen -c 2 -p 10 -n 3 -d 60 -v
```

## Test Results

Test results are stored in `results/` directory:
- `test_run_YYYYMMDD_HHMMSS.log` - Full test run log
- `test_name.log` - Individual test output

## Current Test Coverage

### Phase 1: Foundation (✅ Complete)
- [x] Test infrastructure created (run_tests.sh, test_helpers.sh, starvation_gen.c)
- [x] test_foreground.sh - Foreground mode
- [x] test_log_only.sh - Log-only mode
- [x] test_logging_destinations.sh - Logging options

### Phase 2: Command-Line Options (✅ Complete)
- [x] CPU selection (-c)
- [x] Starvation threshold (-t)
- [x] Boost parameters (-p, -r, -d)
- [x] Force FIFO (-F)
- [x] PID file (-P)
- [x] CPU affinity (-a)

### Phase 3: Core Logic (✅ Complete)
- [x] Starvation detection
- [x] SCHED_DEADLINE boosting
- [x] SCHED_FIFO boosting
- [x] Task merging
- [x] Idle detection
- [x] Runqueue parsing

### Phase 4: Advanced Features (In Progress)
- [x] Backend selection and detection
- [ ] Threading modes (single/adaptive/aggressive)
- [ ] Filtering (-i, -I)
- [ ] Backend comparison tests
- [ ] Integration tests
- [ ] Stress tests

## Troubleshooting

### Tests fail with "RT throttling not disabled"

```bash
echo -1 > /proc/sys/kernel/sched_rt_runtime_us
```

### Tests fail with "permission denied"

Run as root:
```bash
sudo make test
```

### stalld fails to start

1. Check if stalld is already running: `pkill stalld`
2. Verify binary exists: `ls -l ../stalld`
3. Check build: `cd .. && make clean && make`

## Contributing

When adding new tests:
1. Use the appropriate directory (functional/, integration/)
2. Follow naming convention: `test_<feature>.sh`
3. Include SPDX license header
4. Use helper functions from test_helpers.sh
5. Add cleanup for any resources created
6. Document what the test verifies

## References

- [CLAUDE.md](../CLAUDE.md) - stalld architecture and development guide
- [README.md](../README.md) - stalld project overview
- [Makefile](Makefile) - Build system
