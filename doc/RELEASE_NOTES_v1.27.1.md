# stalld v1.27.1 Release Notes

## Overview

This release includes two behavior changes to improve usability and error handling, along with extensive test infrastructure improvements.

## Behavior Changes

### Syslog Logging Now Opt-In

**Make syslog logging opt-in by default** (commit b7b0619421c7)
- **Author**: Wander Lairson Costa
- **Problem**: Previously, stalld enabled syslog output unconditionally with no mechanism to disable it. Users running the daemon from the command line with verbose logging to stderr would still generate unwanted syslog traffic.
- **Solution**: Changed the default behavior to make syslog logging strictly opt-in via the `-s/--log_syslog` flag
- **Impact**: Users must now explicitly pass `-s` to enable syslog. The systemd service file has been updated to include this flag, so systemd deployments retain their current behavior.

### Exit on Invalid CPU Affinity

**Die on invalid CPU affinity** (commit 3e9dd51c7c81)
- **Author**: Wander Lairson Costa
- **Problem**: `set_cpu_affinity()` validates the CPU list and returns -1 on error, but `main()` ignored the return value, allowing stalld to run with an invalid or empty affinity mask.
- **Solution**: stalld now exits with an error when an invalid `-a/--affinity` argument is provided
- **Impact**: Invalid CPU affinity specifications are now caught at startup rather than silently ignored

## Test Infrastructure Improvements

This release includes 33 commits improving the test suite reliability and maintainability:

### New Helper Functions
- `wait_for_stalld_ready()` - Event-driven stalld startup detection
- `start_starvation_gen()` - Helper for controlled starvation testing
- `wait_for_starvation_detected()` and `wait_for_boost_detected()` - Event-driven test synchronization
- `pass()` and `fail()` - Consistent test result reporting

### Reliability Improvements
- Replaced fixed sleep delays with event-driven detection throughout the test suite
- Fixed `stop_stalld()` timeout and shutdown logic
- Fixed fractional sleep timeout bugs
- Improved backend detection for test configuration

### Bug Fixes
- Fixed false positive log matching in test_logging_destinations
- Fixed multi-CPU detection in test_starvation_detection
- Fixed various path and backend configuration issues

## Contributors

- Wander Lairson Costa (behavior changes, test improvements)
- Clark Williams (maintainer)

## Upgrade Notes

**Breaking Change**: If you rely on syslog logging and run stalld manually (not via systemd), you must now add the `-s` flag to your command line. The systemd service file already includes this flag.

## Supported Architectures

stalld supports the following architectures:
- x86_64 (with eBPF support)
- aarch64 (with eBPF support)
- s390x (with eBPF support)
- riscv64
- i686 (sched_debug backend only)
- powerpc/ppc64le (sched_debug backend only)
