/*
 * starvation_gen.c - Controllable starvation generator for testing stalld
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2025 Red Hat Inc
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

struct config {
	int cpu;
	int blocker_priority;
	int blockee_priority;
	int num_blockees;
	int duration;
	unsigned long work_target;
	int verbose;
	int blockee_policy;  /* SCHED_FIFO (default) or SCHED_OTHER */
};

static struct config cfg = {
	.cpu = -1,
	.blocker_priority = 10,
	.blockee_priority = 1,
	.num_blockees = 1,
	.duration = 30,
	.work_target = 1,  /* Minimal work - tests focus on detection/boosting, not progress */
	.verbose = 0,
	.blockee_policy = SCHED_FIFO  /* Default: SCHED_FIFO blockees */
};

volatile int running = 0;  /* Number of blockees still running (initialized in main) */
volatile int *blockee_completed = NULL;  /* Track which blockees finished their work */
pthread_barrier_t start_barrier;  /* Synchronize thread startup */

void *blocker_thread(void *arg) {
	if (cfg.verbose)
		printf("[blocker] Started on CPU %d with priority %d\n", cfg.cpu, cfg.blocker_priority);

	/* Wait for all threads to be ready before starting */
	pthread_barrier_wait(&start_barrier);

	/*
	 * Busy loop to monopolize CPU.
	 * Continue running as long as there are blockees that haven't finished.
	 * The 'running' variable counts the number of active blockees - each blockee
	 * decrements it when completing their work.
	 */
	while (running > 0) {
		/* Intentionally empty - monopolize CPU */
	}

	if (cfg.verbose)
		printf("[blocker] Exiting - all blockees finished\n");

	return NULL;
}

void *blockee_thread(void *arg) {
	int id = *(int*)arg;
	volatile unsigned long counter = 0;
	unsigned long target = cfg.work_target;

	if (cfg.verbose)
		printf("[blockee %d] Started - will work toward %lu iterations\n", id, target);

	/* Wait for all threads to be ready before starting */
	pthread_barrier_wait(&start_barrier);

	/*
	 * Busy loop to stay on runqueue - will be starved by higher priority blocker.
	 * When boosted by stalld, will be able to make progress and eventually complete.
	 * Loop while: (1) there are still blockees running AND (2) haven't reached target
	 */
	while (running > 0 && counter < target) {
		counter++;
	}

	if (counter >= target) {
		blockee_completed[id] = 1;
		printf("[blockee %d] ✓ COMPLETED work after %lu iterations\n", id, counter);
	} else if (cfg.verbose) {
		printf("[blockee %d] Interrupted after %lu/%lu iterations\n", id, counter, target);
	}

	/*
	 * Decrement the running count to signal this blockee has finished.
	 * When the last blockee decrements running to 0, the blocker will exit.
	 */
	__sync_sub_and_fetch(&running, 1);

	return (void *)(unsigned long)blockee_completed[id];
}

void signal_handler(int sig) {
	(void)sig;
	running = 0;
}

void usage(void) {
	printf("Usage: starvation_gen [OPTIONS]\n");
	printf("Generate controlled starvation conditions for testing stalld\n\n");
	printf("Options:\n");
	printf("  -c, --cpu N              CPU to use for test (default: auto-select)\n");
	printf("  -p, --priority N         SCHED_FIFO priority for blocker (default: 10)\n");
	printf("  -b, --blockee-priority N SCHED_FIFO priority for blockees (default: 1)\n");
	printf("  -n, --num-blockees N     Number of blockee threads (default: 1)\n");
	printf("  -d, --duration N         Duration in seconds (default: 30)\n");
	printf("  -w, --work-target N      Work iterations for blockees to complete (default: 1)\n");
	printf("  -o, --other              Use SCHED_OTHER for blockees instead of SCHED_FIFO\n");
	printf("  -v, --verbose            Verbose output\n");
	printf("  -h, --help               Show this help\n");
	printf("\nExamples:\n");
	printf("  starvation_gen -c 2 -p 15 -n 3 -d 60 -v\n");
	printf("    (Create RT starvation: blocker at priority 15, blockees at priority 1)\n\n");
	printf("  starvation_gen -c 2 -p 10 -b 5 -n 2 -d 30\n");
	printf("    (Test FIFO-on-FIFO starvation: blocker at priority 10 starves blockees at priority 5)\n\n");
	printf("  starvation_gen -c 2 -p 80 -o -n 2 -d 30\n");
	printf("    (Test SCHED_OTHER starvation: RT blocker starves SCHED_OTHER blockees)\n\n");
	printf("  starvation_gen -c 2 -w 50000000 -v\n");
	printf("    (Blockees will complete after 50M iterations if boosted by stalld)\n");
}

