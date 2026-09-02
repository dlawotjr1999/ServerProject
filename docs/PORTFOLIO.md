# 프로젝트 개요 — epoll 기반 채팅 서버 + Kubernetes 수평 확장

## 한 줄 요약

C로 작성한 epoll 기반 멀티스레드 TCP 채팅 서버를, 세션 재설계·C++ 부분 도입·Redis 기반 클러스터
조율을 거쳐 Kubernetes 위에서 `replicas: 3`으로 실제 수평 확장되는 시스템까지 발전시킨 프로젝트.
소켓 프로그래밍(동시성, 프로토콜 설계, fd 생명주기)과 인프라(컨테이너화, 관측성, 분산 상태 조율,
장애 복구)를 모두 직접 설계·구현·검증했습니다.

## 기술 스택

| 영역                    | 사용 기술                                                           |
| ----------------------- | ------------------------------------------------------------------- |
| 서버 코어               | C(소켓/epoll/프로토콜), C++17(동시성 상태 관리), pthread            |
| 클러스터 조율           | Redis 7(hiredis 클라이언트), Lua(`EVALSHA` 원자적 매치메이킹)       |
| 컨테이너/오케스트레이션 | Docker(multi-stage build), Kubernetes(kind), StatefulSet/Deployment |
| 관측성                  | Prometheus(메트릭), Loki + Promtail(로그 수집), Grafana(대시보드)   |
| 검증 도구               | ThreadSanitizer(TSan), `gcc`/`g++`, 수동/스크립트 스모크 테스트     |
| 클라이언트              | Python(테스트/데모용 TCP 클라이언트)                                |

---

## 아키텍처

### 스레드 모델 (pod 하나 기준)

```
                         accept/recv/send, 유일한 epoll 스레드
┌─────────────────────────────────────────────────────────────────┐
│  net 스레드 (net.c)                                                │
│  - epoll_wait로 채팅 fd + Redis 구독 fd + eventfd(wakeup) 감시       │
│  - JOB_PACKET(non-blocking, CHAT은 드롭 가능) /                    │
│    JOB_PACKET_BLOCKING(JOIN·LEAVE, 드롭 불가) 를 g_logic_q로 push   │
│  - g_io_q를 드레인해 실제 send()/close() 수행                       │
└───────────────────────┬─────────────────────────────┬─────────────┘
                         │ g_logic_q                    │ g_io_q
                         ▼                               ▲
          ┌──────────────────────────┐                   │
          │ logic worker × 4 (logic.c)│───────────────────┘
          │ - 세션/방 상태 갱신         │
          │ - Redis 매치메이킹/발행 호출│
          └──────────────┬────────────┘
                         │ g_redis_cmd_lock으로 직렬화
                         ▼
          ┌──────────────────────────┐
          │  Redis (redis_client.cpp) │
          │  - EVALSHA 매치메이킹      │
          │  - PUBLISH/SUBSCRIBE      │
          │  - pod lease/heartbeat    │
          └──────────────────────────┘

  metrics 전담 스레드(net.c)         heartbeat 전담 스레드(net.c)
  - /metrics, /healthz, /readyz     - 10초마다 이 pod이 로컬 멤버를
    동기 처리(각 2초 타임아웃)         가진 모든 방의 Redis lease 갱신
  - net 스레드와 완전히 분리          - net 스레드와 완전히 분리
```

핵심 원칙: **net 스레드(epoll 루프)는 절대 blocking I/O에 묶이지 않는다.** 채팅 소켓 I/O, Redis
구독 읽기, `g_io_q` 드레인까지 전부 이 스레드가 담당하므로, 여기서 뭔가 하나라도 오래 막히면 모든
클라이언트가 동시에 영향을 받는다. 그래서 metrics 처리(동기 recv/send, 최대 4초)와 Redis 하트비트
(pod당 최대 256개 방 × 3개 명령)는 각각 전담 스레드로 분리했고, 큐 push조차 non-blocking을
기본으로 하되 JOIN/LEAVE처럼 유실되면 안 되는 상태 변경 요청만 예외적으로 blocking을 허용한다.

### 클러스터 토폴로지 (k8s)

