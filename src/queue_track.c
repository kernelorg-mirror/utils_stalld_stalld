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

static struct stalld_bpf *stalld_obj;

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

static void print_queued_tasks(int cpu)
{
	struct task_map_key key, next_key;
	struct queued_task task;
	int fd;

	if (!DEBUG_STALLD)
		return;

	if (!config_verbose)
		return;

	fd = bpf_map__fd(stalld_obj->maps.stalld_task_map);

	memset(&key, 0, sizeof(key));
	memset(&next_key, 0, sizeof(next_key));

	while (bpf_map_get_next_key(fd, &key, &next_key) == 0) {
		key = next_key;

		if (key.cpu != cpu)
			continue;

		if (bpf_map_lookup_elem(fd, &key, &task) != 0)
			continue;

		log_msg("cpu: %-3d pid: %-8ld ctx: %-8lu\n", cpu,
			task.pid, task.ctxswc);
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

	print_queued_tasks(cpu);

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

	return sizeof(struct stalld_cpu_data);
}

static int queue_track_parse(struct cpu_info *cpu_info, char *buffer, size_t buffer_size)
{
	struct stalld_cpu_data *cpu_data = (struct stalld_cpu_data *) buffer;
	struct task_info *old_tasks = cpu_info->starving;
	int nr_old_tasks = cpu_info->nr_waiting_tasks;
	long nr_running = 0, nr_rt_running = 0;
	struct task_info *tasks, *task;
	struct task_map_key key, next_key;
	struct queued_task qtask;
	int retval = 0;
	int fd;

	tasks = allocate_memory(MAX_QUEUE_TASK, sizeof(struct task_info));

	fd = bpf_map__fd(stalld_obj->maps.stalld_task_map);

	memset(&key, 0, sizeof(key));
	memset(&next_key, 0, sizeof(next_key));

	while (bpf_map_get_next_key(fd, &key, &next_key) == 0) {
		key = next_key;

		if (key.cpu != cpu_info->id)
			continue;

		if (bpf_map_lookup_elem(fd, &key, &qtask) != 0)
			continue;

		if (qtask.is_rt)
			nr_rt_running++;

		if (qtask.pid == cpu_data->current)
			continue;

		if (nr_running >= MAX_QUEUE_TASK)
			break;

		task = &tasks[nr_running];

		retval = fill_process_comm(qtask.tgid, qtask.pid, task->comm, COMM_SIZE);
		if (retval)
			continue;

		task->pid = qtask.pid;
		task->tgid = qtask.tgid;

		task->ctxsw = qtask.ctxswc;

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
		merge_tasks_info(cpu_info->id, old_tasks, nr_old_tasks, cpu_info->starving, cpu_info->nr_waiting_tasks);
                free(old_tasks);
        }

	return 0;
}

static int queue_track_has_starving_task(struct cpu_info *cpu)
{
	return !!cpu->nr_rt_running;
}

/**
 * initialize_maps - Initialize BPF per-CPU data maps
 *
 * Returns: 0 on success, -1 on error
 */
static int initialize_maps(void)
{
	struct stalld_cpu_data stalld_data;

	for (int i = 0; i < config_nr_cpus; i++) {
		/* Init data */
		if (get_cpu_data(&stalld_data, i))
			return -1;

		if (config_monitor_all_cpus || config_monitored_cpus[i])
			stalld_data.monitoring = 1;

		set_cpu_data(&stalld_data, i);
	}

	config_buffer_size = sizeof(struct stalld_cpu_data);
	return 0;
}

/**
 * run_task_iterator - Execute the BPF task iterator
 *
 * Returns: 0 on success, negative value on error
 */
static int run_task_iterator(void)
{
	struct bpf_link *iter_link;
	char buf[64];
	int iter_fd, len;

	if (!stalld_obj) {
		warn("BPF object not loaded\n");
		return -EINVAL;
	}

	/* Create the iterator link */
	iter_link = bpf_program__attach_iter(stalld_obj->progs.iter_task, NULL);
	if (!iter_link) {
		warn("Failed to attach task iterator\n");
		return -EINVAL;
	}

	/* Get file descriptor for the iterator */
	iter_fd = bpf_iter_create(bpf_link__fd(iter_link));
	if (iter_fd < 0) {
		warn("Failed to create iterator fd: %d\n", iter_fd);
		bpf_link__destroy(iter_link);
		return iter_fd;
	}

	while ((len = read(iter_fd, buf, sizeof(buf))) > 0) {
		/* Iterator output is processed by the BPF program itself */
	}

	if (len < 0)
		warn("Iterator read error: %d\n", len);

	close(iter_fd);
	bpf_link__destroy(iter_link);

	log_verbose("Task iterator completed\n");
	return len < 0 ? len : 0;
}

/**
 * load_ebpf_context - sets up ebpf context
 */
static int load_ebpf_context(void)
{
	int err;

	libbpf_set_print(libbpf_print_fn);

	err = bump_memlock_rlimit();
	if (err) {
		warn("failed to increase rlimit: %d\n", err);
		return -1;
	}

	stalld_obj = stalld_bpf__open();
	if (!stalld_obj) {
		warn("failed to open and/or load BPF object\n");
		return -1;
	}

	err = bpf_map__set_max_entries(stalld_obj->maps.stalld_per_cpu_data, config_nr_cpus);
	if (err) {
		warn("failed to resize BPF map: %d\n", err);
		goto cleanup;
	} else {
		log_msg("adjusted stalld map to %d cpus\n", config_nr_cpus);
	}

	err = bpf_map__set_max_entries(stalld_obj->maps.stalld_task_map,
				       MAX_QUEUE_TASK * config_nr_cpus);
	if (err) {
		warn("failed to resize BPF task map: %d\n", err);
		goto cleanup;
	}

	err = stalld_bpf__load(stalld_obj);
	if (err) {
		warn("failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	return 0;

cleanup:
	stalld_bpf__destroy(stalld_obj);
	return -1;
}

static int queue_track_init(void)
{
	if (load_ebpf_context())
		return -1;

	if (initialize_maps())
		goto destroy;

	if (run_task_iterator())
		goto destroy;

	if (stalld_bpf__attach(stalld_obj)) {
		warn("failed to attach BPF programs\n");
		goto destroy;
	}

	return 0;

destroy:
	stalld_bpf__destroy(stalld_obj);
	return -1;
}

static void queue_track_destroy(void)
{
	struct stalld_cpu_data stalld_data;
	int retval, i;

	for (i = 0; i < config_nr_cpus; i++) {
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
