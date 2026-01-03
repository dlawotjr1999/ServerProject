#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "net.h"
#include "logic.h"
#include "job_queue.h"

volatile sig_atomic_t g_running = 1;
job_queue_t g_job_queue;

void handle_sigint(int sig) {
	g_running = 0;
}

int main() {
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);

	job_queue_init(&g_job_queue);

	for(int i = 0; i < WORKER_THREAD_NUM; ++i) {
		pthread_t tid;
		if (pthread_create(&tid, NULL, worker_thread, NULL) != 0) {
			perror("pthread_create");
			exit(1);
		}
		pthread_detach(tid);
	}

	if (net_init() < 0) {
		fprintf(stderr, "net_init failed\n");
		exit(1);
	}
	while(g_running) {
		net_run();
	}

	return 0;
}