```
                    Service(chat-server, LoadBalancer)
                    ┌─────────┬─────────┬─────────┐
                    │ pod A   │ pod B   │ pod C   │  (replicas: 3)
                    └────┬────┴────┬────┴────┬────┘
                         │  PUBLISH/SUBSCRIBE, EVALSHA로 조율
                         └────────┬┴─────────┘
                                  ▼
                            Redis (StatefulSet, replicas: 1)
                            - room:{id}:count       (매치메이킹 상태)
                            - room:{id}:pod:{pod}:* (lease/heartbeat)

  Prometheus ── scrape ──▶ chat-server:9100/metrics, redis
  Promtail   ── tail   ──▶ 각 pod의 stdout(JSON 로그) ──▶ Loki
  Grafana    ── query  ──▶ Prometheus + Loki
```

- 방 상태는 pod 로컬 메모리(`rooms[MAX_ROOMS]`)와 Redis 양쪽에 존재한다 — 로컬은 "이 pod에 실제
  연결된 유저 목록"(브로드캐스트 대상 조회용), Redis는 "클러스터 전역에서 이 방이 얼마나 찼는지"
  (매치메이킹용)를 담당한다.
- 채팅 메시지는 로컬 브로드캐스트를 직접 하지 않고 **항상** Redis에 PUBLISH한 뒤, 발행한 pod
  자신을 포함해 그 채널을 구독 중인 모든 pod가 SUBSCRIBE로 받아 각자의 로컬 멤버에게 전달한다 —
  "로컬 배송"과 "원격 배송"을 코드 경로로 분리하지 않아, 그 둘 사이의 타이밍 차이로 생기는 레이스를
  원천적으로 피한다.

### 컨테이너화

`server/Dockerfile`은 멀티스테이지(`gcc:13` 빌드 → `debian:bookworm-slim` 런타임)로 최종
이미지에 컴파일러를 남기지 않고, non-root 사용자로 실행합니다. `ENTRYPOINT ["/app/server"]`를
**exec 형태**로 쓴 게 사소해 보여도 중요한 선택입니다 — 셸 형태(`CMD server`)로 실행하면 셸이
컨테이너의 PID 1이 되어, `docker stop`/`kubectl delete pod`가 보내는 SIGTERM이 서버 프로세스까지
전달되지 않습니다. 4개 worker를 `pthread_join`으로 기다리는 graceful shutdown을 아무리 잘
구현해도, 이 한 줄이 틀리면 애초에 신호 자체가 프로세스에 도달하지 않아 전부 무의미해집니다.

### 관측성 스택은 전부 선언적으로

Prometheus는 `prometheus-operator`/`ServiceMonitor` 없이, `kubernetes_sd_configs(role: pod)` +
pod annotation(`prometheus.io/scrape`, `prometheus.io/port`, `prometheus.io/path`)만으로
chat-server를 자동 발견합니다. Grafana는 datasource(Prometheus/Loki)와 대시보드를 전부
ConfigMap 프로비저닝으로 등록하고, datasource `uid`를 고정값으로 박아둡니다(안 그러면 클러스터를
새로 만들 때마다 Grafana가 랜덤 UID를 발급해서 대시보드 JSON의 datasource 참조가 깨짐) —
"클러스터를 지우고 새로 만들어도 `kubectl apply -f k8s/` 한 번이면 완전히 동일한 상태로
돌아온다"를 지키기 위해 UI에서 손으로 클릭하는 단계를 하나도 남기지 않았습니다.

---

## 모듈 설명

