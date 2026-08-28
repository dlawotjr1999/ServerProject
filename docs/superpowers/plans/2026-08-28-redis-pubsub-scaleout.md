# Redis Pub/Sub 스케일아웃 (3단계) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 채팅 서버를 `replicas > 1`로 수평 확장해도 서로 다른 pod에 접속한 클라이언트끼리 같은 방에서
대화할 수 있게 만든다 — 매치메이킹과 메시지 전파를 Redis를 통해 클러스터 전역으로 조율한다.

**Architecture:** 매치메이킹(빈 방 찾기/생성)을 Redis Lua 스크립트로 원자화해 room_id를 클러스터 전역에서
일관되게 만든다. 채팅 메시지는 항상 Redis PUBLISH를 거치고, 발행한 pod 자신을 포함한 모든 구독 pod가
SUBSCRIBE로 받아 각자의 로컬 멤버에게만 전달한다(단일 코드 경로). 구독 연결은 net 스레드가 기존 epoll
루프에 등록해 독점 소유하고, 명령 연결(EVAL/PUBLISH)은 뮤텍스로 직렬화해 4개 logic worker가 공유한다.

**Tech Stack:** hiredis(C), 기존 C/C++ 혼용 구조(net.c/logic.c/main.c=C, state.cpp/job_queue.cpp/새
redis_client.cpp=C++), Redis 7.x, kind/StatefulSet.

**Spec:** `2026-08-28-redis-pubsub-design.md` (저장소 루트, gitignore됨 — 브레인스토밍으로 승인된 설계)

## Global Constraints

- 매치메이킹은 Redis EVALSHA로 클러스터 전역 원자적 처리 (spec §결정사항)
- Redis 명령 연결은 단일 연결 + `pthread_mutex_t`/`PosixLockGuard`로 직렬화 (연결 풀 금지)
- 새 Redis 클라이언트 코드는 C++(`redis_client.cpp`)로 작성, `extern "C"` 브릿지로 노출
- `room_id`는 기존과 동일하게 `[0, MAX_ROOMS)` 범위에서 재사용 (무한 증가 카운터 금지) — 로컬
  `room_t rooms[MAX_ROOMS]` 고정 배열은 변경하지 않는다
- 로컬/원격 배송을 분리하지 않는다 — 항상 PUBLISH 후 자기 자신의 SUBSCRIBE로도 수신 (단일 코드 경로)
- 구독 연결(`g_redis_sub`)은 net 스레드만 접근한다. logic worker는 `g_io_q`의
  `JOB_REDIS_SUBSCRIBE`/`JOB_REDIS_UNSUBSCRIBE` job으로 요청만 전달한다
- Redis 장애: 시작 시 연결 실패면 즉시 종료(`net_init()`과 동일 패턴), 런타임 중 실패는 해당 요청만
  에러 응답 + 로그. 재시도/서킷브레이커는 만들지 않는다
- `net.c`/`logic.c`/`main.c`/`job_queue.h`/`job_queue.cpp`/`state.h`/`state.cpp`의 기존 코드 스타일
  (주석 밀도, 락 순서 `g_rooms_lock → room->lock → s->lock`, `PosixLockGuard` 사용)을 그대로 따른다
- 이 프로젝트에는 단위 테스트 프레임워크가 없다 — 기존 관례(컴파일 확인 → 수동/스크립트 스모크 테스트
  → TSan 스트레스 → kind 통합 검증)를 그대로 따른다. `pytest`/`Unity` 등 새 프레임워크를 들이지 않는다

---

### Task 1: hiredis 설치 확인 + 빌드 설정

**Files:**
- Modify: `server/Makefile`

**Interfaces:**
- Produces: 이후 모든 Task가 `#include <hiredis/hiredis.h>`와 `-lhiredis` 링크를 쓸 수 있는 빌드 환경

- [ ] **Step 1: WSL에 hiredis 개발 패키지 설치**

```bash
sudo apt-get update && sudo apt-get install -y libhiredis-dev redis-server
```

- [ ] **Step 2: 헤더/라이브러리가 실제로 설치됐는지 확인**

```bash
dpkg -L libhiredis-dev | grep -E "hiredis\.h$|libhiredis\.so$"
```

Expected: `/usr/include/hiredis/hiredis.h` 와 `/usr/lib/x86_64-linux-gnu/libhiredis.so`(또는 동일 계열
경로) 둘 다 출력됨. 안 나오면 `sudo apt-get install --reinstall libhiredis-dev`로 재시도.

- [ ] **Step 3: `server/Makefile`의 `LDFLAGS`에 `-lhiredis` 추가**

`server/Makefile:5`의 다음 줄을:
```make
LDFLAGS := -pthread
```
다음으로 변경:
```make
LDFLAGS := -pthread -lhiredis
```

- [ ] **Step 4: 빈 빌드로 링크 플래그가 문제없는지 확인**

```bash
cd server && make clean && make
```

Expected: 기존 소스만으로도(아직 hiredis를 안 쓰지만) 경고/에러 없이 빌드 성공 — `-lhiredis`가 아직
쓰이지 않는 심볼이라도 링크 자체는 깨지지 않아야 한다.

- [ ] **Step 5: Commit**

```bash
git add server/Makefile
git commit -m "build: hiredis 링크 설정 추가 (3단계 준비)"
```

---

### Task 2: job_queue에 Redis 연동용 job 타입 추가

**Files:**
- Modify: `server/job_queue.h`
- Modify: `server/job_queue.cpp`
- Test: `server/test_job_queue_redis.cpp` (임시 스모크 테스트, 이 프로젝트에는 테스트 프레임워크가
  없으므로 assert 기반 독립 실행 파일로 작성하고 Task 종료 시 삭제)

**Interfaces:**
- Consumes: 기존 `job_queue_push()`, `PosixLockGuard`(`posix_lock.hpp`)
- Produces: `job_queue_push_redis_subscribe(job_queue_t*, int room_id)`,
  `job_queue_push_redis_unsubscribe(job_queue_t*, int room_id)`,
  `job_queue_push_room_deliver(job_queue_t*, int room_id, int except_session_id, packet_t* pkt)` —
  이후 모든 Task가 이 세 함수와 `job_t.room_id` 필드를 사용함

- [ ] **Step 1: `server/job_queue.h`에 새 job 타입 3개 추가**

`server/job_queue.h:11-17`의 `job_type_t`를:
```c
typedef enum {
	JOB_PACKET,
	JOB_DISCONNECT,
	JOB_SHUTDOWN,
	JOB_SEND,
	JOB_CLOSE       /* logic -> net: 세션 정리가 끝났으니 이 fd를 이제 실제로 close해도 된다는 신호 */
} job_type_t;
```
다음으로 변경:
```c
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
```

- [ ] **Step 2: `job_t`에 `room_id` 필드 추가**

`server/job_queue.h:24-36`의 구조체 주석/필드를:
```c
typedef struct {
	job_type_t type;

	/*
	* JOB_PACKET / JOB_SEND: 대상 세션의 session_id (fd가 아님 -> fd 재사용과 무관하게 항상 같은 신원을 가리킴)
	* JOB_DISCONNECT: session_id(정리 대상) + fd(정리 완료 후 net 스레드가 close할 대상)를 함께 운반
	* JOB_CLOSE: fd만 사용 (net 스레드 내부적으로 실제 close를 수행하기 위함)
	*/
	int session_id;
	int fd;

	packet_t packet;
} job_t;
```
다음으로 변경:
```c
typedef struct {
	job_type_t type;

	/*
	* JOB_PACKET / JOB_SEND: 대상 세션의 session_id (fd가 아님 -> fd 재사용과 무관하게 항상 같은 신원을 가리킴)
	* JOB_DISCONNECT: session_id(정리 대상) + fd(정리 완료 후 net 스레드가 close할 대상)를 함께 운반
	* JOB_CLOSE: fd만 사용 (net 스레드 내부적으로 실제 close를 수행하기 위함)
	* JOB_REDIS_SUBSCRIBE / JOB_REDIS_UNSUBSCRIBE: room_id만 사용 (3단계)
	* JOB_ROOM_DELIVER: room_id + session_id(배송에서 제외할 원 발신자 id로 재사용) + packet (3단계)
	*/
	int session_id;
	int fd;
	int room_id;

	packet_t packet;
} job_t;
```

