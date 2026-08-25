#include "state.h"
#include "job_queue.h"
#include "log.h"
#include "posix_lock.hpp"

#include <cstring>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

/* net.c/main.c(순수 C)에 정의된 심볼이라 C 링키지로 선언해야 링크가 됨 */
extern "C" {
	extern job_queue_t g_io_q;
	void net_wakeup(void);
}

/*
* 세션 수명 관리: std::shared_ptr가 실제 참조 카운트를 대신함(REDESIGN.md §C++ 도입 참고)
*
* g_sessions: 세션의 "기준 소유권". session_create()가 여기 하나를 등록하고,
*             session_remove_by_id()가 여기서 지우면 그게 기준 참조 반환.
* g_extra_refs: session_get_by_id()가 호출될 때마다 shared_ptr 사본을 하나 더 만들어 여기 보관.
*               C 콜사이트는 raw session_t*만 주고받을 수 있으므로, "이 raw pointer에 대응하는
*               shared_ptr 사본을 하나 더 들고 있다가, session_release(raw pointer)가 오면
*               정확히 그 사본 하나를 지운다"는 다리 역할. 마지막 사본이 사라지는 순간
*               shared_ptr의 소멸자가 자동으로 session_t를 정리함 -> "0이면 free" 같은 코드를
*               우리가 직접 쓸 필요가 없음(그게 바로 shared_ptr을 쓰는 이유)
*/
static std::unordered_map<int, std::shared_ptr<session_t>> g_sessions;
static std::unordered_multimap<session_t*, std::shared_ptr<session_t>> g_extra_refs;
static int next_session_id = 1;
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;

/* 방 관련 데이터. rooms 배열 자체는 그대로 고정 배열(세션과 달리 방은 raw pointer를 오래 들고 있지 않고
* 매번 room_get()으로 다시 조회하므로 shared_ptr이 필요 없음). free list만 std::vector로 교체 */
static room_t rooms[MAX_ROOMS];
static int room_count = 0;
static std::vector<int> room_free_list;
static pthread_mutex_t g_rooms_lock = PTHREAD_MUTEX_INITIALIZER;

/* g_sessions_lock을 이미 쥔 상태에서, raw pointer에 대응하는 shared_ptr 사본을 하나 더 만들어 등록 */
static void add_extra_ref_locked(session_t* s)
{
	auto sit = g_sessions.find(s->session_id);
	if (sit != g_sessions.end() && sit->second.get() == s) {
		g_extra_refs.emplace(s, sit->second);
		return;
	}
	auto eit = g_extra_refs.find(s);
	if (eit != g_extra_refs.end()) {
		g_extra_refs.emplace(s, eit->second);
	}
	/* 둘 다 없으면 이미 완전히 정리된 세션에 대한 호출 - 방어적으로 무시 */
}

/* ============================ Session ============================ */

/*
* 세션을 생성하는 함수
* net 스레드가 accept 직후 바로 호출함(예전처럼 첫 패킷이 올 때까지 늦추지 않음) ->
* disconnect가 패킷 한 번 못 받고 발생해도 session_id가 이미 있어야 job에 실을 수 있기 때문
*/
session_t* session_create(void)
{
	session_t* raw = new session_t();
	memset(raw, 0, sizeof(*raw));
	pthread_mutex_init(&raw->lock, nullptr);
	raw->room_id = -1;
	raw->alive = true;

	/* refcount가 0이 되는 순간(모든 shared_ptr 사본이 사라지는 순간) 이 deleter가 자동으로 불림 */
	auto deleter = [](session_t* s) {
		pthread_mutex_destroy(&s->lock);
		delete s;
	};
	std::shared_ptr<session_t> sp(raw, deleter);

	{
		PosixLockGuard lock(g_sessions_lock);
		raw->session_id = next_session_id++;
		g_sessions.emplace(raw->session_id, sp);
	}

	log_json("INFO", "session_created", "session_id", LOG_ARG_INT, raw->session_id, NULL);
	return raw;
}

/*
* session_id로 세션을 조회하는 함수
* 찾으면 shared_ptr 사본을 하나 더 만들어 g_extra_refs에 보관하고 raw pointer를 반환함 ->
* 호출부는 다 쓴 뒤 반드시 session_release()를 호출해야 이 사본이 반환됨
* (이 사본이 살아있는 동안은, 다른 스레드가 session_remove_by_id를 호출해도 실제 파괴는 미뤄짐)
*/
session_t* session_get_by_id(int session_id)
{
	PosixLockGuard lock(g_sessions_lock);

	auto it = g_sessions.find(session_id);
	if (it == g_sessions.end()) return nullptr;

	g_extra_refs.emplace(it->second.get(), it->second);
	return it->second.get();
}

