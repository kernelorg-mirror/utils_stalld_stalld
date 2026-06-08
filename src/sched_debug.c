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

static struct task_format_offsets
    config_task_format_offsets  = { 0, 0, 0, 0 };

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

	if (position > 0)
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
	/*
	 * 'cpu#9999, %u.%03u MHz\n' CONFIG_X86
	 * 'cpu#9999\n' other arch
	 */
	char cpu_header[10];
#if defined(__i386__) || defined(__x86_64__)
	sprintf(cpu_header, "cpu#%d,", cpu);
#else
	sprintf(cpu_header, "cpu#%d\n", cpu);
#endif

	return strstr(buffer, cpu_header);
}

static char *get_next_cpu_info_start(char *start)
{
	const char *next_cpu = "cpu#";

        /*
         * Skip the current CPU definition.
         * We want to move our "cursor" past the current "cpu#" definition.
         * This number is arbitrary. It is purely to assist strstr().
         */
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

	/* add one for the null terminator */
	size = next_cpu_start - cpu_start + 1;

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

/*
 * Note, for our purposes newline is *not* a space
 * and we want to stop when we hit it
 */
static inline char *skipspaces(char *str)
{
	while (*str && isspace(*str) && (*str != '\n'))
		str++;
	return str;
}

static inline char *nextline(char *str)
{
	char *ptr = strchr(str, '\n');
	return ptr ? ptr+1 : NULL;
}

/*
 * skip a specified number of words on a task line
 */

static inline char *skip2word(char *ptr, int nwords)
{
	int i;
	ptr = skipspaces(ptr);
	for (i=1; i < nwords; i++) {
		ptr = skipchars(ptr);
		ptr = skipspaces(ptr);
	}
	return ptr;
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
	int i, count=0;

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

	/* find the delimiter for task information */
	ptr = strstr(buffer, TASK_MARKER);
	if (ptr == NULL) {
		die("unable to find 'runnable tasks' in buffer, invalid input\n");
		exit(-1);
	}

	/* move to the column header line */
	ptr = nextline(ptr);
	i = 0;

	/*
	 * Determine the TASK_FORMAT from the first "word" in the header
	 * line.
	 */
	ptr = skipspaces(ptr);
	if (strncmp(ptr, "S", strlen("S")) == 0) {
		log_msg("detect_task_format: NEW_TASK_FORMAT detected\n");
		retval = NEW_TASK_FORMAT;
		/* move the word offset by one */
		i++;
	}
	else {
		log_msg("detect_task_format: OLD_TASK_FORMAT detected\n");
		retval = OLD_TASK_FORMAT;
	}

	/*
	 * Look for our header keywords and store their offset
	 * we'll use the offsets when we actually parse the task
	 * line data
	 */
	while (*ptr != '\n') {
		ptr = skipspaces(ptr);
		if (strncmp(ptr, "task", strlen("task")) == 0) {
			config_task_format_offsets.task = i;
			count++;
			log_msg("detect_task_format: found 'task' at word %d\n", i);
		}
		else if (strncmp(ptr, "PID", strlen("PID")) == 0) {
			config_task_format_offsets.pid = i;
			count++;
			log_msg("detect_task_format: found 'PID' at word %d\n", i);
		}
		else if (strncmp(ptr, "switches", strlen("switches")) == 0) {
			config_task_format_offsets.switches = i;
			count++;
			log_msg("detect_task_format: found 'switches' at word %d\n", i);
		}
		else if (strncmp(ptr, "prio", strlen("prio")) == 0) {
			config_task_format_offsets.prio = i;
			count++;
			log_msg("detect_task_format: found 'prio' at word %d\n", i);
		}
		ptr = skipchars(ptr);
		i++;
	}

	if (count != 4)
		die("detect_task_format: did not detect all task line fields we need\n");

	free(buffer);
	return retval;
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

static int parse_task_lines(char *buffer, struct task_info *task_info, int nr_entries)
{
	int pid, ctxsw, prio, comm_size;
	char *ptr=NULL, *line = buffer, *end;
	char *buffer_end = buffer + strlen(buffer);
	struct task_info *task;
	char comm[COMM_SIZE];
	int tasks = 0;

	/*
	 * If we have less than two tasks on the CPU there is no
	 * possibility of a stall.
 	 */
	if (nr_entries < 2)
		return 0;


	/* search for the task marker header */
	ptr = strstr(buffer, TASK_MARKER);
	if (ptr == NULL)
		die ("no runnable task section found!\n");

	line = ptr;

	/* skip "runnable tasks:" */
 	line = nextline(line);

	/* skip header lines */
	line = nextline(line);

	/* skip divider line */
	line = nextline(line);
	/* at this point, line should point to the start of a task line */

	/* now loop over the task info
	 * note that we always discount the task that's on the cpu, so the
	 * number of waiting tasks will always be at least one less than
	 * nr_entries.
	 */
	while ((line < buffer_end) && tasks < (nr_entries-1)) {
		task = &task_info[tasks];

		/* move ptr to the first word of the line */
		ptr = skipspaces(line);

		/*
		 * In 3.X kernels, only the singular RUNNING task receives
		 * a "running state" label. Therefore, only care about
		 * tasks that are not R (runnable on a CPU).
		 */
		if ((config_task_format == OLD_TASK_FORMAT) &&
			(*ptr == 'R')) {
			/* Go to the end of the line and ignore this task. */
			line = nextline(line);
			continue;
		}

		/*
		 * in newer kernels (>=4.x) every task info line has a state
		 * but the actual running tasks has a '>R' to denote it.
		 * since we don't care about the currently running tasks
		 * skip it.
		 * Also, we don't care about any states other than 'R' (runnable)
		 * and 'X' (dying)
		 */
		if (config_task_format == NEW_TASK_FORMAT) {
			if (*ptr == '>' || (*ptr != 'R' && *ptr != 'X')) {
				line = nextline(line);
				continue;
			}
		}

		/*
		 * At this point we have a task line to record
		 */
		
		/* get the task field */
		ptr = skip2word(line, config_task_format_offsets.task);

		/* Find the end of the task field */
		end = skipchars(ptr);
		comm_size = end - ptr;

		/* make sure we don't overflow the comm array */
		if (comm_size >= COMM_SIZE) {
			warn("comm_size is too large: %d\n", comm_size);
			comm_size = COMM_SIZE - 1;
		}
		strncpy(comm, ptr, comm_size);
		comm[comm_size] = '\0';

		/* get the PID field */
		ptr = skip2word(line, config_task_format_offsets.pid);
		pid = strtol(ptr, NULL, 10);

		/* get the context switches field */
		ptr = skip2word(line, config_task_format_offsets.switches);
		ctxsw = strtol(ptr, NULL, 10);

		/* get the prio field */
		ptr = skip2word(line, config_task_format_offsets.prio);
		prio = strtol(ptr, NULL, 10);

		/*log_msg("DEBUG: task%d comm:%s pid:%d ctxsw:%d prio:%d\n", tasks, comm, pid, ctxsw, prio);*/

                /*
                 * In older formats, we must check to
                 * see if the process is runnable prior to storing header
                 * fields and incrementing task processing
                 */
                if ((config_task_format == NEW_TASK_FORMAT) || (is_runnable(pid))) {
			strncpy(task->comm, comm, comm_size);
			task->comm[comm_size] = 0;
			task->pid = pid;
			task->tgid = get_tgid(task->pid);
			task->ctxsw = ctxsw;
			task->prio = prio;
			task->since = get_monotonic_time();
			/* increment the count of tasks processed */
			tasks++;
		}

		/* move our line pointer to the next availble line */
		line = nextline(line);
	}
	return tasks;
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

static int fill_waiting_task(char *buffer, struct cpu_info *cpu_info)
{
	int nr_waiting = -1;
	int nr_entries;

	if (cpu_info == NULL) {
		warn("NULL cpu_info pointer!\n");
		cpu_info->starving = NULL;
		return 0;
	}

	if (config_task_format == OLD_TASK_FORMAT)
		nr_entries = count_task_lines(buffer);
	else
		nr_entries = cpu_info->nr_running;

	if (nr_entries <= 0) {
		cpu_info->starving = NULL;
		return 0;
	}

	cpu_info->starving = malloc(sizeof(struct task_info) * nr_entries);
	if (cpu_info->starving == NULL) {
		warn("failed to malloc %d task_info structs", nr_entries);
		return 0;
	}

	nr_waiting = parse_task_lines(buffer, cpu_info->starving, nr_entries);

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
	 * NEW_TASK_FORMAT and produces useful output values for nr_running and
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
		merge_tasks_info(cpu_info->id, old_tasks, nr_old_tasks, cpu_info->starving, cpu_info->nr_waiting_tasks);
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
	if ((config_task_format = detect_task_format()) == TASK_FORMAT_UNKNOWN)
		die("Can't handle task format!\n");
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
