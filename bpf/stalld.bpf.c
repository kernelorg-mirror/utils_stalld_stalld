/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2022 Red Hat Inc, Daniel Bristot de Oliveira <bristot@kernel.org>
 */

#include "vmlinux.h"
#include <string.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "../src/queue_track.h"

#ifndef TASK_RUNNING
#define TASK_RUNNING 0
#endif

/*
 * bpf_helpers.h might not be updated to have barrier, yet.
 */
#ifndef barrier
#define barrier() asm volatile("" ::: "memory")
#endif
/*
 * It is not a per-cpu data because a remote CPU can enqueue a
 * task.
 */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	/* it will be resized */
	__uint(max_entries, 1024);
	__type(key, u32);
	__type(value, struct stalld_cpu_data);
} stalld_per_cpu_data SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	/* resized at load time to MAX_QUEUE_TASK * nr_cpus */
	__uint(max_entries, MAX_QUEUE_TASK);
	__type(key, struct task_map_key);
	__type(value, struct queued_task);
} stalld_task_map SEC(".maps");

#if DEBUG_STALLD
#define log(msg, ...) bpf_printk("%s: " msg, __func__, ##__VA_ARGS__)
#else
#define log(msg, ...) do {} while(0)
#endif

#define log_task_prefix(prefix, p)		\
	log(prefix "%s(%d) pid=%d cpu=%d",	\
	    p->comm,				\
	    p->tgid,				\
	    p->pid,				\
	    task_cpu(p))			\

#define log_task(p) log_task_prefix("", p)
#define log_task_error(p) log_task_prefix("error: ", p)

/*
 * BPF CO-RE compatibility: In older kernels (e.g., RHEL 8.x with 4.18),
 * thread_info lacks the cpu field. We define it here to enable
 * bpf_core_field_exists() checks, allowing runtime detection of whether
 * this field is available on the target kernel.
 */
struct thread_info___legacy {
	int cpu;
};

/*
 * BPF CO-RE "weak" or "candidate" definition.
 *
 * This struct provides a definition for fields that may not exist in the
 * kernel headers used at compile time (e.g., the 'cpu' field was removed
 * from task_struct in modern kernels).
 *
 * Its sole purpose is to satisfy the compiler, allowing the BPF program to
 * build successfully. At runtime, the BPF loader uses the target kernel's BTF
 * (BPF Type Format) to perform a CO-RE (Compile Once - Run Everywhere)
 * relocation. The bpf_core_field_exists() check will correctly determine if
 * the field is actually present on the target system, making the program
 * portable across different kernel versions.
 */
struct task_struct___legacy {
	int cpu;
	unsigned int state;
};

/**
 * task_is_rt - Check if a task is a real-time task.
 * @p: A pointer to the kernel's `task_struct` for the task.
 *
 * Return: `true` if the task has a real-time priority (0-99),
 *         `false` otherwise.
 */
static inline bool task_is_rt(const struct task_struct *p)
{
	return p->prio >= 0 && p->prio <= 99;
}

/**
 * task_cpu - Get the CPU number that a task is currently running on.
 * @p: A pointer to the kernel's `task_struct` for the task.
 *
 * Return: The integer ID of the CPU the task is running on.
 */
static inline int task_cpu(const struct task_struct *p)
{
	const struct task_struct___legacy *lp = (const void *) p;
	const struct thread_info___legacy *lt = (const void *) &p->thread_info;

	return bpf_core_field_exists(lp->cpu)
		? BPF_CORE_READ(lp, cpu)
		: BPF_CORE_READ(lt, cpu);
}

/**
 * compute_ctxswc - Compute the total context switch count for a task.
 * @p: A pointer to the `task_struct` (process descriptor) of the task.
 *
 * Return: The total context switch count (nvcsw + nivcsw) for the given task.
 */
static inline long compute_ctxswc(const struct task_struct *p)
{
	return p->nvcsw + p->nivcsw;
}

static inline unsigned int task_running(const struct task_struct *p)
{
	const struct task_struct___legacy *lp = (const void *) p;

	const unsigned int state = bpf_core_field_exists(p->__state)
					? BPF_CORE_READ(p, __state)
					: BPF_CORE_READ(lp, state);

	return state == TASK_RUNNING;
}

/**
 * Each CPU has its own set of statistics stored on a per-cpu
 * array, this function returns the variable of the current
 * CPU.
 */
static struct stalld_cpu_data *get_cpu_data(int cpu)
{
	struct stalld_cpu_data *stalld_data;
	u32 key = cpu;

	stalld_data = bpf_map_lookup_elem(&stalld_per_cpu_data, &key);

	if (stalld_data && stalld_data->monitoring)
		return stalld_data;

	return NULL;
}

static int enqueue_task(const struct task_struct *p, int cpu)
{
	struct task_map_key key;
	struct queued_task task;

	/*
	 * pid 0 (idle/swapper) must not enter the hash map: userspace
	 * uses pid 0 as "no task" in cpu_starving_vector, so a real
	 * entry with pid 0 would be silently skipped.
	 */
	if (!p->pid)
		return 0;

	key.cpu = cpu;
	key.pid = p->pid;

	task.pid = p->pid;
	task.tgid = p->tgid;
	task.is_rt = task_is_rt(p);
	task.prio = p->prio;
	task.ctxswc = compute_ctxswc(p);

	if (bpf_map_update_elem(&stalld_task_map, &key, &task, BPF_ANY) < 0) {
		log_task_error(p);
		return -1;
	}

	log_task(p);
	return 0;
}