- [ ] **Step 3: `server/job_queue.h`에 새 push 헬퍼 3개 선언 추가**

`server/job_queue.h:58-62`의 다음 줄 뒤에:
```c
void job_queue_push_close(job_queue_t* q, int fd);
void job_queue_push_shutdown(job_queue_t* q);
```
아래 세 줄을 추가:
```c
void job_queue_push_redis_subscribe(job_queue_t* q, int room_id);
void job_queue_push_redis_unsubscribe(job_queue_t* q, int room_id);
void job_queue_push_room_deliver(job_queue_t* q, int room_id, int except_session_id, packet_t* pkt);
```

- [ ] **Step 4: `server/job_queue.cpp`에 세 헬퍼 구현 추가**

`server/job_queue.cpp` 파일 끝(115-119번째 줄, `job_queue_push_shutdown` 정의 뒤)에 추가:
```cpp
/* net 스레드에게 room_id 채널 구독을 시작하라는 job (3단계, logic -> net) */
void job_queue_push_redis_subscribe(job_queue_t* q, int room_id) {
	job_t job{};
	job.type = JOB_REDIS_SUBSCRIBE;
	job.room_id = room_id;
	job_queue_push(q, &job);
}

/* net 스레드에게 room_id 채널 구독을 중단하라는 job (3단계, logic -> net) */
void job_queue_push_redis_unsubscribe(job_queue_t* q, int room_id) {
	job_t job{};
	job.type = JOB_REDIS_UNSUBSCRIBE;
	job.room_id = room_id;
	job_queue_push(q, &job);
}

/*
* Redis pub/sub으로 도착한 메시지를 로컬 멤버에게 전달하라는 job (3단계, net -> logic)
* except_session_id는 job_t.session_id 필드를 재사용함(원 발신자를 배송 대상에서 제외하기 위함)
*/
void job_queue_push_room_deliver(job_queue_t* q, int room_id, int except_session_id, packet_t* pkt) {
	job_t job{};
	job.type = JOB_ROOM_DELIVER;
	job.room_id = room_id;
	job.session_id = except_session_id;
	job.packet = *pkt;
	job_queue_push(q, &job);
}
```

- [ ] **Step 5: 임시 스모크 테스트 작성 — 새 필드/헬퍼가 큐를 왕복하는지 확인**

`server/test_job_queue_redis.cpp` 새로 작성:
```cpp
#include "job_queue.h"
#include <cassert>
#include <cstdio>

int main() {
	job_queue_t q;
	job_queue_init(&q);

	job_queue_push_redis_subscribe(&q, 42);
	job_t out{};
	assert(job_queue_pop(&q, &out, JOBQ_NONBLOCK) == 1);
	assert(out.type == JOB_REDIS_SUBSCRIBE);
	assert(out.room_id == 42);

	job_queue_push_redis_unsubscribe(&q, 7);
	assert(job_queue_pop(&q, &out, JOBQ_NONBLOCK) == 1);
	assert(out.type == JOB_REDIS_UNSUBSCRIBE);
	assert(out.room_id == 7);

	packet_t pkt{};
	pkt.type = 1;
	pkt.length = 5;
	pkt.payload[0] = 'h'; pkt.payload[1] = 'i'; pkt.payload[2] = '\n';
	job_queue_push_room_deliver(&q, 3, 99, &pkt);
	assert(job_queue_pop(&q, &out, JOBQ_NONBLOCK) == 1);
	assert(out.type == JOB_ROOM_DELIVER);
	assert(out.room_id == 3);
	assert(out.session_id == 99);
	assert(out.packet.length == 5);
	assert(out.packet.payload[0] == 'h');

	assert(job_queue_pop(&q, &out, JOBQ_NONBLOCK) == 0);   /* 큐가 비었어야 함 */

	printf("job_queue redis job types: OK\n");
	return 0;
}
```

- [ ] **Step 6: 컴파일 후 실행**

```bash
cd server
g++ -std=c++17 -Wall -Wextra -O2 -pthread -c job_queue.cpp -o /tmp/job_queue_test.o
g++ -std=c++17 -Wall -Wextra -O2 -pthread test_job_queue_redis.cpp /tmp/job_queue_test.o -o /tmp/test_job_queue_redis
/tmp/test_job_queue_redis
```

Expected: `job_queue redis job types: OK` 출력, exit code 0.

- [ ] **Step 7: 스모크 테스트 파일 삭제 (제품 코드가 아님)**

```bash
rm server/test_job_queue_redis.cpp
```

- [ ] **Step 8: 본 빌드로 회귀 확인 후 Commit**

```bash
cd server && make clean && make
```
Expected: 경고/에러 없이 빌드 성공.

```bash
git add server/job_queue.h server/job_queue.cpp
git commit -m "feat: job_queue에 Redis 구독/전달용 job 타입 추가 (3단계)"
```

---

### Task 3: redis_client 모듈 신규 작성

**Files:**
- Create: `server/redis_client.h`
- Create: `server/redis_client.cpp`
- Modify: `server/Makefile` (자동 wildcard로 `.cpp`가 잡히므로 실제 수정 불필요 — 빌드로 확인만)

**Interfaces:**
- Consumes: `posix_lock.hpp`의 `PosixLockGuard`, `log.h`의 `log_json`, `common.h`의 `packet_t`/`MAX_ROOMS`/`MAX_ROOM_USER`/`MAX_PACKET_SIZE`
- Produces:
  - `int redis_client_init(const char* host, int port)`
  - `void redis_client_shutdown(void)`
  - `int redis_client_sub_fd(void)`
  - `int redis_join_room(int session_id, int* out_room_id)`
  - `int redis_leave_room(int session_id, int room_id)`
  - `int redis_publish_chat(int room_id, int except_session_id, const packet_t* pkt)`
  - `int redis_subscribe_room(int room_id)` / `int redis_unsubscribe_room(int room_id)`
  - `int redis_sub_read(int* out_room_id, int* out_except_id, packet_t* out_pkt)`
  이후 Task 4(state.cpp), 5(logic.c), 6(net.c)가 이 함수들을 그대로 호출함

- [ ] **Step 1: `server/redis_client.h` 작성**

