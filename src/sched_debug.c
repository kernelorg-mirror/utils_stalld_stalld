/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2020-2022 Red Hat Inc, Daniel Bristot de Oliveira <bristot@redhat.com>
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <regex.h>

#include "stalld.h"
#include "sched_debug.h"

/*
 * Auto-detected task format from sched_debug.
 */
static int config_task_format;

/*
 * Read the contents of sched_debug into the input buffer.
 */
static int sched_debug_get(char *buffer, int size)
{
	int position = 0;
	int retval;
	int fd;

	fd = open(config_sched_debug_path, O_RDONLY);

	if (fd < 0)
		goto out_error;

	do {
		retval = read(fd, &buffer[position], size - position);
		if (retval < 0)
			goto out_close_fd;

		position += retval;

	} while (retval > 0 && position < size);

	buffer[position-1] = '\0';

	if (position + 100 > config_buffer_size) {
		config_buffer_size = config_buffer_size * 2;
		log_msg("sched_debug is getting larger, increasing the buffer to %zu\n", config_buffer_size);
	}

	close(fd);

	return position;

out_close_fd:
	close(fd);

out_error:
	return 0;
}

/*
 * Find the start of a CPU information block in the input buffer.
 */
static char *get_cpu_info_start(char *buffer, int cpu)
{
	/* 'cpu#9999,\0' */
	char cpu_header[10];

	sprintf(cpu_header, "cpu#%d,", cpu);

	return strstr(buffer, cpu_header);
}

static char *get_next_cpu_info_start(char *start)
{
	const char *next_cpu = "cpu#";

	/* Skip the current CPU definition. */
	start += 10;

	return strstr(start, next_cpu);
}

static char *alloc_and_fill_cpu_buffer(int cpu, char *sched_dbg, int sched_dbg_size)
{
	char *next_cpu_start;
	char *cpu_buffer;
	char *cpu_start;
	int size = 0;

	cpu_start = get_cpu_info_start(sched_dbg, cpu);

	/* The CPU might be offline. */
	if (!cpu_start)
		return NULL;

	next_cpu_start = get_next_cpu_info_start(cpu_start);

	/*
	 * If it did not find the next CPU, it should be the end of the file.
	 */
	if (!next_cpu_start)
		next_cpu_start = sched_dbg + sched_dbg_size;

	size = next_cpu_start - cpu_start;

	if (size <= 0)
		return NULL;

	cpu_buffer = malloc(size);

	if (!cpu_buffer)
		return NULL;

	strncpy(cpu_buffer, cpu_start, size);

	cpu_buffer[size-1] = '\0';

	return cpu_buffer;
}

/*
 * Parsing helpers for skipping white space and chars and detecting
 * next line.
 */
static inline char *skipchars(char *str)
{
	while (*str && !isspace(*str))
		str++;
	return str;
}

static inline char *skipspaces(char *str)
{
	while (*str && isspace(*str))
		str++;
	return str;
}

static inline char *nextline(char *str)
{
	char *ptr = strchr(str, '\n');
	return ptr ? ptr+1 : NULL;
}

/*
 * Read sched_debug and figure out if it's old or new format
 * done once so if we fail just exit the program.
 *
 * NOTE: A side effect of this call is to set the initial value for
 * config_buffer_size used when reading sched_debug for parsing.
 */
