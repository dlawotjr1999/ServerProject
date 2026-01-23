#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include <stdio.h>

extern job_queue_t g_logic_q;

/* 하나의 패킷에 대해, 패킷 타입별 로직을 수행하는 함수 */
static void handle_packet(session_t* s, packet_t* pkt);

/* fd 연결 종료 처리 함수 */
static void handle_disconnect(int fd);

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
		* fd -> session 매핑 확보
		* 만약 session이 없으면 새로 생성함
		*/
		case JOB_PACKET: {
			session_t* s = session_get(job.fd);
			if (!s) s = session_create(job.fd);

			if (!s || !s->alive) {
				printf("[ERROR] session create failed fd=%d\n", job.fd);
				break;
			}

			/* 패킷 타입별 논리 처리 */
			handle_packet(s, &job.packet);

			printf("[WORKER] sid=%d fd=%d type=%d len=%d\n", s->session_id, job.fd, job.packet.type, job.packet.length);
			break;
		}

		/*
		* 연결 종료 처리
		* net thread에서 epoll/err 등으로 disconnect를 감지하면 로직 큐에 JOB_DISCONNECT 삽입
		*/
		case JOB_DISCONNECT: {
			handle_disconnect(job.fd);
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
	if (!s || !s->alive)
		return;

	switch (pkt->type) {

	/* 방 입장
	* 이미 방에 들어가 있는 경우 중복 방지
	* 참가 가능한 방을 탐색 후, 방이 존재하지 않으면 방 생성
	* 이후 현재 세션으로 방에 참가
	*/
	case PKT_JOIN_ROOM: {
		if (s->room_id >= 0)
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
		if (s->room_id < 0)
			break;

		room_t* r = room_get(s->room_id);
		if (!r)
			break;
		room_broadcast(r, s, pkt);
		break;
	}

	/*
	* 방 퇴장
	* room_leave 함수를 통해 세션의 room_id 갱신 및 방 목록 정리를 수행
	*/
	case PKT_LEAVE_ROOM: {
		if (s->room_id < 0)
			break;

		room_leave(s);
		break;
	}

	default:
		break;
	}
}

static void handle_disconnect(int fd) {
	session_t* s = session_get(fd);

	/* 이미 정리됐거나, 세션 생성 전 끊긴 경우 */
	if (!s) {
		return;
	}

	printf("[LOGIC] fd=%d disconnect event\n", fd);

	/* 방에 들어가 있었다면 방에서 제거 */
	if (s->room_id >= 0) {
		room_leave(s);
	}

	/* 세션 제거 */
	session_remove(fd);
}

static void handle_shutdown(void)
{
	printf("[LOGIC] graceful shutdown started\n");

	/*
	* fd 순회를 통해 존재하는 세션을 찾아 제거함
	* 방에 존재하는 세션들은 먼저 room_leave 처리 후 세션 제거
	*/
	for (int fd = 0; fd < MAX_CLIENTS; ++fd) {
		session_t* s = session_get(fd);
		if (!s)
			continue;

		if (s->room_id >= 0) {
			room_leave(s);
		}

		session_remove(fd);
	}

	printf("[LOGIC] graceful shutdown completed\n");
}