```c
#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
* Redis 서버에 연결한다(명령용 연결 1개 + 구독용 연결 1개), 매치메이킹/퇴장 Lua 스크립트를 로드한다.
* net_init()과 동일한 원칙 - 실패하면 -1을 반환하고, 호출부는 서버를 즉시 종료시킨다
*/
int redis_client_init(const char* host, int port);

/* 서버 종료 시 두 연결을 정리한다 */
void redis_client_shutdown(void);

/* epoll에 등록할 구독 전용 연결의 fd. redis_client_init() 성공 후에만 유효하다 */
int redis_client_sub_fd(void);

/*
* 빈 방을 찾거나 새로 만든다(클러스터 전역, 원자적).
* 성공하면 0을 반환하고 *out_room_id에 배정된 방 번호를 채운다.
* 방 정원(MAX_ROOMS) 소진 등으로 실패하면 -1을 반환한다
*/
int redis_join_room(int session_id, int* out_room_id);

/*
* room_id의 인원 카운트를 하나 줄인다. 0이 되면 Redis 쪽에서 자동으로 회수(freelist 반환)된다.
* 실패하면 -1을 반환한다
*/
int redis_leave_room(int session_id, int room_id);

/*
* room:<room_id> 채널로 패킷을 발행한다. except_session_id는 원 발신자(로컬 에코 제외용)다.
* 발신자 자신의 pod도 자신의 SUBSCRIBE를 통해 이 메시지를 받는다(로컬/원격 배송을 분리하지 않음).
* 실패하면 -1을 반환한다
*/
int redis_publish_chat(int room_id, int except_session_id, const packet_t* pkt);

/*
* room:<room_id> 채널 구독을 시작/중단한다. g_redis_sub는 net 스레드만 접근해야 하므로
* 이 두 함수도 net 스레드에서만 호출해야 한다(스레드 안전하지 않음)
*/
int redis_subscribe_room(int room_id);
int redis_unsubscribe_room(int room_id);

/*
* 구독 fd가 readable할 때 net 스레드가 반복 호출한다.
* 채팅 메시지를 하나 파싱했으면 1을 반환하고 out_room_id/out_except_id/*out_pkt를 채운다.
* 더 읽을 완전한 메시지가 없으면 0, 오류면 -1을 반환한다(SUBSCRIBE/UNSUBSCRIBE 확인 응답 등
* "message"가 아닌 pub/sub 응답은 조용히 소비하고 0을 반환한다)
*/
int redis_sub_read(int* out_room_id, int* out_except_id, packet_t* out_pkt);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: `server/redis_client.cpp` 작성 — 연결/스크립트 로드**

```cpp
#include "redis_client.h"
#include "posix_lock.hpp"
#include "log.h"

#include <hiredis/hiredis.h>
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
	g_redis_cmd = redisConnect(host, port);
	if (!g_redis_cmd || g_redis_cmd->err) {
		log_json("ERROR", "redis_connect_failed", "which", LOG_ARG_STR, "cmd", NULL);
		return -1;
	}

	g_redis_sub = redisConnect(host, port);
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
```

- [ ] **Step 3: `server/redis_client.cpp`에 매치메이킹/퇴장/발행 함수 추가**

같은 파일 끝에 추가:
```cpp
int redis_join_room(int session_id, int* out_room_id)
{
	(void)session_id;   /* 지금 스크립트는 세션별 정보를 쓰지 않지만, 로깅/확장 여지를 위해 인터페이스에 남겨둠 */

	PosixLockGuard lock(g_redis_cmd_lock);
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
	redisReply* reply = (redisReply*)redisCommand(g_redis_cmd,
		"EVALSHA %s 0 %d", g_leave_sha, room_id);
	if (!reply) return -1;

	int ok = (reply->type == REDIS_REPLY_INTEGER) ? 0 : -1;
	freeReplyObject(reply);
	return ok;
}

int redis_publish_chat(int room_id, int except_session_id, const packet_t* pkt)
{
	if (!pkt) return -1;

	size_t payload_bytes = (size_t)pkt->length - 2;
	if (payload_bytes > MAX_PACKET_SIZE) payload_bytes = MAX_PACKET_SIZE;

	/* [except_session_id: int][type: uint16][length: uint16][payload bytes...] 로 직렬화 */
	char buf[sizeof(int) + sizeof(uint16_t) + sizeof(uint16_t) + MAX_PACKET_SIZE];
	size_t off = 0;
	memcpy(buf + off, &except_session_id, sizeof(int)); off += sizeof(int);
	memcpy(buf + off, &pkt->type, sizeof(uint16_t)); off += sizeof(uint16_t);
	memcpy(buf + off, &pkt->length, sizeof(uint16_t)); off += sizeof(uint16_t);
	memcpy(buf + off, pkt->payload, payload_bytes); off += payload_bytes;

	char channel[32];
	snprintf(channel, sizeof(channel), "room:%d", room_id);

	PosixLockGuard lock(g_redis_cmd_lock);
	/* %b: hiredis의 바이너리 세이프 인자 지정자 (const char*, size_t) */
	redisReply* reply = (redisReply*)redisCommand(g_redis_cmd, "PUBLISH %s %b", channel, buf, off);
	if (!reply) return -1;
	freeReplyObject(reply);
	return 0;
}
```

- [ ] **Step 4: `server/redis_client.cpp`에 구독/수신 함수 추가**

같은 파일 끝에 추가:
```cpp
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
		return 0;   /* 아직 완전한 메시지가 도착하지 않음 */

	int result = 0;
	/* pub/sub 데이터 메시지는 ["message", <channel>, <payload>] 3-요소 배열로 온다.
	* SUBSCRIBE/UNSUBSCRIBE 확인 응답(["subscribe", channel, count] 등)은 여기 안 걸려서
	* 그냥 소비만 되고(반환 0), 이 루프를 호출하는 net.c가 다음 반복에서 계속 읽어나간다 */
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

			size_t payload_bytes = (size_t)plen - 2;
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
```

- [ ] **Step 5: 빌드 확인**

```bash
cd server && make clean && make
```
Expected: `redis_client.cpp`가 `%.o: %.cpp` 패턴 규칙(Makefile의 `wildcard *.cpp`)으로 자동 포함되어
경고/에러 없이 빌드되고, `-lhiredis`로 정상 링크됨.

- [ ] **Step 6: Commit**

```bash
git add server/redis_client.h server/redis_client.cpp
git commit -m "feat: redis_client 모듈 신규 작성 - 매치메이킹/발행/구독 (3단계)"
```

---

### Task 4: state.cpp/state.h — 방 관리를 Redis 매치메이킹에 맞게 리팩터

**Files:**
- Modify: `server/state.h`
- Modify: `server/state.cpp`

**Interfaces:**
- Consumes: Task 3의 `redis_leave_room()`, Task 2의 `job_queue_push_redis_unsubscribe()`
- Produces:
  - `void state_init(void)` — 서버 시작 시 1회 호출 (Task 7이 `main.c`에서 호출)
  - `room_t* room_get_or_init(int room_id)`
  - `bool room_join(room_t* room, session_t* s)` — **시그니처 변경**: `void` → `bool` (반환값: 이 입장으로
    로컬 인원이 0→1이 됐으면 true)
  - `void room_broadcast_local(int room_id, int except_session_id, packet_t* pkt)` — 기존
    `room_broadcast(room_t*, session_t*, packet_t*)`를 **대체**
  - `room_create()`/`room_find()`는 **삭제** (Redis가 매치메이킹을 대신함)
  이후 Task 5(logic.c)가 이 함수들을 호출함

- [ ] **Step 1: `server/state.h`의 room API 섹션 갱신**

`server/state.h:49-56`의:
```c
/* room API */
room_t* room_get(int room_id);
room_t* room_create(void);
room_t* room_find(void);

void room_join(room_t* room, session_t* s);
void room_leave(session_t* s);
void room_broadcast(room_t* room, session_t* sender, packet_t* pkt);
```
다음으로 변경:
```c
/* room API */
void state_init(void);   /* 서버 시작 시 1회 호출 - 방 슬롯의 뮤텍스를 미리 전부 초기화 (3단계) */
room_t* room_get(int room_id);
room_t* room_get_or_init(int room_id);   /* Redis가 배정한 room_id에 대해 로컬 슬롯을 준비 (3단계) */

