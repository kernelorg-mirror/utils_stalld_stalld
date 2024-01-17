/*
 * stalld: starvation detection and avoidance (with bounds).
 *
 * This program was born after Daniel and Juri started debugging once again
 * problems caused kernel threads starving due to busy-loop sched FIFO threads.
 *
 * The idea is simple: after detecting a thread starving on a given CPU for a
 * given period, this thread will receive a "bounded" chance to run, using
 * SCHED_DEADLINE. In this way, the starving thread is able to make progress
 * causing a bounded Operating System noise (OS Noise).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2020-2022 Red Hat Inc, Daniel Bristot de Oliveira <bristot@redhat.com>
 */

#define _GNU_SOURCE
#include <ctype.h>
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/sched.h>
#include <sys/file.h>

#include "stalld.h"
#include "sched_debug.h"
#if USE_BPF
#include "queue_track.h"
#endif

/*
 * version
 */
const char *version = VERSION;

/*
 * Logging.
 */
int config_verbose = 0;
int config_write_kmesg = 0;
int config_log_syslog = 1;
int config_log_only = 0;
int config_foreground = 0;

/*
 * Denylisting feature.
 */
int config_ignore = 0;

/*
 * Boost parameters (time in nanoseconds).
 */
unsigned long config_dl_period  = 1000000000;
unsigned long config_dl_runtime = 20000;

/*
 * Fifo boost parameters.
 */
unsigned long config_fifo_priority = 98;
unsigned long config_force_fifo = 0;

/*
 * Control loop (time in seconds).
 */
long config_starving_threshold = 20;
long config_boost_duration = 3;
long config_aggressive = 0;
long config_granularity = 5;

/*
 * XXX: Make it a cpu mask, lazy Daniel!
 */
int config_monitor_all_cpus = 1;
char *config_monitored_cpus;
int config_nr_cpus;

/*
 * Size of pages in bytes.
 */
long page_size;

/*
 * This will get set when we finish reading first time
 * in detect_task_format. May change over time as the
 * system gets loaded
 */
size_t config_buffer_size;

/*
 * Boolean for if running under systemd.
 */
int config_systemd;

/*
 * Boolean to choose between deadline and fifo.
 */
int boost_policy;

/*
 * Variable to indicate if stalld is running or shutting down.
 */
int running = 1;

/*
 * Config single threaded: uses less CPU, but has a lower precision.
 */
int config_single_threaded = 1;

/*
 * Config adaptive multi-threaded: use a single thread when nothing
 * is happening, but dispatches a per-cpu thread after a starving
 * thread is waiting for half of the config_starving_threshold.
 */
int config_adaptive_multi_threaded = 0;

/*
 * Check the idle time before parsing sched_debug.
 */
int config_idle_detection = 1;
int STAT_MAX_SIZE = 4096;

/*
 * Variables related to the threads to be ignored.
 */
unsigned int nr_thread_ignore = 0;
regex_t *compiled_regex_thread = NULL;

/*
 * Variables related to the processes to be ignored.
 */
unsigned int nr_process_ignore = 0;
regex_t *compiled_regex_process = NULL;

/*
 * Store the current sched_debug file path.
 */
char *config_sched_debug_path = NULL;

/*
 * CPU reservation to use with SCHED_DEADLINE.
 */
int config_reservation = 0;

/*
 * Select a backend.
 */
struct stalld_backend *backend = &sched_debug_backend;

/*
 * Set of CPUs in which stalld should run.
 */
char *config_affinity_cpus;

/*
 * API to fetch the process group ID for a thread/process.
 */
int get_tgid(int pid)
{
	const char tgid_field[TGID_FIELD] = "Tgid:";
	char file_location[PROC_PID_FILE_PATH_LEN];
	char *status = NULL;
	int tgid, n;
	FILE *fp;

	status = calloc(TMP_BUFFER_SIZE, sizeof(char));
	if (status == NULL) {
		return -ENOMEM;
	}

	n = sprintf(file_location, "/proc/%d/status", pid);
	if (n < 0)
		goto out_free_mem;

	if ((fp = fopen(file_location, "r")) == NULL)
		goto out_free_mem;

	/* Iterate till we find the tgid field. */
	while (1) {
		if (fgets(status, TMP_BUFFER_SIZE, fp) == NULL)
			goto out_close_fd;
		if (!(strncmp(status, tgid_field, (TGID_FIELD - 1))))
			break;
		/*
		 * Zero out the buffer just in case
		 */
		memset(status, 0, TMP_BUFFER_SIZE);
	}
	/*
	 * Since we're now at the line we're interested in, let's read
	 * in the field that we want.
	 */
	if (sscanf(status, "%*s %d", &tgid) != 1)
		goto out_close_fd;

	fclose(fp);
	free(status);
	return tgid;

out_close_fd:
	fclose(fp);
out_free_mem:
	free(status);
	return -EINVAL;
}

