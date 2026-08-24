#include "state.h"
#include "job_queue.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
* 세션 테이블: 더 이상 fd로 인덱싱하지 않음 (fd는 커널이 우리 통제 밖에서 재사용하는 값이라
* 신원으로 쓰면 안 됨). 그냥 빈 슬롯을 찾아 쓰는 배열 + session_id로 조회하는 방식으로 바꿈
*/
static session_t* sessions[MAX_CLIENTS];
static int next_session_id = 1;

/* 방 관련 데이터: room_count는 "한 번도 안 쓴 새 슬롯"의 워터마크, room_free_list는 반납된 슬롯 재사용용 */
static room_t rooms[MAX_ROOMS];
static int room_count = 0;
static int room_free_list[MAX_ROOMS];
static int room_free_top = 0;

/* 세션 테이블, 방 테이블에 대한 mutex (개별 세션/방 자신의 lock과는 별도) */
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rooms_lock = PTHREAD_MUTEX_INITIALIZER;

extern job_queue_t g_io_q;
extern void net_wakeup(void);

/* ============================ Session ============================ */

/*
* 세션을 생성하는 함수
* net 스레드가 accept 직후 바로 호출함(예전처럼 첫 패킷이 올 때까지 늦추지 않음) ->
* disconnect가 패킷 한 번 못 받고 발생해도 session_id가 이미 있어야 job에 실을 수 있기 때문
*/
session_t* session_create(void)
{
	session_t* s = malloc(sizeof(session_t));
	if (!s) return NULL;

	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	s->room_id = -1;
	s->alive = true;
	s->refcount = 1;   /* 테이블에 등록된 상태를 나타내는 기준 참조. session_remove_by_id가 반환함 */

	pthread_mutex_lock(&g_sessions_lock);

	s->session_id = next_session_id++;

	/* 빈 슬롯을 찾아 등록. MAX_CLIENTS는 동시 접속 가능한 fd 수의 상한이므로 이론상 항상 찾아짐 */
	int slot = -1;
	for (int i = 0; i < MAX_CLIENTS; ++i) {
		if (!sessions[i]) { slot = i; break; }
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_sessions_lock);
		pthread_mutex_destroy(&s->lock);
		free(s);
		return NULL;
	}
	sessions[slot] = s;

	pthread_mutex_unlock(&g_sessions_lock);

	log_json("INFO", "session_created", "session_id", LOG_ARG_INT, s->session_id, NULL);
	return s;
}

/*
* session_id로 세션을 조회하는 함수
* 찾으면 참조 카운트를 +1 하고 반환 -> 호출부는 다 쓴 뒤 반드시 session_release()를 호출해야 함
* (이 참조가 살아있는 동안은 다른 스레드가 session_remove_by_id를 호출해도 실제 free는 미뤄짐)
*/
session_t* session_get_by_id(int session_id)
{
	pthread_mutex_lock(&g_sessions_lock);

	session_t* found = NULL;
	for (int i = 0; i < MAX_CLIENTS; ++i) {
		if (sessions[i] && sessions[i]->session_id == session_id) {
			found = sessions[i];
			break;
		}
	}

	if (found) {
		pthread_mutex_lock(&found->lock);
		found->refcount++;
		pthread_mutex_unlock(&found->lock);
	}

	pthread_mutex_unlock(&g_sessions_lock);
	return found;
}

/* 참조 카운트를 +1 하는 함수 (session_get_by_id가 내부적으로 쓰고, 필요하면 외부에서도 명시적으로 호출 가능) */
void session_acquire(session_t* s)
{
	if (!s) return;
	pthread_mutex_lock(&s->lock);
	s->refcount++;
	pthread_mutex_unlock(&s->lock);
}

/* 참조 카운트를 -1 하고, 0이 되는 순간에만 실제로 메모리를 해제하는 함수 */
void session_release(session_t* s)
{
	if (!s) return;

	pthread_mutex_lock(&s->lock);
	s->refcount--;
	bool should_free = (s->refcount == 0);
	pthread_mutex_unlock(&s->lock);

	if (should_free) {
		pthread_mutex_destroy(&s->lock);
		free(s);
	}
}