| 파일                                          | 역할                                                                                                                                                                                                       |
| --------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `main.c`                                      | 프로세스 진입점. 시그널 처리, worker/스레드 생성, graceful shutdown 시퀀스(`net_run()` 종료 → `g_io_q` 드레인하며 worker join 대기 → Redis 연결 정리)                                                      |
| `net.c`                                       | epoll 이벤트 루프, accept/recv/send, 세션 fd 관리, metrics/heartbeat 전담 스레드 소유                                                                                                                      |
| `protocol.c`                                  | length-prefixed 프레이밍 파싱(TCP 스트림의 fragmentation/coalescing 처리), network byte order 변환                                                                                                         |
| `job_queue.cpp`                               | net ↔ logic 스레드 간 job 큐(고정 크기 circular buffer + `pthread_mutex`/`cond`). blocking/non-blocking push를 모두 제공하고, 어떤 job 타입이 어느 쪽을 써야 하는지가 이 프로젝트의 핵심 설계 결정 중 하나 |
| `logic.c`                                     | 패킷 핸들러(JOIN/CHAT/LEAVE), logic worker 4개가 공유                                                                                                                                                      |
| `state.cpp`                                   | 세션/방 생명주기. `session_id` 기반 신원 관리, `shared_ptr` 내부 참조 카운트, 방 매치메이킹을 Redis로 위임                                                                                                 |
| `redis_client.cpp`                            | Redis 연결 관리(명령용/구독용 분리), `EVALSHA` 매치메이킹 Lua 스크립트, PUBLISH/SUBSCRIBE, pod lease/heartbeat, 재연결(lazy recovery)                                                                      |
| `metrics.c`                                   | Prometheus 텍스트 포맷 메트릭 렌더링                                                                                                                                                                       |
| `log.cpp`                                     | 구조화 JSON 로그                                                                                                                                                                                           |
| `posix_lock.hpp`                              | `pthread_mutex_t`용 RAII 락 가드(`PosixLockGuard`)                                                                                                                                                         |
| `server/Dockerfile`                           | 멀티스테이지 빌드(`gcc:13` → `debian:bookworm-slim`), non-root 실행, exec 형태 `ENTRYPOINT`(SIGTERM 전달 보장)                                                                                             |
| `k8s/chat-server.yaml`                        | chat-server Deployment(`replicas: 3`) + Service + ConfigMap                                                                                                                                                |
| `k8s/redis.yaml`                              | Redis StatefulSet(`replicas: 1`, emptyDir) + Service                                                                                                                                                       |
| `k8s/{prometheus,loki,promtail,grafana}.yaml` | 관측성 스택 — pod annotation 기반 Prometheus 자동 discovery, DaemonSet 기반 로그 수집(Promtail→Loki), ConfigMap 기반 Grafana 프로비저닝                                                                    |

---

## 핵심 설계 결정

- **세션 신원은 fd가 아니라 `session_id`.** k8s liveness probe가 TCP 연결을 수 초마다 열고 닫으면서
  fd가 예상보다 훨씬 빠르게 재사용되는 걸 실제로 관측하고 재설계했다(REDESIGN.md §2-3).
- **동시성 상태가 몰린 파일만 C++로.** `net.c`/`protocol.c`/`main.c`/`logic.c`(순수 소켓 계층,
  이미 TSan으로 검증됨)는 C로 남기고, `state.cpp`/`job_queue.cpp`는 C++로 옮겨 RAII 락 가드와
  `shared_ptr` 참조 카운트로 락/생명주기 관리 실수를 구조적으로 줄였다. 공개 API는 `extern "C"`로
  완전히 그대로 유지해 C 콜사이트가 한 글자도 안 바뀌게 했다.
- **클러스터 조율은 전부 Redis로, 로컬/원격 배송 경로를 분리하지 않는다.** 매치메이킹은 `EVALSHA`
  원자적 Lua 스크립트로, 메시지 전파는 자기 자신도 포함한 PUBLISH/SUBSCRIBE 왕복 하나로 통일했다.
- **net 스레드는 절대 blocking I/O에 묶이지 않는다.** 이 원칙이 최종 리뷰에서 두 번(metrics
  스크레이퍼, Redis 하트비트) 깨져 있는 걸 발견하고 각각 전담 스레드로 분리했다.
- **큐 backpressure는 job 종류별로 다르게.** CHAT처럼 "잃어도 그만"인 트래픽은 non-blocking
  드롭, DISCONNECT/JOIN/LEAVE처럼 상태를 바꾸는(그리고 이 프로토콜엔 ACK/재시도가 없는) 요청은
  blocking으로 남겨 절대 조용히 사라지지 않게 했다.
- **Redis 장애는 fail-fast + 로그, 재시도/서킷브레이커는 안 둔다.** 대신 연결 자체가 죽었을 때는
  lazy reconnect(다음 호출이 왔을 때 한 번만 복구 시도)로 self-healing하게 만들었다.
- **pod 크래시로 인한 상태 누수는 lease/heartbeat로 복구.** 정상 종료가 아니라 SIGKILL/OOM으로
  사라진 pod의 Redis 상태 기여분을, 별도 리퍼 프로세스 없이 기존 매치메이킹 스캔에 편승해 지연
  회수(lazy reap)한다.