bool room_join(room_t* room, session_t* s);   /* 반환값: 이 입장으로 로컬 인원이 0->1이 됐으면 true */
void room_leave(session_t* s);
void room_broadcast_local(int room_id, int except_session_id, packet_t* pkt);   /* 3단계: id 기반, 로컬 전달만 담당 */
```

- [ ] **Step 2: `server/state.cpp` 상단 extern "C" 블록에 `redis_leave_room` 선언 추가**

`server/state.cpp:12-16`의:
```cpp
/* net.c/main.c(순수 C)에 정의된 심볼이라 C 링키지로 선언해야 링크가 됨 */
extern "C" {
	extern job_queue_t g_io_q;
	void net_wakeup(void);
}
```
다음으로 변경:
```cpp
/* net.c/main.c(순수 C)에 정의된 심볼이라 C 링키지로 선언해야 링크가 됨 */
extern "C" {
	extern job_queue_t g_io_q;
	void net_wakeup(void);
	int redis_leave_room(int session_id, int room_id);   /* redis_client.cpp (3단계) */
}
```

- [ ] **Step 3: 로컬 free list/count 제거, `state_init()` 추가**

`server/state.cpp:35-40`의:
```cpp
/* 방 관련 데이터. rooms 배열 자체는 그대로 고정 배열(세션과 달리 방은 raw pointer를 오래 들고 있지 않고
* 매번 room_get()으로 다시 조회하므로 shared_ptr이 필요 없음). free list만 std::vector로 교체 */
static room_t rooms[MAX_ROOMS];
static int room_count = 0;
static std::vector<int> room_free_list;
static pthread_mutex_t g_rooms_lock = PTHREAD_MUTEX_INITIALIZER;
```
다음으로 변경:
```cpp
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
```

또한 파일 상단의 `#include <vector>`는 더 이상 쓰이지 않으므로(3-9번째 줄) 제거한다.

- [ ] **Step 4: `room_create()`/`room_find()` 삭제, `room_get()`/`room_get_or_init()`으로 교체**

`server/state.cpp:217-298`의 `room_create()`, `room_get()`, `room_find()` 세 함수 전체를 삭제하고
다음 두 함수로 교체:
```cpp
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
```

- [ ] **Step 5: `room_join()`을 `bool` 반환으로 변경**

`server/state.cpp`의 `room_join()` 함수 전체(위 Step 4 삭제로 줄 번호가 밀리므로 함수 이름으로 찾을 것)를:
```cpp
void room_join(room_t* room, session_t* s)
{
	if (!room || !s) return;

	bool joined = false;
	int joined_room_id = -1;
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
	}

	if (joined) {
		log_json("INFO", "room_joined", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, joined_room_id, NULL);
	}
}
```
다음으로 변경:
```cpp
/*
* 방에 입장하는 함수 (기존 락 순서/원자성 보장은 그대로 유지)
* 3단계: 반환값이 추가됨 - 이 입장으로 로컬 인원이 0->1이 됐으면 true(이 pod에서 이 방에 대한
* 관심이 방금 처음 생겼다는 뜻 -> 호출부(logic.c)가 Redis 구독을 새로 시작해야 함)
*/
bool room_join(room_t* room, session_t* s)
{
	if (!room || !s) return false;

	bool joined = false;
	bool first_local_member = false;
	int joined_room_id = -1;
	{
		PosixLockGuard rooms_lock(g_rooms_lock);
		if (!room->live) return false;

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

	if (joined) {
		log_json("INFO", "room_joined", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, joined_room_id, "first_local_member", LOG_ARG_INT, first_local_member ? 1 : 0, NULL);
	}
	return first_local_member;
}
```

- [ ] **Step 6: `room_leave()`에 Redis 훅 추가**

`room_leave()` 함수 전체를:
```cpp
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
	}

	{
		PosixLockGuard session_lock(s->lock);
		if (s->room_id == room_id) s->room_id = -1;
	}

	if (did_leave) {
		log_json("INFO", "room_left", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, "reclaimed", LOG_ARG_INT, now_empty ? 1 : 0, NULL);
	}
}
```
다음으로 변경(`room_free_list.push_back` 제거, Redis 훅 추가):
```cpp
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

	{
		PosixLockGuard session_lock(s->lock);
		if (s->room_id == room_id) s->room_id = -1;
	}

	if (did_leave) {
		redis_leave_room(s->session_id, room_id);

		log_json("INFO", "room_left", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, "local_empty", LOG_ARG_INT, now_empty_local ? 1 : 0, NULL);

		if (now_empty_local) {
			job_queue_push_redis_unsubscribe(&g_io_q, room_id);
			net_wakeup();
		}
	}
}
```

- [ ] **Step 7: `room_broadcast()`를 `room_broadcast_local()`로 교체**

`server/state.cpp`의 `room_broadcast()` 함수 전체(기존 `room_t* room, session_t* sender, packet_t* pkt`
시그니처)를 삭제하고 다음으로 교체:
```cpp
/*
* 방에 채팅을 전파하는 함수 (3단계: id 기반으로 변경)
* Redis pub/sub을 거쳐 이 pod에 도착한 메시지를 로컬 멤버에게만 전달한다.
* 포맷팅(개행 추가 등)은 이미 발행 시점(logic.c)에 끝났으므로 pkt을 그대로 재사용한다 ->
* 그래야 모든 pod가 같은 바이트를 배송함
*/
void room_broadcast_local(int room_id, int except_session_id, packet_t* pkt)
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
			if (s->session_id == except_session_id) continue;
			target_ids[count++] = s->session_id;
		}
	}

	for (int i = 0; i < count; ++i) {
		job_queue_push_send(&g_io_q, target_ids[i], pkt);
	}

	if (count > 0) net_wakeup();
}
```

- [ ] **Step 8: `state_count_active_rooms()`가 `room_count` 없이 동작하도록 수정**

`server/state.cpp`의 `state_count_active_rooms()` 함수를:
```cpp
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
```
다음으로 변경(`room_count` → `MAX_ROOMS` 고정 상한):
```cpp
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
```

- [ ] **Step 9: 빌드 확인 (아직 logic.c가 옛 시그니처를 쓰므로 에러가 나는 게 정상)**

```bash
cd server && make clean && make
```
Expected: `logic.c`에서 `room_create`/`room_find`/`room_broadcast`가 선언되지 않았다는 컴파일 에러가
남. 이는 Task 5에서 `logic.c`를 고치기 전까지는 **의도된 실패**다 — 여기서는 `state.cpp` 자체가
독립적으로 문법 오류 없이 컴파일되는지만 확인한다(에러 메시지가 `logic.c:104` 등 `state.h` API 사용
지점에서만 나야 하고, `state.cpp` 자체의 문법 오류가 아니어야 함).

- [ ] **Step 10: Commit**

```bash
git add server/state.h server/state.cpp
git commit -m "refactor: 방 매치메이킹을 Redis로 위임, 로컬 room_t는 슬롯 캐시로 축소 (3단계)"
```

---

### Task 5: logic.c 배선

**Files:**
- Modify: `server/logic.c`

**Interfaces:**
- Consumes: Task 3의 `redis_join_room()`/`redis_publish_chat()`, Task 4의 `room_get_or_init()`/
  `room_join()`(bool 반환)/`room_broadcast_local()`, Task 2의 `job_queue_push_redis_subscribe()`
- Produces: 컴파일 가능한 `worker_thread()` — Task 4에서 남겨둔 컴파일 에러가 여기서 전부 해소됨

- [ ] **Step 1: `#include "redis_client.h"` 추가**

`server/logic.c:1-5`의:
```c
#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"
```
다음으로 변경:
```c
#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"
#include "redis_client.h"
```

- [ ] **Step 2: `worker_thread()`의 switch에 `JOB_ROOM_DELIVER` 케이스 추가**

