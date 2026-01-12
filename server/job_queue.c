#include "job_queue.h"

void job_queue_init(job_queue_t* q) {
	q->head = q->tail = q->count = 0;
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);
}

void job_queue_push(job_queue_t *q, job_t* job) {
	pthread_mutex_lock(&q->mutex);

	while(q->count == JOB_QUEUE_SIZE) 
		pthread_cond_wait(&q->cond, &q->mutex);

	q->jobs[q->tail] = *job;
	q->tail = (q->tail + 1) % JOB_QUEUE_SIZE;
	q->count++;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
}

void job_queue_pop(job_queue_t* q, job_t* out) {
	pthread_mutex_lock(&q->mutex);

	while(q->count == 0) 
		pthread_cond_wait(&q->cond, &q->mutex);

	*out = q->jobs[q->head];
	q->head = (q->head + 1) % JOB_QUEUE_SIZE;
	q->count--;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
}

void job_queue_push_packet(job_queue_t* q, int fd, packet_t* pkt) {
	job_t job = {.type = JOB_PACKET, .fd = fd, .packet = *pkt };
	job_queue_push(q, &job);
}

void job_queue_push_disconnect(job_queue_t* q, int fd) {
	job_t job = {
		.type = JOB_DISCONNECT,
		.fd = fd
	};
	job_queue_push(q, &job);
}

void job_queue_push_shutdown(job_queue_t* q) {
	job_t job = {
		.type = JOB_SHUTDOWN
	};
	job_queue_push(q, &job);
}
