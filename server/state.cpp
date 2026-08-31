#include "state.h"
#include "job_queue.h"
#include "log.h"
#include "posix_lock.hpp"

#include <cstring>
#include <cstdio>
#include <memory>
#include <unordered_map>

/* net.c/main.c(순수 C)에 정의된 심볼이라 C 링키지로 선언해야 링크가 됨 */
extern "C" {
	extern job_queue_t g_io_q;
	void net_wakeup(void);
	int redis_leave_room(int session_id, int room_id);   /* redis_client.cpp (3단계) */
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

/*
* 방 관련 데이터. rooms 배열 자체는 그대로 고정 배열(세션과 달리 방은 raw pointer를 오래 들고 있지 않고
* 매번 room_get()으로 다시 조회하므로 shared_ptr이 필요 없음).
* 3단계: 어느 room_id가 비어있는지/누가 다음 채번인지는 이제 Redis가 클러스터 전역으로 관리하므로,
* 로컬 room_count/room_free_list는 제거하고 MAX_ROOMS개 슬롯을 state_init()에서 한 번에 초기화한다
*/
static room_t rooms[MAX_ROOMS];
static pthread_mutex_t g_rooms_lock = PTHREAD_MUTEX_INITIALIZER;

/* 서버 시작 시 1회 호출 - 모든 방 슬롯의 뮤텍스를 미리 초기화해둔다(3단계 이전처럼 방 생성 시점에
* 지연 초기화하지 않음 - Redis가 이미 정해준 room_id가 로컬에서 처음 쓰일 때 곧바로 락을 잡아야 하므로) */
void state_init(void)
{
	for (int i = 0; i < MAX_ROOMS; ++i) {
		memset(&rooms[i], 0, sizeof(rooms[i]));
		pthread_mutex_init(&rooms[i].lock, nullptr);
		rooms[i].room_id = i;
		rooms[i].live = false;
	}
	log_json("INFO", "state_initialized", "max_rooms", LOG_ARG_INT, MAX_ROOMS, NULL);
}

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

/* 방 정보를 가져오는 함수 (3단계: room_count 없이 MAX_ROOMS 고정 상한으로 검사) */
room_t* room_get(int room_id)
{
	if (room_id < 0 || room_id >= MAX_ROOMS) return nullptr;

	PosixLockGuard lock(g_rooms_lock);
	if (!rooms[room_id].live) return nullptr;
	return &rooms[room_id];
}

/*
* Redis가 배정한 room_id에 대해 로컬 슬롯을 준비하는 함수 (3단계)
* 매치메이킹 정책(누구를 어디로 배정할지)은 이제 Redis가 결정하므로, 이 함수는 정책 결정 없이
* 주어진 room_id의 로컬 자리를 살아있는 상태로 만들기만 한다
*/
room_t* room_get_or_init(int room_id)
{
	if (room_id < 0 || room_id >= MAX_ROOMS) return nullptr;

	PosixLockGuard lock(g_rooms_lock);
	room_t* r = &rooms[room_id];
	if (!r->live) {
		PosixLockGuard room_lock(r->lock);
		memset(r->users, 0, sizeof(r->users));
		r->user_count = 0;
		r->room_id = room_id;
		r->live = true;
	}
	return r;
}

/*
* 방에 입장하는 함수 (기존 락 순서/원자성 보장은 그대로 유지)
* 3단계: 입장 성공 여부(0/-1)와 "이 입장으로 로컬 인원이 0->1이 됐는지"(출력 인자)를 분리해서
* 알려준다. 후자는 이 pod에서 이 방에 대한 관심이 방금 처음 생겼다는 뜻 -> 호출부(logic.c)가
* Redis 채널 구독을 새로 시작해야 한다는 신호다.
* (예전에는 first_local_member 하나만 bool로 돌려줘서, 입장 실패가 "첫 멤버 아님"과 구분되지 않았음)
*/
int room_join(room_t* room, session_t* s, bool* out_first_local_member)
{
	if (out_first_local_member) *out_first_local_member = false;
	if (!room || !s) return -1;

	bool joined = false;
	bool first_local_member = false;
	int joined_room_id = -1;
	{
		PosixLockGuard rooms_lock(g_rooms_lock);
		if (!room->live) return -1;

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
				first_local_member = (room->user_count == 1);
			}
		}
	}

	if (!joined) return -1;

	log_json("INFO", "room_joined", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, joined_room_id, "first_local_member", LOG_ARG_INT, first_local_member ? 1 : 0, NULL);

	if (out_first_local_member) *out_first_local_member = first_local_member;
	return 0;
}