/*
 * Read the content of sched_debug into the input buffer.
 */
int read_sched_stat(char *buffer, int size)
{
	int position = 0;
	int retval;
	int fd;

	fd = open("/proc/stat", O_RDONLY);

	if (fd < 0)
		goto out_error;

	do {
		retval = read(fd, &buffer[position], size - position);
		if (retval < 0)
			goto out_close_fd;

		position += retval;

	} while (retval > 0 && position < size);

	buffer[position-1] = '\0';

	close(fd);

	return position;

out_close_fd:
	close(fd);

out_error:
	return 0;
}

/*
 * Get how much time the CPU has been idle.
 *
 * Format:
 * "cpu1 832882 9111 153357 751780 456 32198 15356 0 0 0"
 * "cpu  user   nice system IDLE"
 */
static long get_cpu_idle_time(char *buffer, size_t buffer_size, int cpu)
{
	char cpuid[10]; /* cpuXXXXX\n */
	char *idle_start;
	char *end;
	long val;

	sprintf(cpuid, "cpu%d ", cpu);

	/* CPU */
	idle_start = strstr(buffer, cpuid);
	if (!idle_start)
		return -ENODEV; /* CPU might be offline. */

	/* Find and skip space before user. */
	idle_start = strstr(idle_start, " ");
	if (!idle_start)
		return -EINVAL;

	idle_start+=1;

	/* Find and skip space before nice. */
	idle_start = strstr(idle_start, " ");
	if (!idle_start)
		return -EINVAL;

	idle_start+=1;

	/* Find and skip space before system. */
	idle_start = strstr(idle_start, " ");
	if (!idle_start)
		return -EINVAL;

	idle_start+=1;

	/* Here is the idle! */
	idle_start = strstr(idle_start, " ");
	if (!idle_start)
		return -EINVAL;

	idle_start += 1;

	/* End. */
	end = strstr(idle_start, " ");
	if (!end)
		return -EINVAL;

	errno = 0;
	val = strtol(idle_start, &end, 10);
	if (errno != 0)
		return -EINVAL;

	return val;
}

int cpu_had_idle_time(struct cpu_info *cpu_info)
{
	char sched_stat[STAT_MAX_SIZE];
	long idle_time;

	if (!read_sched_stat(sched_stat, STAT_MAX_SIZE)) {
		warn("fail reading sched stat file");
		warn("disabling idle detection");
		config_idle_detection = 0;
		return 0;
	}

	idle_time = get_cpu_idle_time(sched_stat, STAT_MAX_SIZE, cpu_info->id);
	if (idle_time < 0) {
		if (idle_time != -ENODEV)
			warn("unable to parse idle time for cpu%d\n", cpu_info->id);
		return 0;
	}

	/*
	 * If it is different, there was a change, it does not matter if
	 * it wrapped around.
	 */
	if (cpu_info->idle_time == idle_time)
		return 0;

	if (config_verbose)
		log_msg("last idle time: %ld curr idle time:%ld ", cpu_info->idle_time, idle_time);

	/*
	 * The CPU had idle time!
	 */
	cpu_info->idle_time = idle_time;

	return 1;
}

int get_cpu_busy_list(struct cpu_info *cpus, int nr_cpus, char *busy_cpu_list)
{
	char sched_stat[STAT_MAX_SIZE];
	struct cpu_info *cpu;
	int busy_count = 0;
	long idle_time;
	int i;

	if (!read_sched_stat(sched_stat, STAT_MAX_SIZE)) {
		warn("fail reading sched stat file");
		warn("disabling idle detection");
		config_idle_detection = 0;

		/* Assume they are all busy. */
		return nr_cpus;
	}

	for (i = 0; i < nr_cpus; i++) {
		cpu = &cpus[i];
		/* Consider idle a CPU that has its own monitor. */
		if (cpu->thread_running) {
			if (config_verbose)
				log_msg("\t cpu %d has its own monitor, considering idle\n", cpu->id);
			continue;
		}

		idle_time = get_cpu_idle_time(sched_stat, STAT_MAX_SIZE, cpu->id);
		if (idle_time < 0) {
			if (idle_time != -ENODEV)
				warn("unable to parse idle time for cpu%d\n", cpu->id);
			continue;
		}

		if (config_verbose)
			log_msg ("\t cpu %d had %ld idle time, and now has %ld\n", cpu->id, cpu->idle_time, idle_time);

		/* If the idle time did not change, the CPU is busy. */
		if (cpu->idle_time == idle_time) {
			busy_cpu_list[i] = 1;
			busy_count++;
			continue;
		}

		cpu->idle_time = idle_time;
	}

	return busy_count;
}

