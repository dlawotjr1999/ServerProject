#include "redis_client.h"
#include "posix_lock.hpp"
#include "log.h"

#include <hiredis/hiredis.h>
#include <sys/time.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

/*
* 명령용 연결(EVAL/PUBLISH) - logic worker 4개가 공유하므로 뮤텍스로 직렬화한다
* 구독용 연결(SUBSCRIBE) - net 스레드가 단독 소유하므로 락이 필요 없다
*/
static redisContext* g_redis_cmd = nullptr;
static redisContext* g_redis_sub = nullptr;
static pthread_mutex_t g_redis_cmd_lock = PTHREAD_MUTEX_INITIALIZER;

/* g_redis_cmd가 재연결된 뒤 매치메이킹 스크립트를 아직 다시 로드하지 못한 상태인지 추적한다.
* redis_client_init()이 이미 로드해뒀으므로 시작 값은 true */
static bool g_cmd_scripts_loaded = true;

/* SCRIPT LOAD로 미리 등록해두고, 매 호출은 EVALSHA로 스크립트 본문 재전송 없이 실행한다 */
static char g_join_sha[41];
static char g_leave_sha[41];

/* 이 pod의 안정적인 식별자 - room:*:pod:{이 값}:* 키에 씀. HOSTNAME은 k8s가 pod 이름으로 채워주는
* 표준 환경변수. 로컬/비-k8s 테스트에서는 없을 수 있어 getpid() 기반 값으로 대체한다 */
static char g_pod_id[64];

/*
* 매치메이킹: 기존 로컬 room_free_list/room_count(REDESIGN.md 참고)와 동일한 정책을 그대로 복제한다.
* 1) freelist에 반납된 방이 있으면 재사용 (room_leave 스크립트가 count 0일 때만 반납하므로 항상 비어있음)
* 2) 없으면 이미 만들어진 방들(0..next_id-1) 중 인원 여유가 있는 방을 순차 탐색
*    - 탐색 전에, 그 방에 기여했던 pod들의 lease(room:{id}:pod:{pid}:lease)를 훑어 만료된(=크래시한)
*      pod의 마지막 기여분을 room:{id}:count에서 미리 걷어낸다(3단계: pod lease/heartbeat 복구).
*      정상 종료한 pod은 redis_room_heartbeat(0)이 자기 키를 즉시 지우므로 여기 남아있지 않는다 -
*      여기 남아 lease가 만료된 pod id는 오직 비정상 종료(SIGKILL/OOM/노드 유실)한 경우뿐이다
* 3) 그래도 없으면 새로 채번(MAX_ROOMS 초과 시 -1)
*/
static const char* k_join_script =
	"local rid = redis.call('LPOP', 'room_freelist')\n"
	"if rid then\n"
	"  redis.call('SET', 'room:' .. rid .. ':count', 1)\n"
	"  return tonumber(rid)\n"
	"end\n"
	"local max_rooms = tonumber(ARGV[1])\n"
	"local max_user = tonumber(ARGV[2])\n"
	"local next_id = tonumber(redis.call('GET', 'room_next_id') or '0')\n"
	"for i = 0, next_id - 1 do\n"
	"  local room_key = 'room:' .. i\n"
	"  local pods_key = room_key .. ':pods'\n"
	"  local pod_ids = redis.call('SMEMBERS', pods_key)\n"
	"  for _, pid in ipairs(pod_ids) do\n"
	"    local lease_key = room_key .. ':pod:' .. pid .. ':lease'\n"
	"    if redis.call('EXISTS', lease_key) == 0 then\n"
	"      local count_key = room_key .. ':pod:' .. pid .. ':count'\n"
	"      local stale = tonumber(redis.call('GET', count_key) or '0')\n"
	"      if stale > 0 then\n"
	"        local cur = tonumber(redis.call('GET', room_key .. ':count') or '0')\n"
	"        local dec = stale\n"
	"        if dec > cur then dec = cur end\n"
	"        if dec > 0 then\n"
	"          redis.call('DECRBY', room_key .. ':count', dec)\n"
	"        end\n"
	"      end\n"
	"      redis.call('DEL', count_key)\n"
	"      redis.call('SREM', pods_key, pid)\n"
	"    end\n"
	"  end\n"
	"  local cnt = tonumber(redis.call('GET', room_key .. ':count') or '-1')\n"
	"  if cnt >= 0 and cnt < max_user then\n"
	"    redis.call('INCR', room_key .. ':count')\n"
	"    return i\n"
	"  end\n"
	"end\n"
	"if next_id >= max_rooms then\n"
	"  return -1\n"
	"end\n"
	"redis.call('SET', 'room_next_id', next_id + 1)\n"
	"redis.call('SET', 'room:' .. next_id .. ':count', 1)\n"
	"return next_id\n";

