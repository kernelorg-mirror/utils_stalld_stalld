/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2022 Red Hat Inc, Daniel Bristot de Oliveira <bristot@kernel.org>
 */
#ifndef __QUEUE_TRACK_H
#define __QUEUE_TRACK_H

#define MAX_QUEUE_TASK 2048

struct task_map_key {
	unsigned long cpu;
	long pid;
};

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
};

extern struct stalld_backend queue_track_backend;

#endif /* __QUEUE_TRACK_H */