static int detect_task_format(void)
{
	int bufincrement;
	int retval = -1;
	size_t bufsiz;
	char *buffer;
	int size = 0;
	char *ptr;
	int status;
	int fd;

	bufsiz = bufincrement = BUFFER_PAGES * page_size;

	buffer = malloc(bufsiz);

	if (buffer == NULL)
		die("unable to allocate %d bytes to read sched_debug");

	if ((fd = open(config_sched_debug_path, O_RDONLY)) < 0)
		die("error opening sched_debug for reading: %s\n", strerror(errno));

	ptr = buffer;
	while ((status = read(fd, ptr, bufincrement))) {
		if (status < 0)
			die ("error reading sched_debug: %s\n", strerror(errno));

		size += status;
		bufsiz += bufincrement;
		if ((buffer = realloc(buffer, bufsiz)) == NULL)
			die("realloc failed for %zu size: %s\n", bufsiz, strerror(errno));
		ptr = buffer + size;
	}

	close(fd);

	buffer[size] = '\0';
	config_buffer_size = bufsiz;
	log_msg("initial config_buffer_size set to %zu\n", config_buffer_size);

	ptr = strstr(buffer, TASK_MARKER);
	if (ptr == NULL) {
		fprintf(stderr, "unable to find 'runnable tasks' in buffer, invalid input\n");
		exit(-1);
	}

	ptr += strlen(TASK_MARKER) + 1;
	ptr = skipspaces(ptr);

	if (strncmp(ptr, "task", 4) == 0) {
		retval = OLD_TASK_FORMAT;
		log_msg("detected old task format\n");
	} else if (strncmp(ptr, "S", 1) == 0) {
		retval = NEW_TASK_FORMAT;
		log_msg("detected new task format\n");
	}

	free(buffer);
	return retval;
}

/*
 * Parse the new sched_debug format.
 *
 * Example:
 * ' S           task   PID         tree-key  switches  prio     wait-time             sum-exec        sum-sleep'
 * '-----------------------------------------------------------------------------------------------------------'
 * ' I         rcu_gp     3        13.973264         2   100         0.000000         0.004469         0.000000 0 0 /
 */
static int parse_new_task_format(char *buffer, struct task_info *task_info, int nr_entries)
{
	char *R, *X, *start = buffer;
	struct task_info *task;
	int tasks = 0;
	int comm_size;
	char *end;

	/*
	 * If we have less than two tasks on the CPU there is no
	 * possibility of a stall.
	 */
	if (nr_entries < 2)
		return 0;

	while (tasks < nr_entries) {
		task = &task_info[tasks];

		/*
		 * Runnable tasks.
		 */
		R = strstr(start, "\n R");

		/*
		 * Dying tasks.
		 */
		X = strstr(start, "\n X");

		/*
		 * Get the first one, the only one, or break.
		 */
		if (X && R) {
			start = R < X ? R : X;
		} else if (X || R) {
			start = R ? R : X;
		} else {
			break;
		}

		/* Skip '\n R' || '\n X'. */
		start = &start[3];

		/* Skip the spaces. */
		start = skipspaces(start);

		/* Find the end of the string. */
		end = skipchars(start);

		comm_size = end - start;

		if (comm_size >= COMM_SIZE) {
			warn("comm_size is too large: %d\n", comm_size);
			comm_size = COMM_SIZE - 1;
		}

		strncpy(task->comm, start, comm_size);

		task->comm[comm_size] = '\0';

		/* Go to the end of the task comm. */
		start=end;

		task->pid = strtol(start, &end, 10);

		/* Get the id of the thread group leader. */
		task->tgid = get_tgid(task->pid);

		/* Go to the end of the pid. */
		start=end;

		/* Skip the tree-key. */
		start = skipspaces(start);
		start = skipchars(start);

		task->ctxsw = strtol(start, &end, 10);

		start = end;

		task->prio = strtol(start, &end, 10);

		task->since = time(NULL);

		/* Go to the end and try to find the next occurrence. */
		start = end;

		tasks++;
	}

	return tasks;
}

/*
 * The old format of sched_debug doesn't contain state information so we have
 * to pick up the pid and then open /proc/<pid>/stat to get the process state.
 */
