/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2022 Red Hat Inc, Daniel Bristot de Oliveira <bristot@kernel.org>
 */

#include "vmlinux.h"
#include <string.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
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

#if DEBUG_STALLD
#define log(msg, ...) bpf_printk(msg, ##__VA_ARGS__)
#else
#define log(msg, ...) do {} while(0)
#endif

/**
 * compute_ctxswc - Compute the total context switch count for a task.
 * @p: A pointer to the `task_struct` (process descriptor) of the task.
 *
 * This function calculates the total number of context switches a task
 * has undergone by summing its voluntary and involuntary context switch
 * counts.
 *
 * The `nvcsw` (number of voluntary context switches) increments when a task
 * explicitly yields the CPU (e.g., waiting for I/O, sleeping, or blocking
 * on a lock).
 *
 * The `nivcsw` (number of involuntary context switches) increments when a task
 * is preempted by the scheduler (e.g., its timeslice expires, or a higher
 * priority task becomes runnable).
 *
 * The sum of these two counters provides a comprehensive measure of how many
 * times the task has been context-switched in and out of the CPU. This value
 * is crucial for tools like `stalld` to detect if a task has made progress
 * (i.e., has run at least once) since a previous observation.
 *
 * Return: The total context switch count (nvcsw + nivcsw) for the given task.
 */
static inline long compute_ctxswc(struct task_struct *p)
{
	return p->nvcsw + p->nivcsw;
}

/*
 * update_or_add_task - Manages a task's lifecycle within a per-CPU tracking queue.
 *
 * This function handles the logic for managing individual task entries within
 * stalld's BPF program. It dynamically adds, updates, or removes a task from a
 * specific CPU's tracking array based on its current state. This ensures the
 * array provides an accurate, real-time view of tasks on the run queue.
 *
 * The function's logic is organized into three primary scenarios:
 * 1.  Update: If a task is already tracked and is still in the TASK_RUNNING
 * state, its dynamic properties (context switch count, priority) are
 * refreshed.
 * 2.  Remove: If a tracked task is no longer in the TASK_RUNNING state
 * (e.g., it has gone to sleep or terminated), it is removed from the queue
 * by invalidating its entry (setting pid to 0).
 * 3.  Add: If a new, previously unseen task is encountered and is in the
 * TASK_RUNNING state, it is added to the first available empty slot in
 * the queue.
 *
 * Parameters:
 * cpu_data: A pointer to the `stalld_cpu_data` structure for the target CPU.
 * p:        A pointer to the kernel's `task_struct` for the task to be processed.
 *
 * Returns:
 * A pointer to the `queued_task` entry if the task was successfully added or
 * updated. Returns NULL in all other cases (removed, not added, or queue full).
 */
static struct queued_task *update_or_add_task(struct stalld_cpu_data *cpu_data,
					      struct task_struct *p)
{
	struct queued_task *task_entry;
	const long pid = p->pid;
	const long ctxswc = compute_ctxswc(p);

	const long prio = p->prio;
	const int is_rt = (p->prio <= 99 && p->prio >= 0);

	/* 1. Try to find the task first */
	task_entry = find_queued_task(cpu_data, pid);
	if (task_entry) {
		if (p->__state == TASK_RUNNING) {
			/* Task found: Update its dynamic fields */
			task_entry->ctxswc = ctxswc;
			task_entry->prio = prio;
			task_entry->is_rt = is_rt;
			return task_entry;
		}

		/* Task is not running. Remove it. */
		task_entry->pid = 0;
		return NULL;
	}

	/*
	 * If we reach here, the task was NOT found, so it's new.
	 * Check if the new task is in the `TASK_RUNNING` state before adding to queue.
	 */
	if (p->__state != TASK_RUNNING)
		return NULL; /* Not an error, just don't add non-running tasks */

	/*
	 * 2. Task not found and is running: find an empty slot to add it
	 * We iterate through all slots to find the first empty one.
	 */

	const long tgid = p->tgid;

