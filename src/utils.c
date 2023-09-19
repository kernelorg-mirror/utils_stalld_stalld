/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020-2021 Red Hat Inc, Daniel Bristot de Oliveira <bristot@redhat.com>
 * Copyright (C) 2020 Red Hat Inc, Clark Williams <williams@redhat.com>
 *
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <linux/sched.h>
#include <sys/sysinfo.h>

#include "stalld.h"
#include "sched_debug.h"
#include "queue_track.h"

/*
 * fill_process_comm - process name from task group ID.
 */
int fill_process_comm(int tgid, int pid, char *comm, int comm_size)
{
        char path[PROC_PID_FILE_PATH_LEN];
	int fd, retval;

	if (tgid == 0) {
		/*
		 * 0 is stalld itself for sched_setattr.
		 */
		snprintf(comm, comm_size, "stalld");
		return 0;
	}

	retval = snprintf(path, PROC_PID_FILE_PATH_LEN, "/proc/%d/comm", tgid);
	if (retval < 0)
		goto out_error;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		log_msg("failed to open comm file at %s\n", path);
		goto out_error;
	}

	memset(comm, 0, comm_size);

	retval = read(fd, comm, comm_size - 1); /* last is \0 */
	if (retval < 0) {
		log_msg("failed to read comm file at %s\n", path);
		snprintf(comm, comm_size - 1, "undef");
		goto out_close_fd;
	}

	/* Remove \n */
	comm[strcspn(comm, "\n")] = 0;

	close(fd);
	return 0;

out_close_fd:
	close(fd);
out_error:
	return 1;
}

long get_long_from_str(char *start)
{
	long value;
	char *end;

	errno = 0;
	value = strtol(start, &end, 10);
	if (errno || start == end) {
		warn("Invalid ID '%s'", start);
		return -1;
	}

	return value;
}

long get_long_after_colon(char *start)
{
	/* Find the ":". */
	start = strstr(start, ":");
	if (!start)
		return -1;

	/* Skip ":". */
	start++;

	return get_long_from_str(start);
}

long get_variable_long_value(char *buffer, const char *variable)
{
	char *start;

	/*
	 * Find the ".nr_running".
	 *
	 * Line example:
	 * '  .nr_running                    : 0'
	 */
	start = strstr(buffer, variable);
	if (!start)
		return -1;

	return get_long_after_colon(start);
}

/*
 * SIGINT handler for main.
 */
static void inthandler(int signo, siginfo_t *info, void *extra)
{
	log_msg("received signal %d, starting shutdown\n", signo);
	running = 0;
}

static void set_sig_handler(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));

	action.sa_flags = SA_SIGINFO;
	action.sa_sigaction = inthandler;
	sigemptyset(&action.sa_mask);

	if (sigaction(SIGINT, &action, NULL) == -1) {
		warn("error setting SIGINT handler: %s\n",
		      strerror(errno));
		exit(errno);
	}

	action.sa_flags = SA_SIGINFO;
	action.sa_sigaction = inthandler;

	if (sigaction(SIGTERM, &action, NULL) == -1) {
		warn("error setting SIGTERM handler: %s\n",
		      strerror(errno));
		exit(errno);
	}
}

int setup_signal_handling(void)
{
	sigset_t sigset;
	int status;

	/* Mask off all signals. */
	status = sigfillset(&sigset);
	if (status) {
		warn("setting up full signal set %s\n", strerror(errno));
		return status;
	}

	status = pthread_sigmask(SIG_BLOCK, &sigset, NULL);
	if (status) {
		warn("setting signal mask: %s\n", strerror(status));
		return status;
	}

	/* Now allow SIGINT and SIGTERM to be delivered. */
	status = sigemptyset(&sigset);
	if (status) {
		warn("creating empty signal set: %s\n", strerror(errno));
		return status;
	}

	status = sigaddset(&sigset, SIGINT);
	if (status) {
		warn("adding SIGINT to signal set: %s\n", strerror(errno));
		return status;
	}

	status = sigaddset(&sigset, SIGTERM);
	if (status) {
		warn("adding SIGTERM to signal set: %s\n", strerror(errno));
		return status;
	}

	status = pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
	if (status) {
		warn("unblocking signals: %s\n", strerror(status));
		return status;
	}

	/* Now register our signal handler. */
	set_sig_handler();
	return 0;
}