void print_waiting_tasks(struct cpu_info *cpu_info)
{
	time_t now = time(NULL);
	struct task_info *task;
	int i;

	printf("CPU %d has %d waiting tasks\n", cpu_info->id, cpu_info->nr_waiting_tasks);
	if (!cpu_info->nr_waiting_tasks)
		return;

	for (i = 0; i < cpu_info->nr_waiting_tasks; i++) {
		task = &cpu_info->starving[i];

		printf("%15s %9d %9d %9d %9ld\n", task->comm, task->pid, task->prio, task->ctxsw, (now - task->since));
	}

	return;

}

struct cpu_starving_task_info {
	struct task_info task;
	int pid;
	int tgid;
	time_t since;
	int overloaded;
};

struct cpu_starving_task_info *cpu_starving_vector;

void update_cpu_starving_vector(int cpu, int tgid, int pid, time_t since, struct task_info *task)
{
	struct cpu_starving_task_info *cpu_info = &cpu_starving_vector[cpu];

	/*
	 * If there is another thread already here, mark this CPU as
	 * overloaded.
	 */
	if (cpu_info->pid)
		cpu_info->overloaded = 1;

	/*
	 * If there is no thread in the vector, or if the in the
	 * vector has an earlier since (time stamp), update it.
	 */
	if ((cpu_info->since == 0) || cpu_info->since > since) {
		memcpy(&(cpu_info->task), task, sizeof(struct task_info));
		cpu_info->pid = pid;
		cpu_info->tgid = tgid;
		cpu_info->since = since;
	}
}

void merge_taks_info(int cpu, struct task_info *old_tasks, int nr_old, struct task_info *new_tasks, int nr_new)
{
	struct task_info *old_task;
	struct task_info *new_task;
	int i;
	int j;

	for (i = 0; i < nr_old; i++) {
		old_task = &old_tasks[i];

		for (j = 0; j < nr_new; j++) {
			new_task = &new_tasks[j];

			if (old_task->pid == new_task->pid) {
				if (old_task->ctxsw == new_task->ctxsw) {
					new_task->since = old_task->since;
					if (config_single_threaded)
						update_cpu_starving_vector(cpu, new_task->tgid, new_task->pid, new_task->since, new_task);
				}
				break;
			}
		}
	}
}

int get_current_policy(int pid, struct sched_attr *attr)
{
	int ret;

	ret = sched_getattr(pid, attr, sizeof(*attr), 0);
	if (ret == -1)
		log_msg("get_current_policy: failed with error %s\n", strerror(errno));
	return ret;
}

void print_boosted_info(int tgid, int pid, struct cpu_info *cpu, char *type)
{
	char comm[COMM_SIZE];

	fill_process_comm(tgid, pid, comm, COMM_SIZE);

	if (cpu)
		log_msg("boosted pid %d (%s) (cpu %d) using %s\n", pid, comm, cpu->id, type);
	else
		log_msg("boosted pid %d (%s) using %s\n", pid, comm, type);
}

int boost_with_deadline(int tgid, int pid, struct cpu_info *cpu)
{
	struct sched_attr attr;
	int flags = 0;
	int ret;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_policy   = SCHED_DEADLINE;
	attr.sched_runtime  = config_dl_runtime;
	attr.sched_deadline = config_dl_period;
	attr.sched_period   = config_dl_period;

	ret = sched_setattr(pid, &attr, flags);
	if (ret < 0) {
	    log_msg("boost_with_deadline failed to boost pid %d: %s\n", pid, strerror(errno));
	    return ret;
	}

	print_boosted_info(tgid, pid, cpu, "SCHED_DEADLINE");
	return ret;
}