static int is_runnable(int pid)
{
	char stat_path[128], stat[512];
	int fd, retval, runnable = 0;
	char *ptr;

	if (pid == 0)
		return 0;
	retval = snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
	if (retval < 0 || retval > sizeof(stat_path)) {
		warn("stat path for task %d too long\n", pid);
		goto out_error;
	}
	fd = open(stat_path, O_RDONLY);
	if (fd < 0) {
		warn("error opening stat path for task %d\n", pid);
		goto out_error;
	}
	flock(fd, LOCK_SH);
	retval = read(fd, &stat, sizeof(stat));
	if (retval < 0) {
		warn("error reading stat for task %d\n", pid);
		goto out_close_fd;
	}
	if (retval < sizeof(stat))
		stat[retval] = '\0';

	/*
	 * The process state is the third white-space delimited field
	 * in /proc/PID/stat. Skip to there and check what the value is.
	 */

	/* Skip first word. */
	ptr = skipchars(stat);
	/* Skip spaces. */
	ptr = skipspaces(ptr);
	/* Skip second word. */
	ptr = skipchars(ptr);
	/* Skip spaces. */
	ptr = skipspaces(ptr);

	switch(*ptr) {
	case 'R':
		runnable = 1;
		break;
	case 'S':
	case 'D':
	case 'Z':
	case 'T':
		break;
	default:
		warn("invalid state(%c) in %s\n", *ptr, stat_path);
	}

out_close_fd:
	flock(fd, LOCK_UN);
	close(fd);
out_error:
	return runnable;
}

static int count_task_lines(char *buffer)
{
	int lines = 0;
	char *ptr;
	int len;

	len = strlen(buffer);

	/* Find the runnable tasks: header. */
	ptr = strstr(buffer, TASK_MARKER);
	if (ptr == NULL)
		return 0;

	/* Skip to the end of the dashed line separator. */
	ptr = strstr(ptr, "-\n");
	if (ptr == NULL)
		return 0;

	ptr += 2;
	while(*ptr && ptr < (buffer+len)) {
		lines++;
		ptr = strchr(ptr, '\n');
		if (ptr == NULL)
			break;
		ptr++;
	}
	return lines;
}

/*
 * Parse the old sched debug format:
 *
 * Example:
 * '            task   PID         tree-key  switches  prio     wait-time             sum-exec        sum-sleep
 * ' ----------------------------------------------------------------------------------------------------------
 * '     watchdog/35   296       -11.731402      4081     0         0.000000        44.052473         0.000000 /
 */
static int parse_old_task_format(char *buffer, struct task_info *task_info, int nr_entries)
{
	int pid, ctxsw, prio, comm_size;
	char *start, *end, *buffer_end;
	struct task_info *task;
	char comm[COMM_SIZE];
	int waiting_tasks = 0;

	start = buffer;
	start = strstr(start, TASK_MARKER);
	start = strstr(start, "-\n");
	start++;

	buffer_end = buffer + strlen(buffer);

	/*
	 * We can't short-circuit using nr_entries, we have to scan the
	 * entire list of processes that is on this CPU.
	 */
	while (*start && start < buffer_end) {
		task = &task_info[waiting_tasks];

		/* Only care about tasks that are not R (running on a CPU). */
		if (start[0] == 'R') {
			/* Go to the end of the line and ignore this task. */
			start = strchr(start, '\n');
			start++;
			continue;
		}

		/* Pick up the comm field. */
		start = skipspaces(start);
		end = skipchars(start);
		comm_size = end - start;
		if (comm_size >= COMM_SIZE) {
			warn("comm_size is too large: %d\n", comm_size);
			comm_size = COMM_SIZE - 1;
		}
		strncpy(comm, start, comm_size);
		comm[comm_size] = 0;

		/* Go to the end of the task comm. */
		start=end;

		/* Now pick up the pid. */
		pid = strtol(start, &end, 10);

		/* Go to the end of the pid. */
		start=end;

		/* Skip the tree-key. */
		start = skipspaces(start);
		start = skipchars(start);

		/* Pick up the context switch count. */
		ctxsw = strtol(start, &end, 10);
		start = end;

		/* Get the priority. */
		prio = strtol(start, &end, 10);
		if (is_runnable(pid)) {
			strncpy(task->comm, comm, comm_size);
			task->comm[comm_size] = 0;
			task->pid = pid;
			task->tgid = get_tgid(task->pid);
			task->ctxsw = ctxsw;
			task->prio = prio;
			task->since = time(NULL);
			waiting_tasks++;
		}

		if ((start = nextline(start)) == NULL)
			break;

		if (waiting_tasks >= nr_entries) {
			break;
		}
	}

	return waiting_tasks;
}

