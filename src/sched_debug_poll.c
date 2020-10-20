/*
 * SPDX-License-Identifier: GPL-2.0
 *
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
#include <linux/sched.h>
#include <sys/sysinfo.h>

#include "stalld.h"

static int stalld_running;

static char *sched_buffer;
static int sched_size;
static char **cpu_start_ptrs;
static int ncpus;

static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

static int alloc_sched_buffer(char **buffer, int *size)
{
	char tmp[4096];
	char *p;
	int tsize = 0;
	int fd;
	int status;

	/*
	 * setup an array of pointers that will point to the
	 * start of cpu-specific information in the sched_debug
	 * buffer
	 */
	ncpus = get_nprocs();
	cpu_start_ptrs = calloc(ncpus, sizeof(char *));

	/*
	 * go figure out how big the current sched_debug information
	 * is then double it
	 */
	fd = open("/proc/sched_debug", O_RDONLY);
	if (fd < 0)
		die("alloc_sched_buffer: open of /proc/sched_debug failed");
	while ((status = read(fd, tmp, sizeof(tmp))) > 0)
		tsize += status;
	close(fd);

	/* double it for paranoia sake */
	tsize *= 2;
	printf("current /proc/sched_debug size is %d\n", tsize);
	if ((p = malloc(tsize)) == NULL)
		die ("unable to malloc %d bytes for sched_debug buffer\n");
	*buffer = p;
	*size = tsize;
	return 0;
}

static void free_sched_buffer(char **buffer, int *size)
{
	if (*buffer) {
		free (*buffer);
		*buffer = NULL;
		*size = 0;
	}
}

/*
 * find the start of each cpu block in the 
 * sched_debug buffer (starts with cpu#<n>)
 * and save those positions off indexed by
 * the number of the cpu
 */
static void find_cpu_info_blocks(int size)
{
	char *ptr, *prev, *end, *tmp;
        int cpu;

	ptr = prev = sched_buffer;
	end = sched_buffer + size;
	ptr = strstr(ptr, "cpu#");
	while (ptr < end) {
		*(ptr - 1) = '\0';
		tmp = ptr + strlen("cpu#");
		cpu = strtol(tmp, NULL, 10);
		if (cpu > ncpus)
			die("mixup with cpu block number and online cpus (%d > %d)\n",
			    cpu, ncpus);
		cpu_start_ptrs[cpu] = ptr;
		ptr = strstr(tmp, "cpu#");
	}
}

static int read_sched(char *buffer, int size)
{
	int position = 0;
	int retval;
	int fd;

	fd = open("/proc/sched_debug", O_RDONLY);

	if (!fd)
		goto out_error;

	do {
		retval = read(fd, &buffer[position], size - position);
		if (read < 0)
			goto out_close_fd;

		position += retval;

	} while (retval > 0 && position < size);

	if (position < size)
		buffer[position] = '\0';

	close(fd);

	return position;

out_close_fd:
	close(fd);

out_error:
	return 0;
}

static void *sched_poll_thread(void *arg)
{
	int bufsiz;
	
	if (alloc_sched_buffer(&sched_buffer, &sched_size))
		die("sched_poll_thread: failed to allocate buffer");

	while (stalld_running) {
		/*
		 * refill the cpu information buffer
		 */
		if (pthread_mutex_lock(&buffer_mutex))
			die("sched_poll_thread: failure from pthread_mutex_lock");
		bufsiz = read_sched(sched_buffer, sched_size);
		find_cpu_info_blocks(bufsiz);
		if (pthread_mutex_unlock(&buffer_mutex))
			die("sched_poll_thread: failure from pthread_mutex_unlock");
		sleep(1);
	}
	free_sched_buffer(&sched_buffer, &sched_size);
	return NULL;
}

pthread_t
start_poll_thread(void)
{
	pthread_t tid;

	if (alloc_sched_buffer(&sched_buffer, &sched_size))
		die("start_poll_thread: failed to alloc sched buffer");

	if (pthread_create(&tid, NULL, sched_poll_thread, NULL))
		die("start_poll_thread: pthread_create failed");
	stalld_running = 1;
	return tid;
}


void shutdown_poll_thread(void)
{
	stalld_running = 0;
}