int boost_with_fifo(int tgid, int pid, struct cpu_info *cpu)
{
	struct sched_attr attr;
	int flags = 0;
	int ret;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_policy   = SCHED_FIFO;
	attr.sched_priority = config_fifo_priority;

	ret = sched_setattr(pid, &attr, flags);
	if (ret < 0) {
	    log_msg("boost_with_fifo failed to boost pid %d: %s\n", pid, strerror(errno));
	    return ret;
	}

	print_boosted_info(tgid, pid, cpu, "SCHED_FIFO");
	return ret;
}

int restore_policy(int pid, struct sched_attr *attr)
{
	int flags = 0;
	int ret;

	ret = sched_setattr(pid, attr, flags);
	if (ret < 0)
		log_msg("restore_policy: failed to restore sched policy for pid %d: %s\n",
			pid, strerror(errno));
	return ret;
}

/*
 * This function emulates the behavior of SCHED_DEADLINE but using SCHED_FIFO
 * by boosting the thread, sleeping for runtime, changing the pid policy
 * back to its old policy, then sleeping for the remainder of the period,
 * repeating until all the periods are done.
 */
void do_fifo_boost(int tgid, int pid, struct sched_attr *old_attr, struct cpu_info *cpu)
{
	uint64_t nr_periods = (config_boost_duration * NS_PER_SEC) / config_dl_period;
	struct timespec remainder_ts;
	struct timespec runtime_ts;
	struct timespec ts;
	uint64_t i;

	/* Setup the runtime sleep. */
	memset(&runtime_ts, 0, sizeof(runtime_ts));
	runtime_ts.tv_nsec = config_dl_runtime;
	normalize_timespec(&runtime_ts);

	/* Setup the remainder of the period sleep. */
	memset(&remainder_ts, 0, sizeof(remainder_ts));
	remainder_ts.tv_nsec = config_dl_period - config_dl_runtime;
	normalize_timespec(&remainder_ts);

	for (i=0; i < nr_periods; i++) {
		boost_with_fifo(tgid, pid, cpu);
		ts = runtime_ts;
		clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, 0);
		restore_policy(pid, old_attr);
		ts = remainder_ts;
		clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, 0);
	}
}

int boost_starving_task(int tgid, int pid, struct cpu_info *cpu)
{
	struct sched_attr attr;
	int ret;

	/*
	 * Get the old prio, to be restored at the end of the
	 * boosting period.
	 */
	ret = get_current_policy(pid, &attr);
	if (ret < 0)
		return ret;

	/* Boost. */
	if (boost_policy == SCHED_DEADLINE) {
		ret = boost_with_deadline(tgid, pid, cpu);
		if (ret < 0)
			return ret;
		sleep(config_boost_duration);
		ret = restore_policy(pid, &attr);
		if (ret < 0)
			return ret;
	} else {
		do_fifo_boost(tgid, pid, &attr, cpu);
	}

	/*
	 * XXX: If the proccess dies, we get an error. Deal with that
	 * latter.
	 * if (ret < 0)
	 *   die("sched_setattr failed to set the normal priorities");
	 */

	return 0;

}

/*
 * API to check if the task must not be considered for priority boosting.
 * The task's name itself will be checked or the name of the task
 * group it is a part of will be checked.
 */
int check_task_ignore(struct task_info *task) {
	char group_comm[COMM_SIZE];
	int ret = -EINVAL;
	unsigned int i;

	/*
	 * Check if this task's name has been passed as part of the
	 * thread ignore regex.
	 */
	for (i = 0; i < nr_thread_ignore; i++) {
		ret = regexec(&compiled_regex_thread[i], task->comm, REGEXEC_NO_NMATCH,
				REGEXEC_NO_MATCHPTR, REGEXEC_NO_FLAGS);
		if (!ret) {
			log_msg("Ignoring the thread %s from consideration for boosting\n", task->comm);
			return ret;
		}
	}
	ret = -EINVAL;

	/*
	 * If a valid tgid has been found and its not that of the swapper
	 * (because its not listed on the /proc filesystem) then proceed
	 * to fetch the name of the process.
	 */
	if (task->tgid > SWAPPER) {
		if (fill_process_comm(task->tgid, task->pid, group_comm, COMM_SIZE)) {
			warn("Ran into a tgid without process name");
			return ret;
		}
		/*
		 * Check if the process group that this task is a part has been
		 * requested to be ignored.
		 */
		for (i = 0; i < nr_process_ignore; i++) {
			ret = regexec(&compiled_regex_process[i], group_comm, REGEXEC_NO_NMATCH,
					REGEXEC_NO_MATCHPTR, REGEXEC_NO_FLAGS);
			if (!ret) {
				log_msg("Ignoring the thread %s (spawned by %s) from consideration for boosting\n", task->comm, group_comm);
				goto out;
			}
		}
	}
out:
	return ret;
}