	for_each_task_entry(cpu_data, task_entry)
		if (task_entry->pid == 0) { /* Found an empty slot */
			task_entry->ctxswc = ctxswc;
			task_entry->prio = prio;
			task_entry->is_rt = is_rt;
			task_entry->tgid = tgid;

			/* User reads pid to know that there is no data here.
			 * Update it last.
			 */
			barrier();
			task_entry->pid = pid;
			log("update_or_add: added task %s(%d) to empty slot", p->comm, pid);
			return task_entry;
		}
	/*
	 * If this point is reached, the queue is full and no empty slot was found.
	 * The log() is commented out because the generated code was
	 * too complex for the BPF verifier.
	 */
	//log("update_or_add: error: queue full, cannot add pid %d", pid);
	return NULL;
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

	return stalld_data;
}

static int enqueue_task(struct task_struct *p, struct rq *rq, int rt)
{
	struct stalld_cpu_data *cpu_data = get_cpu_data(rq->cpu);
	struct queued_task *task;
	long ctxswc = compute_ctxswc(p);
	long tgid = p->tgid;
	long prio = p->prio;
	long pid = p->pid;
	int slot = 0;

	if (!cpu_data)
		return 0;

	if (!cpu_data->monitoring)
		return 0;

	for_each_task_entry(cpu_data, task) {
		log("slot %d: %d %d", slot, task->pid, task->ctxswc);
		++slot;

		if (task->pid == 0 || task->pid == pid) {
			task->ctxswc = ctxswc;
			task->prio = prio;
			task->is_rt = rt;
			task->tgid = tgid;

			/*
			 * User reads pid to know that there is no data here.
			 * Update it last.
			 */
			barrier();
			task->pid = pid;
			log("queue %s %d %d", rt ? "rt" : "fair", pid, ctxswc);
			return 0;
		}
	}

	log("error: queue %s %d %d", rt ? "rt" : "fair", pid, ctxswc);


	return 0;
}

static int dequeue_task(struct task_struct *p, struct rq *rq, int rt)
{
	struct stalld_cpu_data *cpu_data = get_cpu_data(rq->cpu);
	struct queued_task *task;
	long pid = p->pid;

	if (!cpu_data)
		return 0;

	if (!cpu_data->monitoring)
		return 0;

	task = find_queued_task(cpu_data, pid);
	if (task) {
		task->pid = 0;
		/*
		 * User reads pid to know that there is no data here.
		 * Update it first.
		 */
		barrier();

		task->prio = 0;
		task->ctxswc = 0;
		log("dequeue %s %d", rt ? "rt" : "fair", pid);
		return 0;
	}

	log("error: dequeue %s %d", rt ? "rt" : "fair", pid);
	return 0;
}

/*
 * Sched deadline is fair by desing.
 */
SEC("fentry/enqueue_task_fair")
int handle__enqueue_task_fair(u64 *ctx)
{
	struct task_struct *p = (void *) ctx[1];
	struct rq *rq = (void *) ctx[0];

	return enqueue_task(p, rq, 0);
}

SEC("fentry/dequeue_task_fair")
int handle__dequeue_task_fair(u64 *ctx)
{
	struct task_struct *p = (void *) ctx[1];
	struct rq *rq = (void *) ctx[0];

	return dequeue_task(p, rq, 0);
}

SEC("fentry/enqueue_task_rt")
int handle__enqueue_task_rt(u64 *ctx)
{
	struct task_struct *p = (void *) ctx[1];
	struct rq *rq = (void *) ctx[0];

	return enqueue_task(p, rq, 1);
}

SEC("fentry/dequeue_task_rt")
int handle__dequeue_task_rt(u64 *ctx)
{
	struct task_struct *p = (void *) ctx[1];
	struct rq *rq = (void *) ctx[0];

	return dequeue_task(p, rq, 1);
}

SEC("tp_btf/sched_switch")
int handle__sched_switch(u64 *ctx)
{
	struct stalld_cpu_data *cpu_data = get_cpu_data(bpf_get_smp_processor_id());
	struct queued_task *task;
	struct task_struct *prev = (void *) ctx[1];
	struct task_struct *next = (void *) ctx[2];
	long pid = next->pid;

	if (!cpu_data)
		return 0;

	if (!cpu_data->monitoring)
		return 0;

	cpu_data->current = pid;

	if (next->prio <= 99 && next->prio >= 0)
		cpu_data->nr_rt_running = 1;

	// update the context switch count of the tasks
	update_or_add_task(cpu_data, next);
	update_or_add_task(cpu_data, prev);

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