/*
 * Example:
 * ' S           task   PID         tree-key  switches  prio     wait-time             sum-exec        sum-sleep'
 * '-----------------------------------------------------------------------------------------------------------'
 * ' I         rcu_gp     3        13.973264         2   100         0.000000         0.004469         0.000000 0 0 /
 */
static int fill_waiting_tasks(char *buffer, struct task_info *task_info, int nr_entries)
{
	struct task_info *task;
	char *start = buffer;
	int tasks = 0;
	int comm_size;
	char *end;

	while (tasks < nr_entries) {
		task = &task_info[tasks];

		/*
		 * only care about tasks in the Runnable state
		 * Note: the actual scheduled task will show up as
		 * "\n>R" so we will skip it.
		 *
		 */
		start = strstr(start, "\n R");

		/*
		 * if no match then there are no more Runnable tasks
		 */
		if (!start)
			break;

		/*
		 * Skip '\n R'
		 */
		start = &start[3];

		/*
		 * skip the spaces.
		 */
		while(start[0] == ' ')
			start++;

		end = start;

		while(end[0] != ' ')
			end++;

		comm_size = end - start;

		if (comm_size > sizeof(task->comm) - 1) {
			warn("comm_size is too large: %d\n", comm_size);
			comm_size = sizeof(task->comm) - 1;
		}

		strncpy(task->comm, start, comm_size);

		task->comm[comm_size] = 0;

		/*
		 * go to the end of the task comm
		 */
		start=end;

		task->pid = strtol(start, &end, 10);

		/*
		 * go to the end of the pid
		 */
		start=end;

		/*
		 * skip the tree-key
		 */
		while(start[0] == ' ')
			start++;

		while(start[0] != ' ')
			start++;

		task->ctxsw = strtol(start, &end, 10);

		start = end;

		task->prio = strtol(start, &end, 10);

		task->since = time(NULL);

		/*
		 * go to the end and try to find the next occurence.
		 */
		start = end;

		tasks++;
	}

	return tasks;
}

void free_cpu_info(struct cpu_info *c)
{
	if (c == NULL)
		return;
	if (c->starving)
		free (c->starving);
	free(c);
}

/*
 * called to get current data on a cpu
 * returns a struct cpu_info pointer which must be freed
 * using free_cpu_info()
 */
struct cpu_info *get_cpu_info(int cpu)
{
	struct cpu_info *c;
	char *ptr;
	int status;
	
	if ((status = pthread_mutex_lock(&buffer_mutex))) {
		errno = status;
		die("get_cpu_info: failed to lock buffer_mutex\n");
	}
	c = malloc(sizeof(*c));
	if (c == NULL)
		die("get_cpu_info: malloc of cpu_info struct failed");
	memset(c, 0, sizeof(*c));
	
	if (cpu >= ncpus)
		die("get_cpu_info: invalid cpu number %d", cpu);
	
	/*
	 * get a pointer to the cpu info block of text for the
	 * input cpu value
	 */
	ptr = cpu_start_ptrs[cpu];
	
	/*
	 * find out how many tasks total tasks are runnable
	 */
	c->nr_running = get_variable_long_value(ptr, ".nr_running");

	if (c->nr_running == -1) {
		warn("get_cpu_info: invalid value for nr_running on cpu %d: %d\n",
		     cpu, c->nr_running);
		free(c);
		return NULL;
	}
			
	/*
	 * find out how many RT tasks are runnable
	 */
	c->nr_rt_running = get_variable_long_value(ptr, ".rt_nr_running");

	if (c->nr_rt_running == -1) {
		warn("get_cpu_info: invalid value for nr_rt_running on cpu %d: %d\n",
		     cpu, c->nr_rt_running);
		free(c);
		return NULL;
	}

	/*
	 * we can only have a starving thread if there is more than one
	 * runnable thread
	 */
	if (c->nr_running > 1) {
		/* note one is the running task, so waiting == nr_running - 1 */
		c->starving = malloc(sizeof(struct task_info) * (c->nr_running - 1));
		c->nr_waiting_tasks = fill_waiting_tasks(ptr, c->starving, c->nr_running);
	}

	if ((status = pthread_mutex_unlock(&buffer_mutex))) {
		errno = status;
		die("get_cpu_info: failed to unlock buffer_mutex\n");
	}
	return c;
}