---

## 트러블슈팅 하이라이트

이 프로젝트에서 가장 가치 있는 부분은 "동작한다"가 아니라 **어떤 실수를 했고, 어떻게 잡았는지**라고
생각합니다. 전체 스토리는 `REDESIGN.md`/`REDESIGN_CPP.md`에 훨씬 자세히 남아 있고, 아래는 그중
포트폴리오에서 보여줄 만한 것들만 추린 것입니다.

| #   | 문제                                                                                                                                                                                          | 어떻게 발견                                                                                                                                                                                                                                                                                         | 해결                                                                                |
| --- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| 1   | Promtail DaemonSet/RBAC/ConfigMap을 전부 올바르게 배포했는데도 Loki에 로그가 하나도 안 쌓임 — Promtail 자체 로그엔 에러가 없어서 "정상인데 조용히 아무것도 안 하는" 가장 까다로운 부류의 장애 | Promtail의 자체 디버그 UI(`/targets`, `/config`)를 직접 열어, **내가 쓴 설정이 아니라 실제로 로드된 설정**을 확인 — `field: spec.nodeName=<pod 이름>`처럼 nodeName 자리에 pod 이름이 들어가 있는 걸 발견(k8s 컨테이너의 기본 `HOSTNAME`이 pod 이름이라 DaemonSet의 자동 노드 필터가 항상 매칭 실패) | downward API로 `HOSTNAME`을 `spec.nodeName`으로 덮어씀 (`k8s/promtail.yaml`)        |
| 2   | k8s liveness probe로 fd가 빠르게 재사용되면서, fd 기반 세션 식별이 깨짐                                                                                                                       | 관측성 스택(Loki) 도입 후 실제 로그에서 발견                                                                                                                                                                                                                                                        | `session_id` 기반 재설계 (REDESIGN.md §2-4)                                         |
| 3   | 세션 참조 카운트 1차 수정이 레이스를 18건→1501건으로 폭증시킴                                                                                                                                 | TSan                                                                                                                                                                                                                                                                                                | `alive` 갱신과 `room_id` 읽기를 하나의 락 구간으로 통합                             |
| 4   | C++ 마이그레이션 중 락 해제 후 `room_id`를 다시 읽는 실수 재도입                                                                                                                              | TSan (또 잡아냄)                                                                                                                                                                                                                                                                                    | 락 안에서 로컬 변수로 캡처 후 사용                                                  |
| 5   | cross-pod 채팅에서 자기 자신 제외 판정이 **pod-로컬** `session_id`를 비교 — 서로 다른 pod의 세션이 우연히 같은 id를 가지면 메시지가 조용히 유실                                               | 실제 3-replica kind 클러스터 통합 테스트(단일 프로세스 테스트로는 재현 불가능한 종류)                                                                                                                                                                                                               | Redis `INCR`로 클러스터 전역 유일 id(`global_id`) 발급, 자기제외 판정만 이걸로 교체 |
| 6   | SIGTERM 시 방에 유저가 남아있으면 이미 정리된 Redis 커넥션을 참조해 세그폴트                                                                                                                  | 전체 브랜치 재검토(스트레스 테스트가 아니라 코드를 처음부터 다시 읽는 방식)                                                                                                                                                                                                                         | Redis 연결 정리를 모든 worker join 이후로 이동                                      |
| 7   | 위 6번 수정이 `g_rooms_lock`을 쥔 채로 blocking queue push를 하게 만들어 **새 데드락**을 만듦                                                                                                 | 수정에 대한 독립적인 재검토(수정자의 "안전하다"는 진술을 그대로 믿지 않음)                                                                                                                                                                                                                          | 해당 push를 non-blocking으로 전환                                                   |
| 8   | pod lease/heartbeat 최초 구현에서 하트비트 명령 순서(SADD 먼저) 때문에 살아있는 pod이 잘못 회수될 수 있었음                                                                                   | 코드 리뷰                                                                                                                                                                                                                                                                                           | `lease → count → SADD` 순서로 재정렬                                                |
| 9   | 위 리뷰 수정 라운드에서 "고아 키 방지"로 count 키에 lease와 같은 TTL을 준 것이 **기능 전체를 무력화**하는 회귀였음(reap이 도착하기 전에 count도 같이 만료)                                    | 실제 crash+30초 대기 라이브 테스트                                                                                                                                                                                                                                                                  | count 키 TTL 제거                                                                   |
| 10  | net 스레드를 blocking I/O에서 자유롭게 만든 수정(non-blocking 큐 push)이, CHAT뿐 아니라 JOIN/LEAVE 요청도 조용히 드롭할 수 있게 만듦                                                          | 외부 코드 리뷰                                                                                                                                                                                                                                                                                      | JOIN/LEAVE 전용 blocking push 경로 분리                                             |