/**
 * dequeue_task - Removes a task from a CPU's queue.
 * @p:   Pointer to the task_struct of the task to remove.
 * @cpu: The CPU number to dequeue from.
 *
 * Return: 1 if the task was found and removed, 0 otherwise.
 */
static int dequeue_task(const struct task_struct *p, int cpu)
{
	struct task_map_key key;

	key.cpu = cpu;
	key.pid = p->pid;

	if (bpf_map_delete_elem(&stalld_task_map, &key) == 0) {
		log_task(p);
		return 1;
	}

	log_task_error(p);
	return 0;
}

/*
 * update_or_add_task - Manages a task's lifecycle within the hash map.
 *
 * Three scenarios:
 * 1. Update: task exists and is TASK_RUNNING -> refresh fields in-place.
 * 2. Remove: task exists but not TASK_RUNNING -> delete from map.
 * 3. Add:    task not found and is TASK_RUNNING -> insert into map.
 */
static void update_or_add_task(const struct task_struct *p, int cpu)
{
	struct task_map_key key;
	struct queued_task *task_entry;

	key.cpu = cpu;
	key.pid = p->pid;

	task_entry = bpf_map_lookup_elem(&stalld_task_map, &key);
	if (task_entry) {
		if (task_running(p)) {
			task_entry->ctxswc = compute_ctxswc(p);
			task_entry->prio = p->prio;
			task_entry->is_rt = task_is_rt(p);
		} else {
			log_task_prefix("dequeue ", p);
			bpf_map_delete_elem(&stalld_task_map, &key);
		}

		return;
	}

	if (!task_running(p))
		return;

	enqueue_task(p, cpu);
}

/**
 * __sched_wakeup - Common handler for task wakeup tracepoints.
 * @ctx: A pointer to the tracepoint context.
 *
 * Return: Always returns 0.
 */
static int __sched_wakeup(u64 *ctx)
{
	const struct task_struct *p = (void *) ctx[0];
	int cpu = task_cpu(p);
	struct stalld_cpu_data *cpu_data = get_cpu_data(cpu);

	if (cpu_data)
		update_or_add_task(p, cpu);

	return 0;
}

SEC("tp_btf/sched_wakeup")
int handle__sched_wakeup(u64 *ctx)
{
	return __sched_wakeup(ctx);
}

SEC("tp_btf/sched_wakeup_new")
int handle__sched_wakeup_new(u64 *ctx)
{
	return __sched_wakeup(ctx);
}

SEC("tp_btf/sched_process_exit")
int handle__sched_process_exit(u64 *ctx)
{
	const struct task_struct *p = (void *) ctx[0];
	int cpu = task_cpu(p);
	struct stalld_cpu_data *cpu_data = get_cpu_data(cpu);

	if (cpu_data)
		dequeue_task(p, cpu);

	return 0;
}

SEC("tp_btf/sched_switch")
int handle__sched_switch(u64 *ctx)
{
	int cpu = bpf_get_smp_processor_id();
	struct stalld_cpu_data *cpu_data = get_cpu_data(cpu);
	const struct task_struct *prev = (void *) ctx[1];
	const struct task_struct *next = (void *) ctx[2];

	if (!cpu_data)
		return 0;
	cpu_data->current = next->pid;

	cpu_data->nr_rt_running = task_is_rt(next);

	update_or_add_task(next, cpu);
	update_or_add_task(prev, cpu);

	return 0;
}

SEC("tp_btf/sched_migrate_task")
int handle__sched_migrate_task(u64 *ctx)
{
	const struct task_struct *p = (void *) ctx[0];
	const int dest_cpu = ctx[1];
	const int orig_cpu = task_cpu(p);
	struct stalld_cpu_data *cpu_data;

	cpu_data = get_cpu_data(orig_cpu);

	/*
	 * Dequeue the task from its original CPU and re-enqueue it on the
	 * destination CPU. This ensures its run queue state is tracked
	 * correctly across migrations.
	 */
	if (cpu_data) {
		log("task=%s(%ld) orig=%d dest=%d",
		    p->comm, p->tgid, orig_cpu, dest_cpu);
		if (dequeue_task(p, orig_cpu)) {
			cpu_data = get_cpu_data(dest_cpu);
			if (cpu_data)
				enqueue_task(p, dest_cpu);
		}
	}

	return 0;
}

/**
 * iter_task - BPF iterator program for task enumeration
 * @ctx: Iterator context containing the current task
 */
SEC("iter/task")
int iter_task(struct bpf_iter__task *ctx)
{
	const struct task_struct *p = ctx->task;
	int cpu;

	if (!p)
		return 0;

	cpu = task_cpu(p);
	if (!get_cpu_data(cpu))
		return 0;

	log_task(p);

	if (task_running(p))
		enqueue_task(p, cpu);

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