int pick_cpu(void) {
	int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	/* Pick last CPU */
	return num_cpus - 1;
}

int main(int argc, char **argv) {
	pthread_t blocker;
	pthread_t *blockees = NULL;
	cpu_set_t cpuset;
	struct sched_param param;
	pthread_attr_t attr;
	int *blockee_ids = NULL;
	int i, ret;

	struct option long_options[] = {
		{"cpu",              required_argument, 0, 'c'},
		{"priority",         required_argument, 0, 'p'},
		{"blockee-priority", required_argument, 0, 'b'},
		{"num-blockees",     required_argument, 0, 'n'},
		{"duration",         required_argument, 0, 'd'},
		{"work-target",      required_argument, 0, 'w'},
		{"other",            no_argument,       0, 'o'},
		{"verbose",          no_argument,       0, 'v'},
		{"help",             no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	/* Parse command line */
	while (1) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "c:p:b:n:d:w:ovh", long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
		case 'c':
			cfg.cpu = atoi(optarg);
			break;
		case 'p':
			cfg.blocker_priority = atoi(optarg);
			break;
		case 'b':
			cfg.blockee_priority = atoi(optarg);
			break;
		case 'n':
			cfg.num_blockees = atoi(optarg);
			break;
		case 'd':
			cfg.duration = atoi(optarg);
			break;
		case 'w':
			cfg.work_target = strtoul(optarg, NULL, 10);
			break;
		case 'o':
			cfg.blockee_policy = SCHED_OTHER;
			break;
		case 'v':
			cfg.verbose = 1;
			break;
		case 'h':
			usage();
			exit(0);
		default:
			usage();
			exit(1);
		}
	}

	/* Auto-select CPU if not specified */
	if (cfg.cpu == -1)
		cfg.cpu = pick_cpu();

	/* Validate parameters */
	if (cfg.blocker_priority < 1 || cfg.blocker_priority > 99) {
		fprintf(stderr, "Error: blocker priority must be 1-99\n");
		exit(1);
	}

	/* For SCHED_FIFO blockees, validate priority */
	if (cfg.blockee_policy == SCHED_FIFO) {
		if (cfg.blockee_priority < 1 || cfg.blockee_priority > 99) {
			fprintf(stderr, "Error: blockee priority must be 1-99 for SCHED_FIFO\n");
			exit(1);
		}

		if (cfg.blockee_priority >= cfg.blocker_priority) {
			fprintf(stderr, "Error: blockee priority (%d) must be less than blocker priority (%d)\n",
				cfg.blockee_priority, cfg.blocker_priority);
			exit(1);
		}
	}

	if (cfg.num_blockees < 1 || cfg.num_blockees > 10) {
		fprintf(stderr, "Error: num_blockees must be 1-10\n");
		exit(1);
	}

	/* Setup signal handler */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/*
	 * Initialize barrier to synchronize thread startup.
	 * Count = 1 blocker + num_blockees (all threads must reach barrier before starting).
	 */
	ret = pthread_barrier_init(&start_barrier, NULL, cfg.num_blockees + 1);
	if (ret != 0) {
		fprintf(stderr, "pthread_barrier_init failed: %s\n", strerror(ret));
		exit(1);
	}

	/* Initialize pthread attributes */
	ret = pthread_attr_init(&attr);
	if (ret != 0) {
		fprintf(stderr, "pthread_attr_init failed: %s\n", strerror(ret));
		exit(1);
	}

	ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	if (ret != 0) {
		fprintf(stderr, "pthread_attr_setinheritsched failed: %s\n", strerror(ret));
		exit(1);
	}

	/* Set CPU affinity */
	CPU_ZERO(&cpuset);
	CPU_SET(cfg.cpu, &cpuset);
	ret = pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
	if (ret != 0) {
		fprintf(stderr, "pthread_attr_setaffinity_np failed: %s\n", strerror(ret));
		exit(1);
	}

	/* Setup blocker thread */
	ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	if (ret != 0) {
		fprintf(stderr, "pthread_attr_setschedpolicy failed: %s\n", strerror(ret));
		exit(1);
	}

	param.sched_priority = cfg.blocker_priority;
	ret = pthread_attr_setschedparam(&attr, &param);
	if (ret != 0) {
		fprintf(stderr, "pthread_attr_setschedparam failed: %s\n", strerror(ret));
		exit(1);
	}

	ret = pthread_create(&blocker, &attr, blocker_thread, NULL);
	if (ret != 0) {
		fprintf(stderr, "pthread_create (blocker) failed: %s\n", strerror(ret));
		exit(1);
	}

	/* Setup blockee threads */
	blockees = malloc(cfg.num_blockees * sizeof(pthread_t));
	blockee_ids = malloc(cfg.num_blockees * sizeof(int));
	blockee_completed = calloc(cfg.num_blockees, sizeof(int));
	if (!blockees || !blockee_ids || !blockee_completed) {
		fprintf(stderr, "malloc failed\n");
		exit(1);
	}

	/*
	 * Initialize running count to number of blockees.
	 * The blocker will continue running while running > 0.
	 * Each blockee decrements this when it finishes.
	 */
	running = cfg.num_blockees;

	/* Set blockee policy (SCHED_FIFO or SCHED_OTHER)
	 * - SCHED_FIFO with lower priority than blocker creates RT starvation
	 * - SCHED_OTHER tests normal task starvation by high-priority RT blocker */
	ret = pthread_attr_setschedpolicy(&attr, cfg.blockee_policy);
	if (ret != 0) {
		fprintf(stderr, "pthread_attr_setschedpolicy (blockee) failed: %s\n", strerror(ret));
		exit(1);
	}

	/* For SCHED_OTHER, priority must be 0 (nice value is separate) */
	param.sched_priority = (cfg.blockee_policy == SCHED_OTHER) ? 0 : cfg.blockee_priority;
	ret = pthread_attr_setschedparam(&attr, &param);
	if (ret != 0) {
		fprintf(stderr, "pthread_attr_setschedparam (blockee) failed: %s\n", strerror(ret));
		exit(1);
	}

	for (i = 0; i < cfg.num_blockees; i++) {
		blockee_ids[i] = i;
		ret = pthread_create(&blockees[i], &attr, blockee_thread, &blockee_ids[i]);
		if (ret != 0) {
			fprintf(stderr, "pthread_create (blockee %d) failed: %s\n", i, strerror(ret));
			exit(1);
		}
	}

	/* Print configuration */
	printf("Starvation generator started:\n");
	printf("  CPU:              %d\n", cfg.cpu);
	printf("  Blocker:          SCHED_FIFO priority %d\n", cfg.blocker_priority);
	if (cfg.blockee_policy == SCHED_FIFO) {
		printf("  Blockee policy:   SCHED_FIFO priority %d\n", cfg.blockee_priority);
	} else {
		printf("  Blockee policy:   SCHED_OTHER\n");
	}
	printf("  Blockee threads:  %d\n", cfg.num_blockees);
	printf("  Work target:      %lu iterations\n", cfg.work_target);
	printf("  Duration:         %d seconds\n", cfg.duration);
	printf("  Blocker TID:    %ld\n", (long)blocker);
	for (i = 0; i < cfg.num_blockees; i++) {
		printf("  Blockee %d TID:   %ld\n", i, (long)blockees[i]);
	}
	printf("\nBlockees will complete if boosted enough to finish %lu iterations\n", cfg.work_target);
	printf("Press Ctrl+C to stop early\n");
	fflush(stdout);

	/*
	 * Wait for specified duration OR until all blockees complete (whichever comes first).
	 * The blocker will exit automatically when all blockees decrement running to 0.
	 * We use a timeout to prevent hanging if blockees never complete.
	 */
	for (i = 0; i < cfg.duration && running > 0; i++) {
		sleep(1);
	}

	/* Force shutdown if duration expires and blockees still haven't finished */
	if (running > 0) {
		if (cfg.verbose)
			printf("\n[main] Duration expired, forcing shutdown\n");
		running = 0;
	}

	/* Wait for all threads to finish */
	pthread_join(blocker, NULL);
	for (i = 0; i < cfg.num_blockees; i++) {
		pthread_join(blockees[i], NULL);
	}

	/* Report results */
	printf("\nStarvation generator completed\n");
	int num_completed = 0;
	for (i = 0; i < cfg.num_blockees; i++) {
		if (blockee_completed[i]) {
			num_completed++;
		}
	}
	printf("Blockees completed: %d/%d\n", num_completed, cfg.num_blockees);

	if (num_completed == cfg.num_blockees) {
		printf("✓ SUCCESS: All blockees completed their work (stalld boosting worked!)\n");
	} else if (num_completed > 0) {
		printf("⚠ PARTIAL: Some blockees completed (%d/%d)\n", num_completed, cfg.num_blockees);
	} else {
		printf("✗ NONE: No blockees completed (may need longer boost or lower work target)\n");
	}

	free(blockees);
	free(blockee_ids);
	free((void *)blockee_completed);
	pthread_barrier_destroy(&start_barrier);
	pthread_attr_destroy(&attr);

	return (num_completed == cfg.num_blockees) ? 0 : 1;
}
