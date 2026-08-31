#ifndef JOB_QUEUE_H
#define JOB_QUEUE_H

#include <pthread.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	JOB_PACKET,
	JOB_DISCONNECT,
	JOB_SHUTDOWN,
	JOB_SEND,
	JOB_CLOSE,              /* logic -> net: 세션 정리가 끝났으니 이 fd를 이제 실제로 close해도 된다는 신호 */
	JOB_REDIS_SUBSCRIBE,    /* logic -> net: room_id 채널 구독을 시작하라는 요청 (3단계) */
	JOB_REDIS_UNSUBSCRIBE,  /* logic -> net: room_id 채널 구독을 중단하라는 요청 (3단계) */
	JOB_ROOM_DELIVER        /* net -> logic: Redis pub/sub으로 도착한 메시지를 로컬 멤버에게 전달하라는 요청 (3단계) */
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
	* JOB_REDIS_SUBSCRIBE / JOB_REDIS_UNSUBSCRIBE: room_id만 사용 (3단계)
	* JOB_ROOM_DELIVER: room_id + session_id(배송에서 제외할 원 발신자의 클러스터 전역 id를 담는 데 재사용
	*                   - pod-로컬 session_id가 아님에 주의) + packet (3단계)
	*/
	int session_id;
	int fd;
	int room_id;

	packet_t packet;
} job_t;

/*
* 고정 크기 circular buffer(job_queue.cpp에서 std::deque 등으로 안 바꾸고 그대로 유지) + POSIX mutex/cond
* 전부 POD라서 job_queue_t 자체는 C에서도 값으로 선언 가능함(main.c의 job_queue_t g_logic_q; 전역 변수)
* -> pimpl/opaque 포인터가 필요 없음. job_queue.cpp 내부에서는 락/조건변수를 RAII로 감싸 사용하지만
*    이 구조체의 레이아웃 자체는 C 시절과 동일함
*/
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
void job_queue_push_redis_subscribe(job_queue_t* q, int room_id);
void job_queue_push_redis_unsubscribe(job_queue_t* q, int room_id);
void job_queue_push_room_deliver(job_queue_t* q, int room_id, int except_global_id, packet_t* pkt);

#ifdef __cplusplus
}
#endif

#endif