/* alive 플래그를 락 보호 하에 안전하게 읽는 함수 */
bool session_is_alive(session_t* s)
{
	if (!s) return false;
	pthread_mutex_lock(&s->lock);
	bool alive = s->alive;
	pthread_mutex_unlock(&s->lock);
	return alive;
}

/* room_id를 락 보호 하에 안전하게 읽는 함수 */
int session_get_room_id(session_t* s)
{
	if (!s) return -1;
	pthread_mutex_lock(&s->lock);
	int room_id = s->room_id;
	pthread_mutex_unlock(&s->lock);
	return room_id;
}

/*
* alive를 false로 내리고, 그 순간의 room_id를 원자적으로(같은 lock 구간 안에서) 함께 반환하는 함수
*
* 왜 필요한가: disconnect 처리가 "room_id를 읽어서 나갈지 결정" -> "alive를 내림"을 두 단계로 나눠서 하면,
* 그 사이(=lock이 풀린 틈)에 다른 worker가 처리 중인 JOB_PACKET(JOIN_ROOM)이 끼어들어 이 세션을
* room에 추가할 수 있음 -> disconnect는 이미 "room_id가 -1"이라고 (낡은 값으로) 판단해 room_leave를
* 안 부르고 세션을 지워버리는데, 그 직후 join이 뒤늦게 성공해 이미 지워진(free 예정인) 세션 포인터를
* room->users[]에 남겨버림 -> room_broadcast가 나중에 그 죽은 포인터를 참조(use-after-free)
*
* room_id를 읽는 시점과 alive를 내리는 시점을 하나의 lock 구간으로 묶고, room_join도 같은 lock 아래서
* "alive인지 확인 후에만 추가"하도록 하면, 두 함수 중 어느 쪽이 먼저 lock을 잡든 결과가 항상 일관됨
*/
int session_deactivate(session_t* s)
{
	if (!s) return -1;
	pthread_mutex_lock(&s->lock);
	s->alive = false;
	int room_id = s->room_id;
	pthread_mutex_unlock(&s->lock);
	return room_id;
}

/*
* session_id로 세션을 테이블에서 제거하는 함수
* 제거 즉시 session_get_by_id는 더 이상 이 세션을 찾지 못함(발견 가능성을 먼저 끊음)
* 그 다음 alive를 false로 내리고, 생성 시 잡아둔 기준 참조를 반환함(다른 참조가 남아있으면 여기선 free 안 됨)
*/
void session_remove_by_id(int session_id)
{
	pthread_mutex_lock(&g_sessions_lock);

	session_t* found = NULL;
	int slot = -1;
	for (int i = 0; i < MAX_CLIENTS; ++i) {
		if (sessions[i] && sessions[i]->session_id == session_id) {
			found = sessions[i];
			slot = i;
			break;
		}
	}
	if (!found) {
		pthread_mutex_unlock(&g_sessions_lock);
		return;
	}
	sessions[slot] = NULL;

	pthread_mutex_unlock(&g_sessions_lock);

	pthread_mutex_lock(&found->lock);
	found->alive = false;
	pthread_mutex_unlock(&found->lock);

	log_json("INFO", "session_removed", "session_id", LOG_ARG_INT, session_id, NULL);

	session_release(found);
}

/* 서버 종료 시 남아있는 모든 세션을 방에서 빼고 정리하는 함수 */
void session_remove_all(void)
{
	for (;;) {
		pthread_mutex_lock(&g_sessions_lock);

		session_t* s = NULL;
		for (int i = 0; i < MAX_CLIENTS; ++i) {
			if (sessions[i]) { s = sessions[i]; break; }
		}
		if (!s) {
			pthread_mutex_unlock(&g_sessions_lock);
			break;
		}

		int session_id = s->session_id;
		pthread_mutex_lock(&s->lock);
		s->refcount++;   /* 아래에서 쓰는 동안 유효하도록 임시 참조 확보 */
		pthread_mutex_unlock(&s->lock);

		pthread_mutex_unlock(&g_sessions_lock);

		int room_id = session_deactivate(s);
		if (room_id >= 0) {
			room_leave(s);
		}
		session_release(s);              /* 위에서 잡은 임시 참조 반환 */
		session_remove_by_id(session_id); /* 테이블 제거 + 기준 참조 반환(refcount 0이면 실제 free) */
	}
}

