#ifndef JOB_QUEUE_H
#define JOB_QUEUE_H

#include <pthread.h>
#include "common.h"

typedef enum {
	JOB_PACKET,
	JOB_DISCONNECT,
	JOB_SHUTDOWN,
	JOB_SEND,
	JOB_CLOSE       /* logic -> net: 세션 정리가 끝났으니 이 fd를 이제 실제로 close해도 된다는 신호 */
} job_type_t;

typedef enum {
	JOBQ_BLOCK,
	JOBQ_NONBLOCK
} jobq_mode_t;

typedef struct {
	job_type_t type;

	/*
	* JOB_PACKET / JOB_SEND: 대상 세션의 session_id (fd가 아님 -> fd 재사용과 무관하게 항상 같은 신원을 가리킴)
	* JOB_DISCONNECT: session_id(정리 대상) + fd(정리 완료 후 net 스레드가 close할 대상)를 함께 운반
	* JOB_CLOSE: fd만 사용 (net 스레드 내부적으로 실제 close를 수행하기 위함)
	*/
	int session_id;
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
int job_queue_depth(job_queue_t* q);

void job_queue_push_packet(job_queue_t* q, int session_id, packet_t* pkt);
void job_queue_push_disconnect(job_queue_t* q, int session_id, int fd);
void job_queue_push_send(job_queue_t* q, int session_id, packet_t* pkt);
void job_queue_push_close(job_queue_t* q, int fd);
void job_queue_push_shutdown(job_queue_t* q);

#endif