`server/logic.c:66-69`의 `JOB_DISCONNECT` 케이스 뒤에 추가:
```c
			case JOB_DISCONNECT: {
				handle_disconnect(job.session_id, job.fd);
				break;
			}
```
다음 케이스를 그 바로 뒤에 추가:
```c
			/*
			* Redis pub/sub으로 도착한 채팅 메시지를 이 pod의 로컬 멤버에게 전달 (3단계)
			* job.room_id: 대상 방, job.session_id: 배송에서 제외할 원 발신자(재사용된 필드)
			*/
			case JOB_ROOM_DELIVER: {
				room_broadcast_local(job.room_id, job.session_id, &job.packet);
				break;
			}
```

- [ ] **Step 3: `PKT_JOIN_ROOM` 케이스를 Redis 매치메이킹으로 교체**

`server/logic.c:100-108`의:
```c
	case PKT_JOIN_ROOM: {
		if (session_get_room_id(s) >= 0)
			break;

		room_t* r = room_find();
		if (!r) r = room_create();
		room_join(r, s);
		break;
	}
```
다음으로 변경:
```c
	case PKT_JOIN_ROOM: {
		if (session_get_room_id(s) >= 0)
			break;

		/* 매치메이킹(빈 방 찾기/생성)은 Redis가 클러스터 전역으로 원자적으로 처리함 (3단계) */
		int room_id;
		if (redis_join_room(s->session_id, &room_id) != 0) {
			log_json("ERROR", "redis_join_failed", "session_id", LOG_ARG_INT, s->session_id, NULL);
			break;
		}

		room_t* r = room_get_or_init(room_id);
		if (!r) {
			log_json("ERROR", "room_init_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			break;
		}

		/* 이 pod에서 이 방에 로컬 멤버가 처음 생긴 경우에만 Redis 채널 구독을 시작함 */
		bool first_local_member = room_join(r, s);
		if (first_local_member) {
			job_queue_push_redis_subscribe(&g_io_q, room_id);
			net_wakeup();
		}
		break;
	}
```

- [ ] **Step 4: `PKT_CHAT` 케이스를 발행 방식으로 교체**

`server/logic.c:115-128`의:
```c
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
```
다음으로 변경:
```c
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

		if (redis_publish_chat(room_id, s->session_id, &out) != 0) {
			log_json("ERROR", "redis_publish_failed", "session_id", LOG_ARG_INT, s->session_id, "room_id", LOG_ARG_INT, room_id, NULL);
			break;
		}

		/* 발행 성공 시점에 카운트 (기존과 달리 "실제 배송 성공"이 아니라 "발행 성공" 기준으로 의미가 약간 바뀜) */
		metrics_inc_messages();
		break;
	}
```

`PKT_LEAVE_ROOM` 케이스와 `handle_disconnect()`는 **변경하지 않는다** — Task 4에서 `room_leave()` 내부에
Redis 훅을 이미 넣어뒀으므로, 이 두 호출부는 그대로 두면 새 동작을 자동으로 얻는다.

- [ ] **Step 5: 빌드 확인**

```bash
cd server && make clean && make
```
Expected: 경고/에러 없이 빌드 성공. (net.c가 아직 `redis_client_init`/`redis_client_sub_fd` 등을
안 부르므로 링크는 되지만, 서버를 당장 실행해도 Redis 연결/구독은 아직 동작하지 않음 — Task 6에서 완성)

- [ ] **Step 6: Commit**

```bash
git add server/logic.c
git commit -m "feat: logic.c가 Redis 매치메이킹/발행을 사용하도록 배선 (3단계)"
```

---

### Task 6: net.c — epoll에 Redis 구독 연결 통합

**Files:**
- Modify: `server/net.c`

**Interfaces:**
- Consumes: Task 3의 `redis_client_init()`/`redis_client_sub_fd()`/`redis_subscribe_room()`/
  `redis_unsubscribe_room()`/`redis_sub_read()`/`redis_client_shutdown()`, Task 2의
  `JOB_REDIS_SUBSCRIBE`/`JOB_REDIS_UNSUBSCRIBE`, `job_queue_push_room_deliver()`
- Produces: 완전히 동작하는 서버 — 이 Task가 끝나면 Task 8(로컬 통합 검증)을 실행할 수 있음

- [ ] **Step 1: `common.h`에 Redis 접속 정보 상수 추가**

`server/common.h:24-25`의:
```c
#define PORTNUM 3800
#define METRICS_PORT 9100
```
다음으로 변경:
```c
#define PORTNUM 3800
#define METRICS_PORT 9100
#define REDIS_HOST "127.0.0.1"   /* k8s에서는 서비스 이름으로 바뀔 예정 (Task 10에서 배포 시 확정) */
#define REDIS_PORT 6379
```

- [ ] **Step 2: `net.c`에 `#include "redis_client.h"` 추가**

`server/net.c:1-9`의:
```c
#include <sys/eventfd.h>

#include "common.h"
#include "net.h"
#include "protocol.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"
```
다음으로 변경:
```c
#include <sys/eventfd.h>

#include "common.h"
#include "net.h"
#include "protocol.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"
#include "redis_client.h"
```

- [ ] **Step 3: `net_init()`에서 Redis 연결 + 구독 fd를 epoll에 등록**

`server/net.c:386-392`의:
```c
	/* 위에서 쓴 ev 변수를 재사용해 metrics_listen_fd도 같은 epoll 인스턴스(epfd)에 등록 */
	ev.events = EPOLLIN;
	ev.data.fd = metrics_listen_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, metrics_listen_fd, &ev);

	/* 채팅용/메트릭용 리스닝 소켓이 모두 bind에 성공했으므로 이 시점부터 준비 완료로 표시 */
	g_net_ready = true;
```
다음으로 변경:
```c
	/* 위에서 쓴 ev 변수를 재사용해 metrics_listen_fd도 같은 epoll 인스턴스(epfd)에 등록 */
	ev.events = EPOLLIN;
	ev.data.fd = metrics_listen_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, metrics_listen_fd, &ev);

	/*
	* Redis 연결(명령용 + 구독용) 초기화 (3단계)
	* net_init()의 다른 실패 경로와 동일하게, 실패하면 -1을 반환해 서버가 즉시 종료되게 함
	*/
	if (redis_client_init(REDIS_HOST, REDIS_PORT) < 0) {
		fprintf(stderr, "redis_client_init failed\n");
		return -1;
	}

	/* 구독 전용 연결의 fd를 같은 epoll 인스턴스에 등록 - metrics_listen_fd와 동일한 패턴 */
	ev.events = EPOLLIN;
	ev.data.fd = redis_client_sub_fd();
	epoll_ctl(epfd, EPOLL_CTL_ADD, redis_client_sub_fd(), &ev);

	/* 채팅용/메트릭용 리스닝 소켓이 모두 bind에 성공했으므로 이 시점부터 준비 완료로 표시 */
	g_net_ready = true;
```

- [ ] **Step 4: `net_run()`의 IO 큐 드레인 루프에 새 job 타입 처리 추가**

`server/net.c:440-448`의:
```c
		job_t job;
		while (job_queue_pop(&g_io_q, &job, JOBQ_NONBLOCK)) {
			if (job.type == JOB_SEND) {
				handle_send_job(&job);
			}
			else if (job.type == JOB_CLOSE) {
				close_connection(job.fd);
			}
		}
```
다음으로 변경:
```c
		job_t job;
		while (job_queue_pop(&g_io_q, &job, JOBQ_NONBLOCK)) {
			if (job.type == JOB_SEND) {
				handle_send_job(&job);
			}
			else if (job.type == JOB_CLOSE) {
				close_connection(job.fd);
			}
			else if (job.type == JOB_REDIS_SUBSCRIBE) {
				redis_subscribe_room(job.room_id);
			}
			else if (job.type == JOB_REDIS_UNSUBSCRIBE) {
				redis_unsubscribe_room(job.room_id);
			}
		}
```