/* ============================ Room ============================ */

/*
* 방을 생성하는 함수
* 반납된(비어서 회수된) 슬롯이 있으면 그걸 먼저 재사용하고, 없으면 새 슬롯을 씀
*/
room_t* room_create(void)
{
	pthread_mutex_lock(&g_rooms_lock);

	int idx;
	bool reused;
	if (room_free_top > 0) {
		idx = room_free_list[--room_free_top];
		reused = true;
	}
	else if (room_count < MAX_ROOMS) {
		idx = room_count++;
		reused = false;
	}
	else {
		pthread_mutex_unlock(&g_rooms_lock);
		return NULL;
	}

	room_t* r = &rooms[idx];
	if (reused) {
		/*
		* 이 슬롯의 mutex는 이전 생애주기에서 이미 init된 채로 계속 살아있으므로 그대로 재사용함
		* (destroy 후 재init하면, 그 사이 다른 스레드가 마침 이 mutex를 lock하려던 참이었을 경우와
		* 경쟁할 위험이 있어 destroy 자체를 하지 않는 쪽을 택함 - 파괴하지 않는 대신 필드만 초기화)
		*
		* users[]/user_count는 room->lock으로 보호되는 필드라서(room_join/room_leave/room_broadcast가
		* 그렇게 접근함), 여기서도 room->lock을 잡고 초기화해야 함. g_rooms_lock만으로는 room_broadcast처럼
		* room->lock만 쥐고 접근하는 쪽과 동기화가 안 됨(TSan이 실제로 이 레이스를 잡아냄)
		*/
		pthread_mutex_lock(&r->lock);
		memset(r->users, 0, sizeof(r->users));
		r->user_count = 0;
		pthread_mutex_unlock(&r->lock);
	}
	else {
		memset(r, 0, sizeof(*r));
		pthread_mutex_init(&r->lock, NULL);
	}
	r->room_id = idx;
	r->live = true;

	pthread_mutex_unlock(&g_rooms_lock);

	/*
	* r->room_id 대신 이미 알고 있는 idx를 그대로 씀 -> g_rooms_lock을 놓은 뒤에 r->room_id를 다시 읽으면
	* 그 사이 이 슬롯이 다른 스레드에 의해 또 reclaim/재사용되면서 값이 바뀌는 것과 레이스가 날 수 있음
	*/
	log_json("INFO", "room_created", "room_id", LOG_ARG_INT, idx, "reused", LOG_ARG_INT, reused ? 1 : 0, NULL);
	return r;
}

/* 방 정보를 가져오는 함수 */
room_t* room_get(int room_id)
{
	/* room_count는 여러 스레드에서 동시에 변경될 수 있으므로 mutex로 보호 */
	pthread_mutex_lock(&g_rooms_lock);
	int max = room_count;
	pthread_mutex_unlock(&g_rooms_lock);

	/* 유효하지 않은 room_id의 경우 NULL 반환 */
	if (room_id < 0 || room_id >= max)
		return NULL;

	return &rooms[room_id];
}

/* 방을 조회하는 함수 (인원 여유가 있는 살아있는 방 하나를 찾음) */
room_t* room_find(void)
{
	pthread_mutex_lock(&g_rooms_lock);
	for (int i = 0; i < room_count; i++) {
		/* live가 아닌 슬롯(회수되어 재사용 대기 중)은 후보에서 제외 */
		if (!rooms[i].live) continue;

		/* user_count는 room->lock으로 보호되는 필드이므로, g_rooms_lock만으로 읽으면 안 됨(TSan이 레이스로 잡음) */
		pthread_mutex_lock(&rooms[i].lock);
		bool has_space = rooms[i].user_count < MAX_ROOM_USER;
		pthread_mutex_unlock(&rooms[i].lock);

		if (has_space) {
			room_t* r = &rooms[i];
			pthread_mutex_unlock(&g_rooms_lock);
			return r;
		}
	}
	pthread_mutex_unlock(&g_rooms_lock);
	return NULL;
}

