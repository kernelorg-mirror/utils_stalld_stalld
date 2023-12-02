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
	long ctxswc = p->nvcsw + p->nivcsw;
	long tgid = p->tgid;
	long prio = p->prio;
	long pid = p->pid;
	int i;

	if (!cpu_data)
		return 0;

	if (!cpu_data->monitoring)
		return 0;

	for (i = 0; i < MAX_QUEUE_TASK; i++) {
#ifdef BPF_DEBUG
		bpf_printk("slot %d: %d %d", i, cpu_data->tasks[i].pid, cpu_data->tasks[i].ctxswc);
#endif
		if (cpu_data->tasks[i].pid == 0 || cpu_data->tasks[i].pid == pid) {
			cpu_data->tasks[i].ctxswc = ctxswc;
			cpu_data->tasks[i].prio = prio;
			cpu_data->tasks[i].is_rt = rt;
			cpu_data->tasks[i].tgid = tgid;

			/*
			 * User reads pid to know that there is no data here.
			 * Update it last.
			 */
			barrier();
			cpu_data->tasks[i].pid = pid;
#ifdef BPF_DEBUG
			bpf_printk("queue %s %d %d", rt ? "rt" : "fair", pid, ctxswc);
#endif

			return 0;
		}
	}

#ifdef BPF_DEBUG
	bpf_printk("error: queue %s %d %d", rt ? "rt" : "fair", pid, ctxswc);
#endif


	return 0;
}

static int dequeue_task(struct task_struct *p, struct rq *rq, int rt)
{
	struct stalld_cpu_data *cpu_data = get_cpu_data(rq->cpu);
	long pid = p->pid;
	int i;

	if (!cpu_data)
		return 0;

	if (!cpu_data->monitoring)
		return 0;

	for (i = 0; i < MAX_QUEUE_TASK; i++) {
		if (cpu_data->tasks[i].pid == pid) {

			cpu_data->tasks[i].pid = 0;
			/*
			 * User reads pid to know that there is no data here.
			 * Update it first.
			 */
			barrier();

			cpu_data->tasks[i].prio = 0;
			cpu_data->tasks[i].ctxswc = 0;
#ifdef BPF_DEBUG
			bpf_printk("dequeue %s %d", rt ? "rt" : "fair", pid);
#endif
			return 0;
		}
	}

#ifdef BPF_DEBUG
	bpf_printk("error: dequeue %s %d", rt ? "rt" : "fair", pid);
#endif
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
