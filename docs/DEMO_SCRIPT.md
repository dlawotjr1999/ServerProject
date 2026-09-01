# 포트폴리오 데모 스크립트

녹화(영상) 또는 캡처(스크린샷 시퀀스)용 시나리오. "동작한다"보다 "왜 이 구조가 필요했는지"가
드러나는 순서로 구성했습니다. 전체 6막, 다 넣으면 4~5분 분량 — 시간이 짧게 필요하면 3막까지만
써도 완결성 있습니다(1·2·4·6막은 선택).

## 사전 준비

Docker Desktop을 켠 상태에서 (직접 켜야 합니다 - 자동화 안 함):

```bash
kind create cluster --config k8s/kind-config.yaml   # 이미 있으면 생략
docker build -t chat-server:dev server/
kind load docker-image chat-server:dev --name chat-server
kubectl apply -f k8s/redis.yaml
kubectl apply -f k8s/
kubectl rollout status statefulset/redis
kubectl rollout status deployment/chat-server
kubectl get pods -l app=chat-server   # 3개 모두 1/1 Running 확인
```

녹화 중간에 이 세팅 과정을 보여줄 필요는 없습니다 — 미리 다 띄워두고 화면만 깨끗하게 시작하는 걸
추천합니다.

---

## 1막 — 기본 동작 (선택, ~30초)

로컬에서 서버 하나만 띄우고 클라이언트 2개(터미널 분할 또는 화면 분할)로 join → chat → leave.
지루하지만 베이스라인을 보여주는 용도. 시간이 없으면 스킵하고 3막부터 시작해도 무방합니다.

---

## 2막 — 관측성 (선택, ~30~40초)

Grafana 대시보드를 열어두고 1막을 진행하는 동안 `active_connections`, `messages_total` 같은
지표가 실시간으로 움직이는 걸 같이 보여줌. Loki에서 구조화된 JSON 로그(`session_created`,
`room_joined` 등)를 검색하는 것도 좋습니다 — "그냥 로그만 찍는 게 아니라 관측 가능하게
설계했다"는 인상을 줍니다.

---

## 3막 — 하이라이트: 수평 확장 (필수, ~1~1.5분)

이 프로젝트 전체의 핵심 성과. 가장 힘을 줘야 하는 구간입니다.

```bash
kubectl get pods -l app=chat-server
```
서로 다른 pod 2개에 각각 port-forward:
```bash
kubectl port-forward pod/<pod-A-name> 3801:3800 &
kubectl port-forward pod/<pod-B-name> 3802:3800 &
```

터미널 A: `python3 client/client.py --host 127.0.0.1 --port 3801 --local-echo` → `/join`
터미널 B: `python3 client/client.py --host 127.0.0.1 --port 3802 --local-echo` → `/join`

터미널 A에서 `cross-pod hello` 입력 → 터미널 B에 `[CHAT] cross-pod hello` 출력.

**완전히 다른 프로세스에 붙은 클라이언트끼리 대화되는 것**이 핵심이니, 왜 이게 원래 안 됐는지
한 줄 내레이션/자막을 넣으면 좋습니다 — "방 상태가 pod마다 로컬 메모리라 원래는 서로 다른 pod에
붙은 유저끼리 대화가 안 됨 → Redis pub/sub로 클러스터 전역 조율."

`kubectl logs -l app=chat-server --prefix=true | grep session_created` 같은 걸로 A와 B가 실제로
다른 pod 이름인 것도 잠깐 비춰주면 신뢰도가 올라갑니다.

```bash
kill %1 %2   # port-forward 정리
```

---

## 4막 — 회복력: graceful shutdown (선택, ~30초)

`kubectl delete pod <pod-A-name>` 하거나 그 pod에 직접 SIGTERM을 보내서, 남은 pod들은 멀쩡하고
죽은 pod은 k8s가 알아서 재생성하는 것을 보여줌. `kubectl logs` 로 `shutdown_started` →
`shutdown_completed` 시퀀스가 깔끔하게 찍히는 것도 같이 보여주면 좋습니다.

---

## 5막 — 하이라이트 ②: pod 크래시 복구 (신규, ~40~50초)

3막이 "정상 동작"의 하이라이트라면, 이건 "장애 상황에 대한 설계"의 하이라이트입니다. 최근에 추가한
기능(pod lease/heartbeat 기반 Redis 상태 복구)을 직접 보여주는 장면 — 이런 걸 실제로 만들고
검증까지 했다는 게 포트폴리오에서 제일 차별화되는 부분일 수 있습니다.

**보여줄 것**: pod 하나가 `kill -9`로 (SIGTERM이 아니라!) 비정상 종료되면, 그 pod에 붙어있던
유저들의 "방 좌석"이 Redis에 영원히 남아 방을 잠식하는 게 아니라, 일정 시간 뒤 자동으로 회수되는
것.

이건 클러스터 안에서 `kill -9`를 직접 실행하기가 까다로워서(pod 안에서 자기 자신을 죽이는 방식),
아래처럼 진행하는 게 현실적입니다:

```bash
# room:0 상태를 미리 보여줌 (누가 어느 pod에 좌석을 갖고 있는지)
kubectl exec redis-0 -- redis-cli SMEMBERS room:0:pods
kubectl exec redis-0 -- redis-cli GET room:0:count

# pod A를 강제로 죽임 (graceful termination을 건너뛰는 게 핵심 - delete --force --grace-period=0)
kubectl delete pod <pod-A-name> --grace-period=0 --force

# 좌석이 아직 안 지워진 걸 확인 (lease는 30초 TTL)
kubectl exec redis-0 -- redis-cli TTL room:0:pod:<pod-A-name>:lease

# 30초+ 대기 후, 새 클라이언트가 join하면(매치메이킹 스캔이 이 방을 지나가면서) 자동 회수됨
kubectl exec redis-0 -- redis-cli SMEMBERS room:0:pods   # 죽은 pod 흔적 사라짐 확인
```

