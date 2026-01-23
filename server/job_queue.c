#include "job_queue.h"

/*
* 스레드 간 작업(job_t) 전달을 위한 고정 크기의 circular queue
* producer / consumer 패턴으로 구현
* producer: job_queue_push()로 작업을 넣음
* consumer: job_queue_pop()으로 작업을 꺼냄
*/

/* 큐 초기화 함수 */
void job_queue_init(job_queue_t* q) {
	q->head = q->tail = q->count = 0;
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);
}

/* job 하나를 push하는 함수 */
void job_queue_push(job_queue_t *q, job_t* job) {
	
	/* 연산은 mutex로 보호됨 */
	pthread_mutex_lock(&q->mutex);

	/* 큐가 가득 차면 공간이 생길 때까지 대기 */
	while(q->count == JOB_QUEUE_SIZE) 
		pthread_cond_wait(&q->cond, &q->mutex);

	/* push 진행, circular queue이므로 moular 연산으로 push가 진행됨*/
	q->jobs[q->tail] = *job;
	q->tail = (q->tail + 1) % JOB_QUEUE_SIZE;
	q->count++;

	/* consumer가 대기 중일 수 있으므로 깨움 */
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
}

/* job 하나를 pop하는 함수 */
int job_queue_pop(job_queue_t* q, job_t* out, jobq_mode_t mode) {
	
	/* 연산은 mutex로 보호됨 */
	pthread_mutex_lock(&q->mutex);

	/* 큐가 비어있으면 mode에 따라 BLOCK 또는 즉시 반환 */
	while (q->count == 0) {
		if (mode == JOBQ_NONBLOCK) {
			pthread_mutex_unlock(&q->mutex);
			return 0;   
		}
		pthread_cond_wait(&q->cond, &q->mutex);
	}

	/* pop 진행, circular queue이므로 moular 연산으로 pop이 진행됨*/
	*out = q->jobs[q->head];
	q->head = (q->head + 1) % JOB_QUEUE_SIZE;
	q->count--;

	/* producer가 가득 차 있어 대기 중일 수 있으므로 깨움 */
	pthread_cond_signal(&q->cond);1
	pthread_mutex_unlock(&q->mutex);

	return 1;
}

/* ======================= 이하 helper 함수 ======================= */
/* job 타입별로 필수 필드가 다르므로, 생성 규칙을 한 곳에 모음 */
/* 또한, job_t의 내부 구조가 바뀌어도(필드 추가/초기화 규칙 변경) helper만 수정하면 됨 */

/* 패킷 수신 이벤트를 job 형태(JOB_PACKET)로 만들어 큐에 삽입 */
void job_queue_push_packet(job_queue_t* q, int fd, packet_t* pkt) {
	job_t job = {.type = JOB_PACKET, .fd = fd, .packet = *pkt };
	job_queue_push(q, &job);
}

/* 연결 종료 이벤트를 job 형태(JOB_DISCONNECT)로 만들어 큐에 삽입 */
void job_queue_push_disconnect(job_queue_t* q, int fd) {
	job_t job = { .type = JOB_DISCONNECT,.fd = fd };
	job_queue_push(q, &job);
}

/* 서버 종료 요청을 job 형태(JOB_SHUTDOWN)로 만들어 큐에 삽입 */
void job_queue_push_shutdown(job_queue_t* q) {
	job_t job = { .type = JOB_SHUTDOWN };
	job_queue_push(q, &job);
}
