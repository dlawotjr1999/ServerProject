#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"
#include "redis_client.h"
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
		* Redis pub/sub으로 도착한 채팅 메시지를 이 pod의 로컬 멤버에게 전달 (3단계)
		* job.room_id: 대상 방, job.session_id: 배송에서 제외할 원 발신자의 클러스터 전역 id
		* (job_t.session_id 필드를 재사용한 것일 뿐, pod-로컬 session_id가 아님)
		*/
		case JOB_ROOM_DELIVER: {
			room_broadcast_local(job.room_id, job.session_id, &job.packet);
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

		/* 매치메이킹(빈 방 찾기/생성)은 Redis가 클러스터 전역으로 원자적으로 처리함 (3단계) */
		int room_id;
		if (redis_join_room(s->session_id, &room_id) != 0) {
			log_json("ERROR", "redis_join_failed", "session_id", LOG_ARG_INT, s->session_id, NULL);
			break;
		}

		/*
		* 여기부터는 redis_join_room()이 이미 Redis 쪽 인원 카운트를 +1 해둔 상태다.
		* 그러니 아래 어떤 경로로 실패해서 빠져나가든, 반드시 방금 잡은 자리를 반납해야 한다.
		* 반납하지 않으면 아무도 없는 방의 카운트가 0으로 돌아가지 않아 freelist로 회수되지 못하고,
		* MAX_ROOMS 고정 상한이 조금씩 영구적으로 잠식된다
		*/
		room_t* r = room_get_or_init(room_id);
		if (!r) {
			log_json("ERROR", "room_init_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			if (redis_leave_room(s->session_id, room_id) != 0)
				log_json("ERROR", "redis_leave_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			break;
		}

		/* cross-pod pub/sub 자기 자신 제외 판정에 쓸 클러스터 전역 id 발급 (3단계 버그 수정) -
		* 로컬 session_id는 pod마다 독립적으로 증가해 클러스터 전역에서 유일하지 않아, 서로 다른 pod의
		* 세션이 우연히 같은 session_id를 가지면 상대 pod이 자기 메시지로 착각해 걸러버리는 문제가 있었음 */
		int global_id;
		if (redis_next_global_id(&global_id) != 0) {
			log_json("ERROR", "redis_global_id_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			if (redis_leave_room(s->session_id, room_id) != 0)
				log_json("ERROR", "redis_leave_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			break;
		}
		/* s->lock 보호 하에 기록 - leave->rejoin이 in-flight PKT_CHAT과 겹칠 때의 락 없는 읽기/쓰기 회피 */
		session_set_global_id(s, global_id);

		/* 입장 자체가 실패하는 경우(처리 중 세션이 죽었거나 방이 이미 꽉 참)도 자리를 반납해야 한다.
		* 예전에는 room_join이 first_local_member만 돌려줘서 이 실패가 아예 보이지 않았음 */
		bool first_local_member = false;
		if (room_join(r, s, &first_local_member) != 0) {
			log_json("ERROR", "room_join_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			if (redis_leave_room(s->session_id, room_id) != 0)
				log_json("ERROR", "redis_leave_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			break;
		}

		/* 이 pod에서 이 방에 로컬 멤버가 처음 생긴 경우에만 Redis 채널 구독을 시작함 */
		if (first_local_member) {
			job_queue_push_redis_subscribe(&g_io_q, room_id);
			net_wakeup();
		}
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

		/*
		* 3단계: payload를 여기서 한 번만 재포맷(개행 추가 + 길이 재계산)해서 발행한다.
		* 수신 측(room_broadcast_local)은 재포맷 없이 그대로 전달만 하므로 모든 pod가 같은 바이트를 봄
		* (기존 state.c의 room_broadcast가 하던 포맷팅 로직을 그대로 옮겨온 것)
		*/
		int payload_len = (int)pkt->length - 2;
		if (payload_len <= 0) break;
		if (payload_len > MAX_PACKET_SIZE) payload_len = MAX_PACKET_SIZE;

		packet_t out;
		memset(&out, 0, sizeof(out));
		int n = snprintf(out.payload, MAX_PACKET_SIZE, "%.*s\n", payload_len, pkt->payload);
		if (n <= 0 || n >= MAX_PACKET_SIZE)
			break;
		out.type = PKT_CHAT;
		out.length = 2 + (uint16_t)n;

		if (redis_publish_chat(room_id, session_get_global_id(s), &out) != 0) {
			log_json("ERROR", "redis_publish_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			break;
		}

		/* 발행 성공 시점에 카운트 (기존과 달리 "실제 배송 성공"이 아니라 "발행 성공" 기준으로 의미가 약간 바뀜) */
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