/*
* 방에 입장하는 함수
* room_find/room_create가 반환한 포인터를 받아 쓰는 시점 사이에 그 방이 다른 스레드에 의해
* 회수됐을 수 있으므로, 실제로 인원을 추가하기 직전에 g_rooms_lock 하에 live를 한 번 더 확인함
*
* "정말로 추가해도 되는지(alive)"와 "실제로 추가 + room_id 기록"을 s->lock 하나의 구간으로 묶어서,
* session_deactivate()(disconnect 처리)와 어느 쪽이 먼저 실행되든 항상 일관된 결과가 나오게 함
* (자세한 이유는 session_deactivate()의 주석 참고)
*/
void room_join(room_t* room, session_t* s)
{
	if (!room || !s) return;

	pthread_mutex_lock(&g_rooms_lock);
	if (!room->live) {
		pthread_mutex_unlock(&g_rooms_lock);
		return;
	}
	pthread_mutex_lock(&room->lock);
	pthread_mutex_unlock(&g_rooms_lock);

	pthread_mutex_lock(&s->lock);

	/* 그 사이 세션이 이미 끊긴 상태라면(disconnect가 먼저 alive를 내렸다면) 추가하지 않음 */
	if (!s->alive) {
		pthread_mutex_unlock(&s->lock);
		pthread_mutex_unlock(&room->lock);
		return;
	}

	/* 이미 세션에 방에 존재하면 무시(중복 추가 방지) */
	bool already_in = false;
	for (int i = 0; i < room->user_count; i++) {
		if (room->users[i] == s) { already_in = true; break; }
	}

	/* 방의 유저 수가 방의 최대 인원보다 많은 경우에도 무시 */
	if (already_in || room->user_count >= MAX_ROOM_USER) {
		pthread_mutex_unlock(&s->lock);
		pthread_mutex_unlock(&room->lock);
		return;
	}

	/*
	* 현재 방에 현재 세션(인원)을 추가 후 인원 수 증가
	* 현재 세션의 room_id를 현재 방의 room_id로 저장
	*/
	room->users[room->user_count++] = s;
	s->room_id = room->room_id;

	pthread_mutex_unlock(&s->lock);

	log_json("INFO", "room_joined", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room->room_id, NULL);

	pthread_mutex_unlock(&room->lock);
}

/*
* 방에서 떠나는 함수
* room_id는 s->lock 하에 읽고, 마지막에 -1로 되돌리는 것도 s->lock 하에 함
* (room_join도 room_id를 s->lock 아래서 설정하므로, 이렇게 해야 둘 사이의 순서가 항상 일관되게 결정됨)
*
* 인원이 0이 되면 그 자리에서(g_rooms_lock을 쥔 채로) live를 내리고 free list에 반납함
* -> 방금 인원이 빠져 0이 됐는지 확인하는 시점과 반납하는 시점 사이에 다른 스레드가
*    room_join으로 끼어드는 걸 막기 위해, 판단과 반납을 하나의 g_rooms_lock 구간 안에서 함께 처리함
*/
void room_leave(session_t* s)
{
	if (!s) return;

	pthread_mutex_lock(&s->lock);
	int room_id = s->room_id;
	pthread_mutex_unlock(&s->lock);

	if (room_id < 0 || room_id >= MAX_ROOMS) return;

	pthread_mutex_lock(&g_rooms_lock);

	room_t* room = &rooms[room_id];
	if (!room->live) {
		/* 이미 회수된 방이면 세션의 소속 정보만 정리 */
		pthread_mutex_unlock(&g_rooms_lock);
		pthread_mutex_lock(&s->lock);
		if (s->room_id == room_id) s->room_id = -1;
		pthread_mutex_unlock(&s->lock);
		return;
	}

	pthread_mutex_lock(&room->lock);

	/* 반복문을 돌며 현재 세션이 존재하는 방을 탐색
	* 제거할 자리를 마지막 사용자로 덮어써 배열 유지
	* 마지막 칸은 더 이상 사용하지 않으므로 NULL로 변경
	* 이후 방의 인원수 감소
	*/
	for (int i = 0; i < room->user_count; i++) {
		if (room->users[i] == s) {
			room->users[i] = room->users[room->user_count - 1];
			room->users[room->user_count - 1] = NULL;
			room->user_count--;
			break;
		}
	}

	/* 인원이 다 빠졌으면 이 슬롯을 회수해서 다음 room_create()가 재사용할 수 있게 함 */
	bool now_empty = (room->user_count == 0);
	if (now_empty) {
		room->live = false;
		room_free_list[room_free_top++] = room_id;
	}

	pthread_mutex_unlock(&room->lock);
	pthread_mutex_unlock(&g_rooms_lock);

	log_json("INFO", "room_left", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, "reclaimed", LOG_ARG_INT, now_empty ? 1 : 0, NULL);

	pthread_mutex_lock(&s->lock);
	if (s->room_id == room_id) s->room_id = -1;
	pthread_mutex_unlock(&s->lock);
}

