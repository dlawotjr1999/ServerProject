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

/* SCRIPT LOAD로 미리 등록해두고, 매 호출은 EVALSHA로 스크립트 본문 재전송 없이 실행한다 */
static char g_join_sha[41];
static char g_leave_sha[41];

/*
* 매치메이킹: 기존 로컬 room_free_list/room_count(REDESIGN.md 참고)와 동일한 정책을 그대로 복제한다.
* 1) freelist에 반납된 방이 있으면 재사용 (room_leave 스크립트가 count 0일 때만 반납하므로 항상 비어있음)
* 2) 없으면 이미 만들어진 방들(0..next_id-1) 중 인원 여유가 있는 방을 순차 탐색
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
	"  local cnt = tonumber(redis.call('GET', 'room:' .. i .. ':count') or '-1')\n"
	"  if cnt >= 0 and cnt < max_user then\n"
	"    redis.call('INCR', 'room:' .. i .. ':count')\n"
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

int redis_client_init(const char* host, int port)
{
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

	redisReply* r1 = (redisReply*)redisCommand(g_redis_cmd, "SCRIPT LOAD %s", k_join_script);
	if (!r1 || r1->type != REDIS_REPLY_STRING) {
		log_json("ERROR", "redis_script_load_failed", "which", LOG_ARG_STR, "join", NULL);
		if (r1) freeReplyObject(r1);
		return -1;
	}
	snprintf(g_join_sha, sizeof(g_join_sha), "%s", r1->str);
	freeReplyObject(r1);

	redisReply* r2 = (redisReply*)redisCommand(g_redis_cmd, "SCRIPT LOAD %s", k_leave_script);
	if (!r2 || r2->type != REDIS_REPLY_STRING) {
		log_json("ERROR", "redis_script_load_failed", "which", LOG_ARG_STR, "leave", NULL);
		if (r2) freeReplyObject(r2);
		return -1;
	}
	snprintf(g_leave_sha, sizeof(g_leave_sha), "%s", r2->str);
	freeReplyObject(r2);

	log_json("INFO", "redis_client_ready", "host", LOG_ARG_STR, host, "port", LOG_ARG_INT, port, NULL);
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

int redis_join_room(int session_id, int* out_room_id)
{
	(void)session_id;   /* 지금 스크립트는 세션별 정보를 쓰지 않지만, 로깅/확장 여지를 위해 인터페이스에 남겨둠 */

	PosixLockGuard lock(g_redis_cmd_lock);
	if (!g_redis_cmd) return -1;   /* 종료 절차에서 이미 정리된 연결 - 이 요청 하나만 실패시킨다 */

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
	if (!g_redis_cmd) return -1;   /* 종료 절차에서 이미 정리된 연결 - 이 요청 하나만 실패시킨다 */

	redisReply* reply = (redisReply*)redisCommand(g_redis_cmd,
		"EVALSHA %s 0 %d", g_leave_sha, room_id);
	if (!reply) return -1;

	int ok = (reply->type == REDIS_REPLY_INTEGER) ? 0 : -1;
	freeReplyObject(reply);
	return ok;
}

int redis_next_global_id(int* out_global_id)
{
	PosixLockGuard lock(g_redis_cmd_lock);
	if (!g_redis_cmd) return -1;   /* 종료 절차에서 이미 정리된 연결 - 이 요청 하나만 실패시킨다 */

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
	if (!g_redis_cmd) return -1;   /* 종료 절차에서 이미 정리된 연결 - 이 요청 하나만 실패시킨다 */

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
