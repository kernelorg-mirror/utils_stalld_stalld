/*
 * test01 - create a blocker thread and a starving thread and see if
 * 		stalld fixes the issue
 */
#define _GNU_SOURCE
#include <ctype.h>
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/sched.h>

/* behavior switches */
static long verbose = 0;
static long quiet = 0;
static long debugging = 0;

/* cpu core to use for test */
static int testcpu = -1;

/* FIFO priority for blocker */
static unsigned int blockerprio = 2;

/*
 * shared variable to indicate the
 * state of the two threads
 */
static unsigned int blocked = 1;

/* pthread barrier for synchronized start */
static pthread_barrier_t all_threads_ready;

/* thread routines */
static void *blockee(void *arg);
static void *blocker(void *arg);

static void process_command_line(int argc, char **argv);

/* thread ids */
static pthread_t blocker_tid;
static pthread_t blockee_tid;

#define BUFFERSIZE 1024

void debug(const char *fmt, ...)
{
	va_list ap;

	if (debugging) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
}


static int isonline(int cpu)
{
	char buffer[BUFFERSIZE];
	FILE *fp;
	int online;

	sprintf(buffer, "/sys/devices/system/cpu/cpu%d/online", cpu);
	if (access(buffer, F_OK) == -1)
		return 1;

	fp= fopen(buffer, "r");
	if (fp == NULL)
		return 1;
	if (fscanf(fp, "%d", &online) != 1) {
		fclose(fp);
		return 0;
	}
	fclose(fp);
	return online;
}

static int pick_cpu(void)
{
	int i;
	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	for (i = ncpus-1; i > 0; i--) {
		if (isonline(i))
			return i;
	}
	return -1;
}

static int setup_thread(pthread_t *id, int cpu, int policy, int priority, void *(routine)(void *))
{
	int status;
	pthread_t tid;
	pthread_attr_t attr;
	cpu_set_t  cpuset;

	*id = 0;
	status = pthread_attr_init(&attr);
	if (status != 0) {
		fprintf(stderr, "failed to initialize pthread attribute struct");
		return status;
	}

	status = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	if (status != 0) {
		fprintf(stderr, "failed to set attr PTHREAD_EXPLICIT_SCHED\n");
		return status;
	}

	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	status = pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
	if (status != 0) {
		fprintf(stderr, "failed to set blocker affinity to cpu %d\n", cpu);
		return status;
	}

	status = pthread_attr_setschedpolicy(&attr, policy);
	if (status != 0) {
		fprintf(stderr, "failed to set policy to %d\n", policy);
		return status;
	}

	if (priority > 0) {
		struct sched_param param;
		memset(&param, 0, sizeof(param));
		param.sched_priority = priority;
		status = pthread_attr_setschedparam(&attr, &param);
		if (status != 0) {
			fprintf(stderr, "failed to set priority to %d\n", priority);
			return status;
		}
	}

	status = pthread_create(&tid, &attr, routine, NULL);
	if (status != 0) {
		fprintf(stderr, "failed to create thread\n");
		return status;
	}
	*id = tid;
	return 0;
}

static int setup_blocker(void)
{
	int status = setup_thread(&blocker_tid, testcpu, SCHED_FIFO, blockerprio, blocker);
	printf("blocker id: %ld\n", blocker_tid);
	return status;
}

static int setup_blockee(void)
{
	int status = setup_thread(&blockee_tid, testcpu, SCHED_OTHER, 0, blockee);
	printf("blockee id: %ld\n", blockee_tid);
	return status;
}

static void usage(void)
{
	printf("usage: test01 [-c N] [-p N] [-v] [-d] [-q]\n");
}

struct option options[] = {
	{ "help", 	no_argument, 		NULL, 	'h' },
	{ "cpu", 	required_argument, 	NULL,	'c' },
	{ "priority", 	required_argument, 	NULL,	'p' },
	{ "verbose", 	no_argument, 		NULL, 	'v' },
	{ "quiet", 	no_argument, 		NULL, 	'q' },
	{ "debug", 	no_argument, 		NULL, 	'd' },
	{ 0, 		0, 			0, 	0 }
};

