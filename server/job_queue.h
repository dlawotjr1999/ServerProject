#ifndef JOB_QUEUE_H
#define JOB_QUEUE_H

#include <pthread.h>
#include "common.h"

typedef enum {
	JOB_PACKET,
	JOB_DISCONNECT,
	JOB_SHUTDOWN,
	JOB_SEND
} job_type_t;

typedef enum {
	JOBQ_BLOCK,
	JOBQ_NONBLOCK
} jobq_mode_t;

typedef struct {
	job_type_t type;
	int fd;
	packet_t packet;
} job_t;

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
int job_queue_pop(job_queue_t* q, job_t* out, jobq_mode_t mode);

void job_queue_push_packet(job_queue_t* q, int fd, packet_t* pkt);
void job_queue_push_disconnect(job_queue_t* q, int fd);
void job_queue_push_shutdown(job_queue_t* q);

#endif
