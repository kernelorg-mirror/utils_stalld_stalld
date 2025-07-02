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
	long ctxswc = p->nvcsw + p->nivcsw;
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

	for_each_task_entry(cpu_data, task) {
		if (task->pid == pid) {
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

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