int check_starving_tasks(struct cpu_info *cpu)
{
	struct task_info *tasks = cpu->starving;
	struct task_info *task;
	int starving = 0;
	int i;

	for (i = 0; i < cpu->nr_waiting_tasks; i++) {
		task = &tasks[i];

		if ((time(NULL) - task->since) >= config_starving_threshold) {

			log_msg("%s-%d starved on CPU %d for %d seconds\n",
				task->comm, task->pid, cpu->id,
				(time(NULL) - task->since));

			/*
			 * Check if this task needs to be ignored from being boosted
			 * if yes, update the time stamp so that it doesn't keep
			 * getting reported as being starved.
			 */
			if (config_ignore && !(check_task_ignore(task))) {
				task->since = time(NULL);
				continue;
			}

			starving+=1;

			/*
			 * It it is only logging, just reset the time counter
			 * after logging.
			 */
			if (config_log_only) {
				task->since = time(NULL);
				continue;
			}

			boost_starving_task(task->tgid, task->pid, cpu);
		}
	}

	return starving;
}

int check_might_starve_tasks(struct cpu_info *cpu)
{
	struct task_info *tasks = cpu->starving;
	struct task_info *task;
	int starving = 0;
	int i;

	if (cpu->thread_running)
		warn("checking a running thread!!!???");

	for (i = 0; i < cpu->nr_waiting_tasks; i++) {
		task = &tasks[i];

		if ((time(NULL) - task->since) >= config_starving_threshold/2) {

			log_msg("%s-%d might starve on CPU %d (waiting for %d seconds)\n",
				task->comm, task->pid, cpu->id,
				(time(NULL) - task->since));

			starving = 1;
		}
	}

	return starving;
}

static int get_cpu_and_parse(struct cpu_info *cpu, char *buffer, int buffer_size)
{
	int retval;

	if (backend->get_cpu) {
		retval = backend->get_cpu(buffer, buffer_size, cpu->id);
		if(!retval) {
			warn("fail reading backend");
			warn("Dazed and confused, but trying to continue");
			return 1;
		}
	}

	retval = backend->parse(cpu, buffer, buffer_size);
	if (retval) {
		warn("error parsing CPU info");
		warn("Dazed and confused, but trying to continue");
		return 1;
	}

	return 0;
}

static int cpu_main_parse_starving_task(struct cpu_info *cpu)
{
	int retval;

	if (backend->get) {
		retval = backend->get(cpu->buffer, cpu->buffer_size);
		if(!retval) {
			warn("fail reading backend");
			warn("Dazed and confused, but trying to continue");
			return 1;
		}
	}

	return get_cpu_and_parse(cpu, cpu->buffer, cpu->buffer_size);
}

void *cpu_main(void *data)
{
	struct cpu_info *cpu = data;
	int nothing_to_do = 0;
	int retval;

	while (cpu->thread_running && running) {

		/* Buffer size should increase. See sched_debug_get(). */
		if (config_buffer_size != cpu->buffer_size) {
			char *old_buffer = cpu->buffer;
			cpu->buffer = realloc(cpu->buffer, config_buffer_size);
			if (!cpu->buffer) {
				warn("fail to increase the buffer... continue");
				cpu->buffer = old_buffer;
			} else {
				cpu->buffer_size = config_buffer_size;
			}
		}

		if (config_idle_detection) {
			if (cpu_had_idle_time(cpu)) {
				if (config_verbose)
					log_msg("cpu %d had idle time! skipping next phase\n", cpu->id);
				nothing_to_do++;
				goto skipped;
			}
		}

		retval = cpu_main_parse_starving_task(cpu);
		if (retval)
			goto skipped;

		if (config_verbose)
			print_waiting_tasks(cpu);

		if (backend->has_starving_task(cpu)) {
			nothing_to_do = 0;
			check_starving_tasks(cpu);
		} else {
			nothing_to_do++;
		}

skipped:
		/*
		 * It not in aggressive mode, give up after 10 cycles with
		 * nothing to do.
		 */
		if (!config_aggressive && nothing_to_do == 10) {
			cpu->thread_running=0;
			pthread_exit(NULL);
		}

		sleep(config_granularity);
	}

	return NULL;
}

static const char *join_thread(pthread_t *thread)
{
	void *result;

	pthread_join(*thread, &result);

	return result;
}

