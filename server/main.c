#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "net.h"
#include "logic.h"
#include "job_queue.h"

job_queue_t g_job_queue;

int main() {
	job_queue_init(&g_job_queue);

	for(int i = 0; i < WORKER_THREAD_NUM; ++i) {
		pthread_t tid;
		pthread_create(&tid, NULL, worker_thread, NULL);
		pthread_detach(tid);
	}


	if(net_init() < 0)
		return 1;
	net_run();
	return 0;
}