/* 참조를 하나 더 잡아두는 함수 (이미 raw pointer를 들고 있는 상태에서 추가로 하나 더 필요할 때) */
void session_acquire(session_t* s)
{
	if (!s) return;
	PosixLockGuard lock(g_sessions_lock);
	add_extra_ref_locked(s);
}

/* session_get_by_id()로 확보했던 shared_ptr 사본을 하나 반환하는 함수 */
void session_release(session_t* s)
{
	if (!s) return;
	PosixLockGuard lock(g_sessions_lock);

	auto it = g_extra_refs.find(s);
	if (it != g_extra_refs.end()) {
		g_extra_refs.erase(it);
	}
	/* 이 erase가 마지막 shared_ptr 사본이었다면, 여기서 곧바로(락을 쥔 채로) session_t가 파괴됨 */
}

/* alive 플래그를 락 보호 하에 안전하게 읽는 함수 */
bool session_is_alive(session_t* s)
{
	if (!s) return false;
	PosixLockGuard lock(s->lock);
	return s->alive;
}

/* room_id를 락 보호 하에 안전하게 읽는 함수 */
int session_get_room_id(session_t* s)
{
	if (!s) return -1;
	PosixLockGuard lock(s->lock);
	return s->room_id;
}

/*
* alive를 false로 내리고, 그 순간의 room_id를 원자적으로(같은 lock 구간 안에서) 함께 반환하는 함수
*
* 왜 필요한가: disconnect 처리가 "room_id를 읽어서 나갈지 결정" -> "alive를 내림"을 두 단계로 나눠서 하면,
* 그 사이(=lock이 풀린 틈)에 다른 worker가 처리 중인 JOB_PACKET(JOIN_ROOM)이 끼어들어 이 세션을
* room에 추가할 수 있음 -> disconnect는 이미 "room_id가 -1"이라고(낡은 값으로) 판단해 room_leave를
* 안 부르고 세션을 지워버리는데, 그 직후 join이 뒤늦게 성공해 이미 지워진(파괴 예정인) 세션 포인터를
* room->users[]에 남겨버림 -> room_broadcast가 나중에 그 죽은 포인터를 참조(use-after-free)
*
* room_id를 읽는 시점과 alive를 내리는 시점을 하나의 lock 구간으로 묶고, room_join도 같은 lock 아래서
* "alive인지 확인 후에만 추가"하도록 하면, 두 함수 중 어느 쪽이 먼저 lock을 잡든 결과가 항상 일관됨
*/
int session_deactivate(session_t* s)
{
	if (!s) return -1;
	PosixLockGuard lock(s->lock);
	s->alive = false;
	return s->room_id;
}

/*
* session_id로 세션을 테이블에서 제거하는 함수
* 기준 참조(session_create가 등록한 shared_ptr)를 반환함 -> 다른 사본(g_extra_refs)이 남아있으면
* 거기서 refcount가 0이 될 때까지 실제 파괴는 미뤄짐
*/
void session_remove_by_id(int session_id)
{
	std::shared_ptr<session_t> keep_alive;   /* 이 함수가 끝날 때까지는 파괴되지 않도록 로컬로 사본을 하나 더 들고 있음 */
	session_t* raw = nullptr;

	{
		PosixLockGuard lock(g_sessions_lock);
		auto it = g_sessions.find(session_id);
		if (it == g_sessions.end()) return;
		raw = it->second.get();
		keep_alive = it->second;
		g_sessions.erase(it);   /* 기준 참조만 반환. keep_alive가 있어 아직 파괴되지 않음 */
	}

	{
		PosixLockGuard lock(raw->lock);
		raw->alive = false;
	}

	log_json("INFO", "session_removed", "session_id", LOG_ARG_INT, session_id, NULL);

	/* keep_alive가 이 함수를 빠져나가며 소멸함. 그게 진짜 마지막 사본이었다면 여기서 자동으로 정리됨 */
}

