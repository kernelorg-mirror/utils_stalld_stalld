#if USE_BPF
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2022 Red Hat Inc, Daniel Bristot de Oliveira <bristot@kernel.org>
 */
#define _GNU_SOURCE

#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "queue_track.h"
#include "stalld.skel.h"
#include "stalld.h"

#include <pthread.h>

static volatile bool exiting = 0;

struct stalld_bpf *stalld_obj;

/*
 * Older versions of BPF does not have bpf_map__set_max_entries.
 * Use the old function.
 */
#if (LIBBPF_MAJOR_VERSION == 0 && LIBBPF_MINOR_VERSION < 8)
#define bpf_map__set_max_entries bpf_map__resize
#endif

/**
 * libbpf_print_fn - libbpf print callback
 */
static int libbpf_print_fn(enum libbpf_print_level level,
		const char *format, va_list args)
{

	if (!config_verbose)
		return 0;

	return vfprintf(stderr, format, args);
}

/**
 * bump_memlock_rlimit - increase the memlock limit
 *
 * Required for eBPF.
 */
static int bump_memlock_rlimit(void)
{
	struct rlimit rlim_new = {
		.rlim_cur	= RLIM_INFINITY,
		.rlim_max	= RLIM_INFINITY,
	};

	return setrlimit(RLIMIT_MEMLOCK, &rlim_new);
}

static void print_queued_tasks(struct stalld_cpu_data *stalld_data, int cpu)
{
	int is_current, i;

	if (!DEBUG_STALLD)
		return;

	if (!config_verbose)
		return;

	for (i = 0; i < MAX_QUEUE_TASK; i++) {
		if (!stalld_data->tasks[i].pid)
			continue;

		is_current = (stalld_data->current == stalld_data->tasks[i].pid);
		log_msg("cpu: %-3d pid: %-8d ctx: %-8lu %s\n", cpu, stalld_data->tasks[i].pid,
				stalld_data->tasks[i].ctxswc,
				is_current ? "R" : "");
	}
}

static int get_cpu_data(struct stalld_cpu_data *stalld_cpu_data, int cpu)
{
	struct bpf_map *map = stalld_obj->maps.stalld_per_cpu_data;
	int fd = bpf_map__fd(map);
	__u32 key = cpu;

	if (bpf_map_lookup_elem(fd, &key, stalld_cpu_data) != 0) {
		warn("Failed to lookup stalld_cpu_data\n");
		return ENODATA;
	}

	print_queued_tasks(stalld_cpu_data, cpu);

	return 0;
}

/**
 * Set the content of the eBPF map with the content from user-space.
 */
static int set_cpu_data(struct stalld_cpu_data *stalld_data, int cpu)
{
	struct bpf_map *map = stalld_obj->maps.stalld_per_cpu_data;
	int fd = bpf_map__fd(map);
	__u32 key = cpu;

	if (bpf_map_update_elem(fd, &key, stalld_data, 0) < 0) {
		 warn("Failed to update stalld_cpu_data\n");
		 return -EINVAL;
	}

	return 0;
}

/**
 * load_ebpf_context - sets up ebpf context
 *
 * Set up the basics for the ebpf program to run, raising
 * memlock limit, loading and attaching the eBPF code, set
 * up the perf buffer and return the ebpf object.
 */
static struct stalld_bpf *load_ebpf_context(void)
{
	struct stalld_bpf *stalld_obj;
	int err;

	libbpf_set_print(libbpf_print_fn);

	err = bump_memlock_rlimit();
	if (err) {
		warn("failed to increase rlimit: %d\n", err);
		return 0;
	}

	stalld_obj = stalld_bpf__open();
	if (!stalld_obj) {
		warn("failed to open and/or load BPF object\n");
		return 0;
	}

	err = bpf_map__set_max_entries(stalld_obj->maps.stalld_per_cpu_data, config_nr_cpus);
	if (err) {
		warn("failed to resize BPF map: %d\n", err);
		return 0;
	} else {
		log_msg("adjusted stalld map to %d cpus\n", config_nr_cpus);
	}

