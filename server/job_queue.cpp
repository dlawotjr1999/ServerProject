#include "job_queue.h"
#include "posix_lock.hpp"

/*
* 스레드 간 작업(job_t) 전달을 위한 고정 크기의 circular queue
* producer / consumer 패턴으로 구현
* producer: job_queue_push()로 작업을 넣음
* consumer: job_queue_pop()으로 작업을 꺼냄
*
* 저장소는 C 버전과 동일하게 고정 배열을 그대로 씀(std::deque 등으로 바꾸지 않음) ->
* std::deque는 기본이 무제한 확장이라, 지금의 "가득 차면 producer를 블록시키는" 배압(backpressure)
* 동작을 유지하려면 결국 크기 체크를 손으로 다시 짜야 해서 컨테이너를 바꾸는 실익이 없었음.
* 대신 락/조건변수 짝 맞추기만 PosixLockGuard(RAII)로 감싸서 자동화함
*/

/* 큐 초기화 함수 */
void job_queue_init(job_queue_t* q) {
	q->head = q->tail = q->count = 0;
	pthread_mutex_init(&q->mutex, nullptr);
	pthread_cond_init(&q->cond, nullptr);
}

/* job 하나를 push하는 함수 */
void job_queue_push(job_queue_t* q, job_t* job) {
	PosixLockGuard lock(q->mutex);

	/* 큐가 가득 차면 공간이 생길 때까지 대기 */
	while (q->count == JOB_QUEUE_SIZE)
		pthread_cond_wait(&q->cond, &q->mutex);

	/* push 진행, circular queue이므로 modular 연산으로 push가 진행됨 */
	q->jobs[q->tail] = *job;
	q->tail = (q->tail + 1) % JOB_QUEUE_SIZE;
	q->count++;

	/* consumer가 대기 중일 수 있으므로 깨움 */
	pthread_cond_signal(&q->cond);

	/* 락 해제는 lock의 소멸자(스코프 종료)가 자동으로 처리 */
}

/* job_queue_push()와 달리 큐가 가득 차도 절대 블록하지 않고 즉시 실패(-1)를 반환하는 non-blocking push.
* 호출자가 이미 다른 락(예: g_rooms_lock)을 쥔 채로 이 큐에 push해야 해서, 블로킹이 그 락을 필요로
* 하는 다른 스레드(예: net 스레드의 /metrics 처리)와 맞물려 데드락으로 이어질 수 있는 상황에 쓴다 */
int job_queue_try_push(job_queue_t* q, job_t* job) {
	PosixLockGuard lock(q->mutex);

	if (q->count == JOB_QUEUE_SIZE)
		return -1;   /* 블록하지 않고 즉시 실패 반환 */

	q->jobs[q->tail] = *job;
	q->tail = (q->tail + 1) % JOB_QUEUE_SIZE;
	q->count++;

	pthread_cond_signal(&q->cond);
	return 0;
}

/* job 하나를 pop하는 함수 */
int job_queue_pop(job_queue_t* q, job_t* out, jobq_mode_t mode) {
	PosixLockGuard lock(q->mutex);

	/* 큐가 비어있으면 mode에 따라 BLOCK 또는 즉시 반환 */
	while (q->count == 0) {
		if (mode == JOBQ_NONBLOCK) {
			return 0;   /* 조기 반환이어도 lock의 소멸자가 알아서 언락함 */
		}
		pthread_cond_wait(&q->cond, &q->mutex);
	}

	/* pop 진행, circular queue이므로 modular 연산으로 pop이 진행됨 */
	*out = q->jobs[q->head];
	q->head = (q->head + 1) % JOB_QUEUE_SIZE;
	q->count--;

	/* producer가 가득 차 있어 대기 중일 수 있으므로 깨움 */
	pthread_cond_signal(&q->cond);

	return 1;
}

/* 큐에 대기 중인 job 개수를 조회하는 함수 (메트릭 노출용) */
int job_queue_depth(job_queue_t* q) {
	/* count는 push/pop 스레드가 동시에 변경할 수 있으므로 lock으로 보호된 상태에서 읽음 */
	PosixLockGuard lock(q->mutex);
	return q->count;
}

