# ServerProject — epoll 기반 채팅 서버 + Kubernetes 수평 확장

[![CI](https://github.com/dlawotjr1999/ServerProject/actions/workflows/ci.yml/badge.svg)](https://github.com/dlawotjr1999/ServerProject/actions/workflows/ci.yml)
![C](https://img.shields.io/badge/C-epoll%20%2F%20pthread-blue)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Redis](https://img.shields.io/badge/Redis-7-red)
![Kubernetes](https://img.shields.io/badge/Kubernetes-kind-326ce5)

리눅스 소켓 API만으로 **TCP 채팅 서버를 처음부터 구현**한 개인 프로젝트. 비동기 I/O
멀티플렉싱(epoll), TCP 스트림 위의 자체 바이너리 프로토콜 설계와 파싱, 연결별 수신/송신 버퍼 관리,스레드 간 작업 큐와 세션 수명 관리를 주로 구현.

이후 **Redis 기반 수평 확장(Kubernetes `replicas: 3`)**과 **관측성 스택(Prometheus/Loki/Grafana)**까지 확장. 확장 과정에서 드러난
동시성 결함(fd 재사용 경쟁, 세션 use-after-free)은 관측과 TSan으로 재현해 세션 구조를 재설계.

| 항목 | 내용 |
| --- | --- |
| 기간 | 2025.12 ~ 2026.08 |
| 인원 | 개인 프로젝트 (설계·구현·테스트·인프라 전체) |
| 언어 | C(소켓/epoll/프로토콜), C++17(상태 관리), Python(테스트 클라이언트) |
| 코어 | Linux socket API, epoll(level-triggered), non-blocking I/O, eventfd, pthread |
| 확장 | Docker, Kubernetes(kind), Redis 7, Prometheus, Loki/Promtail, Grafana, GitHub Actions |
| 검증 | ThreadSanitizer, 동시 접속 스트레스, 3-replica 클러스터 통합 테스트, pod 강제 종료 복구 테스트 |


### 기술 흐름

1. **네트워크 및 프로토콜 구현** : TCP는 메시지 경계가 없는 바이트 스트림이라, 채팅 메시지 단위로 주고받으려면 애플리케이션 계층에서 경계를 정함.
2. **epoll 환경 구축** : 접속자마다 스레드를 두면 연결 수만큼 스레드와 컨텍스트 스위칭 비용이 늘어나므로, 적은 스레드로 많은 연결을 감시하는 구조.
3. **net / logic 스레드 분리 + job 큐** : 로직 처리가 느려지면 소켓 I/O도 같이 멈춰 모든 접속자가 영향을 받으므로, I/O 지연과 로직 지연을 서로 격리.
4. **컨테이너화 + Kubernetes 적용** : 공부한 컨테이너/오케스트레이션을 실제로 적용해 볼 대상이 필요했고, 기능이 이미 갖춰진 이 서버를 대상으로 채택.
5. **관측성 스택 (Prometheus / Loki / Grafana)** : k8s에서는 pod이 사라지면 로그도 같이 사라지고, 동시성 문제는 재현 없이 추측만 하게 되므로, 관측할 수단이 먼저 필요했음
6. **세션 재설계 (fd → `session_id`) + 동시성 상태의 C++ 전환** : 관측 결과 liveness probe 때문에 fd가 빠르게 재사용되어, fd를 세션 신원으로 쓰는 전제 자체가 깨져 있었고, 락/수명 관리의 강화
7. **수평 확장 (Redis Pub/Sub, `replicas: 3`)** : fd는 프로세스 로컬이라 다른 pod에 붙은 접속자에게는 보낼 수 없으므로, 저장소가 아니라 pod 간 메시지 전달 경로를 위해 Redis을 활용하여 수평 확장
8. **pod 크래시 복구 (lease / heartbeat)** : 수평 확장 후 pod이 비정상 종료되면 Redis에 남은 방 카운트를 아무도 정리하지 않아, 실제로는 비어 있는 방이 찬 것으로 남는 문제가 생겼음
9. **자동 검증 (TSan CI, 부하 테스트)** : 비용 실측을 위한 자동 검증

---

## 목차

1. [아키텍처](#아키텍처)
2. [네트워크 코어](#네트워크-코어-epoll-이벤트-루프)
3. [핵심 설계 결정](#핵심-설계-결정)
4. [문제 해결](#문제-해결)
5. [검증](#검증)
6. [성능](#성능-아키텍처-변천에-따른-부하-테스트)
7. [알려진 한계](#알려진-한계)

---

## 아키텍처

### 모듈 별 설명
#### server
- main.c            프로세스 진입점. 시그널 처리, worker/스레드 생성, graceful shutdown 시퀀스
- net.c             epoll 이벤트 루프, accept/recv/send, fd 관리, metrics/heartbeat 전담 스레드 소유
- protocol.c        length-prefixed 프레임 파싱, byte order 변환
- job_queue.cpp     net ↔ logic 스레드 간 job 큐 (고정 크기 circular buffer + mutex/cond)
- logic.c           패킷 핸들러(JOIN/CHAT/LEAVE), logic worker 4개가 공유
- state.cpp         세션/방 생명주기. session_id 신원 관리, shared_ptr 참조 카운트
- redis_client.cpp  Redis 연결(명령용/구독용 분리), EVALSHA 매치메이킹, PUBLISH/SUBSCRIBE, lease/heartbeat, lazy 재연결
- metrics.c         Prometheus 텍스트 포맷 렌더링
- log.cpp           구조화 JSON 로그 (fd / session_id / room_id 포함)
- posix_lock.hpp    pthread_mutex_t용 RAII 락 가드
- Dockerfile        멀티스테이지 빌드, non-root, exec 형태 ENTRYPOINT

#### client
- client.py         대화형 TCP 클라이언트
- loadtest.py       asyncio 기반 부하 테스트 도구

#### k8s
- chat-server.yaml  Deployment(replicas 3) + Service + ConfigMap
- redis.yaml        StatefulSet + Service
- prometheus.yaml · loki.yaml · promtail.yaml · grafana.yaml (+ grafana-dashboards/)
- kind-config.yaml  로컬 클러스터 정의

### 모듈 간 관계

![모듈 간 의존 관계](assets/module-dependencies.svg)


## 네트워크 코어: epoll 이벤트 루프

### 이벤트 처리 흐름

```
epoll_wait (level-triggered)
  ├─ listen fd           : accept → non-blocking 설정 → connection_t 생성 → epoll 등록
  ├─ EPOLLIN             : recv 루프(EAGAIN까지) → 파싱 루프 → job 큐로 전달
  ├─ EPOLLOUT            : send 루프(부분 송신은 send_offset 유지) → 다 보내면 EPOLLOUT 해제
  ├─ eventfd             : worker가 넣은 송신 job을 꺼내 g_io_q 드레인
  └─ EPOLLERR / EPOLLHUP : 연결 정리
```
※ listen 소켓 생성 → SO_REUSEADDR → bind → listen(backlog 256) → non-blocking → epoll 등록

### 프로토콜과 parsing

![프로토콜과 파싱](assets/protocol-parsing.svg)

### 스레드 모델 (pod 하나 기준)

![스레드 모델](assets/thread-model.svg)

**큐 정책**

| 구분 | 정책 | 로직 |
| --- | --- | --- |
| CHAT | non-blocking push, 큐가 차면 드롭 | 잃어도 서비스가 깨지지 않는 트래픽 |
| JOIN / LEAVE / DISCONNECT | blocking push | 상태를 바꾸는 요청이고 프로토콜에 ACK/재시도가 없어 조용히 사라지면 안 됨 |
| 송신 | logic worker → `g_io_q` → eventfd로 epoll 깨움 | 송신 지연 방지, send()/close()는 net 스레드만 수행 |

### 확장 ①: Redis 기반 수평 확장 (k8s)

```
                   Service(chat-server, ClusterIP)
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

### 컨테이너화

- **멀티스테이지 빌드** (`gcc:13` → `debian:bookworm-slim`) : 최종 이미지에 컴파일러를 남기지 않음
- **non-root 실행** : 서버 프로세스를 일반 사용자 권한으로 실행
- **exec 형태 `ENTRYPOINT`** : 셸이 PID 1이 되면 SIGTERM이 서버까지 전달되지 않아 graceful shutdown이 동작하지 않음

### 확장 ②: 관측성 스택은 전부 선언적으로

- **Prometheus** : operator 없이 pod annotation만으로 chat-server를 자동 발견
- **Loki + Promtail** : DaemonSet이 각 pod의 JSON 로그를 수집해 중앙 보관
- **Grafana** : datasource와 대시보드를 ConfigMap으로 프로비저닝. `uid`를 고정해 클러스터를 다시 만들어도 대시보드 참조가 유지됨
- **재현성** : `kubectl apply -f k8s/` 한 번으로 동일한 상태가 되며, UI에서 수동으로 설정하는 단계가 없음

| 지표 | 타입 | 의미 |
| --- | --- | --- |
| `chat_connections_active` | gauge | 현재 접속 중인 클라이언트 수 |
| `chat_rooms_active` | gauge | 멤버가 1명 이상인 방 수 |
| `chat_messages_total` | counter | 브로드캐스트한 채팅 메시지 누적 수 |
| `chat_disconnects_total` | counter | 처리한 연결 종료 누적 수 |
| `chat_jobqueue_depth{queue}` | gauge | 큐별 대기 job 수 (`logic`, `io`) |

| 엔드포인트 (포트 9100) | 용도 |
| --- | --- |
| `/metrics` | Prometheus 스크레이핑 |
| `/healthz` | liveness probe. 프로세스가 살아 있으면 항상 200 |
| `/readyz` | readiness probe. 리스닝 소켓 초기화가 끝나야 200, 아니면 503 |

---


## 핵심 설계 결정

| 결정 | 이유 |
| --- | --- |
| 세션 신원은 fd가 아닌 `session_id` + 참조 카운트 | fd는 닫히면 바로 재사용되어 다른 접속자를 가리킬 수 있음 |
| net 스레드는 blocking I/O 금지 | 유일한 epoll 스레드가 막히면 모든 접속자가 지연됨. metrics와 heartbeat는 전담 스레드로 분리 |
| 동시성 상태가 몰린 파일만 C++ 전환 | 락 해제 누락과 수명 관리 실수를 RAII / `shared_ptr`로 방지. 공개 API는 `extern "C"`로 유지해 C 코드는 무수정 |
| 로컬 / 원격 전달을 Redis Pub/Sub 하나로 통일 | 경로가 둘이면 타이밍 차이로 레이스가 생김 |
| Redis 장애는 fail-fast + lazy reconnect | 재시도 로직 없이 단순하게 유지. 복구 불가 시 비정상 종료 코드로 k8s 재시작에 위임 |
| pod 크래시 상태 누수는 lease / heartbeat로 지연 회수 | 별도 정리 프로세스 없이 복구 (heartbeat 10초, lease TTL 30초) |

---

## 문제 해결

| 문제 | 발견 경로 | 해결 |
| --- | --- | --- |
| liveness probe로 fd가 빠르게 재사용되어 세션 식별이 깨짐 | Loki 로그 | `session_id` 기반 재설계 |
| 참조 카운트 1차 수정 후 레이스가 **18건 → 1501건**으로 증가 | TSan | `alive` 갱신과 `room_id` 읽기를 하나의 락 구간으로 통합 |
| 큐를 non-blocking으로 바꾼 뒤 JOIN / LEAVE까지 드롭됨 | 코드 리뷰 | 상태 변경 요청 전용 blocking 경로 분리 |
| cross-pod 채팅에서 메시지 유실 (pod별 `session_id` 충돌) | 3-replica 통합 테스트 | Redis `INCR`로 전역 유일 `global_id` 발급 |
| SIGTERM 시 세그폴트, 그 수정이 다시 데드락을 유발 | 코드 재검토 | Redis 정리를 worker join 이후로 이동, 락 보유 중 push는 non-blocking으로 전환 |
| Promtail이 에러 없이 로그를 수집하지 않음 | 실제 로드된 설정 확인 | downward API로 `HOSTNAME`에 nodeName 주입 |

---

## 검증

- **ThreadSanitizer**: 세션 재설계, C++ 마이그레이션, Redis 통합 각 단계마다 재실행 — 최종 레이스 0건
- **CI (GitHub Actions)**: push/PR마다 `make clean && make` 컴파일 확인 및 TSan 빌드 + Redis 서비스
  컨테이너 + 50 클라이언트 동시 join/chat/quit 스트레스 + SIGTERM 후 레이스 0건 확인
- **기능 스트레스 테스트**: 수천 ~ 수만 커넥션의 동시 join/chat/leave 반복, `/metrics` 카운터와 실제 이벤트 수 일치 확인. 방이 재사용되지 않아 256개 소진 후 입장이 불가능하던 결함을 free list로 해결
- **kind 3-replica 클러스터**: 서로 다른 pod에 붙은 클라이언트끼리 실제 채팅 성립 확인
- **라이브 장애 재현**: `kill -9`로 pod을 강제 종료해 Redis 상태 누수를 재현하고, 단축된 타임아웃이
  아니라 실제 30초 lease TTL이 만료될 때까지 기다려 자동 복구 확인
- **graceful shutdown**: 방에 유저가 남은 상태로 SIGTERM을 보내는 것을 표준 회귀 테스트로 반복
- **관측성**: `kubectl delete pod`로 pod을 지우면 그 pod의 로그는 `kubectl logs`로 더 볼 수 없지만, Loki 쿼리로는 4개 worker의 `shutdown_started`/`shutdown_completed`가 그대로 남아
  있음을 확인

---

## 성능: 아키텍처 변천에 따른 부하 테스트

`asyncio`(단일 이벤트 루프)를 이용한 동시 연결 300, 클라이언트당 메시지 20개, 총 1.7만~1.8만 개 레이턴시 샘플:

| 버전 | 설명 | p50 | p95 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| ① | 최초 버전 (fd 기반 세션, 순수 C) | 3.31 ms | 14.51 ms | 34.94 ms | 77.52 ms |
| ② | 세션 재설계(`session_id`) + C++ 혼용, Redis 이전 | 3.27 ms | 15.09 ms | 37.01 ms | 59.34 ms |
| ③ | Redis pub/sub 포함 (같은 pod 안에서도 항상 Redis 왕복) | **9.84 ms** | **91.84 ms** | **101.73 ms** | 105.50 ms |

- **① → ②: 성능 차이가 사실상 없음.** `session_id` 재설계와 C++ 전환이 측정 가능한 성능 비용 없이 들어감
- **② → ③: 비용이 확실히 드러남.** p50 3배, **p95 6배(15ms → 92ms)**, p99 2.7배. 같은 pod 안에서도 항상 Redis PUBLISH/SUBSCRIBE를 거치는 결정의 비용임. 명령 연결이 `g_redis_cmd_lock` 하나로 logic worker 4개를 직렬화하므로, 동시 요청이 늘수록 락 대기 시간이 tail latency에 반영되는 것으로 해석함
- **수평 확장 능력을 얻은 대가.** ①·②는 `replicas > 1`로 돌릴 수 없으므로, 이 표는 속도 비교가 아니라 그 능력을 얻기 위해 지불한 지연의 가격표임

---

## 알려진 한계


- 방 입장 완료와 Redis `SUBSCRIBE` 사이에 짧은 구독 경쟁 윈도우가 존재 (Redis Pub/Sub 자체가
  at-most-once).
- pod lease/heartbeat는 강한 분산 정합성이 아니라 best-effort 복구
- Redis 명령 연결이 단일 락으로 직렬화되어 있어, 부하가 높아지면 이 락이 tail latency를 결정
- 운영 하드닝(resource limits, `securityContext`, Redis 인증/NetworkPolicy, PDB)은 로컬 kind 데모 수준
- 부하/장애 테스트는 자동화된 스위트가 아니라 스크립트와 수동 절차 