- [ ] **Step 5: 이벤트 디스패치 루프에 구독 fd 처리 추가**

`server/net.c:465-469`의:
```c
			/* 메트릭/헬스체크 스크레이퍼 연결 처리 (채팅 연결과 다른 경로이므로 이후 로직으로 흘러가지 않도록 continue) */
			if (fd == metrics_listen_fd) {
				handle_metrics_accept();
				continue;
			}
```
다음 블록을 그 바로 뒤에 추가:
```c
			/* Redis 구독 연결에 pub/sub 메시지가 도착 (3단계) - 완전한 메시지를 전부 소진할 때까지 반복 */
			if (fd == redis_client_sub_fd()) {
				int room_id, except_id;
				packet_t pkt;
				int rc;
				while ((rc = redis_sub_read(&room_id, &except_id, &pkt)) == 1) {
					job_queue_push_room_deliver(&g_logic_q, room_id, except_id, &pkt);
				}
				if (rc < 0) {
					log_json("ERROR", "redis_sub_read_error", NULL);
				}
				continue;
			}
```

- [ ] **Step 6: 서버 종료 시 Redis 연결 정리**

`server/net.c:656-658` 근처(기존 `metrics_listen_fd` close 처리부)를 확인하고, 그 블록 뒤에 추가:
```c
	if (metrics_listen_fd >= 0) {
		close(metrics_listen_fd);
		metrics_listen_fd = -1;
	}
	redis_client_shutdown();
```

- [ ] **Step 7: 빌드 확인**

```bash
cd server && make clean && make
```
Expected: 경고/에러 없이 빌드 성공.

- [ ] **Step 8: Commit**

```bash
git add server/common.h server/net.c
git commit -m "feat: net.c epoll 루프에 Redis 구독 연결 통합 (3단계)"
```

---

### Task 7: main.c — state_init() 호출 추가

**Files:**
- Modify: `server/main.c`

**Interfaces:**
- Consumes: Task 4의 `state_init()`
- Produces: 서버 기동 시 방 슬롯 뮤텍스가 worker 스레드 생성 전에 전부 초기화됨

- [ ] **Step 1: `job_queue_init` 호출 뒤, worker 스레드 생성 전에 `state_init()` 추가**

`server/main.c:40-49`의:
```c
	/* 스레드 간 작업 큐 초기화 */
	job_queue_init(&g_logic_q);
	job_queue_init(&g_io_q);

	/*
	* 로직 worker thread 생성
	* detach하지 않고 tid를 배열에 보관 -> 종료 시 pthread_join으로 정리 완료를 기다리기 위함
	* (k8s가 pod 삭제 시 보내는 SIGTERM에 대해 graceful termination을 보장해야 함)
	*/
	pthread_t worker_tids[WORKER_THREAD_NUM];
```
다음으로 변경:
```c
	/* 스레드 간 작업 큐 초기화 */
	job_queue_init(&g_logic_q);
	job_queue_init(&g_io_q);

	/* 방 슬롯의 뮤텍스를 미리 전부 초기화 (3단계) - worker가 방을 건드리기 전에 끝나야 함 */
	state_init();

	/*
	* 로직 worker thread 생성
	* detach하지 않고 tid를 배열에 보관 -> 종료 시 pthread_join으로 정리 완료를 기다리기 위함
	* (k8s가 pod 삭제 시 보내는 SIGTERM에 대해 graceful termination을 보장해야 함)
	*/
	pthread_t worker_tids[WORKER_THREAD_NUM];
```

- [ ] **Step 2: `#include "state.h"` 추가**

`server/main.c:1-9`의:
```c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "net.h"
#include "logic.h"
#include "job_queue.h"
```
다음으로 변경:
```c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "net.h"
#include "logic.h"
#include "job_queue.h"
#include "state.h"
```

- [ ] **Step 3: 빌드 확인**

```bash
cd server && make clean && make
```
Expected: 경고/에러 없이 빌드 성공. 이 시점부터 `./server` 실행이 (WSL에 redis-server가 떠 있다면)
정상적으로 가능해야 한다.

- [ ] **Step 4: Commit**

```bash
git add server/main.c
git commit -m "feat: main.c가 state_init()을 워커 생성 전에 호출하도록 배선 (3단계)"
```

---

### Task 8: WSL 로컬 통합 검증

**Files:** (코드 변경 없음 — 검증 전용 Task)

**Interfaces:**
- Consumes: Task 1-7의 전체 결과물
- Produces: 매치메이킹/발행-구독 파이프라인이 실제로 올바르게 동작한다는 증거 (단일 프로세스 기준 —
  cross-pod 검증은 Task 10)

- [ ] **Step 1: redis-server 기동 확인**

```bash
sudo service redis-server start
redis-cli PING
```
Expected: `PONG`.

- [ ] **Step 2: Redis 상태 초기화(이전 테스트 잔여물 제거) 후 서버 기동**

```bash
redis-cli FLUSHALL
cd server && make clean && make && ./server
```
Expected: `server_started`, `redis_client_ready`, `state_initialized` 로그가 순서대로 JSON 한 줄씩
출력됨.

- [ ] **Step 3: 별도 터미널 두 개에서 클라이언트 접속 후 매치메이킹 확인**

터미널 A:
```bash
python3 client/client.py --host 127.0.0.1 --port 3800 --local-echo
```
`/join` 입력 → `[INFO] sent JOIN` 출력 확인.

터미널 B:
```bash
python3 client/client.py --host 127.0.0.1 --port 3800 --local-echo
```
`/join` 입력.

- [ ] **Step 4: Redis 쪽 상태를 직접 확인**

세 번째 터미널:
```bash
redis-cli GET room_next_id
redis-cli GET room:0:count
```
Expected: `room_next_id` = `1`, `room:0:count` = `2` (두 클라이언트가 같은 방에 배정됨).

- [ ] **Step 5: 발행-구독 왕복(자기 자신의 SUBSCRIBE로 수신) 확인**

터미널 A에서 `hello`를 입력해서 전송. 터미널 B의 화면에 `[CHAT] hello`가 출력되는지 확인한다.
(같은 프로세스 안에서도 이 메시지는 실제로 Redis PUBLISH → SUBSCRIBE 왕복을 거쳐서 온 것이다 —
`room_broadcast_local`이 아니라 `redis_publish_chat`/`redis_sub_read` 경로가 정상 동작함을 뜻함)

터미널 A 자신의 화면에는 `hello`가 (로컬 에코 `[ME] hello` 한 번만) 나오고 `[CHAT] hello`로는 **다시
오지 않아야 한다** — `except_session_id` 제외 로직이 올바르게 동작함을 뜻함.

- [ ] **Step 6: 퇴장 후 회수 확인**

터미널 A, B 모두 `/quit`으로 종료. 세 번째 터미널에서:
```bash
redis-cli EXISTS room:0:count
redis-cli LRANGE room_freelist 0 -1
```
Expected: `room:0:count`는 `0`(존재하지 않음), `room_freelist`에 `0`이 들어있음(회수됨).

- [ ] **Step 7: 서버 로그에서 이상 로그(ERROR 레벨) 없는지 확인**

서버를 띄운 터미널의 출력을 스크롤해서 `"level":"ERROR"`가 한 건도 없는지 확인한다.

- [ ] **Step 8: 서버 종료(Ctrl+C)로 graceful shutdown 로그 확인**

