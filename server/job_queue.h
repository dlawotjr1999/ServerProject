#ifndef JOB_QUEUE_H
#define JOB_QUEUE_H

#include <pthread.h>
#include "common.h"

typedef struct {
	job_t jobs[JOB_QUEUE_SIZE];
	int head;
	int tail;
	int count;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} job_queue_t;

void job_queue_init(job_queue_t* q);
void job_queue_push(job_queue_t* q, job_t* job);
void job_queue_pop(job_queue_t* q, job_t* out);

#endif
