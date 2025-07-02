/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2022 Red Hat Inc, Daniel Bristot de Oliveira <bristot@kernel.org>
 */
#ifndef __QUEUE_TRACK_H
#define __QUEUE_TRACK_H

#define MAX_QUEUE_TASK 2048

struct queued_task {
	long pid;
	long tgid;
	int is_rt;
	int prio;
	long ctxswc;
};

struct stalld_cpu_data {
	int monitoring;
	int current;
	int nr_rt_running;
	struct queued_task tasks[MAX_QUEUE_TASK];
};

/*
 * Macro: for_each_task_entry
 * --------------------------
 * Iterates over *all* possible entries within the `tasks` array of a
 * `stalld_cpu_data` structure. This includes both active (valid) task entries
 * and empty (unused) slots.
 *
 * Usage:
 * for_each_task_entry(cpu_data, task_ptr) {
 * 	// Code to execute for each entry.
 * 	// task_ptr will be a pointer to a `struct queued_task`.
 * 	// Check task_ptr->pid to determine if the slot is active.
 * }
 *
 * Parameters:
 * @cpu_data: A pointer to a `struct stalld_cpu_data` instance
 * (e.g., obtained by `get_cpu_data()` from an eBPF map).
 * @task:     A pointer variable of type `struct queued_task *` that will
 * point to the current `queued_task` entry in each iteration.
 *
 * Example:
 * struct stalld_cpu_data *my_cpu_data = get_cpu_data(0);
 * struct queued_task *entry;
 * for_each_task_entry(my_cpu_data, entry) {
 * 	if (entry->pid != 0) {
 * 		// Process active task entry
 * 		printf("Active task: PID %ld, TGID %ld\n", entry->pid, entry->tgid);
 * 	} else {
 * 		// Slot is empty
 * 		printf("Empty slot\n");
 * 	}
 * }
 */
#define for_each_task_entry(cpu_data, task)	\
	task = cpu_data->tasks;			\
	for (unsigned int i = 0;		\
	     i < MAX_QUEUE_TASK;		\
	     ++i, task = cpu_data->tasks + i)

/*
 * Macro: for_each_queued_task
 * ---------------------------
 * Iterates specifically over *active* tasks currently present in the
 * `tasks` array of a `stalld_cpu_data` structure. It skips empty slots.
 * An entry is considered active if its `pid` field is non-zero.
 *
 * This macro builds upon `for_each_task_entry` and applies a filter
 * to process only valid, currently tracked tasks.
 *
 * Usage:
 * for_each_queued_task(cpu_data, task_ptr) {
 *	// Code to execute for each active (non-empty) task entry.
 *	// task_ptr will be a pointer to a `struct queued_task`.
 * }
 *
 * Parameters:
 * @cpu_data: A pointer to a `struct stalld_cpu_data` instance.
 * @task:     A pointer variable of type `struct queued_task *` that will
 * point to the current active `queued_task` entry in each
 * iteration.
 *
 * Example:
 * struct stalld_cpu_data *data_for_cpuX = get_data_from_map_for_cpu(X);
 * struct queued_task *q_task;
 * for_each_queued_task(data_for_cpuX, q_task) {
 *	// This block only executes for tasks where q_task->pid is not 0
 *	printf("Queued task on CPU %d: PID %ld (RT: %d, Prio: %d)\n",
 *		X, q_task->pid, q_task->is_rt, q_task->prio);
 * }
 */
#define for_each_queued_task(cpu_data, task)	\
	for_each_task_entry(cpu_data, task)	\
		if (task->pid)

/**
 * find_queued_task - Search for a task within a CPU's queued_task array
 * @cpu_data: A pointer to the `stalld_cpu_data` structure for a specific CPU.
 * @pid:      The Process ID (PID) of the task to search for.
 *
 * This function iterates through all possible task slots within the
 * `tasks` array of the provided `cpu_data`. It returns a pointer to the
 * `queued_task` structure if an entry with a matching PID is found.
 * If no task with the given PID is found after checking all slots,
 * the function returns `NULL`.
 *
 * This helper is used by the BPF program to efficiently locate tasks
 * for operations like enqueuing or dequeuing.
 */
static inline struct queued_task *find_queued_task(struct stalld_cpu_data *cpu_data, long pid)
{
	struct queued_task *task;

	for_each_task_entry(cpu_data, task) {
		if (task->pid == pid)
			return task;
	}

	// we don't have the NULL definition
	return (struct queued_task *) 0;
}

extern struct stalld_backend queue_track_backend;

#endif /* __QUEUE_TRACK_H */