void aggressive_main(struct cpu_info *cpus, int nr_cpus)
{
	int i;

	for (i = 0; i < nr_cpus; i++) {
		if (!should_monitor(i))
			continue;

		cpus[i].id = i;
		cpus[i].thread_running = 1;
		pthread_create(&cpus[i].thread, NULL, cpu_main, &cpus[i]);
	}

	for (i = 0; i < nr_cpus; i++) {
		if (!should_monitor(i))
			continue;

		join_thread(&cpus[i].thread);
	}
}

void conservative_main(struct cpu_info *cpus, int nr_cpus)
{
	char busy_cpu_list[nr_cpus];
	pthread_attr_t dettached;
	size_t buffer_size = 0;
	struct cpu_info *cpu;
	char *buffer = NULL;
	int has_busy_cpu;
	int retval;
	int i;

	buffer = malloc(config_buffer_size);
	if (!buffer)
		die("cannot allocate buffer");

	buffer_size = config_buffer_size;

	pthread_attr_init(&dettached);
	pthread_attr_setdetachstate(&dettached, PTHREAD_CREATE_DETACHED);

	for (i = 0; i < nr_cpus; i++) {
		cpus[i].id = i;
		cpus[i].thread_running = 0;
	}

	while (running) {

		/* Buffer size should increase. See sched_debug_get(). */
		if (config_buffer_size != buffer_size) {
			char *old_buffer = buffer;
			buffer = realloc(buffer, config_buffer_size);
			if (!buffer) {
				warn("fail to increase the buffer... continue");
				buffer = old_buffer;
			} else {
				buffer_size = config_buffer_size;
			}
		}

		if (config_idle_detection) {
			memset(&busy_cpu_list, 0, nr_cpus);
			has_busy_cpu = get_cpu_busy_list(cpus, nr_cpus, busy_cpu_list);
			if (!has_busy_cpu) {
				if (config_verbose)
					log_msg("all CPUs had idle time, skipping parse\n");
				goto skipped;
			}
		}

		if (backend->get) {
			retval = backend->get(buffer, buffer_size);
			if (!retval) {
				warn("Dazed and confused, but trying to continue");
				continue;
			}
		}

		for (i = 0; i < nr_cpus; i++) {
			if (!should_monitor(i))
				continue;

			cpu = &cpus[i];

			if (cpu->thread_running)
				continue;

			if (config_idle_detection && !busy_cpu_list[i])
				continue;

			retval = get_cpu_and_parse(cpu, buffer, buffer_size);
			if (retval)
				continue;

			if (config_verbose)
				printf("\tchecking cpu %d - rt: %d - starving: %d\n",
				       i, cpu->nr_rt_running, cpu->nr_waiting_tasks);

			if (check_might_starve_tasks(cpu)) {
				cpus[i].id = i;
				cpus[i].thread_running = 1;
				pthread_create(&cpus[i].thread, &dettached, cpu_main, &cpus[i]);
			}
		}

skipped:
		sleep(config_granularity);
	}
}

int boost_cpu_starving_vector(struct cpu_starving_task_info *vector, int nr_cpus, struct cpu_info *cpus)
{
	struct cpu_starving_task_info *cpu;
	struct sched_attr attr[nr_cpus];
	int deboost_vector[nr_cpus];
	int boosted = 0;
	time_t now;
	int ret;
	int i;

	now = time(NULL);

	/* Boost phase. */
	for (i = 0; i < nr_cpus; i++) {

		/* Clear the deboost vector for this CPU. */
		deboost_vector[i] = 0;

		cpu = &cpu_starving_vector[i];

		if (config_verbose && cpu->pid)
			log_msg("\t cpu %d: pid: %d starving for %llu\n", i, cpu->pid, (now - cpu->since));

		if (config_log_only)
			continue;

		if (cpu->pid != 0 && (now - cpu->since) > config_starving_threshold) {
			/*
			 * Check if this task name is part of a denylist
			 * If yes, do not boost it.
			 */
			if (config_ignore && !check_task_ignore(&cpu->task))
				continue;

			/* Save the task policy. */
			ret = get_current_policy(cpu->pid, &attr[i]);

			/* It is ok if a task die. */
			if (ret < 0)
				continue;

			/* Boost! */
			ret = boost_with_deadline(cpu->tgid, cpu->pid, &cpus[i]);

			/* It is ok if a task die. */
			if (ret < 0)
				continue;

			/* Save it for the deboost. */
			deboost_vector[i] = cpu->pid;

			boosted++;
		}
	}

	if (!boosted)
		return 0;

	sleep(config_boost_duration);

	for (i = 0; i < nr_cpus; i++) {
		if (deboost_vector[i] != 0)
			restore_policy(deboost_vector[i], &attr[i]);
	}

	return boosted;
}