static int fill_waiting_task(char *buffer, struct cpu_info *cpu_info)
{
	int nr_waiting = -1;
	int nr_entries;

	if (cpu_info == NULL) {
		warn("NULL cpu_info pointer!\n");
		return 0;
	}
	nr_entries = cpu_info->nr_running;

	switch (config_task_format) {
	case NEW_TASK_FORMAT:
		cpu_info->starving = malloc(sizeof(struct task_info) * nr_entries);
		if (cpu_info->starving == NULL) {
			warn("failed to malloc %d task_info structs", nr_entries);
			return 0;
		}
		nr_waiting = parse_new_task_format(buffer, cpu_info->starving, nr_entries);
		break;
	case OLD_TASK_FORMAT:
		/*
		 * The old task format does not output a correct value for
		 * nr_running (the initializer for nr_entries) so count the
		 * task lines for this CPU data and use that instead.
		 */
		nr_entries = count_task_lines(buffer);
		if (nr_entries <= 0)
			return 0;
		cpu_info->starving = malloc(sizeof(struct task_info) * nr_entries);
		if (cpu_info->starving == NULL) {
			warn("failed to malloc %d task_info structs", nr_entries);
			return 0;
		}
		nr_waiting = parse_old_task_format(buffer, cpu_info->starving, nr_entries);
		break;
	default:
		die("invalid value for config_task_format: %d\n", config_task_format);
	}
	return nr_waiting;
}

static int sched_debug_parse(struct cpu_info *cpu_info, char *buffer, size_t buffer_size)
{

	struct task_info *old_tasks = cpu_info->starving;
	int nr_old_tasks = cpu_info->nr_waiting_tasks;
	long nr_running = 0, nr_rt_running = 0;
	int cpu = cpu_info->id;
	char *cpu_buffer;
	int retval = 0;

	cpu_buffer = alloc_and_fill_cpu_buffer(cpu, buffer, buffer_size);
	/*
	 * It is not necessarily a problem, the CPU might be offline. Cleanup
	 * and leave.
	 */
	if (!cpu_buffer) {
		if (old_tasks)
			free(old_tasks);
		cpu_info->nr_waiting_tasks = 0;
		cpu_info->nr_running = 0;
		cpu_info->nr_rt_running = 0;
		cpu_info->starving = 0;
		goto out;
	}

	/*
	 * The NEW_TASK_FORMAT produces useful output values for nr_running and
	 * rt_nr_running, so in this case use them. For the old format just leave
	 * them initialized to zero.
	 */
	if (config_task_format == NEW_TASK_FORMAT) {
		nr_running = get_variable_long_value(cpu_buffer, ".nr_running");
		nr_rt_running = get_variable_long_value(cpu_buffer, ".rt_nr_running");
		if ((nr_running == -1) || (nr_rt_running == -1)) {
			retval = -EINVAL;
			goto out_free;
		}
	}

	cpu_info->nr_running = nr_running;
	cpu_info->nr_rt_running = nr_rt_running;

	cpu_info->nr_waiting_tasks = fill_waiting_task(cpu_buffer, cpu_info);
	if (old_tasks) {
		merge_taks_info(cpu_info->id, old_tasks, nr_old_tasks, cpu_info->starving, cpu_info->nr_waiting_tasks);
		free(old_tasks);
	}

out_free:
	free(cpu_buffer);
out:
	return retval;
}

static int sched_debug_has_starving_task(struct cpu_info *cpu)
{
	if (config_task_format == NEW_TASK_FORMAT)
		return !!cpu->nr_rt_running;
	else
		return cpu->nr_waiting_tasks;
}

static int sched_debug_init(void)
{
	find_sched_debug_path();
	config_task_format = detect_task_format();
	return 0;
}

static void sched_debug_destroy(void)
{
	return;
}

struct stalld_backend sched_debug_backend = {
	.init			= sched_debug_init,
	.get			= sched_debug_get,
	.parse			= sched_debug_parse,
	.has_starving_task	= sched_debug_has_starving_task,
	.destroy		= sched_debug_destroy,
};