1번(Promtail)이 특히 인프라 트러블슈팅으로서 곱씹을 만한 사례입니다 — "에러 로그가 없다"가
"정상이다"를 의미하지 않는다는 걸 보여주는 전형적인 케이스였고, 이후 서버 코드 쪽 버그들(2~10)도
전부 같은 태도(내가 짠 게 아니라 실제로 무슨 일이 일어났는지 직접 확인)로 잡았습니다.

공통 패턴: **정적 코드 리뷰와 동적 테스트(TSan, 실제 클러스터, 라이브 크래시 재현)가 서로 다른
종류의 버그를 잡는다**는 것, 그리고 **수정 자체가 새 버그를 만들 수 있어서 "고쳤다"를 그대로 믿지
않고 재검토하는 과정이 반드시 필요하다**는 것입니다.

---

## 검증

- **ThreadSanitizer**: 세션 재설계, C++ 마이그레이션, Redis 통합 각 단계마다 재실행 — 최종 레이스 0건
- **기능 스트레스 테스트**: 수천~수만 커넥션 동시 join/chat/leave 반복, `/metrics`의 카운터와 실제
  이벤트 수 일치 확인
- **kind 3-replica 클러스터**: 서로 다른 pod에 붙은 클라이언트끼리 실제 채팅 성립 확인(cross-pod
  session_id 충돌 시나리오까지 의도적으로 재현해서 확인)
- **라이브 장애 재현**: `kill -9`로 pod을 강제 종료해 Redis 상태 누수를 재현하고, lease TTL이 실제로
  만료되는 것까지 기다려 자동 복구를 확인(단축된 타임아웃이 아니라 실제 30초 값으로)
- **graceful shutdown**: 방에 유저가 남아있는 상태로 SIGTERM을 보내는 걸 표준 회귀 테스트로 반복
  사용(이 시나리오가 여러 버그를 실제로 잡아냄)
- **관측성 스택 자체의 가치를 직접 증명**: `kubectl delete pod`로 pod을 강제 삭제하면 그 pod의
  로그는 `kubectl logs`로 더 이상 못 보지만, Loki 쿼리로는 4개 worker의 `shutdown_started`/
  `shutdown_completed` 로그가 그대로 남아있는 걸 확인 — "왜 중앙집중 로그 수집이 필요한가"를
  텍스트로 설명하는 대신 실제로 pod를 지워보고 증명

---

## 성능: 아키텍처 변천에 따른 부하 테스트

"고쳤다"가 아니라 "그래서 얼마나 비용이 들었나"를 숫자로 보여주기 위해, git 히스토리의 세 시점을
`git worktree`로 각각 체크아웃해 빌드하고, 동일한 부하 테스트 도구(`client/loadtest.py`)로 같은
조건에서 측정했습니다.

**측정 도구 자체도 한 번 다시 만들었습니다.** 처음엔 클라이언트 100개를 파이썬 `threading`(OS
스레드)으로 동시에 돌렸는데, GIL 경합이 서버보다 먼저 병목이 되어 p95/p99 tail latency가 세
버전 모두 40ms대로 거의 동일하게 나왔습니다 — 서버 아키텍처 차이가 아니라 측정 도구의 한계가
신호를 가리고 있었던 것입니다. `asyncio`(단일 이벤트 루프, OS 스레드 경합 없음) 기반으로
다시 짜고 동시 연결도 300으로 올리자, 그제서야 진짜 신호가 드러났습니다. **숫자를 좋게 보이도록
다듬기보다, 측정 방법 자체가 틀렸다는 걸 알아채고 도구를 고친 과정**이 이 결과표 자체보다
더 이야기할 가치가 있다고 생각합니다.