	err = stalld_bpf__load(stalld_obj);
	if (err) {
		warn("failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	err = stalld_bpf__attach(stalld_obj);
	if (err) {
		warn("failed to attach BPF programs\n");
		goto cleanup;
	}

	return stalld_obj;

cleanup:
	stalld_bpf__destroy(stalld_obj);
	return 0;
}

static int queue_track_get_cpu(char *buffer, int size, int cpu)
{
	int retval;
	if (size < sizeof(struct stalld_cpu_data)) {
		config_buffer_size = sizeof(struct stalld_cpu_data);
		log_msg("queue_track is larger than the buffer, increasing the buffer to %zu\n",
			config_buffer_size);
		return 1;
	}

	retval = get_cpu_data((struct stalld_cpu_data *) buffer, cpu);
	if (retval)
		return 0;

	/*
	 * Make it compatible with ->get that returned the buffer size.
	 */
	return sizeof(struct stalld_cpu_data);
}

static int queue_track_parse(struct cpu_info *cpu_info, char *buffer, size_t buffer_size)
{
	struct stalld_cpu_data *cpu_data = (struct stalld_cpu_data *) buffer;
	struct task_info *old_tasks = cpu_info->starving;
	int nr_old_tasks = cpu_info->nr_waiting_tasks;
	long nr_running = 0, nr_rt_running = 0;
	struct task_info *tasks, *task;
	struct queued_task *qtask;
	int retval = 0;
	int i;

	tasks = calloc(MAX_QUEUE_TASK, sizeof(struct task_info));
	if (tasks == NULL) {
		warn("failed to malloc %d task_info structs", MAX_QUEUE_TASK);
		goto error;
	}

	for (i = 0; i < MAX_QUEUE_TASK; i++) {
		qtask = &cpu_data->tasks[i];

		if (!qtask->pid)
			continue;

		if (qtask->is_rt)
			nr_rt_running++;

		/*
		 * Current task is not starving.
		 */
		if (qtask->pid == cpu_data->current)
			continue;

		task = &tasks[nr_running];

		/*
		 * if we cannot get the process name, the process died.
		 * RIP process, a loop of silence.
		 */
		retval = fill_process_comm(qtask->tgid, qtask->pid, task->comm, COMM_SIZE);
		if (retval)
			continue;

		task->pid = qtask->pid;
		task->tgid = qtask->tgid;

		task->ctxsw = qtask->ctxswc;

		task->since = time(NULL);

		nr_running++;

		log_msg("found task: %s:%d starving in CPU %d\n", task->comm, task->pid, cpu_info->id);
	}

	nr_running++; /* the current task */

	cpu_info->starving = tasks;
	cpu_info->nr_running = nr_running;
	cpu_info->nr_rt_running = nr_rt_running;
	if (cpu_info->nr_running >= 1)
		cpu_info->nr_waiting_tasks = nr_running - 1;

	if (old_tasks) {
		merge_taks_info(cpu_info->id, old_tasks, nr_old_tasks, cpu_info->starving, cpu_info->nr_waiting_tasks);
                free(old_tasks);
        }

	return 0;

error:
	return 1;
}

static int queue_track_has_starving_task(struct cpu_info *cpu)
{
	return !!cpu->nr_rt_running;
}

static int queue_track_init(void)
{
	struct stalld_cpu_data stalld_data;
	int retval, i;

	stalld_obj = load_ebpf_context();
	if (!stalld_obj)
		return 1;

	for (i = 0; i < config_nr_cpus; i++) {
		/* Init data */
		retval = get_cpu_data(&stalld_data, i);
		if (retval)
			goto destroy;

		if (config_monitor_all_cpus || config_monitored_cpus[i])
			stalld_data.monitoring = 1;

		set_cpu_data(&stalld_data, i);
	}

	/* it is static */
	config_buffer_size = sizeof(struct stalld_cpu_data);
	return 0;

destroy:
	stalld_bpf__destroy(stalld_obj);
	return 1;
}

static void queue_track_destroy(void)
{
	struct stalld_cpu_data stalld_data;
	int retval, i;

	for (i = 0; i < config_nr_cpus; i++) {
		/* Init data */
		retval = get_cpu_data(&stalld_data, i);
		if (retval)
			continue;

		stalld_data.monitoring = 0;
		set_cpu_data(&stalld_data, i);
	}
	stalld_bpf__destroy(stalld_obj);
}

struct stalld_backend queue_track_backend = {
	.init			= queue_track_init,
	.get_cpu		= queue_track_get_cpu,
	.parse			= queue_track_parse,
	.has_starving_task	= queue_track_has_starving_task,
	.destroy		= queue_track_destroy,
};
#endif /* USE_BPF */