static void process_command_line(int argc, char **argv)
{
	int opt;
	while ((opt = getopt_long(argc, argv, "hvqp:c:d", options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage();
			exit(0);
		case 'c':
			testcpu = atoi(optarg);
			break;
		case 'p':
			blockerprio = atoi(optarg);
			break;
		case 'v':
			verbose = 1;
			quiet = 0;
			break;
		case 'q':
			verbose = 0;
			quiet = 1;
			break;
		case 'd':
			debugging = 1;
			break;
		}
	}
}

/*
 * loop decrementing a variable until it hits zero
 */
static void *blockee(void *arg)
{
	int ret;

	ret = pthread_barrier_wait(&all_threads_ready);
	debug("blockee: running\n");

	if (ret != PTHREAD_BARRIER_SERIAL_THREAD && ret != 0) {
		perror("barrier wait in blocker failed");
		return (void *) -1;
	}
	while(blocked)
		blocked--;
	return 0;
}

/*
 * loop waiting for blocked variable to go to zero
 */

static void *blocker(void *arg)
{
	int ret = pthread_barrier_wait(&all_threads_ready);

	debug("blocker: running\n");

	if (ret != PTHREAD_BARRIER_SERIAL_THREAD && ret != 0) {
		perror("barrier wait in blocker failed");
		return (void *) -1;
	}

	while(blocked > 0)
		;
	return 0;
}

int main (int argc, char **argv)
{
	int status;
	cpu_set_t cpuset;

	/* handle the command line options */
	process_command_line(argc, argv);

	/* set up our ready barrier */
	status = pthread_barrier_init(&all_threads_ready, NULL, 3);
	if ((status ) != 0) {
		perror("pthread_barrier_init");
		exit(errno);
	}

	/* if one wasn't specified, pick a core on which to test */
	if (testcpu == -1)
		testcpu = pick_cpu();

	debug("testcpu: %d\n", testcpu);

	CPU_ZERO(&cpuset);
	status = sched_getaffinity(0, sizeof(cpuset), &cpuset);
	if (status < 0) {
		perror("Error getting main affinity");
		exit(errno);
	}

	if (setup_blocker() < 0) {
		perror("setting up blocker failed");
		exit(errno);
	}
	debug("setup blocker thread (tid: %ld)\n", blocker_tid);

	if (setup_blockee() < 0) {
		perror("setting up blockee failed");
		exit(errno);
	}
	debug("setup blockee thread (tid: %ld)\n", blockee_tid);

	/*
	 * ensure that main doesn't run on the test cpu
	 */
	CPU_CLR(testcpu, &cpuset);
	status = sched_setaffinity(0, sizeof(cpuset), &cpuset);
	if (status < 0) {
		perror("Error setting original affinity");
		exit(errno);
	}
	debug("set main affinity to not use cpu %d\n", testcpu);

	/*
	 * start the blocker and blockee
	 */
	debug("calling pthread_barrier_wait to start threads\n");

	status = pthread_barrier_wait(&all_threads_ready);
	if (status != PTHREAD_BARRIER_SERIAL_THREAD && status != 0) {
		perror("Error in main from pthread_barrier_wait");
		exit(errno);
	}

	debug("joining with blocker\n");
	status = pthread_join(blocker_tid, NULL);
	if (status < 0) {
		perror("Error joining blocker thread");
		exit(errno);
	}
	debug("Joined blocker\n");

	debug("joining with blockee\n");
	status = pthread_join(blockee_tid, NULL);
	if (status < 0) {
		perror("Error joining blockee thread");
		exit(errno);
	}
	debug("Joined blockee\n");

	printf("test completed successfully!\n");
	exit(0);
}
