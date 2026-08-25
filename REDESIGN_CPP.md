C/C++ 혼용 리팩터링 계획

     Context

     2단계(세션 재설계) 작업 중 state.c에서 참조 카운트, 중첩 락(g_rooms_lock → room->lock → s->lock) 순서를 전부 손으로 맞추다가 실수로 새
     데이터 레이스를 만들어낸 적이 있었다(TSan 레이스 18건 → 1차 수정 후 1501건 → 재수정 후 0건, REDESIGN.md §6 참고). 이런 종류의 실수(락
     짝 안 맞음, 참조 카운트 계산 실수, 가변인자 타입 태그 실수)는 C++의 RAII(lock_guard 패턴)와 shared_ptr가 구조적으로 막아주는 문제라,
     사용자가 이 부분들을 C++로 옮기는 방향을 제안했다.

     목표: 동시성 상태 관리가 몰려있는 state.c, job_queue.c, log.c를 C++(.cpp)로 옮겨 RAII/스마트 포인터의 이득을 얻되,
     net.c/protocol.c/main.c/logic.c는 순수 C로 유지한다(소켓/epoll 계층은 이미 TSan으로 검증됐고 건드릴 이유가 없다는 원칙 유지). 동시성
     프리미티브 자체는 pthread_mutex_t/pthread_cond_t(POSIX)를 그대로 쓰고, 그 위에 RAII 래퍼만 씌운다(std::mutex로 교체하지 않음).

     핵심 제약: C 파일(net.c, logic.c, main.c)이 계속 이 모듈들을 호출하므로, state.h/job_queue.h/log.h에 선언된 공개 함수의 시그니처(반환
     타입, 매개변수)는 지금과 완전히 동일하게 유지해야 한다(extern "C"). C++ 전용 이득(shared_ptr 반환, RAII 타입 노출 등)은 각 .cpp 파일
     내부 구현에서만 실현되고, 콜사이트(logic.c의 session_release() 수동 호출 등)는 변경되지 않는다.

     변경 범위

     ┌────────────────────────────────────────────────────────────────┬───────────────────────────────────────────────────────────────┐
     │                              파일                              │                             처리                              │
     ├────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤
     │ server/state.c → server/state.cpp                              │ C++로 재작성                                                  │
     ├────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤
     │ server/job_queue.c → server/job_queue.cpp                      │ C++로 재작성                                                  │
     ├────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤
     │ server/log.c → server/log.cpp                                  │ C++로 재작성                                                  │
     ├────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤
     │ server/state.h, server/job_queue.h, server/log.h               │ extern "C" 가드 추가, 시그니처는 불변                         │
     ├────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤
     │ server/net.c, server/protocol.c, server/main.c, server/logic.c │ 변경 없음 (순수 C 유지)                                       │
     ├────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤
     │ server/Makefile                                                │ .cpp 컴파일 규칙(g++) 추가, 링크 시 C++ 런타임 포함           │
     ├────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤
     │ server/Dockerfile                                              │ 빌드 스테이지에 g++ 필요 여부 확인(gcc:13 이미지에 보통 포함) │
     └────────────────────────────────────────────────────────────────┴───────────────────────────────────────────────────────────────┘

     설계 상세

     1. state.h/job_queue.h/log.h — extern "C" 가드

     #ifdef __cplusplus
     extern "C" {
     #endif
     /* 기존 선언 그대로 */
     #ifdef __cplusplus
     }
     #endif
     세 헤더 모두 이 패턴으로 감싸서, .c/.cpp 양쪽에서 동일하게 include 가능하게 만든다. 함수 시그니처(예: session_t* session_get_by_id(int
     session_id);)는 한 글자도 바꾸지 않는다 — net.c/logic.c가 재컴파일 없이(아니, 재컴파일은 하지만 코드 수정 없이) 그대로 링크되게 하기
     위함.

     2. state.cpp 내부 재설계

     - 세션 테이블: session_t* sessions[MAX_CLIENTS] 선형 배열 → 내부적으로 std::unordered_map<int, std::shared_ptr<SessionImpl>> g_sessions
       (session_id로 O(1) 조회). session_t는 기존 POD 구조체를 그대로 두거나, SessionImpl이라는 내부 C++ 클래스로 감싸고 공개 API에서는
       reinterpret_cast/opaque handle로 session_t*를 반환(레이아웃 호환 유지).
     - 참조 카운트: 손으로 만든 refcount/session_acquire/session_release 로직을 std::shared_ptr의 제어 블록에 위임. 공개 함수
       session_release(session_t*)는 내부적으로 해당 세션의 shared_ptr 참조를 하나 소멸시키는 방식으로 구현(예: 세션별로 "이 raw pointer에
       대응하는 shared_ptr을 찾아 리셋"하는 내부 매핑 필요 — 구현 시 raw pointer ↔ shared_ptr 대응을 안전하게 유지하는 방법을 설계 단계에서
       확정).
     - 락: session_t/room_t에 있는 pthread_mutex_t lock은 그대로 두고, 그 위에 작은 RAII 클래스(예: class PosixLockGuard {
       PosixLockGuard(pthread_mutex_t&); ~PosixLockGuard(); };)를 만들어 모든 lock/unlock 쌍을 이걸로 교체. session_deactivate(),
       room_join()/room_leave()의 중첩 락(g_rooms_lock → room->lock → s->lock) 순서는 그대로 유지하되, 각 단계를 RAII 객체의 스코프로
       표현해서 "이 return 경로에서 unlock 깜빡함" 클래스의 버그를 구조적으로 방지.
     - 방 free list: int room_free_list[MAX_ROOMS] + room_free_top → std::vector<int>(스택처럼 push_back/pop_back 사용)로 교체 가능(선택
       사항, 기능은 동일).
     - 공개 API는 100% 유지: session_create, session_get_by_id, session_acquire, session_release, session_remove_by_id, session_is_alive,
       session_get_room_id, session_deactivate, session_remove_all, room_get, room_create, room_find, room_join, room_leave, room_broadcast,
       state_count_active_sessions, state_count_active_rooms — 이름, 시그니처, 동작 전부 동일.

     3. job_queue.cpp 내부 재설계

     - 내부 저장소를 고정 배열(circular buffer) → std::deque<job_t>(또는 std::queue<job_t>)로 교체.
     - 중요: 지금의 유계(bounded) + 배압(backpressure) 동작을 유지해야 함 — std::deque는 기본이 무제한 확장이므로, push 시 size() >=
       JOB_QUEUE_SIZE면 pthread_cond_wait로 블록시키는 로직을 직접 구현(사용자와 이미 합의: STL 컨테이너 사용하되 유계 동작은 직접 구현).
     - 락/조건변수는 pthread_mutex_t/pthread_cond_t 그대로, RAII 래퍼(§2와 동일한 PosixLockGuard)로 감쌈.
     - 공개 API 전부 유지: job_queue_init, job_queue_push, job_queue_pop, job_queue_depth, job_queue_push_packet, job_queue_push_disconnect,
       job_queue_push_send, job_queue_push_close, job_queue_push_shutdown. job_queue_t는 여전히 C에서 값으로 다룰 수 있는 타입이어야
       함(main.c가 job_queue_t g_logic_q; 전역으로 선언) — opaque 포인터로 바꾸거나, 구조체 안에 C++ 객체를 pimpl로 숨기는 방식 중 빌드
       검증하며 결정.

     4. log.cpp

     - 기존 extern "C" void log_json(const char* level, const char* event, ...) — 그대로 유지(net.c/logic.c/main.c가 이 시그니처로 계속
       호출).
     - 내부 구현(json_escape, va_list 처리)은 C++로 옮기되 동작 동일.
     - (선택) state.cpp 자신의 호출부를 위해 타입 안전한 C++ 전용 오버로드/템플릿을 추가로 노출할 수 있음 — 필수는 아니고, 시간이 되면 추가.

     5. 빌드 시스템

     - server/Makefile: .cpp 파일은 g++ -std=c++17 -Wall -Wextra -O2 -pthread -c로, .c 파일은 기존 gcc로 컴파일. 최종 링크는 g++로 수행(C++
       런타임 자동 링크).
     - server/Dockerfile: build 스테이지(gcc:13)에 g++가 포함되어 있는지 실제로 빌드해서 확인. 없으면 이미지 태그를 gcc:13(g++ 포함 버전
       확인) 또는 apt-get install g++ 한 줄 추가.

     검증 계획

     1. make clean && make — 경고 없이 빌드되는지 확인 (C/C++ 혼합 컴파일)
     2. 2단계에서 썼던 것과 동일한 TSan 스트레스 테스트를 재실행해서 레이스 0건이 유지되는지 확인 (-fsanitize=thread, join/chat/leave 반복
        스트레스, WSL에서 setarch -R로 실행)
     3. 10,000개 연결 기능 스트레스 테스트 재실행 — /metrics의 chat_disconnects_total 정확성, 방 회수(reused 카운트) 확인
     4. docker build로 이미지가 정상 빌드되는지 확인 (g++ 관련 이슈 조기 발견)
     5. REDESIGN.md에 이번 변경(C/C++ 혼용 이유와 범위)을 추가 섹션으로 기록