Expected: `shutdown_started` → `shutdown_completed` 로그가 끝까지 출력되고 프로세스가 정상 종료됨
(0단계 #3의 검증과 동일한 기준).

이 Task는 코드 변경이 없으므로 커밋하지 않는다. 문제가 발견되면 해당 Task로 돌아가 수정 후 다시
검증한다.

---

### Task 9: TSan 재검증

**Files:** (코드 변경 없음 — 검증 전용 Task)

**Interfaces:**
- Consumes: Task 1-8의 전체 결과물
- Produces: Redis 관련 코드를 포함해도 데이터 레이스가 0건임을 보이는 증거

- [ ] **Step 1: TSan 빌드**

```bash
cd server
g++ -std=c++17 -Wall -Wextra -O0 -g -fsanitize=thread -pthread -no-pie \
  $(ls *.c) $(ls *.cpp) -lhiredis -o server_tsan -x c $(ls *.c) -x c++ $(ls *.cpp)
```

기존 2단계에서 쓰던 것과 동일하게, C 파일과 C++ 파일을 각각 올바른 언어 모드로 컴파일해야 하므로
Makefile의 `%.o: %.c` / `%.o: %.cpp` 규칙을 TSan 플래그로 재사용하는 편이 안전하다:
```bash
cd server
make clean
CFLAGS="-Wall -Wextra -O0 -g -fsanitize=thread -pthread" \
CXXFLAGS="-Wall -Wextra -O0 -g -fsanitize=thread -pthread -std=c++17" \
LDFLAGS="-fsanitize=thread -pthread -lhiredis -no-pie" \
make
```

- [ ] **Step 2: ASLR 비활성화 후 실행 (WSL2 TSan 섀도우 메모리 이슈 회피, 2단계에서 확립한 방법)**

```bash
redis-cli FLUSHALL
setarch $(uname -m) -R ./server &
```

- [ ] **Step 3: 다수 동시 접속/즉시 종료를 반복해 레이스 유도**

2단계에서 쓴 것과 동일한 방식으로, `client/client.py`를 짧은 생명주기로 반복 실행하는 셸 루프를 돌린다:
```bash
for i in $(seq 1 200); do
  (echo "/join"; sleep 0.05; echo "hi from $i"; sleep 0.05; echo "/quit") | \
    timeout 2 python3 client/client.py --host 127.0.0.1 --port 3800 &
done
wait
```

- [ ] **Step 4: 서버를 종료하고 TSan 리포트 확인**

```bash
kill %1
wait
```
Expected: `WARNING: ThreadSanitizer: data race` 문자열이 서버 출력 어디에도 없어야 한다(0건).
있으면 리포트에 찍힌 파일/라인을 근거로 해당 Task로 돌아가 락 순서(`g_rooms_lock → room->lock →
s->lock`)나 `g_redis_cmd_lock` 사용을 재검토한다.

이 Task는 코드 변경이 없으므로 커밋하지 않는다. 레이스가 발견되면 원인 Task에서 수정 후 이 Task를
다시 수행한다.

---

### Task 10: k8s 매니페스트 + kind 최종 검증

**Files:**
- Create: `k8s/redis.yaml`
- Modify: `k8s/chat-server.yaml`
- Modify: `server/Dockerfile`

**Interfaces:**
- Consumes: Task 1-9의 전체 결과물
- Produces: `replicas: 3`으로 배포된 클러스터에서 서로 다른 pod에 붙은 클라이언트끼리 대화가
  성립한다는 증거 (spec의 최종 성공 기준)

- [ ] **Step 1: `server/Dockerfile`의 build/runtime 스테이지에 hiredis 패키지 추가**

`server/Dockerfile:1-10`의:
```dockerfile
# ---- build stage ----
# 컴파일에 필요한 gcc 툴체인은 이 스테이지에만 있으면 되고, 최종 런타임 이미지에는 남기지 않음
FROM gcc:13 AS build

WORKDIR /src

# 소스 전체를 빌드 컨테이너 안으로 복사한 뒤, 기존 Makefile 그대로 사용해 빌드
# (호스트 OS/컴파일러 버전에 상관없이 컨테이너 안에서 재현 가능하게 만드는 것이 이 단계의 목적)
COPY . .
RUN make clean && make
```
다음으로 변경:
```dockerfile
# ---- build stage ----
# 컴파일에 필요한 gcc 툴체인은 이 스테이지에만 있으면 되고, 최종 런타임 이미지에는 남기지 않음
FROM gcc:13 AS build

# hiredis 헤더/라이브러리 (3단계) - libhiredis-dev는 런타임 .so도 의존성으로 함께 설치함
RUN apt-get update && apt-get install -y --no-install-recommends libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 소스 전체를 빌드 컨테이너 안으로 복사한 뒤, 기존 Makefile 그대로 사용해 빌드
# (호스트 OS/컴파일러 버전에 상관없이 컨테이너 안에서 재현 가능하게 만드는 것이 이 단계의 목적)
COPY . .
RUN make clean && make
```

**주의**: 빌드 컨텍스트는 `server/`(검증 명령이 `docker build -t chat-server:dev server/`이므로) —
즉 `COPY . .`가 이미 `server/` 안의 내용을 `/src`에 복사하는 것이라 `RUN` 줄은 원본 그대로
`make clean && make`이며 `cd server &&`를 붙이면 안 된다(붙이면 `/src/server`가 없어서 빌드가 깨짐).

`server/Dockerfile:12-23`의 runtime 스테이지(`FROM debian:bookworm-slim` 이후)에도 동일하게 추가:
```dockerfile
# ---- runtime stage ----
# 빌드 도구가 전혀 필요 없는 최소 이미지. 공격 표면과 이미지 크기를 줄이기 위해 slim 사용
FROM debian:bookworm-slim

# hiredis 런타임 라이브러리 (3단계) - -dev 패키지를 그대로 써서 정확한 런타임 전용 패키지명을
# 추측하지 않음(포트폴리오 규모에서 이미지 크기보다 확실한 동작을 우선함)
RUN apt-get update && apt-get install -y --no-install-recommends libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

# 컨테이너 안에서 root로 프로세스를 띄우지 않도록 전용 사용자/그룹 생성
```

`COPY --from=build /src/server /app/server` 줄(런타임 스테이지)은 그대로 둔다 — 빌드 컨텍스트가
`server/`이므로 `/src` 안에서 만들어지는 실행 파일 경로는 `/src/server`(디렉터리가 아니라 Makefile의
`TARGET := server`가 만드는 바이너리 파일)가 맞다.

- [ ] **Step 2: `k8s/redis.yaml` 신규 작성**

```yaml
# Redis: 3단계(pub/sub 스케일아웃)의 클러스터 전역 매치메이킹/메시지 전파 저장소.
# k8s/loki.yaml과 동일한 패턴(StatefulSet + PVC + Service)
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: redis-data
spec:
  accessModes: ["ReadWriteOnce"]
  resources:
    requests:
      storage: 512Mi
---
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: redis
  labels:
    app: redis
spec:
  serviceName: redis
  replicas: 1
  selector:
    matchLabels:
      app: redis
  template:
    metadata:
      labels:
        app: redis
    spec:
      containers:
        - name: redis
          image: redis:7-alpine
          ports:
            - name: redis
              containerPort: 6379
          volumeMounts:
            - name: data
              mountPath: /data
      volumes:
        - name: data
          persistentVolumeClaim:
            claimName: redis-data
---
apiVersion: v1
kind: Service
metadata:
  name: redis
  labels:
    app: redis
spec:
  selector:
    app: redis
  ports:
    - name: redis
      port: 6379
      targetPort: 6379
```

- [ ] **Step 3: `server/common.h`의 `REDIS_HOST`를 k8s 서비스 이름으로 변경**

`server/common.h`의 (Task 6에서 추가한):
```c
#define REDIS_HOST "127.0.0.1"   /* k8s에서는 서비스 이름으로 바뀔 예정 (Task 10에서 배포 시 확정) */
```
다음으로 변경:
```c
#define REDIS_HOST "redis"   /* k8s Service 이름 - k8s DNS가 자동으로 해석함 (k8s/redis.yaml) */
```

**주의**: 이 변경은 WSL 로컬 실행(Task 8/9)을 더 이상 지원하지 않게 만든다(`redis`라는 호스트명은
k8s 클러스터 안에서만 풀림). 로컬 반복 개발이 이미 끝난 이 시점(Task 10)에는 문제가 없지만, 이후
로컬에서 다시 디버깅해야 한다면 `/etc/hosts`에 `127.0.0.1 redis`를 추가하거나 이 상수를 임시로
되돌려야 한다.

- [ ] **Step 4: `k8s/chat-server.yaml`을 `replicas: 3`으로 변경 + 주석 갱신**

`k8s/chat-server.yaml:1-4`의:
```yaml
# 채팅 서버 본체: ConfigMap(자리만 마련, 현재는 미사용) + Deployment(replicas=1) + Service
#
# replicas=1인 이유: 세션이 fd 기반이라(server/state.c) pod 간 메시지 전달 경로가 없음
# (2026-08-20-infra-expansion-design.md 참고). 3단계에서 Redis pub/sub 도입 후 늘릴 예정
```
다음으로 변경:
```yaml
# 채팅 서버 본체: ConfigMap(자리만 마련, 현재는 미사용) + Deployment(replicas=3) + Service
#
# replicas=3: 3단계(Redis pub/sub)에서 매치메이킹과 메시지 전파를 Redis로 위임해
# pod 간 메시지 전달 경로가 생겼으므로 수평 확장이 가능해짐 (2026-08-28-redis-pubsub-design.md 참고)
```

`k8s/chat-server.yaml`의 `spec.replicas: 1`을 `spec.replicas: 3`으로 변경.

- [ ] **Step 5: kind 클러스터에 배포**

```bash
kind create cluster --config k8s/kind-config.yaml   # 이미 있으면 생략
docker build -t chat-server:dev server/
kind load docker-image chat-server:dev --name chat-server
kubectl apply -f k8s/redis.yaml
kubectl apply -f k8s/
kubectl rollout status statefulset/redis
kubectl rollout status deployment/chat-server
```
Expected: 모든 리소스가 `Running`/`Ready` 상태.

- [ ] **Step 6: pod 3개 확인**

```bash
kubectl get pods -l app=chat-server
```
Expected: `chat-server-xxx` pod 3개 모두 `1/1 Running`.

- [ ] **Step 7: 서로 다른 pod에 붙은 클라이언트끼리 대화 확인 (spec의 최종 성공 기준)**

```bash
kubectl port-forward pod/$(kubectl get pods -l app=chat-server -o jsonpath='{.items[0].metadata.name}') 3801:3800 &
kubectl port-forward pod/$(kubectl get pods -l app=chat-server -o jsonpath='{.items[1].metadata.name}') 3802:3800 &
```

터미널 A: `python3 client/client.py --host 127.0.0.1 --port 3801 --local-echo` → `/join`
터미널 B: `python3 client/client.py --host 127.0.0.1 --port 3802 --local-echo` → `/join`

터미널 A에서 `cross-pod hello` 입력 → 터미널 B에 `[CHAT] cross-pod hello`가 출력되면 성공.

- [ ] **Step 8: pod 로그로 어느 pod에 붙었는지 교차 확인**

```bash
kubectl logs -l app=chat-server --prefix=true | grep -E "session_created|redis_client_ready"
```
`--prefix=true`가 붙여주는 pod 이름으로, A와 B가 실제로 서로 다른 pod에 접속했음을 확인한다.

- [ ] **Step 9: port-forward 정리**

```bash
kill %1 %2
```

- [ ] **Step 10: Commit**

```bash
git add k8s/redis.yaml k8s/chat-server.yaml server/Dockerfile server/common.h
git commit -m "feat: k8s에 Redis StatefulSet 추가, chat-server를 replicas 3으로 확장 (3단계)"
```

---

### Task 11: REDESIGN.md에 3단계 섹션 추가

**Files:**
- Modify: `REDESIGN.md` (저장소 루트, gitignore됨 — 있는 그대로 개인 포트폴리오 문서로 갱신)

**Interfaces:** 없음 (문서 전용 Task)

- [ ] **Step 1: 기존 §6(세션 재설계)/§8(C++ 혼용) 서술 방식을 확인**

`REDESIGN.md`를 열어 기존 섹션들이 "기존 흐름 → 문제 → 재설계 → 담당 함수" 순서로 서술돼 있는지
확인한다(브레인스토밍 이전 대화에서 이미 이 형식으로 작성된 문서).

- [ ] **Step 2: §9(가칭) 섹션 추가**

기존 섹션과 동일한 형식으로, 다음 내용을 담아 새 섹션을 추가한다:
- 기존 흐름: pod-local 매치메이킹(`room_find`/`room_create`)과 `room_broadcast()`의 fd 직접 전달이
  `replicas > 1`에서 왜 깨지는지 (design doc §4의 다이어그램 인용)
- 문제: 매치메이킹 자체가 pod마다 독립적이라 room_id가 pod 경계를 넘어 안정적이지 않음
- 재설계: Redis EVALSHA로 클러스터 전역 매치메이킹, PUBLISH/SUBSCRIBE로 단일 배송 경로, net 스레드의
  구독 연결 독점 소유(`JOB_REDIS_SUBSCRIBE`/`JOB_REDIS_UNSUBSCRIBE`)
- 담당 함수: `redis_client.cpp`의 `redis_join_room`/`redis_leave_room`/`redis_publish_chat`/
  `redis_sub_read`, `state.cpp`의 `room_get_or_init`/`room_broadcast_local`, `logic.c`의
  `PKT_JOIN_ROOM`/`PKT_CHAT` 핸들러
- 검증 결과: Task 8(WSL 통합)/Task 9(TSan)/Task 10(kind cross-pod) 각각의 결과를 요약

- [ ] **Step 3: Commit**

```bash
git add REDESIGN.md
git commit -m "docs: REDESIGN.md에 3단계(Redis pub/sub) 섹션 추가"
```

---

## Self-Review 결과

- **스펙 커버리지**: `2026-08-28-redis-pubsub-design.md`의 모든 결정 사항(매치메이킹 범위, 연결 동시성,
  언어, room_id 스킴, 배송 경로 통일, 구독 소유권, 장애 처리, 로컬 개발 환경, k8s 변경, 검증 계획)에
  대응하는 Task가 각각 존재함(Task 1-11).
- **플레이스홀더 스캔**: "TBD"/"나중에 구현" 등 표현 없음. Task 10 Step 1 초안에서 빌드 컨텍스트가
  `server/`임을 놓치고 `RUN cd server && make`로 잘못 쓴 것을 자체 검토 중 발견해 원본 그대로
  `RUN make clean && make`로 수정함(빌드 컨텍스트가 이미 `server/`라 `COPY . .`가 그 안의 내용을
  `/src`에 복사하므로).
- **타입/시그니처 일관성**: `room_join()`의 `bool` 반환, `room_broadcast_local(int, int, packet_t*)`,
  `job_t.room_id` 필드, `redis_client.h`의 모든 함수 시그니처가 Task 3에서 정의된 그대로 Task 4-6에서
  동일하게 사용됨을 확인함.
