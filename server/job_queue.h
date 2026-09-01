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
/* job_queue_push()와 달리 큐가 가득 차도 절대 블록하지 않는다 - 가득 찼으면 즉시 -1을 반환하고
* 아무것도 넣지 않는다. 호출자가 락을 쥔 채로 push해야 해서 블로킹이 데드락으로 이어질 수 있는
* 상황(예: room_leave가 g_rooms_lock을 쥔 채로 이 큐에 push하는 경우)에 쓴다. 성공하면 0 반환 */
int job_queue_try_push(job_queue_t* q, job_t* job);
int job_queue_pop(job_queue_t* q, job_t* out, jobq_mode_t mode);
int job_queue_depth(job_queue_t* q);

/*
* 이 함수는 net 스레드(epoll 루프를 도는 유일한 스레드)가 직접 호출한다. job_queue_push()(blocking)를
* 썼다면, g_logic_q가 가득 찬 순간 이 스레드 전체가 멈춰 채팅 accept/recv/send/Redis 구독 처리가
* 전부 같이 중단됐을 것이다(순수 소켓 계층인데 큐 포화 하나로 전체 이벤트 루프가 인질로 잡히는 셈).
* 그래서 non-blocking(job_queue_try_push)으로 push하고, 큐가 가득 찬 순간엔 이 패킷 하나만
* 드롭한다 - 네트워크 자체도 혼잡하면 패킷을 드롭하는 것과 같은 급의 트레이드오프이고, 클라이언트
* 하나의 메시지 하나를 잃는 것이지 세션 정리(JOB_DISCONNECT, 아래 참고)처럼 자원 누수로 이어지지
* 않는다. 0이면 push 성공, -1이면 드롭(호출자가 로그만 남기면 됨) */
int job_queue_push_packet(job_queue_t* q, int session_id, packet_t* pkt);
/*
* 위 job_queue_push_packet()과 달리 이 함수는 여전히 blocking(job_queue_push)이다 - 의도적인
* 비대칭이다. JOB_DISCONNECT를 드롭하면 logic 스레드가 그 세션을 영영 정리하지 않아 session_t가
* 누수되고, 이미 죽은 fd가 방의 유저 목록에 그대로 남아 나중에 엉뚱한(재사용된) fd로 브로드캐스트를
* 시도하게 된다 - 이 프로젝트가 fd 재사용 문제를 피하려고 session_id 재설계(REDESIGN.md §2-3)까지
* 했던 것과 정면으로 배치되는 결과라, 메시지 하나를 잃는 것보다 훨씬 나쁘다. job_queue_push_packet/
* job_queue_push_room_deliver를 non-blocking으로 바꿔 g_logic_q에 몰리는 트래픽의 대부분을 이미
* 덜어냈으므로, 이 함수가 실제로 큐 포화로 블록될 가능성은 그만큼 줄었다(완전히 없어진 것은 아님 -
* 종료 시퀀스는 net_drain_io_queue()로 별도로 다룬다. net.c/main.c 참고) */
void job_queue_push_disconnect(job_queue_t* q, int session_id, int fd);
void job_queue_push_send(job_queue_t* q, int session_id, packet_t* pkt);
void job_queue_push_close(job_queue_t* q, int fd);
void job_queue_push_shutdown(job_queue_t* q);
/*
* 위 job_queue_push_shutdown()과 별개로 존재하는 non-blocking 버전. main.c의 종료 시퀀스가
* net_run() 반환 후 g_logic_q에 JOB_SHUTDOWN을 밀어넣는 지점에서 쓴다 - blocking push였다면,
* 마침 g_logic_q가 가득 찬 채로 모든 worker가 (net_run()이 이미 끝나버려 아무도 안 비우는)
* g_io_q에 blocking push하다 막혀있는 최악의 경우, main 스레드까지 여기서 같이 멈춰버려 아무도
* g_io_q를 드레인해줄 수 없는 완전한 교착 상태가 될 수 있다. non-blocking으로 만들어서 main.c가
* "될 때까지 재시도하면서 그 사이 g_io_q도 계속 드레인"하는 루프를 돌 수 있게 한다.
* 0이면 push 성공, -1이면 큐가 가득 차서 못 넣었음(호출자가 다음 기회에 재시도) */
int job_queue_try_push_shutdown(job_queue_t* q);
void job_queue_push_redis_subscribe(job_queue_t* q, int room_id);
/* room_leave()가 g_rooms_lock을 쥔 채로 호출하므로 non-blocking(job_queue_try_push)으로 push한다.
* 0이면 push 성공, -1이면 큐가 가득 차서 드롭됐음을 뜻함(호출자가 로그만 남기고 넘어가면 됨) */
int job_queue_push_redis_unsubscribe(job_queue_t* q, int room_id);
/*
* 이 함수도 net 스레드가 직접 호출한다(Redis 구독 연결에서 메시지를 받아 g_logic_q로 넘기는 지점) -
* job_queue_push_packet()과 완전히 같은 이유로 non-blocking(job_queue_try_push)이다. 0이면 성공,
* -1이면 드롭(그 채팅 메시지 하나가 이 pod의 로컬 멤버들에게 전달되지 않고 유실됨 - 다른 pod은
* 영향 없음)
*/
int job_queue_push_room_deliver(job_queue_t* q, int room_id, int except_global_id, packet_t* pkt);

#ifdef __cplusplus
}
#endif

#endif