/* ======================= 이하 helper 함수 ======================= */
/* job 타입별로 필수 필드가 다르므로, 생성 규칙을 한 곳에 모음 */
/* 또한, job_t의 내부 구조가 바뀌어도(필드 추가/초기화 규칙 변경) helper만 수정하면 됨 */

/* 패킷 수신 이벤트를 job 형태(JOB_PACKET)로 만들어 큐에 삽입. fd가 아니라 session_id로 대상을 지정함 */
void job_queue_push_packet(job_queue_t* q, int session_id, packet_t* pkt) {
	job_t job{};
	job.type = JOB_PACKET;
	job.session_id = session_id;
	job.packet = *pkt;
	job_queue_push(q, &job);
}

/*
* 연결 종료 이벤트를 job 형태(JOB_DISCONNECT)로 만들어 큐에 삽입
* session_id는 logic 스레드가 세션을 정리할 대상, fd는 그 정리가 끝난 뒤 net 스레드가 close할 대상
*/
void job_queue_push_disconnect(job_queue_t* q, int session_id, int fd) {
	job_t job{};
	job.type = JOB_DISCONNECT;
	job.session_id = session_id;
	job.fd = fd;
	job_queue_push(q, &job);
}

/* SEND 작업을 job 형태(JOB_SEND)로 만들어 큐에 삽입. 역시 session_id로 대상을 지정함 */
void job_queue_push_send(job_queue_t* q, int session_id, packet_t* pkt) {
	job_t job{};
	job.type = JOB_SEND;
	job.session_id = session_id;
	job.packet = *pkt;
	job_queue_push(q, &job);
}

/* logic 스레드가 세션 정리를 끝낸 뒤, net 스레드에게 이 fd를 이제 close해도 된다고 알리는 job */
void job_queue_push_close(job_queue_t* q, int fd) {
	job_t job{};
	job.type = JOB_CLOSE;
	job.fd = fd;
	job_queue_push(q, &job);
}

/* 서버 종료 요청을 job 형태(JOB_SHUTDOWN)로 만들어 큐에 삽입 */
void job_queue_push_shutdown(job_queue_t* q) {
	job_t job{};
	job.type = JOB_SHUTDOWN;
	job_queue_push(q, &job);
}

/* net 스레드에게 room_id 채널 구독을 시작하라는 job (3단계, logic -> net) */
void job_queue_push_redis_subscribe(job_queue_t* q, int room_id) {
	job_t job{};
	job.type = JOB_REDIS_SUBSCRIBE;
	job.room_id = room_id;
	job_queue_push(q, &job);
}

/*
* net 스레드에게 room_id 채널 구독을 중단하라는 job (3단계, logic -> net)
* room_leave()가 g_rooms_lock을 쥔 채로 호출하므로 non-blocking(job_queue_try_push)을 쓴다.
* 0이면 push 성공, -1이면 큐가 가득 차서 드롭됐음(호출자가 로그만 남기고 넘어감 - no retry)
*/
int job_queue_push_redis_unsubscribe(job_queue_t* q, int room_id) {
	job_t job{};
	job.type = JOB_REDIS_UNSUBSCRIBE;
	job.room_id = room_id;
	return job_queue_try_push(q, &job);
}

/*
* Redis pub/sub으로 도착한 메시지를 로컬 멤버에게 전달하라는 job (3단계, net -> logic)
* except_global_id는 job_t.session_id 필드를 재사용함(원 발신자를 배송 대상에서 제외하기 위함).
* 담기는 값은 pod-로컬 session_id가 아니라 클러스터 전역 id(redis_next_global_id 발급분)다
*/
void job_queue_push_room_deliver(job_queue_t* q, int room_id, int except_global_id, packet_t* pkt) {
	job_t job{};
	job.type = JOB_ROOM_DELIVER;
	job.room_id = room_id;
	job.session_id = except_global_id;
	job.packet = *pkt;
	job_queue_push(q, &job);
}
