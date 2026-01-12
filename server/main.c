#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "net.h"
#include "logic.h"
#include "job_queue.h"

job_queue_t g_job_queue;

void handle_sigint(int sig) {
	if(sig == SIGINT || sig == SIGTERM)
		job_queue_push_shutdown(&g_job_queue);
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
	while(1) {
		net_run();
	}

	return 0;
}