/*
 * Print any error messages and exit.
 */
void __die(const char *fmt, ...)
{
	volatile int zero = 0;
	int ret = errno;
	va_list ap;

	if (errno)
		perror("stalld");
	else
		ret = -1;

	va_start(ap, fmt);
	fprintf(stderr, "  ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");

	/* Die with a division by zero to keep the stack on GDB. */
	if (config_verbose)
		zero = 10 / zero;

	exit(ret);
}

/*
 * Print the error messages but do not exit.
 */
void __warn(const char *fmt, ...)
{
	va_list ap;

	if (errno)
		perror("stalld");

	va_start(ap, fmt);
	fprintf(stderr, "  ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
}

/*
 * Print an informational message if config_verbose is true.
 */
void __info(const char *fmt, ...)
{
	va_list ap;

	if (config_verbose) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
}

void log_msg(const char *fmt, ...)
{
	const char *log_prefix = "stalld: ";
	char message[1024];
	size_t bufsz;
	int kmesg_fd;
	va_list ap;
	char *log;

	strncpy(message, log_prefix, sizeof(message));
	log = message + strlen(log_prefix);
	bufsz = sizeof(message) - strlen(log_prefix);
	va_start(ap, fmt);
	vsnprintf(log, bufsz, fmt, ap);
	va_end(ap);

	/* Print the entire message (including PREFIX). */
	if (config_verbose)
		fprintf(stderr, "%s", message);

	/* Print the entire message (including PREFIX). */
	if (config_write_kmesg) {
		kmesg_fd = open("/dev/kmsg", O_WRONLY);

		/* Log iff possible. */
		if (kmesg_fd != -1) {
			if (write(kmesg_fd, message, strlen(message)) < 0)
				warn("write to klog failed");
			close(kmesg_fd);
		}
	}

	/* Print the log (syslog adds PREFIX). */
	if (config_log_syslog)
		syslog(LOG_INFO, "%s", log);
}

/*
 * Based on:
 * https://github.com/pasce/daemon-skeleton-linux-c
 */
void daemonize(void)
{
	pid_t pid;

	/* Fork off the parent process. */
	pid = fork();

	/* An error occurred. */
	if (pid < 0)
		die("Error while forking the deamon");

	/* Success: Let the parent terminate. */
	if (pid > 0)
		exit(EXIT_SUCCESS);

	/* On success: The child process becomes session leader. */
	if (setsid() < 0)
		die("Error while creating the deamon (setsid)");

	/*
	 * Catch, ignore and handle signals.
	 * XXX: Implement a working signal handler.
	 *
	 * signal(SIGCHLD, SIG_IGN);
	 * signal(SIGHUP, SIG_IGN);
	 */

	/*
	 * Fork off for the second time.
	 */
	pid = fork();

	/* An error occurred. */
	if (pid < 0)
		die("Error while forking the deamon (the second)");

	/* Success: Let the parent terminate. */
	if (pid > 0)
		exit(EXIT_SUCCESS);

	/* Set new file permissions. */
	umask(DAEMON_UMASK);

	/* Change the working directory to the root directory. */
	if (chdir("/"))
		die("Cannot change directory to '/'");
}

/*
 * Function to generate a meaningful error message from an
 * error code generated by one of the regex functions.
 */
char *get_regerror(int errcode, regex_t *compiled)
{
	size_t length;
	char *buffer;

	length = regerror(errcode, compiled, NULL, 0);

	buffer = malloc(length);
	if (buffer == NULL) {
		warn("Malloc failure!!");
		return NULL;
	}
	regerror(errcode, compiled, buffer, length);
	return buffer;
}

/*
 * Cleanup the regex compiled expressions and free up the memory.
 */
void cleanup_regex(unsigned int *nr_task, regex_t **compiled_expr)
{
	regex_t *compiled;
	unsigned int i;

	compiled = *compiled_expr;

	if (compiled != NULL) {
		for (i = 0; i < *nr_task; i++) {
			regfree(&compiled[i]);
		}
		free(compiled);
		*compiled_expr = NULL;
	}
	*nr_task = 0;
}

/*
 * Set HRTICK and frinds: Based on cyclicdeadline by Steven Rostedt.
 */

#define _STR(x) #x
#define STR(x) _STR(x)

static int find_mount(const char *mount, char *debugfs)
{
	char type[100];
	FILE *fp;

	if ((fp = fopen("/proc/mounts","r")) == NULL)
		return 0;

	while (fscanf(fp, "%*s %"
		      STR(MAX_PATH)
		      "s %99s %*s %*d %*d\n",
		      debugfs, type) == 2) {
		if (strcmp(type, mount) == 0)
			break;
	}
	fclose(fp);

	if (strcmp(type, mount) != 0)
		return 0;
	return 1;
}

static const char *find_debugfs(void)
{
	static char debugfs[MAX_DIR_PATH];
	static int debugfs_found;

	if (debugfs_found)
		return debugfs;

	if (!find_mount("debugfs", debugfs))
		return "";

	debugfs_found = 1;

	return debugfs;
}

/*
 * Return true if the file at *path exists.
 */
static int check_file_exists(char *path)
{
	struct stat st;
	int retval;

	retval = !stat(path, &st);

	log_msg("%s %s\n", path, retval ? "exists" : "doesn't exist");

	return retval;
}

/*
 * Look for the sched/debug file in the debugfs.
 */
static int find_debugfs_sched_debug(void)
{
	const char *debugfs;
	char *path;
	int found;

	debugfs = find_debugfs();
	if (!debugfs)
		return 0;

	path = malloc(strlen(debugfs) + strlen("sched/debug") + 1);
	if (!path)
		return 0;

	sprintf(path, "%s/%s", debugfs, "sched/debug");

	found = check_file_exists(path);
	if (found)
		config_sched_debug_path = path;
	else
		free(path);

	return found;
}

/*
 * Look for the sched_debug file in the procfs.
 */
static int find_proc_sched_debug(void)
{
	char *path;
	int found;

	path = malloc(strlen("/proc/sched_debug") + 1);
	if (!path)
		return 0;

	sprintf(path, "/proc/sched_debug");

	found = check_file_exists(path);
	if (found)
		config_sched_debug_path = path;
	else
		free(path);

	return found;
}

/*
 * Look for the sched debug file on the possible locations.
 *
 * stalld depends on sched_debug file, if it is not found: die.
 */
void find_sched_debug_path(void)
{
	int found;

	found = find_debugfs_sched_debug();
	if (found)
		return;

	found = find_proc_sched_debug();
	if (found)
		return;

	die("stalld could not find the sched_debug file.\n");
}

/*
 * is_lockdown_mode - check if lockdown mode is on.
 */
static int is_lockdown_mode(void)
{
	char buff[1024];
	int retval, fd;

	fd = open("/sys/kernel/security/lockdown", O_RDONLY);
	if (fd < 0)
		goto out_err;

	memset(buff, 0, sizeof(buff));

	retval = read(fd, buff, 1023);
	if (retval <= 0)
		goto out_close;

	/* if it is set to none, lockdown is disabled */
	retval = !strstr(buff, "[none]");

	log_msg("lockdown mode is %s\n", retval ? "on" : "off");

	close(fd);
	return retval;

out_close:
	close(fd);
out_err:
	/* it is probably an older kernel */
	warn("failed to open/read lockdown file: assuming lockdown is OFF\n");
	return 0;
}

static int fill_sched_features_path(char *path, int path_size)
{
	const char *debugfs;
	int retval;

	debugfs = find_debugfs();
	if (strlen(debugfs) == 0)
		return 0;

	snprintf(path, path_size, "%s/sched/features", debugfs);
	retval = check_file_exists(path);
	if (retval)
		return 1;

	snprintf(path, path_size, "%s/sched_features", debugfs);
	retval = check_file_exists(path);
	if (retval)
		return 1;

	memset(path, 0, path_size);

	return 0;
}

int setup_hr_tick(void)
{
	char path[MAX_PATH];
	static int set = 0;
	int hrtick_dl = 0;
	int ret, len, fd;
	char buf[500];
	char *p;

	if (set)
		return 1;

	set = 1;

	ret = is_lockdown_mode();
	if (ret) {
		log_msg("hrtick cannot be set in lockdown mode: assuming that the user already set HRTICK_DL\n");
		log_msg("hrtick cannot be set in lockdown mode: user workload might face high latencies\n");
		return 1;
	}

	ret = fill_sched_features_path(path, MAX_PATH);
	if (!ret) {
		log_msg("did not find sched features, do not try to set HRTICK\n");
		return 0;
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		log_msg("could not open %s to set HRTICK: %s", path, strerror(errno));
		return 0;
	}

	len = sizeof(buf);

	ret = read(fd, buf, len);
	if (ret < 0) {
		perror(path);
		close(fd);
		return 0;
	}
	if (ret >= len)
		ret = len - 1;
	buf[ret] = 0;

	ret = 1;

	p = strstr(buf, "HRTICK_DL");
	if (p && p - 3 >= buf) {
		hrtick_dl = 1;
		p -= 3;
		if (strncmp(p, "NO_HRTICK_DL", 12) == 0) {
			log_msg("dl_runtime is shorter than 1ms, setting HRTICK_DL\n");
			ret = write(fd, "HRTICK_DL", 9);
			if (ret != 9)
				ret = 0;
			else
				ret = 1;
		}
	}

	/* Backward compatibility on kernels that only have HRTICK. */
	if (!hrtick_dl) {
		p = strstr(buf, "HRTICK");
		if (p && p - 3 >= buf) {
			p -= 3;
			if (strncmp(p, "NO_HRTICK", 9) == 0) {
				log_msg("dl_runtime is shorter than 1ms, setting HRTICK\n");
				ret = write(fd, "HRTICK", 6);
				if (ret != 6)
					ret = 0;
				else
					ret = 1;
			}
		}
	}

	close(fd);
	return ret;
}

int should_monitor(int cpu)
{
	if (config_monitor_all_cpus)
		return 1;
	if (config_monitored_cpus[cpu])
		return 1;

	return 0;
}

/*
 * Path to file for storing daemon pid.
 */
char pidfile[MAX_PATH];

void write_pidfile(void)
{
	if (strlen(pidfile)) {
		FILE *f = fopen(pidfile, "w");
		if (f != NULL) {
			fprintf(f, "%d", getpid());
			fclose(f);
		}
		else
			die("unable to open pidfile %s: %s\n", pidfile, strerror(errno));
	}
}

/*
 * set_reservation - configure stalld to run with SCHED_DEADLINE
 *
 * Set stalld to run with reservation % of CPU time using SCHED_DEADLINE.
 *
 * If the period is < 4 s (see kernel.sched_deadline_period_max_us), stalld
 * will have the dl_period set as period. If it is higher than 4s, the
 * reservation will be configure as 1s period. This does not change the
 * picture, as at the end, the task will receive the % of time, while
 * avoiding have yet another knob to handle.
 */
int set_reservation(int period, int reservation)
{
	unsigned long dl_period, dl_runtime;
	struct sched_attr attr;
	int flags = 0;
	int ret;

	if (reservation == 0)
		return 0;

	if (period > 4)
		period = 1;

	dl_period = period * 1000 * 1000 * 1000;
	dl_runtime = dl_period * reservation / 100;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_policy   = SCHED_DEADLINE;
	attr.sched_runtime  = dl_runtime;
	attr.sched_deadline = dl_period;
	attr.sched_period   = dl_period;

	ret = sched_setattr(0, &attr, flags);
	if (ret < 0) {
		log_msg("failed to set -R/--reservation: %s\n", strerror(errno));
		return ret;
	}

	log_msg("successfully set %d%% SCHED_DEADLINE reservation runtime/period = %lld/%lld\n",
		reservation, dl_runtime, dl_period);
	return 0;
}

static void print_usage(void)
{
	int i;

	char *msg[] = {
		"stalld: starvation detection and avoidance (with bounds)",
		"  usage: stalld [-l] [-v] [-k] [-s] [-f] [-h] \\",
		"          [-c cpu-list] \\",
		"          [-p time in ns] [-r time in ns] \\",
		"          [-d time in seconds] [-t time in seconds] \\",
		"          [-R percentage ] [-a cpu-list]",
		"",
		"       logging options:",
		"          -l/--log_only: only log information (do not boost)",
		"          -v/--verbose: print info to the std output",
		"          -k/--log_kmsg: print log to the kernel buffer",
		"          -s/--log_syslog: print log to syslog",
		"          -f/--foreground: run in foreground [implict when -v]",
		"        boosting options:",
		"          -p/--boost_period: SCHED_DEADLINE period [ns] that the starving task will receive",
		"          -r/--boost_runtime: SCHED_DEADLINE runtime [ns] that the starving task will receive",
		"          -d/--boost_duration: how long [s] the starving task will run with SCHED_DEADLINE",
		"          -F/--force_fifo: use SCHED_FIFO for boosting",
		"        monitoring options:",
		"          -t/--starving_threshold: how long [s] the starving task will wait before being boosted",
		"          -A/--aggressive_mode: dispatch one thread per run queue, even when there is no starving",
		"                               threads on all CPU (uses more CPU/power).",
		"          -M/--adaptive_mode: when a CPU shows threads starving for more than half of the",
		"                               starving_threshold time, dispatch a specialized thread to monitor",
		"                               it.",
		"	   -O/--power_mode: works as a single threaded tool. Saves CPU, but loses precision.",
		"	   -g/--granularity: set the granularity at which stalld checks for starving threads",
		"	   -R/--reservation: percentage of CPU time reserved to stalld using SCHED_DEADLINE.",
		"	   -a/--affinity: limit stalld's affinity",
		"        ignoring options:",
		"          -i/--ignore_threads: regexes (comma-separated) of thread names that must be ignored",
		"                               from being boosted",
		"          -I/--ignore_processes: regexes (comma-separated) of process names that must be ignored",
		"                               from being boosted",
		"	backend:",
		"	   -b/--backend: backend to be used to search for starving tasks, options are:",
		"		sched_debug || S: for sched/debug file,",
		"		queue_track || Q: for tracking enqueue/dequeue of tasks in the runqueues.",
		"	misc:",
		"          --pidfile: write daemon pid to specified file",
		"          -S/--systemd: running as systemd service, don't fiddle with RT throttling",
		"          -h/--help: print this menu",
		NULL,
	};

	for(i = 0; msg[i]; i++)
		fprintf(stderr, "%s\n", msg[i]);
	fprintf(stderr, "  stalld version: %s\n", version);
}

void usage(const char *fmt, ...)
{
	va_list ap;

	print_usage();

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");

	exit(EINVAL);
}

static void compile_regex(char *task_ignore_string, unsigned int *nr_task, regex_t **compiled_expr,
				unsigned int ignore_flag)
{
	char *input = task_ignore_string;
	char *separator = ",";
	char *err_str = NULL;
	regex_t *compiled;
	char *args;
	int err;

	args = strtok(input, separator);
	while (args != NULL) {
		(*nr_task)++;

		/* Make space for the regex and copy it over. */
		*compiled_expr = realloc(*compiled_expr, (*nr_task) * sizeof(regex_t));
		if (*compiled_expr == NULL) {
			warn("Unable to allocate memory. Tasks cannot be ignored");
			(*nr_task)--;
			/*
			 * If we are unable to make space for any regex, then these set of
			 * arguments will be discarded.
			 */
			goto error;
		}

		/* Dereference and assign to a temporary variable. */
		compiled = *compiled_expr;

		/* Compile the regex pattern. */
		err = regcomp(&compiled[(*nr_task) - 1], args, REG_EXTENDED | REG_NOSUB);
		if (err) {
			/* The regex couldn't be compiled, so denylisting will not work. */
			err_str = get_regerror(err, &compiled[(*nr_task) - 1]);
			if (err_str) {
				warn("regcomp: regex compilation failed. %s", err_str);
				free(err_str);
			}
			goto error;
		}
		args = strtok(NULL, separator);
	}
	return;
error:
	if (ignore_flag == IGNORE_THREADS)
		warn("-i arguments will be discarded");
	else if (ignore_flag == IGNORE_PROCESSES)
		warn("-I arguments will be discarded");
	cleanup_regex(nr_task, compiled_expr);
}

static void parse_task_ignore_string(char *task_ignore_string, unsigned int ignore_flag)
{
	log_msg("task ignore string %s\n", task_ignore_string);

	switch (ignore_flag) {
		case IGNORE_THREADS:
			compile_regex(task_ignore_string, &nr_thread_ignore, &compiled_regex_thread,
					ignore_flag);
			break;
		case IGNORE_PROCESSES:
			compile_regex(task_ignore_string, &nr_process_ignore, &compiled_regex_process,
					ignore_flag);
			break;
	}
}

int set_cpu_affinity(char *cpu_list)
{
	cpu_set_t set;
	const char *p;
	int end_cpu;
	int retval;
	int cpu;
	int i;

	CPU_ZERO(&set);

	for (p = cpu_list; *p; ) {
		cpu = atoi(p);
		if (cpu < 0 || (!cpu && *p != '0') || cpu >= config_nr_cpus)
			goto err;

		while (isdigit(*p))
			p++;
		if (*p == '-') {
			p++;
			end_cpu = atoi(p);
			if (end_cpu < cpu || (!end_cpu && *p != '0') || end_cpu >= config_nr_cpus)
				goto err;
			while (isdigit(*p))
				p++;
		} else
			end_cpu = cpu;

		if (cpu == end_cpu) {
			log_msg("self-affinity: adding cpu %d\n", cpu);
			CPU_SET(cpu, &set);
		} else {
			for (i = cpu; i <= end_cpu; i++) {
				log_msg("self-affinity: adding cpu %d\n", i);
				CPU_SET(i, &set);
			}
		}

		if (*p == ',')
			p++;
	}

	retval = sched_setaffinity(getpid(), sizeof(set), &set);
	if (retval == -1) {
		log_msg("self-affinity: error setting affinity %d", strerror(errno));
		return -1;
	}

	return 0;
err:
	log_msg("self-affinity: error parsing the cpu set %s\n", cpu_list);
	return -1;
}

static void parse_cpu_list(char *cpulist)
{
	const char *p;
	int end_cpu;
	int cpu;
	int i;

	config_monitored_cpus = malloc(config_nr_cpus * sizeof(char));
	if (!config_monitored_cpus)
		goto err;

	memset(config_monitored_cpus, 0, (config_nr_cpus * sizeof(char)));

	for (p = cpulist; *p; ) {
		cpu = atoi(p);
		if (cpu < 0 || (!cpu && *p != '0') || cpu >= config_nr_cpus)
			goto err;

		while (isdigit(*p))
			p++;
		if (*p == '-') {
			p++;
			end_cpu = atoi(p);
			if (end_cpu < cpu || (!end_cpu && *p != '0') || end_cpu >= config_nr_cpus)
				goto err;
			while (isdigit(*p))
				p++;
		} else
			end_cpu = cpu;

		if (cpu == end_cpu) {
			if (config_verbose)
				printf("cpulist: adding cpu %d\n", cpu);
			config_monitored_cpus[cpu] = 1;
		} else {
			for (i = cpu; i <= end_cpu; i++) {
				if (config_verbose)
					printf("cpulist: adding cpu %d\n", i);
				config_monitored_cpus[i] = 1;
			}
		}

		if (*p == ',')
			p++;
	}

	return;
err:
	die("Error parsing the cpu list %s", cpulist);
}

int parse_args(int argc, char **argv)
{
	int c;

	/* Ensure the pidfile is an empty string. */
	pidfile[0] = '\0';

	while (1) {
		static struct option long_options[] = {
			{"cpu",			required_argument, 0, 'c'},
			{"log_only",		no_argument,	   0, 'l'},
			{"verbose",		no_argument,	   0, 'v'},
			{"log_kmsg",		no_argument,	   0, 'k'},
			{"log_syslog",		no_argument,	   0, 's'},
			{"foreground",		no_argument,	   0, 'f'},
			{"aggressive_mode",	no_argument,	   0, 'A'},
			{"power_mode",		no_argument,	   0, 'O'},
			{"adaptive_mode",	no_argument,	   0, 'M'},
			{"help",		no_argument,	   0, 'h'},
			{"boost_period",	required_argument, 0, 'p'},
			{"boost_runtime",	required_argument, 0, 'r'},
			{"boost_duration",	required_argument, 0, 'd'},
			{"starving_threshold",	required_argument, 0, 't'},
			{"pidfile",		required_argument, 0, 'P'},
			{"force_fifo", 		no_argument, 	   0, 'F'},
			{"version", 		no_argument,	   0, 'V'},
			{"systemd",		no_argument,	   0, 'S'},
			{"granularity",		required_argument, 0, 'g'},
			{"reservation",		required_argument, 0, 'R'},
			{"ignore_threads",      required_argument, 0, 'i'},
			{"ignore_processes",    required_argument, 0, 'I'},
			{"backend",		required_argument, 0, 'b'},
			{"affinity",		required_argument, 0, 'a'},
			{0, 0, 0, 0}
		};

		/* getopt_long stores the option index here. */
		int option_index = 0;

		c = getopt_long(argc, argv, "lvkfAOMhsp:r:d:t:c:FVSg:i:I:R:b:a:",
				 long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			config_monitor_all_cpus = 0;
			parse_cpu_list(optarg);
			break;
		case 'i':
			config_ignore = 1;
			parse_task_ignore_string(optarg, IGNORE_THREADS);
			break;
		case 'I':
			config_ignore = 1;
			parse_task_ignore_string(optarg, IGNORE_PROCESSES);
			break;
		case 'l':
			config_log_only = 1;
			break;
		case 'v':
			config_verbose = 1;
			config_foreground = 1;
			break;
		case 'k':
			config_write_kmesg = 1;
			break;
		case 's':
			config_log_syslog = 1;
			break;
		case 'f':
			config_foreground = 1;
			break;
		case 'A':
			config_aggressive = 1;
			/*
			 * Clean the other options so the last one in the
			 * cmd-line gets selected.
			 */
			config_adaptive_multi_threaded = 0;
			config_single_threaded = 0;
			break;
		case 'p':
			config_dl_period = get_long_from_str(optarg);
			if (config_dl_period < 200000000)
				usage("boost_period should be at least 200 ms");
			if (config_dl_period > 4000000000)
				usage("boost_period should be at most 4 s");
			break;
		case 'r':
			config_dl_runtime = get_long_from_str(optarg);
			if (config_dl_runtime < 8000)
				usage("boost_runtime should be at least 8 us");
			if (config_dl_runtime > 1000000)
				usage("boost_runtime should be at most 1 ms");
			break;
		case 'd':
			config_boost_duration = get_long_from_str(optarg);
			if (config_boost_duration < 1)
				usage("boost_duration should be at least 1 second");

			if (config_boost_duration > 60)
				usage("boost_duration should be at most 60 seconds");

			break;
		case 't':
			config_starving_threshold = get_long_from_str(optarg);
			if (config_starving_threshold < 1)
				usage("starving_threshold should be at least 1 second");

			if (config_starving_threshold > 3600)
				usage("boost_duration should be at most one hour");

			break;
		case 'h':
			print_usage();
			exit(EXIT_SUCCESS);
			break;
		case 'P':
			strncpy(pidfile, optarg, sizeof(pidfile)-1);
			break;
		case 'F':
			config_force_fifo = 1;
			break;
		case 'V':
			puts(version);
			exit(0);
			break;
		case 'S':
			config_systemd = 1;
			break;
		case 'g':
			config_granularity = get_long_from_str(optarg);
			if (config_granularity < 1)
				usage("granularity should be at least 1 second");

			if (config_granularity > 600)
				usage("granularity should not be more than 10 minutes");

			break;
		case 'O':
			config_single_threaded = 1;
			/*
			 * Clean the other options so the last one
			 * in the cmd-line gets selected.
			 */
			config_adaptive_multi_threaded = 0;
			config_aggressive = 0;
			break;
		case 'M':
			config_adaptive_multi_threaded = 1;
			/*
			 * Clean the other options so the last one
			 * in the cmd-line gets selected.
			 */
			config_single_threaded = 0;
			config_aggressive = 0;
			break;
		case 'R':
			config_reservation = get_long_from_str(optarg);
			if (config_reservation < 10 || config_reservation > 90)
				usage("Reservation needs to be at least 10%% and at most 90%%");
			break;
		case 'b':
			if (!strcmp(optarg, "sched_debug") || !strcmp(optarg, "S")) {
				backend = &sched_debug_backend;
				log_msg("using sched_debug backend\n");
			} else if (!strcmp(optarg, "queue_track") || !strcmp(optarg, "Q")) {
				backend = &queue_track_backend;
				log_msg("using queue_track backend\n");
			} else {
				usage("unknown backend %s\n", optarg);
			}
			break;
		case 'a':
			config_affinity_cpus = optarg;
			break;
		case '?':
			usage("Invalid option");
			break;
		default:
			usage("Invalid option");
		}
	}

	if (config_dl_period < config_dl_runtime)
		usage("runtime is longer than the period");

	if (config_dl_period > (config_boost_duration * NS_PER_SEC))
		usage("the period is longer than the boost_duration: the boosted task might not be able to run");

	if (config_boost_duration > config_starving_threshold)
		usage("the boost duration cannot be longer than the starving threshold ");

	if (config_force_fifo && config_single_threaded) {
		log_msg("-F/--force_fifo does not work in single-threaded mode\n");
		log_msg("falling back to the adaptive mode\n");
		config_adaptive_multi_threaded = 1;
		config_single_threaded = 0;
		config_aggressive = 0;
	}

	if (config_reservation && (config_aggressive || config_adaptive_multi_threaded))
		usage("-R/--reservation only works in the single-threaded mode");

	/*
	 * stalld needs root permission to read kernel debug files
	 * and to set SCHED_DEADLINE parameters.
	 */
	if (geteuid()) {
		log_msg("stalld needs root permission\n");
		exit(EXIT_FAILURE);
	}

	/*
	 * Runtime is always < 1 ms, enable hrtick if we're not in logging only
	 * mode and if we'll be using SCHED_DEADLINE.
	 */
	if (!config_log_only && !config_force_fifo) {
		/*
		 * If HRTICK can't be enabled, then we can't do a reliable
		 * job of ensuring only runtime worth of bandwidth is allocated.
		 * Don't allow stalld to continue in this mode.
		 */
		if (!setup_hr_tick()) {
			log_msg("stalld can't enable HRTICK. stalld cannot run in this mode. Exiting..\n");
			exit(EXIT_FAILURE);
		}
	}

	return(0);
}
