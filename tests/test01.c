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
long verbose = 0;
long debugging = 0;

/* cpu core to use for test */
long testcpu = -1;

/* FIFO priority for blocker */
unsigned long blockerprio = 2;

/*
 * shared variable to indicate the
 * state of the two threads
 */
unsigned long blocked = 1;

/* pthread barrier for synchronized start */
pthread_barrier_t all_threads_ready;

/* thread routines */
void *starver(void *arg);
void *blocker(void *arg);

#define BUFFERSIZE 1024

int isonline(int cpu)
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

int pick_cpu(void)
{
	int i, cpu = -1;
	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	for (i = ncpus-1; i > 0; i--) {


}

int main (int argc, char **argv)
{
	int status;

	/* handle the command line options */
	process_command_line(argc, argv);

	/* set up our ready barrier */
	if ((status = pthread_barrier_init(&all_threads_ready, NULL, 2, "all_threads_ready")) != 0) {
		perror("pthread_barrier_init");
		exit(errno);
	}



}

void usage(void)
{
	printf("usage: test01 [-c N] [-p N] [-v] [-d] [-q]\n");
}

void process_command_line(int argc, char **argv)
{
	int opt;
	while ((opt = getopt_long(argc, argv, "+hD:vqi:g:rs:pdVum", options, NULL)) != -1) {
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
void *blocker(void *arg)
{
}

void *starver(void *arg)
{
}