동시 연결 300, 클라이언트당 메시지 20개, 총 1.7만~1.8만 개 레이턴시 샘플 기준(`asyncio` 버전):

| 버전        | 설명                                                  | p50         | p95          | p99           | max       |
| ----------- | ----------------------------------------------------- | ----------- | ------------ | ------------- | --------- |
| ① `71ff0c6` | 최초 버전 (fd 기반 세션, 순수 C)                      | 3.31 ms     | 14.51 ms     | 34.94 ms      | 77.52 ms  |
| ② `19c9603` | 세션 재설계(`session_id`) + C++ 혼용, Redis 이전      | 3.27 ms     | 15.09 ms     | 37.01 ms      | 59.34 ms  |
| ③ 현재      | Redis pub/sub 포함(같은 pod 안에서도 항상 Redis 왕복) | **9.84 ms** | **91.84 ms** | **101.73 ms** | 105.50 ms |

**해석:**

- **①→②: 이번에도 성능 차이가 사실상 없습니다.** 300 동시 연결로 부하를 3배 올려도 마찬가지입니다
  — fd 재사용 버그를 막기 위한 `session_id` 재설계와 락 실수를 구조적으로 줄이기 위한 C++ 전환이,
  측정 가능한 성능 비용 없이 들어갔다는 결론이 더 높은 부하에서도 재확인됩니다.
- **②→③: 이번엔 확실히 드러납니다.** p50이 3배(3.3ms→9.8ms), **p95는 6배(15ms→92ms)**,
  p99는 2.7배(37ms→102ms) — "같은 pod 안에서도 항상 Redis PUBLISH/SUBSCRIBE를 거친다"는 설계
  결정의 실제 비용이 부하가 늘어날수록 뚜렷해집니다. 이건 우연이 아닙니다 — 명령 연결(`EVAL`/
  `PUBLISH`)이 `g_redis_cmd_lock` 하나로 logic worker 4개를 직렬화하는 구조라(아키텍처 절 참고),
  동시 요청이 늘어날수록 이 락 대기 시간이 그대로 tail latency에 반영되는 것으로 해석하고
  있습니다.
- **이 비용은 "버그"가 아니라 "수평 확장 능력을 얻은 대가"**입니다 — ①·②는 애초에 `replicas>1`로
  못 돌아가므로, 이 표는 "더 빠른 걸 골라야 한다"가 아니라 "그 능력을 얻기 위해 이 정도 지연을
  지불했다"는 트레이드오프의 가격표로 읽어야 합니다.

---

## 알려진 한계

의도적으로 손대지 않은 부분들과 그 이유는 `REDESIGN.md`의 "알려진 한계" 절에 자세히 남겨뒀습니다.
요약하면:

- 방 입장 완료와 Redis `SUBSCRIBE` 사이의 짧은 구독 경쟁 윈도우(Redis Pub/Sub 자체가 at-most-once)
- pod lease/heartbeat는 강한 분산 정합성이 아니라 best-effort 복구
- 운영 하드닝(resource limits, `securityContext`, Redis 인증/NetworkPolicy, PDB)은 로컬 kind
  데모 수준 — 실제 운영 배포 전에는 반드시 채워야 함
- 자동화된 부하/장애 테스트 스위트 부재 — 이 프로젝트가 처음부터 유지해온 "테스트 프레임워크 없이
  컴파일 확인 → 스모크 테스트 → TSan → kind 통합 검증" 방침과 상충하는 지점이라 별도 판단이 필요

---

## 데모 시나리오

실제로 실행해서 보여주는 순서는 `DEMO_SCRIPT.md`에 정리해뒀습니다 — 수평 확장(cross-pod 채팅)과
pod 크래시 복구가 이 프로젝트의 핵심을 보여주는 두 장면입니다.

## 관련 문서

- `README.md` — 실행 방법, 디렉토리 구조
- `REDESIGN.md` — 세션 재설계 · Redis pub/sub 확장 · pod lease/heartbeat까지 전체 과정과 겪은 문제들 (가장 자세한 기록)
- `REDESIGN_CPP.md` — C/C++ 혼용 결정 배경
- `INFRA.md` — 컨테이너화 및 관측성 스택 구축 과정
- `DEMO_SCRIPT.md` — 녹화/캡처용 시나리오