/* 서버 종료 시 남아있는 모든 세션을 방에서 빼고 정리하는 함수 */
void session_remove_all(void)
{
	for (;;) {
		int session_id;
		{
			PosixLockGuard lock(g_sessions_lock);
			if (g_sessions.empty()) return;
			session_id = g_sessions.begin()->first;
		}

		session_t* s = session_get_by_id(session_id);   /* 임시 참조 확보 */
		if (!s) continue;   /* 그 사이 다른 경로로 이미 지워졌으면 다음으로 */

		int room_id = session_deactivate(s);
		if (room_id >= 0) {
			room_leave(s);
		}
		session_release(s);              /* 위에서 확보한 임시 참조 반환 */
		session_remove_by_id(session_id); /* 테이블 제거 + 기준 참조 반환 */
	}
}

/* ============================ Room ============================ */

/*
* 방을 생성하는 함수
* 반납된(비어서 회수된) 슬롯이 있으면 그걸 먼저 재사용하고, 없으면 새 슬롯을 씀
*/
room_t* room_create(void)
{
	int idx;
	bool reused;

	{
		PosixLockGuard lock(g_rooms_lock);

		if (!room_free_list.empty()) {
			idx = room_free_list.back();
			room_free_list.pop_back();
			reused = true;
		}
		else if (room_count < MAX_ROOMS) {
			idx = room_count++;
			reused = false;
		}
		else {
			return nullptr;
		}

		room_t* r = &rooms[idx];
		if (reused) {
			/*
			* 이 슬롯의 mutex는 이전 생애주기에서 이미 init된 채로 계속 살아있으므로 그대로 재사용함
			* (destroy 후 재init하면, 그 사이 다른 스레드가 마침 이 mutex를 lock하려던 참이었을 경우와
			* 경쟁할 위험이 있어 destroy 자체를 하지 않는 쪽을 택함 - 파괴하지 않는 대신 필드만 초기화)
			*
			* users[]/user_count는 room->lock으로 보호되는 필드라서(room_join/room_leave/room_broadcast가
			* 그렇게 접근함), 여기서도 room->lock을 잡고 초기화해야 함
			*/
			PosixLockGuard room_lock(r->lock);
			memset(r->users, 0, sizeof(r->users));
			r->user_count = 0;
		}
		else {
			memset(r, 0, sizeof(*r));
			pthread_mutex_init(&r->lock, nullptr);
		}
		r->room_id = idx;
		r->live = true;
	} /* g_rooms_lock 해제 */

	log_json("INFO", "room_created", "room_id", LOG_ARG_INT, idx, "reused", LOG_ARG_INT, reused ? 1 : 0, NULL);
	return &rooms[idx];
}

/* 방 정보를 가져오는 함수 */
room_t* room_get(int room_id)
{
	int max;
	{
		PosixLockGuard lock(g_rooms_lock);
		max = room_count;
	}

	if (room_id < 0 || room_id >= max)
		return nullptr;

	return &rooms[room_id];
}

/* 방을 조회하는 함수 (인원 여유가 있는 살아있는 방 하나를 찾음) */
room_t* room_find(void)
{
	PosixLockGuard lock(g_rooms_lock);

	for (int i = 0; i < room_count; i++) {
		if (!rooms[i].live) continue;

		/* user_count는 room->lock으로 보호되는 필드이므로, g_rooms_lock만으로 읽으면 안 됨 */
		PosixLockGuard room_lock(rooms[i].lock);
		if (rooms[i].user_count < MAX_ROOM_USER) {
			return &rooms[i];
		}
	}
	return nullptr;
}

