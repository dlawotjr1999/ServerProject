#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"
#include <stdio.h>

extern job_queue_t g_logic_q;
extern job_queue_t g_io_q;
extern void net_wakeup(void);

/* 하나의 패킷에 대해, 패킷 타입별 로직을 수행하는 함수 */
static void handle_packet(session_t* s, packet_t* pkt);

/* 연결 종료 처리 함수 (session_id 기반 정리 + net 스레드에 close 허가 통보) */
static void handle_disconnect(int session_id, int fd);

/* 서버 정상 종료 시 전체 세션 및 방 정리 함수 */
static void handle_shutdown(void);

/* 로직 스레드 메인 루프 */
void* worker_thread(void* arg)
{
	(void)arg;
	job_t job;

	while (1) {
		/* 큐에 작업이 들어올 때까지 대기 */
		job_queue_pop(&g_logic_q, &job, JOBQ_BLOCK);

		switch (job.type) {

		/*
		* 네트워크 이벤트로부터 온 패킷 처리
		* session_id로 세션을 조회(참조 카운트 +1) -> 세션은 net 스레드가 accept 시점에
		* 이미 만들어뒀으므로, 여기서 못 찾으면 이미 정리(disconnect)된 세션이라는 뜻
		*/
		case JOB_PACKET: {
			session_t* s = session_get_by_id(job.session_id);
			if (!s) {
				log_json("ERROR", "session_not_found", "session_id", LOG_ARG_INT, job.session_id, NULL);
				break;
			}
			if (!session_is_alive(s)) {
				session_release(s);
				break;
			}

			/* 패킷 타입별 논리 처리 */
			handle_packet(s, &job.packet);

			log_json("INFO", "packet_handled",
				"session_id", LOG_ARG_INT, s->session_id,
				"type", LOG_ARG_INT, job.packet.type,
				"len", LOG_ARG_INT, job.packet.length,
				NULL);

			session_release(s);
			break;
		}

		/*
		* 연결 종료 처리
		* net thread에서 epoll/err 등으로 disconnect를 감지하면 로직 큐에 JOB_DISCONNECT 삽입
		*/
		case JOB_DISCONNECT: {
			handle_disconnect(job.session_id, job.fd);
			break;
		}

		/*
		* 정상 종료 처리
		* 모든 세션을 순회하며 방에서 제거 후 세션 정리
		* 정리 완료 후 worker thread 종료
		*/
		case JOB_SHUTDOWN: {
			handle_shutdown();
			return NULL;
		}

		default:
			break;
		}
	}

	return NULL;
}

static void handle_packet(session_t* s, packet_t* pkt) {
	if (!s || !session_is_alive(s))
		return;

	switch (pkt->type) {

	/* 방 입장
	* 이미 방에 들어가 있는 경우 중복 방지
	* 참가 가능한 방을 탐색 후, 방이 존재하지 않으면 방 생성
	* 이후 현재 세션으로 방에 참가
	*/
	case PKT_JOIN_ROOM: {
		if (session_get_room_id(s) >= 0)
			break;

		room_t* r = room_find();
		if (!r) r = room_create();
		room_join(r, s);
		break;
	}

	/* 채팅 메시지 처리
	* 방 미입장 시의 채팅은 무시
	* 세션의 room_id를 통해 방의 정보를 가져옴
	* 브로드캐스팅을 통해 같은 방의 세션들에 채팅 전파
	*/
	case PKT_CHAT: {
		int room_id = session_get_room_id(s);
		if (room_id < 0)
			break;

		room_t* r = room_get(room_id);
		if (!r)
			break;
		room_broadcast(r, s, pkt);

		/* 브로드캐스트된 채팅 메시지 수 누적 (메트릭 노출용) */
		metrics_inc_messages();
		break;
	}

	/*
	* 방 퇴장
	* room_leave 함수를 통해 세션의 room_id 갱신 및 방 목록 정리를 수행
	*/
	case PKT_LEAVE_ROOM: {
		if (session_get_room_id(s) < 0)
			break;

		room_leave(s);
		break;
	}

	default:
		break;
	}
}

/*
* session_id로 지정된 세션을 정리하는 함수
* 정리가 다 끝난 뒤에야 net 스레드에 JOB_CLOSE로 알려서 실제 close()가 일어나게 함
* (그 전에 close가 먼저 일어나면 fd가 재사용될 수 있어서, 반드시 이 순서를 지켜야 함)
*/
static void handle_disconnect(int session_id, int fd) {
	session_t* s = session_get_by_id(session_id);

	if (s) {
		/*
		* alive를 내리는 것과 room_id를 읽는 것을 원자적으로 함께 처리(session_deactivate 참고)
		* -> 동시에 처리 중인 JOIN_ROOM(room_join)과 순서가 어떻게 되든 결과가 항상 일관됨
		*/
		int room_id = session_deactivate(s);

		log_json("INFO", "disconnect_handled",
			"fd", LOG_ARG_INT, fd,
			"session_id", LOG_ARG_INT, session_id,
			"room_id", LOG_ARG_INT, room_id,
			NULL);

		/* 방에 들어가 있었다면 방에서 제거 */
		if (room_id >= 0) {
			room_leave(s);
		}

		session_release(s);   /* session_get_by_id에서 잡은 임시 참조 반환 */
	}

	/* 테이블에서 제거 + 생성 시 잡아둔 기준 참조 반환(다른 참조가 없으면 여기서 실제 free) */
	session_remove_by_id(session_id);

	/* 세션 정리가 끝났으니, 이제 net 스레드가 이 fd를 실제로 close해도 됨을 알림 */
	job_queue_push_close(&g_io_q, fd);
	net_wakeup();
}

static void handle_shutdown(void)
{
	log_json("INFO", "shutdown_started", NULL);

	/* 남아있는 모든 세션을 방에서 빼고 정리 (net 스레드는 net_run 종료 시 남은 연결을 별도로 직접 닫음) */
	session_remove_all();

	log_json("INFO", "shutdown_completed", NULL);
}