/* 퇴장: count -1, 0 이하가 되면 카운트 키를 지우고 freelist에 반납한다 */
static const char* k_leave_script =
	"local key = 'room:' .. ARGV[1] .. ':count'\n"
	"local cnt = redis.call('DECR', key)\n"
	"if cnt <= 0 then\n"
	"  redis.call('DEL', key)\n"
	"  redis.call('RPUSH', 'room_freelist', ARGV[1])\n"
	"end\n"
	"return cnt\n";

/* net.c의 static set_nonblocking()과 동일한 2줄짜리 패턴 - 파일 경계를 넘어 공유할 만큼 크지 않아 그대로 중복 */
static bool set_nonblocking_fd(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return false;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/*
* 매치메이킹 스크립트 2개를 SCRIPT LOAD로 등록해 g_join_sha/g_leave_sha를 채운다.
* 최초 연결 시(redis_client_init)와, 끊긴 연결을 재연결한 뒤(ensure_cmd_connected) 둘 다에서 쓰인다 -
* 재연결이 완전히 새로운/재시작된 Redis 프로세스로 붙었을 수도 있어서, 그런 경우 기존에 캐시해둔
* SHA는 그 프로세스에 없으므로(NOSCRIPT) 매번 다시 로드해야 한다
*/
static bool load_matchmaking_scripts(void)
{
	redisReply* r1 = (redisReply*)redisCommand(g_redis_cmd, "SCRIPT LOAD %s", k_join_script);
	if (!r1 || r1->type != REDIS_REPLY_STRING) {
		log_json("ERROR", "redis_script_load_failed", "which", LOG_ARG_STR, "join", NULL);
		if (r1) freeReplyObject(r1);
		return false;
	}
	snprintf(g_join_sha, sizeof(g_join_sha), "%s", r1->str);
	freeReplyObject(r1);

	redisReply* r2 = (redisReply*)redisCommand(g_redis_cmd, "SCRIPT LOAD %s", k_leave_script);
	if (!r2 || r2->type != REDIS_REPLY_STRING) {
		log_json("ERROR", "redis_script_load_failed", "which", LOG_ARG_STR, "leave", NULL);
		if (r2) freeReplyObject(r2);
		return false;
	}
	snprintf(g_leave_sha, sizeof(g_leave_sha), "%s", r2->str);
	freeReplyObject(r2);

	return true;
}

int redis_client_init(const char* host, int port)
{
	/* 이 pod의 안정적인 식별자를 한 번만 정해둔다(3단계 pod lease/heartbeat 복구) - HOSTNAME은
	* k8s가 pod 이름으로 채워주는 표준 환경변수. 로컬/비-k8s 테스트처럼 없을 수 있는 환경에서는
	* getpid() 기반 값으로 대체해 null pod id로 인한 크래시 없이 로컬 테스트가 되게 한다 */
	const char* hostname_env = getenv("HOSTNAME");
	if (hostname_env && hostname_env[0] != '\0') {
		snprintf(g_pod_id, sizeof(g_pod_id), "%s", hostname_env);
	} else {
		snprintf(g_pod_id, sizeof(g_pod_id), "local-%d", (int)getpid());
	}

	/*
	* 명령용 연결에는 연결/읽기/쓰기 타임아웃을 반드시 걸어둔다.
	* g_redis_cmd는 logic worker 4개가 g_redis_cmd_lock으로 직렬화해 쓰는 "유일한" 명령 커넥션이라,
	* 네트워크 파티션(연결이 깨끗하게 끊기지 않고 그냥 응답이 안 오는 상황)에서 타임아웃이 없으면
	* 락을 쥔 워커가 redisCommand() 안에서 영원히 멈추고 나머지 3개도 뮤텍스 뒤에 줄줄이 밀려
	* logic tier 전체가 멎는다.
	* 3초: 같은 클러스터 안(pod <-> Service) request/response 경로라 정상 왕복은 밀리초 단위이므로
	* 정상 지연에는 절대 걸리지 않으면서, 장애 시에는 사람이 체감하기 전에 풀린다.
	* 이건 재시도/서킷브레이커가 아니라, "에러를 로그하고 그 요청 하나만 실패시킨다"는 기존 정책이
	* 실제로 도달 가능해지게 만드는 장치다(무한 대기하면 그 정책까지 못 감) */
	struct timeval redis_cmd_timeout = { 3, 0 };

	g_redis_cmd = redisConnectWithTimeout(host, port, redis_cmd_timeout);
	if (!g_redis_cmd || g_redis_cmd->err) {
		log_json("ERROR", "redis_connect_failed", "which", LOG_ARG_STR, "cmd", NULL);
		return -1;
	}

	/* redisConnectWithTimeout()은 "연결 수립"까지만 제한한다. 연결된 뒤의 read/write에도 같은
	* 상한을 적용하려면 redisSetTimeout()을 따로 호출해야 한다 */
	if (redisSetTimeout(g_redis_cmd, redis_cmd_timeout) != REDIS_OK) {
		log_json("ERROR", "redis_set_timeout_failed", "which", LOG_ARG_STR, "cmd", NULL);
		return -1;
	}

	/* redisConnect()(블로킹) + fcntl로 fd만 논블로킹으로 바꾸면 hiredis 내부의
	* REDIS_BLOCK 플래그가 그대로 남아있어, redisBufferRead()가 EAGAIN을 정상적인
	* "아직 읽을 데이터 없음"이 아니라 진짜 I/O 에러로 취급해버린다(매 폴링마다
	* redis_sub_read_error 오탐 발생). redisConnectNonBlock()은 애초에 REDIS_BLOCK
	* 플래그 없이 컨텍스트를 만들어주는 hiredis의 표준 논블로킹 커넥터라 이 문제가 없다 */
	/* 구독용 연결에는 일부러 타임아웃을 걸지 않는다. 이미 논블로킹 컨텍스트라 redisBufferRead()가
	* 블로킹하지 않고, 읽기 시점은 epoll이 결정한다(다음 메시지가 몇 분 뒤에 올 수도 있는 게 정상).
	* 여기에 SO_RCVTIMEO를 걸어봐야 논블로킹 소켓에서는 효과가 없고, 오히려 "타임아웃 = 장애"라는
	* 잘못된 신호만 만든다. 구독 연결이 실제로 죽는 건 net.c의 redis_sub_read() 에러 경로가 잡는다 */
	g_redis_sub = redisConnectNonBlock(host, port);
	if (!g_redis_sub || g_redis_sub->err) {
		log_json("ERROR", "redis_connect_failed", "which", LOG_ARG_STR, "sub", NULL);
		return -1;
	}
	if (!set_nonblocking_fd(g_redis_sub->fd)) {
		log_json("ERROR", "redis_sub_nonblock_failed", NULL);
		return -1;
	}

	if (!load_matchmaking_scripts()) return -1;

	log_json("INFO", "redis_client_ready", "host", LOG_ARG_STR, host, "port", LOG_ARG_INT, port, "pod_id", LOG_ARG_STR, g_pod_id, NULL);
	return 0;
}

int redis_client_sub_fd(void)
{
	return g_redis_sub ? g_redis_sub->fd : -1;
}

void redis_client_shutdown(void)
{
	if (g_redis_cmd) { redisFree(g_redis_cmd); g_redis_cmd = nullptr; }
	if (g_redis_sub) { redisFree(g_redis_sub); g_redis_sub = nullptr; }
}

/*
* g_redis_cmd 호출 전에 매번 거치는 지연 복구(lazy recovery) 함수.
* 정상 상태면 아무 것도 하지 않고 즉시 true를 반환한다(핫 패스에 비용 없음).
* 연결이 에러 상태(타임아웃/끊김)면 redisReconnect()로 한 번만 재연결을 시도하고, 성공하면
* redisSetTimeout()으로 타임아웃을 다시 걸고 매치메이킹 스크립트도 다시 로드한다 - redisReconnect()는
* 소켓만 새로 열 뿐 타임아웃 설정이나 SCRIPT LOAD로 캐시해둔 SHA를 되살려주지 않기 때문이다.
* 재시도 루프가 아니다 - 이 한 번의 시도가 실패하면 이 호출은 실패로 끝나고(false 반환, 호출부가
* 로그 남기고 그 요청만 실패시킴), 다음 호출이 왔을 때 다시 한 번 시도한다.
* 반드시 g_redis_cmd_lock을 쥔 상태에서 호출해야 한다
*/
static bool ensure_cmd_connected(void)
{
	if (!g_redis_cmd) return false;

	if (g_redis_cmd->err) {
		if (redisReconnect(g_redis_cmd) != REDIS_OK) {
			log_json("ERROR", "redis_cmd_reconnect_failed", NULL);
			return false;
		}

		/* 재연결 성공 시점에 바로 내려야 한다 - 이 아래 redisSetTimeout()이 실패해서 여기서
		* return false 하더라도, 다음 호출이 (err가 이미 지워져 재연결 분기를 안 타는 채로)
		* 스크립트를 다시 로드하지 않고 넘어가는 걸 막기 위함 */
		g_cmd_scripts_loaded = false;

		struct timeval redis_cmd_timeout = { 3, 0 };
		if (redisSetTimeout(g_redis_cmd, redis_cmd_timeout) != REDIS_OK) {
			log_json("ERROR", "redis_set_timeout_failed", "which", LOG_ARG_STR, "cmd", NULL);
			return false;
		}

		log_json("INFO", "redis_cmd_reconnected", NULL);
	}

	if (!g_cmd_scripts_loaded) {
		if (!load_matchmaking_scripts()) return false;
		g_cmd_scripts_loaded = true;
	}

	return true;
}

int redis_join_room(int session_id, int* out_room_id)
{
	(void)session_id;   /* 지금 스크립트는 세션별 정보를 쓰지 않지만, 로깅/확장 여지를 위해 인터페이스에 남겨둠 */

	PosixLockGuard lock(g_redis_cmd_lock);
	if (!ensure_cmd_connected()) return -1;   /* 종료 절차에서 정리됐거나(g_redis_cmd==nullptr), 재연결/스크립트 재로드가 실패한 경우 - 이 요청 하나만 실패시킨다 */

	redisReply* reply = (redisReply*)redisCommand(g_redis_cmd,
		"EVALSHA %s 0 %d %d", g_join_sha, MAX_ROOMS, MAX_ROOM_USER);
	if (!reply) return -1;

	int ok = -1;
	if (reply->type == REDIS_REPLY_INTEGER && reply->integer >= 0) {
		*out_room_id = (int)reply->integer;
		ok = 0;
	}
	freeReplyObject(reply);
	return ok;
}

int redis_leave_room(int session_id, int room_id)
{
	(void)session_id;

	PosixLockGuard lock(g_redis_cmd_lock);
	if (!ensure_cmd_connected()) return -1;   /* 종료 절차에서 정리됐거나(g_redis_cmd==nullptr), 재연결/스크립트 재로드가 실패한 경우 - 이 요청 하나만 실패시킨다 */

	redisReply* reply = (redisReply*)redisCommand(g_redis_cmd,
		"EVALSHA %s 0 %d", g_leave_sha, room_id);
	if (!reply) return -1;

	int ok = (reply->type == REDIS_REPLY_INTEGER) ? 0 : -1;
	freeReplyObject(reply);
	return ok;
}

/*
* 이 pod이 room_id에 대해 갖고 있는 로컬 멤버 수를 하트비트로 알린다(3단계 pod lease/heartbeat
* 복구) - 상세 설명은 redis_client.h 참고. 원자적 Lua 스크립트가 아니라 3개의 개별 명령으로
* 처리한다 - 이 파일의 기존 정책대로, 부분 실패는 다음 하트비트에서 스스로 교정되는 지연일 뿐이지
* 정합성 버그가 아니기 때문이다(redis_client_init()의 3단계 초기화 시퀀스와 동일한 전제)
*/
int redis_room_heartbeat(int room_id, int local_count)
{
	/* count_key/lease_key는 "room:<room_id>:pod:<g_pod_id>:count|lease" 형태 - room_id(int, 최악
	* 11자리)와 g_pod_id(최대 63자)를 모두 감안해, -Wformat-truncation이 오탐하지 않을 만큼
	* 넉넉하게 잡는다(pods_key는 g_pod_id를 포맷 문자열에 안 쓰므로 그대로 48이면 충분) */
	char pods_key[48], count_key[128], lease_key[128];
	snprintf(pods_key, sizeof(pods_key), "room:%d:pods", room_id);
	snprintf(count_key, sizeof(count_key), "room:%d:pod:%s:count", room_id, g_pod_id);
	snprintf(lease_key, sizeof(lease_key), "room:%d:pod:%s:lease", room_id, g_pod_id);

	PosixLockGuard lock(g_redis_cmd_lock);
	if (!ensure_cmd_connected()) return -1;

	if (local_count > 0) {
		/* lease -> count -> SADD 순서를 반드시 지켜야 한다: 중간에 실패해도 SADD가 아직 안 됐다면
		* 이 pod은 :pods SET에 아직 보이지 않으므로 join 스크립트의 reap 루프가 아예 건드리지 않고,
		* lease 자체의 TTL로 조용히 자연 소멸한다(살아있는 pod의 기여분이 lease 없이 노출되는 창을
		* 없앰) - SADD를 먼저 하는 순서였다면 count/lease가 아직 안 쓰인 채로 :pods에만 보여서,
		* 다음 reap 스캔이 "lease 없음"으로 오판해 아직 살아있는 pod의 진짜 인원수를 걷어가 버린다 */
		redisReply* r1 = (redisReply*)redisCommand(g_redis_cmd, "SET %s 1 EX 30", lease_key);
		if (!r1 || r1->type == REDIS_REPLY_ERROR) { if (r1) freeReplyObject(r1); return -1; }
		freeReplyObject(r1);

		/*
		* count_key에는 일부러 TTL을 안 준다(이전 라운드에서 lease와 같은 EX 30을 줬다가 실제
		* 크래시+30초 대기 라이브 테스트로 회귀를 잡음 - 리포트 참고). reap은 "이 방을 매치메이킹
		* 스캔이 다시 방문할 때"라는, 시간이 얼마나 걸릴지 알 수 없는 시점에 일어난다. lease와 같은
		* TTL을 주면, reap이 실제로 도착했을 때쯤엔 count_key도 이미 같이 만료돼 GET이 nil을
		* 반환하고(stale=0으로 읽혀) 회수할 값 자체가 사라져서, 죽은 pod의 좌석이 영원히 회수되지
		* 않는 채로 남는다 - 이 기능 전체가 막으려던 바로 그 누수를 재현하는 셈이라 본말전도.
		* TTL 없이 두면 reap 루프가 실제로 이 pod을 방문할 때까지(:pods SET에서 발견 -> lease
		* 만료 확인 -> count 읽어서 회수 -> SREM+DEL) count_key가 항상 살아있어 정확한 회수량을
		* 읽을 수 있다. "스캔이 영원히 재방문 안 하는 방"이 쌓이는 이론적 위험은 있지만
		* MAX_ROOMS(256)로 상한이 있는 아주 작은 키라 무시 가능한 수준이다(살아있는 pod은 매
		* 하트비트마다 SET으로 덮어써지므로 stale 상태로 오래 남는 건 애초에 죽은 pod의 흔적뿐) */
		redisReply* r2 = (redisReply*)redisCommand(g_redis_cmd, "SET %s %d", count_key, local_count);
		if (!r2 || r2->type == REDIS_REPLY_ERROR) { if (r2) freeReplyObject(r2); return -1; }
		freeReplyObject(r2);

		redisReply* r3 = (redisReply*)redisCommand(g_redis_cmd, "SADD %s %s", pods_key, g_pod_id);
		if (!r3 || r3->type == REDIS_REPLY_ERROR) { if (r3) freeReplyObject(r3); return -1; }
		freeReplyObject(r3);
	} else {
		redisReply* r1 = (redisReply*)redisCommand(g_redis_cmd, "SREM %s %s", pods_key, g_pod_id);
		if (!r1 || r1->type == REDIS_REPLY_ERROR) { if (r1) freeReplyObject(r1); return -1; }
		freeReplyObject(r1);

		redisReply* r2 = (redisReply*)redisCommand(g_redis_cmd, "DEL %s", count_key);
		if (!r2 || r2->type == REDIS_REPLY_ERROR) { if (r2) freeReplyObject(r2); return -1; }
		freeReplyObject(r2);

		redisReply* r3 = (redisReply*)redisCommand(g_redis_cmd, "DEL %s", lease_key);
		if (!r3 || r3->type == REDIS_REPLY_ERROR) { if (r3) freeReplyObject(r3); return -1; }
		freeReplyObject(r3);
	}

	return 0;
}

int redis_next_global_id(int* out_global_id)
{
	PosixLockGuard lock(g_redis_cmd_lock);
	if (!ensure_cmd_connected()) return -1;   /* 종료 절차에서 정리됐거나(g_redis_cmd==nullptr), 재연결/스크립트 재로드가 실패한 경우 - 이 요청 하나만 실패시킨다 */

	redisReply* reply = (redisReply*)redisCommand(g_redis_cmd, "INCR global_session_seq");
	if (!reply) return -1;

	int ok = -1;
	if (reply->type == REDIS_REPLY_INTEGER) {
		*out_global_id = (int)reply->integer;
		ok = 0;
	}
	freeReplyObject(reply);
	return ok;
}

int redis_publish_chat(int room_id, int except_global_id, const packet_t* pkt)
{
	if (!pkt) return -1;

	/* pkt->length는 (type 2바이트 + payload) 길이라 최소 2다. size_t 뺄셈이므로 2 미만이면
	* 그대로 거대한 값으로 언더플로우한다(아래 클램프가 결과적으로 막아주긴 하지만, 손상된
	* 입력을 조용히 통과시키지 않도록 redis_sub_read()의 plen < 2 가드와 같은 형태로 명시함) */
	if (pkt->length < 2) return -1;

	size_t payload_bytes = (size_t)pkt->length - 2;
	if (payload_bytes > MAX_PACKET_SIZE) payload_bytes = MAX_PACKET_SIZE;

	/* [except_global_id: int][type: uint16][length: uint16][payload bytes...] 로 직렬화 */
	char buf[sizeof(int) + sizeof(uint16_t) + sizeof(uint16_t) + MAX_PACKET_SIZE];
	size_t off = 0;
	memcpy(buf + off, &except_global_id, sizeof(int)); off += sizeof(int);
	memcpy(buf + off, &pkt->type, sizeof(uint16_t)); off += sizeof(uint16_t);
	memcpy(buf + off, &pkt->length, sizeof(uint16_t)); off += sizeof(uint16_t);
	memcpy(buf + off, pkt->payload, payload_bytes); off += payload_bytes;

	char channel[32];
	snprintf(channel, sizeof(channel), "room:%d", room_id);

	PosixLockGuard lock(g_redis_cmd_lock);
	if (!ensure_cmd_connected()) return -1;   /* 종료 절차에서 정리됐거나(g_redis_cmd==nullptr), 재연결/스크립트 재로드가 실패한 경우 - 이 요청 하나만 실패시킨다 */

	/* %b: hiredis의 바이너리 세이프 인자 지정자 (const char*, size_t) */
	redisReply* reply = (redisReply*)redisCommand(g_redis_cmd, "PUBLISH %s %b", channel, buf, off);
	if (!reply) return -1;
	freeReplyObject(reply);
	return 0;
}

int redis_subscribe_room(int room_id)
{
	if (!g_redis_sub) return -1;
	char channel[32];
	snprintf(channel, sizeof(channel), "room:%d", room_id);

	if (redisAppendCommand(g_redis_sub, "SUBSCRIBE %s", channel) != REDIS_OK)
		return -1;

	int done = 0;
	while (!done) {
		if (redisBufferWrite(g_redis_sub, &done) == REDIS_ERR)
			return -1;
	}
	return 0;
}

int redis_unsubscribe_room(int room_id)
{
	if (!g_redis_sub) return -1;
	char channel[32];
	snprintf(channel, sizeof(channel), "room:%d", room_id);

	if (redisAppendCommand(g_redis_sub, "UNSUBSCRIBE %s", channel) != REDIS_OK)
		return -1;

	int done = 0;
	while (!done) {
		if (redisBufferWrite(g_redis_sub, &done) == REDIS_ERR)
			return -1;
	}
	return 0;
}

int redis_sub_read(int* out_room_id, int* out_except_id, packet_t* out_pkt)
{
	if (!g_redis_sub) return -1;

	if (redisBufferRead(g_redis_sub) != REDIS_OK)
		return -1;

	void* reply_ptr = nullptr;
	if (redisGetReplyFromReader(g_redis_sub, &reply_ptr) != REDIS_OK)
		return -1;

	redisReply* reply = (redisReply*)reply_ptr;
	if (!reply)
		return 0;   /* reader에 완전한 응답이 남아있지 않음 - 소켓에서 더 받아와야 하므로 진짜 "더 읽을 것 없음" */

	/*
	* 여기까지 왔으면 응답 하나를 실제로 소비한 것이다. 그게 채팅 메시지가 아니었더라도
	* (SUBSCRIBE/UNSUBSCRIBE 확인 응답, 손상된 메시지 등) reader 버퍼에 뒤이어 진짜 메시지가
	* 함께 들어와 있을 수 있으므로 2를 반환해 호출부가 계속 읽게 한다.
	* 여기서 0을 반환하면(예전 동작) net.c의 드레인 루프가 확인 응답 하나에 멈춰버리고,
	* 뒤에 붙어있던 채팅 메시지는 OS 소켓이 이미 비어 epoll(level-trigger)도 다시 안 깨워주므로
	* 무관한 다음 publish가 올 때까지 무한정 지연된다
	*/
	int result = 2;

	/* pub/sub 데이터 메시지는 ["message", <channel>, <payload>] 3-요소 배열로 온다 */
	if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3 &&
	    reply->element[0]->str && strcmp(reply->element[0]->str, "message") == 0) {
		const char* chan = reply->element[1]->str;
		int room_id = atoi(chan + 5);   /* "room:" 다음부터 파싱 */

		const char* buf = reply->element[2]->str;
		size_t len = reply->element[2]->len;
		size_t header_len = sizeof(int) + sizeof(uint16_t) + sizeof(uint16_t);

		if (len >= header_len) {
			int except_id;
			uint16_t type, plen;
			memcpy(&except_id, buf, sizeof(int));
			memcpy(&type, buf + sizeof(int), sizeof(uint16_t));
			memcpy(&plen, buf + sizeof(int) + sizeof(uint16_t), sizeof(uint16_t));

			if (plen < 2) {
				freeReplyObject(reply);
				return 2;   /* 손상된 메시지는 조용히 버림 - 정상 메시지가 아니므로 배송 실패로 취급하지 않음 */
			}

			size_t payload_bytes = (size_t)plen - 2;

			/* 헤더가 선언한 길이만큼 실제로 도착했는지 반드시 확인한다. room:N 채널은 누구나
			* PUBLISH할 수 있으므로(k8s/redis.yaml: auth/NetworkPolicy 없음) 선언 길이를 그대로
			* 믿고 memcpy하면 reply 버퍼 밖을 최대 1KB까지 읽어 그 쓰레기 값이 실제 클라이언트에게
			* 채팅 내용인 것처럼 배송된다 */
			if (len < header_len + payload_bytes) {
				freeReplyObject(reply);
				return 2;   /* plen < 2와 동일하게 손상된 메시지로 보고 조용히 버림 */
			}

			if (payload_bytes > MAX_PACKET_SIZE) payload_bytes = MAX_PACKET_SIZE;

			memset(out_pkt, 0, sizeof(*out_pkt));
			out_pkt->type = type;
			out_pkt->length = plen;
			memcpy(out_pkt->payload, buf + header_len, payload_bytes);

			*out_room_id = room_id;
			*out_except_id = except_id;
			result = 1;
		}
	}

	freeReplyObject(reply);
	return result;
}