void single_threaded_main(struct cpu_info *cpus, int nr_cpus)
{
	char busy_cpu_list[nr_cpus];
	size_t buffer_size = 0;
	struct cpu_info *cpu;
	char *buffer = NULL;
	int overloaded = 0;
	int has_busy_cpu;
	int boosted = 0;
	int retval;
	int i;

	log_msg("single threaded mode\n");

	if (!config_log_only && boost_policy != SCHED_DEADLINE)
		die("Single threaded mode only works with SCHED_DEADLINE");

	cpu_starving_vector = malloc(sizeof(struct cpu_starving_task_info) * nr_cpus);
	if (!cpu_starving_vector)
		die("cannot allocate cpu starving vector");

	buffer = malloc(config_buffer_size);
	if (!buffer)
		die("cannot allocate buffer");

	buffer_size = config_buffer_size;

	for (i = 0; i < nr_cpus; i++) {
		cpus[i].id = i;
		cpus[i].thread_running = 0;
		cpu_starving_vector[i].pid = 0;
		cpu_starving_vector[i].since = 0;
		cpu_starving_vector[i].overloaded = 0;
		memset(&cpu_starving_vector[i].task, 0, sizeof(struct task_info));
	}

	while (running) {

		/* Buffer size should increase. See sched_debug_get(). */
		if (config_buffer_size != buffer_size) {
			char *old_buffer = buffer;
			buffer = realloc(buffer, config_buffer_size);
			if (!buffer) {
				warn("fail to increase the buffer... continue");
				buffer = old_buffer;
			} else {
				buffer_size = config_buffer_size;
			}
		}

		if (config_idle_detection) {
			memset(&busy_cpu_list, 0, nr_cpus);
			has_busy_cpu = get_cpu_busy_list(cpus, nr_cpus, busy_cpu_list);
			if (!has_busy_cpu) {
				if (config_verbose)
					log_msg("all CPUs had idle time, skipping parse\n");
				goto skipped;
			}
		}

		if (backend->get) {
			retval = backend->get(buffer, buffer_size);
			if (!retval) {
				warn("Dazed and confused, but trying to continue");
				continue;
			}
		}

		for (i = 0; i < nr_cpus; i++) {
			if (!should_monitor(i))
				continue;

			cpu = &cpus[i];

			if (config_idle_detection && !busy_cpu_list[i])
				continue;

			retval = get_cpu_and_parse(cpu, buffer, buffer_size);
			if (retval)
				continue;

			if (config_verbose)
				printf("\tchecking cpu %d - rt: %d - starving: %d\n",
				       i, cpu->nr_rt_running, cpu->nr_waiting_tasks);

		}

		boosted = boost_cpu_starving_vector(cpu_starving_vector, nr_cpus, cpus);
		if (!boosted)
			goto skipped;

		/* Cleanup the CPU starving vector. */
		for (i = 0; i < nr_cpus; i++) {
			memset(&(cpu_starving_vector[i].task), 0, sizeof(struct task_info));
			cpu_starving_vector[i].pid = 0;
			cpu_starving_vector[i].since = 0;
			if (cpu_starving_vector[i].overloaded)
				overloaded = 1;
			cpu_starving_vector[i].overloaded = 0;
		}

		/*
		 * If any CPU had more than one thread starving, the system is overloaded.
		 * Re-run the loop without sleeping for two reasons: to boost the other
		 * thread, and to detect other starving threads on other CPUs, given
		 * that the system seems to be overloaded.
		 */
		if (overloaded) {
			overloaded = 0;
			continue;
		}

skipped:
		/* If no boost was required, just sleep. */
		if (!boosted) {
			sleep(config_granularity);
			continue;
		}

		/*
		 * If the boost duration is longer than the granularity, there
		 * is no need for a sleep.
		 */
		if (config_granularity <= config_boost_duration)
			continue;

		/*
		 * Ok, sleep for the rest of the time.
		 *
		 * Yeah, but is it worth to get the time to compute the overhead?
		 * at the end, it should be less than one second anyway.
		 */
		sleep(config_granularity - config_boost_duration);
	}
	if (buffer)
		free(buffer);
}