내레이션 포인트: "이 서버가 원래 갖고 있던, 그리고 실제 리뷰에서 지적받아 고친 문제 — pod가
비정상 종료되면 Redis에 남은 좌석 카운트가 영원히 새는 문제를 lease/heartbeat 메커니즘으로
해결했다"는 스토리를 짧게 얹으면 좋습니다. `REDESIGN.md`에 이 기능을 만들다가 겪은 버그(회귀
포함)까지 문서로 남겨뒀다는 것도 언급할 만합니다.

---

## 6막 — 숫자로 보여주기: 부하 테스트 (선택, ~40초)

지금까지는 "동작한다"를 보여줬다면, 이 막은 "얼마나 버티는지"를 숫자로 보여줍니다.
`client/loadtest.py`가 이 프로젝트 전용으로 만든 부하 테스트 도구입니다 — 여러 클라이언트를
스레드로 동시에 띄워서 join → chat(N개, 송신 시각을 payload에 실음) → leave를 반복하고, 수신
측에서 왕복(브로드캐스트) 레이턴시를 계산합니다. 같은 파이썬 프로세스 안에서 전부 돌기 때문에
`time.time()` 하나로 모든 클라이언트의 시계가 자동으로 동기화되어 있어, 별도 시계 동기화 없이도
정확한 end-to-end 레이턴시를 잴 수 있습니다.

### 기본 사용법

```bash
# 서버 + Redis가 떠 있는 상태에서 (로컬이면 REDIS_HOST를 127.0.0.1로 임시 변경 필요 - server/common.h 참고)
python3 client/loadtest.py --host 127.0.0.1 --port 3800 --clients 100 --messages 20
```

`--clients`(동시 연결 수), `--messages`(클라이언트당 전송 메시지 수), `--send-interval`(같은
클라이언트의 메시지 간 간격)을 조절할 수 있습니다. 방 하나의 정원(`MAX_ROOM_USER=4`)이 있어서,
`--clients 100`이면 자동으로 ~25개 방에 나눠 매치메이킹됩니다 — 이것도 실제로는 이 서버가
평소에 처리하는 것과 같은 패턴(계속 방이 생성/회수되는)입니다.

출력에 총 연결/JOIN 성공 수, 초당 송수신 메시지 수(처리량), 그리고 **레이턴시 p50/p95/p99**가
나옵니다. 이 레이턴시가 흥미로운 이유: 이 서버는 같은 pod 안에서 나누는 채팅조차 로컬에서 직접
브로드캐스트하지 않고 **항상 Redis PUBLISH/SUBSCRIBE를 거칩니다**(3막에서 설명한 "로컬/원격
배송 경로를 분리하지 않는다"는 설계) — 즉 이 숫자가 "그 설계 결정의 실제 비용"입니다.

### (선택, 더 하드코어) Redis 도입 전/후 비교

원한다면 "Redis pub/sub을 도입하기 전(로컬 직접 브로드캐스트)"과 "도입 후(지금, 항상 Redis
왕복)"를 같은 조건에서 직접 비교하는 숫자도 뽑을 수 있습니다. **"어느 게 더 빠른가"가 아니라
"수평 확장 능력을 얻는 대가로 지불한 지연"으로 프레이밍**하는 게 핵심입니다 — 이전 버전은
애초에 `replicas > 1`로 못 돌아가므로 애플-투-애플 비교가 아닙니다.

```bash
# Redis 도입 직전 커밋을 별도 워크트리로 체크아웃해서 빌드
git worktree add /tmp/pre-redis-server 19c9603
cd /tmp/pre-redis-server/server && make clean && make

# 이전 버전 기동 (Redis 불필요) + 측정
./server &
python3 /path/to/client/loadtest.py --host 127.0.0.1 --port 3800 --clients 100 --messages 20

# 정리
kill %1
git worktree remove /tmp/pre-redis-server

# 지금 버전으로 동일 조건 측정 (Redis 필요, REDIS_HOST=127.0.0.1로 임시 변경 후)
cd server && ./server &
python3 ../client/loadtest.py --host 127.0.0.1 --port 3800 --clients 100 --messages 20
```

두 결과의 p50/p95를 나란히 놓고 "Redis 왕복 하나 추가하는 데 pXX ms가 든다, 그 대가로
`replicas`를 몇 개로든 늘릴 수 있게 됐다"는 식으로 이야기하면 설계 트레이드오프를 정량적으로
보여줄 수 있습니다.

---

## 부록: 자막/내레이션 포인트 모음

- "세션을 fd가 아니라 session_id로 관리 — k8s liveness probe가 fd를 빠르게 재사용시켜서 생긴
  버그를 계기로 재설계했음"
- "동시성 상태 관리 부분만 C++로 옮겨서 RAII/shared_ptr로 락 실수를 구조적으로 방지"
- "Redis pub/sub로 클러스터 전역 매치메이킹 + 메시지 전파, 로컬/원격 배송 경로를 분리하지 않음"
- "최종 리뷰에서 SIGTERM 시 세그폴트, 그 수정이 만든 새 데드락까지 발견 — 리뷰 프로세스 자체를
  신뢰하기보다 재검토로 검증"
- "pod가 비정상 종료돼도 Redis 상태가 lease/heartbeat로 자동 복구됨"

## 이 문서에 대해

이 파일은 개인 참고용 스크립트라 `.gitignore`에 추가해뒀습니다(README.md 같은 공개 문서와
성격이 다름). 실제 녹화/캡처 순서나 멘트는 자유롭게 수정하세요 — 여기 있는 건 뼈대일 뿐입니다.