/*
* 방에서 떠나는 함수 (기존 락 순서/원자성 보장은 그대로 유지)
* 3단계: 로컬 정리 후 Redis 쪽 인원 카운트도 -1 한다(Redis가 회수/freelist 반납을 대신 처리).
* 이 pod의 로컬 인원이 0이 되면(다른 pod에는 아직 인원이 남아있을 수 있음) 구독을 끊어달라는
* job을 net 스레드에 전달한다
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
	bool now_empty_local = false;
	{
		PosixLockGuard rooms_lock(g_rooms_lock);
		room_t* room = &rooms[room_id];

		if (!room->live) {
			PosixLockGuard session_lock(s->lock);
			if (s->room_id == room_id) s->room_id = -1;
			return;
		}

		{
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

			now_empty_local = (room->user_count == 0);
			if (now_empty_local) {
				room->live = false;
			}
		}

		/*
		* 구독 해제 job은 "이 방이 로컬에서 비었다(live=false)"고 판정한 상태 전이와 반드시 같은
		* g_rooms_lock 구간 안에서 큐에 넣어야 한다.
		* 예전처럼 락을 놓고 blocking redis_leave_room()까지 마친 뒤에 넣으면, 그 사이 다른 워커가
		* 같은 room_id로 PKT_JOIN_ROOM을 처리해(room_get_or_init이 live=true로 되살리고 로컬 인원이
		* 0->1이 되어 first_local_member=true) JOB_REDIS_SUBSCRIBE를 먼저 밀어넣을 수 있다.
		* g_io_q는 FIFO라 net 스레드가 SUBSCRIBE -> UNSUBSCRIBE 순으로 실행해버리고, 살아있는 로컬
		* 멤버가 있는 방을 이 pod만 조용히 못 듣게 된다(에러 로그도 남지 않음).
		* 락 순서: g_io_q 내부 뮤텍스는 g_rooms_lock/room->lock 아래의 leaf다(job_queue_push는 큐
		* 뮤텍스만 잡고, 큐를 소비하는 쪽은 항상 pop으로 뮤텍스를 놓은 뒤에야 방/세션 락을 잡는다)
		*
		* 다만 g_io_q의 유일한 소비자인 net 스레드는 /metrics 스크레이프를 처리할 때
		* (handle_metrics_accept -> metrics_render -> state_count_active_rooms) 이 g_rooms_lock을
		* 다시 필요로 한다. 그래서 여기서 blocking push(job_queue_push)를 쓰면, 마침 g_io_q가 가득 차
		* 있고 net 스레드가 그 /metrics 처리 중 g_rooms_lock 대기에 걸린 순간이 겹칠 경우 서로를
		* 영원히 기다리는 데드락이 된다(이 워커는 큐에 자리가 나길 기다리는데 큐를 비울 net 스레드는
		* 이 워커가 놓아줄 g_rooms_lock을 기다림). 그래서 반드시 non-blocking인 job_queue_try_push
		* 기반의 job_queue_push_redis_unsubscribe를 써야 한다 - 큐가 가득 차면 블록 대신 그냥
		* 드롭하고 로그만 남긴다(JOB_REDIS_UNSUBSCRIBE는 이 코드베이스의 기존 정책대로 재시도 없는
		* best-effort 요청이라, 드롭돼도 이 pod가 잠시 더 오래 구독 상태로 남아 불필요한 메시지를
		* 더 받는 정도이지 메시지 유실로 이어지지는 않는다)
		*/
		if (did_leave && now_empty_local) {
			if (job_queue_push_redis_unsubscribe(&g_io_q, room_id) == 0) {
				net_wakeup();
			} else {
				log_json("ERROR", "redis_unsubscribe_job_dropped", "room_id", LOG_ARG_INT, room_id, NULL);
			}
		}
	}

	{
		PosixLockGuard session_lock(s->lock);
		if (s->room_id == room_id) s->room_id = -1;
	}

	if (did_leave) {
		/* Redis 왕복은 상태 락 밖에서 해도 되는 부분이라 여기 그대로 둔다.
		* 다만 실패를 무시하면 Redis 쪽 인원 카운트가 영영 안 줄어 그 방이 freelist로 회수되지
		* 않으므로(=MAX_ROOMS 고정 상한의 영구 잠식), 최소한 로그는 남긴다(재시도는 하지 않음) */
		if (redis_leave_room(s->session_id, room_id) != 0) {
			log_json("ERROR", "redis_leave_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
		}

		log_json("INFO", "room_left", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, "local_empty", LOG_ARG_INT, now_empty_local ? 1 : 0, NULL);
	}
}

/*
* 방에 채팅을 전파하는 함수 (3단계: id 기반으로 변경)
* Redis pub/sub을 거쳐 이 pod에 도착한 메시지를 로컬 멤버에게만 전달한다.
* 포맷팅(개행 추가 등)은 이미 발행 시점(logic.c)에 끝났으므로 pkt을 그대로 재사용한다 ->
* 그래야 모든 pod가 같은 바이트를 배송함
*/
void room_broadcast_local(int room_id, int except_global_id, packet_t* pkt)
{
	if (!pkt) return;
	room_t* room = room_get(room_id);
	if (!room) return;

	/*
	* 전송 대상 session_id 목록을 임시로 저장 (fd가 아님!)
	* room->lock을 잡은 상태에서 직접 send하지 않기 위해 사용
	*/
	int target_ids[MAX_ROOM_USER];
	int count = 0;

	{
		PosixLockGuard lock(room->lock);
		for (int i = 0; i < room->user_count; ++i) {
			session_t* s = room->users[i];
			if (!s) continue;
			if (!session_is_alive(s)) continue;
			if (s->global_id == except_global_id) continue;   /* pod-로컬 session_id가 아니라 클러스터 전역 id로 비교해야 함(3단계 버그 수정) */
			target_ids[count++] = s->session_id;
		}
	}

	for (int i = 0; i < count; ++i) {
		job_queue_push_send(&g_io_q, target_ids[i], pkt);
	}

	if (count > 0) net_wakeup();
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
	for (int i = 0; i < MAX_ROOMS; ++i) {
		if (!rooms[i].live) continue;
		PosixLockGuard room_lock(rooms[i].lock);
		if (rooms[i].user_count > 0) count++;
	}
	return count;
}
