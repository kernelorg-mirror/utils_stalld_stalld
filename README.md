<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# stalld

The stalld program (which stands for 'stall daemon') is a
mechanism to prevent the *starvation* of operating system threads in a
Linux system. The premise is to start up on a *housekeeping* cpu (one
that is not used for real-application purposes) and to periodically
monitor the state of each thread in the system, looking for a thread
that has been on a run queue (i.e. ready to run) for a specifed length
of time without being run. This condition is usually hit when the
thread is on the same cpu as a high-priority cpu-intensive task and
therefore is being given no opportunity to run.

When a thread is judged to be starving, stalld changes
that thread to use the SCHED_DEADLINE policy and gives the thread a
small slice of time for that cpu (specified on the command line). The
thread then runs and when that timeslice is used, the thread is then
returned to its original scheduling policy and stalld then
continues to monitor thread states.

There is now an experimental option to boost using SCHED_FIFO. This
logic is used if the running kernel does not support the
SCHED_DEADLINE policy and may be forced by using the -F/--force_fifo
option.

## Command Line Options

`Usage: stalld [-l] [-v] [-k] [-s] [-f] [-h] [-F]
          [-c cpu-list]
          [-p time in ns] [-r time in ns]
          [-d time in seconds] [-t time in seconds]`

### Logging options
- -l/--log_only: only log information (do not boost) [false]
- -v/--verbose: print info to the std output [false]
- -k/--log_kmsg: print log to the kernel buffer [false]
- -s/--log_syslog: print log to syslog [true]
- -e,--no_idle_detection: do not detect idle CPUs [false]

### Startup options
- -c/--cpu: list of cpus to monitor for stalled threads [all cpus]
- -f/--foreground: run in foreground [false but true when -v]
- -P/--pidfile: write daemon pid to specified file [no pidfile]

### Boosting options
- -p/--boost_period: SCHED_DEADLINE period [ns] that the starving task will receive [1000000000]
- -r/--boost_runtime: SCHED_DEADLINE runtime [ns] that the starving task will receive [20000]
- -d/--boost_duration: how long [s] the starving task will run with SCHED_DEADLINE [3]
- -F/--force_fifo: force using SCHED_FIFO for boosting

### Monitoring options
- -t/--starving_threshold: how long [s] the starving task will wait before being boosted [60]
- -A/--aggressive_mode: dispatch one thread per run queue, even when there is no starving
                          threads on all CPU (uses more CPU/power). [false]
### Miscellaneous
- -h/--help: print this menu

## Do I need stalld?

Probably not. Stalld was developed as a temporary solution to overcome some
corner cases of real-time throttling on Linux.

The main use-case for stalld is for the DPDK case where the user has *fully
isolated CPU* and run a *single busy-loop RT task on the isolated CPU*.
Many decisions on stalld were made for this particular scenario, and it
is tested mostly for this case.

The main limitation it solves is the ability to boost starving tasks for
a limited amount of time, at a microsecond granularity, only in the
presence of starving tasks.

Once this problem is solved on kernel, stalld may become just an interface
for that mechanism, including logging.

## Is stalld recommended for safety-critical use cases?

**No, stalld is absolutely not recommended for safety-critical systems,
and no user-space tool will be.**

Stalld is a user-space program that uses standard syscalls to boost
starving tasks. Thus, stalld competes with resources with other
processes, including those that could be starving, creating live
lock scenarios. Running stalld with SCHED_DEADLINE priority (see -R)
with the PREEMPT_RT kernel can partially mitigate these live locks.
Nevertheless, because the priority/deadline inheritance does not cover
all locking mechanisms, stalld can still suffer from unbounded priority
inversion.

These problems should not happen in the DPDK case because it is assumed that
the system administrators did their best to isolate CPUs, and stalld will
only be activated for inevitable per-cpu tasks, such as kworkers.

**For safety-critical cases, it is safer to keep using RT throttling.**

## Repositories
The repository at https://gitlab.com/rt-linux-tools/stalld is the main
repository, where the development takes place.

The repository at https://git.kernel.org/pub/scm/utils/stalld/stalld.git is the
distribution repository, where distros can pick the latest released version.