/*
* 방에 입장하는 함수
* room_find/room_create가 반환한 포인터를 받아 쓰는 시점 사이에 그 방이 다른 스레드에 의해
* 회수됐을 수 있으므로, 실제로 인원을 추가하기 직전에 g_rooms_lock 하에 live를 한 번 더 확인함
*
* "정말로 추가해도 되는지(alive)"와 "실제로 추가 + room_id 기록"을 s->lock 하나의 구간으로 묶어서,
* session_deactivate()(disconnect 처리)와 어느 쪽이 먼저 실행되든 항상 일관된 결과가 나오게 함
*/
void room_join(room_t* room, session_t* s)
{
	if (!room || !s) return;

	bool joined = false;
	int joined_room_id = -1;   /* room->room_id를 락 해제 후에 다시 읽지 않기 위해 락 안에서 로컬로 캡처 */
	{
		PosixLockGuard rooms_lock(g_rooms_lock);
		if (!room->live) return;

		PosixLockGuard room_lock(room->lock);
		PosixLockGuard session_lock(s->lock);

		if (s->alive) {
			bool already_in = false;
			for (int i = 0; i < room->user_count; i++) {
				if (room->users[i] == s) { already_in = true; break; }
			}

			if (!already_in && room->user_count < MAX_ROOM_USER) {
				room->users[room->user_count++] = s;
				s->room_id = room->room_id;
				joined = true;
				joined_room_id = room->room_id;
			}
		}
	} /* session_lock -> room_lock -> rooms_lock 역순으로 전부 해제.
	   * 이 시점부터는 room이 가리키던 슬롯이 다른 스레드에 의해 재사용/파괴될 수 있으므로
	   * room->room_id를 다시 읽으면 안 됨 - 위에서 캡처해둔 joined_room_id만 사용 */

	if (joined) {
		log_json("INFO", "room_joined", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, joined_room_id, NULL);
	}
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

	int room_id;
	{
		PosixLockGuard lock(s->lock);
		room_id = s->room_id;
	}
	if (room_id < 0 || room_id >= MAX_ROOMS) return;

	bool did_leave = false;
	bool now_empty = false;
	{
		PosixLockGuard rooms_lock(g_rooms_lock);
		room_t* room = &rooms[room_id];

		if (!room->live) {
			/* 이미 회수된 방이면 세션의 소속 정보만 정리 */
			PosixLockGuard session_lock(s->lock);
			if (s->room_id == room_id) s->room_id = -1;
			return;
		}

		PosixLockGuard room_lock(room->lock);

		for (int i = 0; i < room->user_count; i++) {
			if (room->users[i] == s) {
				room->users[i] = room->users[room->user_count - 1];
				room->users[room->user_count - 1] = nullptr;
				room->user_count--;
				did_leave = true;
				break;
			}
		}

		now_empty = (room->user_count == 0);
		if (now_empty) {
			room->live = false;
			room_free_list.push_back(room_id);
		}
	} /* room_lock, rooms_lock 해제 */

	{
		PosixLockGuard session_lock(s->lock);
		if (s->room_id == room_id) s->room_id = -1;
	}

	if (did_leave) {
		log_json("INFO", "room_left", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, "reclaimed", LOG_ARG_INT, now_empty ? 1 : 0, NULL);
	}
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
	int except_id = sender ? sender->session_id : -1;

	{
		PosixLockGuard lock(room->lock);
		for (int i = 0; i < room->user_count; ++i) {
			session_t* s = room->users[i];
			if (!s) continue;
			if (!session_is_alive(s)) continue;
			if (s->session_id == except_id) continue;
			target_ids[count++] = s->session_id;
		}
	}

	/*
	* pkt->length는 (type + payload)의 길이
	* payload의 길이가 최대 패킷길이보다 긴 경우 최대 패킷길이로 고정
	*/
	int payload_len = (int)pkt->length - 2;
	if (payload_len <= 0) return;
	if (payload_len > MAX_PACKET_SIZE) payload_len = MAX_PACKET_SIZE;

	packet_t out;
	memset(&out, 0, sizeof(out));

	int n = snprintf(out.payload, MAX_PACKET_SIZE, "%.*s\n", payload_len, pkt->payload);
	if (n <= 0 || n >= MAX_PACKET_SIZE)
		return;

	out.type = PKT_CHAT;
	out.length = 2 + (uint16_t)n;

	for (int i = 0; i < count; ++i) {
		job_queue_push_send(&g_io_q, target_ids[i], &out);
	}

	net_wakeup();
}

/* ============================ Metrics ============================ */

/* 현재 활성 세션 수를 세는 함수 (메트릭 노출용) */
int state_count_active_sessions(void)
{
	PosixLockGuard lock(g_sessions_lock);
	return (int)g_sessions.size();
}

/* 인원이 1명 이상인 방의 수를 세는 함수 (메트릭 노출용) */
int state_count_active_rooms(void)
{
	PosixLockGuard lock(g_rooms_lock);

	int count = 0;
	for (int i = 0; i < room_count; ++i) {
		if (!rooms[i].live) continue;
		PosixLockGuard room_lock(rooms[i].lock);
		if (rooms[i].user_count > 0) count++;
	}
	return count;
}