/* 방에 채팅을 전파하는 함수 */
void room_broadcast(room_t* room, session_t* sender, packet_t* pkt)
{
	if (!room || !pkt) return;

	/*
	* 전송 대상 session_id 목록을 임시로 저장 (fd가 아님!)
	* room->lock을 잡은 상태에서 직접 send하지 않기 위해 사용
	* 실제 fd는 net 스레드가 SEND 작업을 처리하는 시점에 session_id로 다시 조회함 ->
	* 그 사이 대상이 끊기고 그 fd가 다른 사람 것이 됐어도 엉뚱한 곳으로 보내지 않음
	*/
	int target_ids[MAX_ROOM_USER];
	int count = 0;

	/* 송신자를 제외하기 위한 session_id (없으면 -1) */
	int except_id = sender ? sender->session_id : -1;

	pthread_mutex_lock(&room->lock);
	for (int i = 0; i < room->user_count; ++i) {
		session_t* s = room->users[i];
		if (!s) continue;
		if (!session_is_alive(s)) continue;
		if (s->session_id == except_id) continue;
		target_ids[count++] = s->session_id;
	}
	pthread_mutex_unlock(&room->lock);

	/*
	* pkt->length는 (type + payload)의 길이
	* payload의 길이가 최대 패킷길이보다 긴 경우 최대 패킷길이로 고정
	*/
	int payload_len = (int)pkt->length - 2;
	if (payload_len <= 0) return;
	if (payload_len > MAX_PACKET_SIZE) payload_len = MAX_PACKET_SIZE;

	/* 브로드캐스트용 출력 패킷 생성 */
	packet_t out;
	memset(&out, 0, sizeof(out));

	/*
	* payload를 안전하게 복사하며 개행 추가
	* snprintf를 사용해 버퍼 오버플로 방지
	*/
	int n = snprintf(out.payload, MAX_PACKET_SIZE, "%.*s\n", payload_len, pkt->payload);
	if (n <= 0 || n >= MAX_PACKET_SIZE)
		return;

	/*
	* 채팅 패킷 타입 설정
	* 전체 패킷 길이 = type(2바이트) + payload 길이
	*/
	out.type = PKT_CHAT;
	out.length = 2 + (uint16_t)n;

	/* 수집된 session_id 목록을 기반으로 각 대상에게 SEND 작업을 IO 큐에 등록 */
	for (int i = 0; i < count; ++i) {
		job_queue_push_send(&g_io_q, target_ids[i], &out);
	}

	/* IO 스레드를 깨워 큐에 쌓인 작업 처리 유도 */
	net_wakeup();
}

/* ============================ Metrics ============================ */

/* 현재 활성 세션 수를 세는 함수 (메트릭 노출용) */
int state_count_active_sessions(void)
{
	int count = 0;

	pthread_mutex_lock(&g_sessions_lock);
	for (int i = 0; i < MAX_CLIENTS; ++i) {
		if (sessions[i]) count++;
	}
	pthread_mutex_unlock(&g_sessions_lock);

	return count;
}

/* 인원이 1명 이상인 방의 수를 세는 함수 (메트릭 노출용) */
int state_count_active_rooms(void)
{
	int count = 0;

	pthread_mutex_lock(&g_rooms_lock);
	for (int i = 0; i < MAX_ROOMS; ++i) {
		if (!rooms[i].live) continue;
		pthread_mutex_lock(&rooms[i].lock);
		bool occupied = rooms[i].user_count > 0;
		pthread_mutex_unlock(&rooms[i].lock);
		if (occupied) count++;
	}
	pthread_mutex_unlock(&g_rooms_lock);

	return count;
}