int check_policies(void)
{
	int saved_runtime = config_dl_runtime;
	int boosted = SCHED_DEADLINE;
	struct sched_attr attr;
	int ret;

	/* If we specified FIFO on the command line just return false. */
	if (config_force_fifo) {
		log_msg("forcing SCHED_FIFO for boosting\n");
		return SCHED_FIFO;
	}

	/* Set runtime to half of period. */
	config_dl_runtime = config_dl_period / 2;

	/* Save off the current policy. */
	if (get_current_policy(0, &attr))
		die("unable to get scheduling policy!");

	/* Try boosting to SCHED_DEADLINE. */
	ret = boost_with_deadline(0, 0, NULL);
	if (ret < 0) {
		/* Try boosting with FIFO to see if we have permission. */
		ret = boost_with_fifo(0, 0, NULL);
		if (ret < 0) {
			log_msg("check_policies: unable to change policy to either deadline or fifo,"
				"defaulting to logging only\n");
			config_log_only = 1;
			boosted = 0;
		}
		else
			boosted = SCHED_FIFO;
	}
	/* If we successfully boosted to something, restore the old policy. */
	if (boosted) {
		ret = restore_policy(0, &attr);
		/* If we can't restore the policy then quit now. */
		if (ret < 0)
			die("unable to restore policy: %s\n", strerror(errno));
 	}

	/* Restore the actual runtime value. */
	config_dl_runtime = saved_runtime;
	if (boosted == SCHED_DEADLINE)
		log_msg("using SCHED_DEADLINE for boosting\n");
	else if (boosted == SCHED_FIFO)
		log_msg("using SCHED_FIFO for boosting\n");
	return boosted;
}

int main(int argc, char **argv)
{
	struct cpu_info *cpus;
	int retval;
	int i;

	/* Get the system page size so we can use it when allocating buffers. */
	if ((page_size = sysconf(_SC_PAGE_SIZE)) < 0)
		die("Unable to get system page size: %s\n", strerror(errno));

	config_nr_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (config_nr_cpus < 1)
		die("Can not calculate number of CPUS\n");

	parse_args(argc, argv);

	/*
	 * it will not die...
	 */
	if (config_affinity_cpus)
		set_cpu_affinity(config_affinity_cpus);

	/*
	 * Check RT throttling:
	 *
	 * If --systemd was specified then RT throttling should already be off
	 * otherwise turn it off. In both cases verify that it actually got
	 * turned off since we can't run with it on.
	 */
	if (config_systemd) {
		if (!config_log_only && !rt_throttling_is_off())
			die ("RT throttling is on! stalld cannot run...\n");
	}
	else if (!config_log_only) {
		turn_off_rt_throttling();
		if (!rt_throttling_is_off())
			die("turning off RT throttling failed, stalld cannot run\n");
	}

	/* See if SCHED_DEADLINE is available. */
	if (!config_log_only)
		boost_policy = check_policies();

	cpus = malloc(sizeof(struct cpu_info) * config_nr_cpus);
	if (!cpus)
		die("Cannot allocate memory");

	memset(cpus, 0, sizeof(struct cpu_info) * config_nr_cpus);

	for (i = 0; i < config_nr_cpus; i++) {
		cpus[i].buffer = malloc(config_buffer_size);
		if (!cpus[i].buffer)
			die("Cannot allocate memory");

		cpus[i].buffer_size = config_buffer_size;
	}

	if (config_log_syslog)
		openlog("stalld", 0, LOG_DAEMON);

	if (backend->init())
		die("Cannot init backend");

	setup_signal_handling();

	if (config_idle_detection)
		STAT_MAX_SIZE = config_nr_cpus * page_size;

	if (!config_foreground)
		daemonize();

	/*
	 * Set stalld as SCHED_DEADLINE using config_reservation %
	 * of the CPU time.
	 */
	if (config_reservation) {
		retval = set_reservation(config_granularity, config_reservation);
		if (retval) {
			log_msg("error setting the reservation\n");
			exit(EXIT_FAILURE);
		}
	}

	write_pidfile();

	/* The less likely first. */
	if (config_aggressive)
		aggressive_main(cpus, config_nr_cpus);
	else if (config_adaptive_multi_threaded)
		conservative_main(cpus, config_nr_cpus);
	else
		single_threaded_main(cpus, config_nr_cpus);

	cleanup_regex(&nr_thread_ignore, &compiled_regex_thread);
	cleanup_regex(&nr_process_ignore, &compiled_regex_process);
	if (config_log_syslog)
		closelog();

	backend->destroy();

	exit(0);
}
