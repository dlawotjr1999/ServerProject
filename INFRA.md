## 1. 개요

`README.md`가 소켓 서버 자체(`server/`)를 다룬다면, 이 문서는 그 서버를 컨테이너화하고 로컬 k8s(kind) 위에 관측성 스택(Prometheus + Loki + Grafana)까지 붙인 인프라 작업을 다룹니다.

목적은 애플리케이션 기능 추가가 아니라 **인프라 그 자체**입니다. 그래서 소켓 코드는 최소한만 고치고(0단계), 대부분의 작업은 빌드 시스템 → 구조화 로깅 → 메트릭 → 컨테이너화 → k8s 배포 → 관측성 순서로 쌓았습니다.

## 2. 아키텍처

```
                 ┌────────────────────────┐
 클라이언트 ───▶ │ chat-server (pod)       │
                 │  :3800 채팅            │
                 │  :9100 /metrics        │──────▶ Prometheus (pod annotation으로 자동 discovery)
                 │  :9100 /healthz,/readyz│                │
                 │  stdout: 한 줄 JSON 로그│                ▼
                 └───────────┬────────────┘            Grafana ◀── datasource 자동 프로비저닝(ConfigMap)
                             │ containerd가 파일로 적재         ▲
                             ▼                                 │
                       /var/log/pods/...                  Loki
                             │                                 ▲
                             ▼                                 │
                       promtail (DaemonSet) ────────────────────┘
```

- 서버는 `printf` 대신 `log_json()`(server/log.c) 하나로 모든 로그를 한 줄 JSON으로 stdout에 씀 → 컨테이너 로그 수집의 전제 조건
- `/metrics`, `/healthz`, `/readyz`는 별도 HTTP 서버가 아니라 **기존 epoll 루프에 두 번째 리스닝 소켓을 등록**해서 처리 (server/net.c) — "인프라가 주 어필이니 소켓 계층에 기능을 더 얹지 않는다"는 원칙을 지키기 위함
- Prometheus/Grafana 모두 **수동 클릭으로 등록하지 않음** — pod annotation 기반 자동 discovery(Prometheus), ConfigMap 기반 datasource/dashboard 프로비저닝(Grafana)으로 전부 선언적으로 재현됨

## 3. 설계 결정과 근거

### 3.1. replicas: 1로 고정한 이유

세션이 `fd`(파일 디스크립터) 단위로 식별되고(`server/state.c`), 브로드캐스트는 세션의 fd에 직접 `write`합니다. fd는 프로세스 로컬 커널 핸들이라 **다른 pod의 소켓에는 쓸 수 없습니다.** 즉 지금 구조로 `replicas > 1`을 하면 서로 다른 pod에 붙은 클라이언트끼리 대화가 안 됩니다. 이 저장소의 로드맵에서는 이후 단계(fd → session_id 재설계, Redis pub/sub)에서 풀 문제이고, 인프라 단계에서는 `replicas: 1`을 명시적으로 고정해 이 한계를 숨기지 않았습니다.

### 3.2. 메트릭/헬스체크를 별도 서버로 만들지 않은 이유

Prometheus가 긁어갈 `/metrics`가 필요하다고 바로 Express/Flask 스타일로 별도 HTTP 서버를 붙이면 애플리케이션 코드가 늘어나고, 이미 있는 epoll 루프와 별개의 스레드/포트 관리가 하나 더 생깁니다. 대신 `net_init()`에서 두 번째 `listen` 소켓(9100)을 **같은 epoll 인스턴스**에 등록하고, 요청이 오면 accept 즉시 그 자리에서 동기적으로 처리합니다(server/net.c `handle_metrics_accept`). 응답이 느린 클라이언트가 net 스레드를 막지 않도록 `SO_RCVTIMEO`/`SO_SNDTIMEO`로 짧은 타임아웃만 걸었습니다.

### 3.3. 구조화 로깅: 이벤트를 세분화한 이유

`log_json(level, event, key-value...)` 하나를 모든 로그의 단일 진입점으로 만들었습니다. 중요한 건 필드 설계보다 **이벤트를 얼마나 잘게 쪼갰는가**입니다. 예를 들어 연결 종료 하나를 `close`(net 스레드가 소켓을 닫음) / `disconnect_queued`(logic 큐에 정리 작업을 넣음) / `disconnect_handled`(logic 스레드가 실제로 세션을 정리함) 세 이벤트로 분리했습니다. 이렇게 나누지 않으면, 나중에 "같은 fd가 close된 뒤 disconnect_handled 되기 전에 다시 accept됐다"처럼 fd 재사용 경쟁을 로그만으로 재구성할 수 없습니다. 이건 이후 2단계에서 다룰 버그(fd를 세션 신원으로 쓰는 문제)를 관측으로 잡아내기 위한 사전 설계입니다.

한 줄 전체를 mutex로 보호된 단일 `write()` 호출로 내보내는 것도 의도적입니다 — net 스레드와 4개의 logic worker가 동시에 로그를 찍는데, `fprintf`를 여러 번 나눠 부르면 컨테이너 로그에서 서로 다른 스레드의 줄이 섞일 수 있습니다.

### 3.4. Dockerfile: non-root + exec 형태 ENTRYPOINT

멀티스테이지(`gcc:13` 빌드 → `debian:bookworm-slim` 런타임)로 최종 이미지에 컴파일러를 남기지 않고, non-root 사용자로 실행합니다. `ENTRYPOINT ["/app/server"]`를 exec 형태로 쓴 이유는 셸 형태(`CMD server`)로 실행하면 셸이 컨테이너의 PID 1이 되어 `docker stop`/`kubectl delete pod`가 보내는 SIGTERM이 서버 프로세스까지 전달되지 않기 때문입니다. 이건 0단계에서 고친 graceful shutdown(worker 4개 `pthread_join`)이 실제로 의미를 가지려면 반드시 필요한 조건이었습니다.

### 3.5. Prometheus/Grafana를 선언적으로만 구성

Prometheus는 `prometheus-operator`/`ServiceMonitor` 없이, `kubernetes_sd_configs(role: pod)` + pod annotation(`prometheus.io/scrape`, `prometheus.io/port`, `prometheus.io/path`)만으로 chat-server를 자동 발견합니다. Grafana는 datasource(Prometheus/Loki)와 대시보드(`k8s/grafana-dashboards/chat.json`)를 전부 ConfigMap 프로비저닝으로 등록합니다. "클러스터를 지우고 새로 만들어도 `kubectl apply -f k8s/` 한 번이면 완전히 동일한 상태로 돌아온다"를 지키기 위해 UI에서 손으로 클릭하는 단계를 하나도 남기지 않았습니다. 이를 위해 datasource에 `uid: prometheus` / `uid: loki`를 고정값으로 박아뒀는데, 안 그러면 Grafana가 클러스터마다 랜덤 UID를 새로 발급해서 대시보드 JSON의 datasource 참조가 매번 깨집니다.

## 4. 트러블슈팅

### 4.1. Promtail이 로그를 하나도 못 가져오는 문제 (핵심 사례)

**증상**: `promtail` DaemonSet, RBAC, ConfigMap을 전부 올바르게 배포했는데도 Loki에 로그가 전혀 안 쌓임. Promtail 자체 로그에는 에러가 하나도 없었음(정상적으로 시작해서 조용히 아무 일도 안 하는 상태).

**원인 추적**:
1. `kubectl logs promtail-...`에는 아무 이상 신호가 없어서, promtail의 자체 디버그 UI(`/targets`, `/service-discovery`, `/config`)를 `kubectl port-forward`로 직접 열어봄
2. `/targets`에 `kubernetes-pods (0/0 ready)` — 발견된 대상이 **0개**. 필터링돼서 0개가 아니라 애초에 discovery 단계에서 아무것도 못 찾고 있었음
3. `/config`(promtail이 실제로 로드한 최종 설정)를 보니, 내가 작성하지 않은 필드가 자동으로 추가되어 있었음:
   ```
   kubernetes_sd_configs:
     - role: pod
       selectors:
       - role: pod
         field: spec.nodeName=promtail-jzmk6
   ```
4. `promtail-jzmk6`는 **pod 이름**이지 노드 이름이 아님. Promtail은 DaemonSet 패턴에서 "내 노드의 pod만 본다"는 필터를 자동으로 걸면서, 컨테이너의 `HOSTNAME` 환경변수를 노드 이름으로 씀 — 그런데 k8s 컨테이너의 기본 `HOSTNAME`은 **pod 이름**이라, `spec.nodeName`(예: `chat-server-control-plane`)과 절대 일치하지 않음. 그래서 field selector가 항상 매칭 실패 → 대상 0개

**해결**: downward API로 `HOSTNAME` 환경변수를 실제 노드 이름으로 덮어씀 (`k8s/promtail.yaml`)
```yaml
env:
  - name: HOSTNAME
    valueFrom:
      fieldRef:
        fieldPath: spec.nodeName
```
적용 후 `/targets`가 `10/14 ready`로 바뀌고, Loki 쿼리에 chat-server 로그가 정상적으로 잡히기 시작함.

**교훈**: "에러 로그가 없다"가 "정상이다"를 의미하지 않는다. Promtail은 대상을 못 찾아도 warning/error를 찍지 않고 그냥 조용히 아무것도 안 함. `/config`로 **내가 쓴 설정이 아니라 실제로 로드된 설정**을 확인하는 게 이런 부류의 문제를 잡는 유일한 방법이었음.

### 4.2. kubectl delete pod로 관측성 스택의 가치를 직접 증명

버그는 아니지만 검증 과정에서 나온 의미 있는 결과라 기록: chat-server pod를 `kubectl delete pod`로 강제 삭제하면 pod 오브젝트 자체가 사라져서 `kubectl logs`로는 그 pod의 로그를 더 이상 볼 수 없습니다. 하지만 Loki 쿼리로는 4개 worker의 `shutdown_started`/`shutdown_completed` 로그가 그대로 남아있는 걸 확인했습니다. "왜 중앙집중 로그 수집이 필요한가"를 텍스트로 설명하는 대신 실제로 pod를 지워보고 증명한 셈입니다.

### 4.3. Windows + Docker Desktop + kind 조합의 속도

이 환경(WSL2 백엔드의 Docker Desktop 위에 kind)에서는 클러스터 하나 뜨는 데(`kind create cluster`) control-plane 컴포넌트가 다 뜬 뒤에도 CNI(kindnet) 설치까지 체감상 5분 가까이 걸렸습니다. 네이티브 리눅스보다 확연히 느린 편이라, 로컬 반복 검증 사이클을 짤 때 이 지연을 감안해야 했습니다.

## 5. 실행 방법

```bash
# 1. 이미지 빌드
docker build -t chat-server:dev server/

# 2. 로컬 kind 클러스터 생성 + 이미지 주입
kind create cluster --config k8s/kind-config.yaml
kind load docker-image chat-server:dev --name chat-server

# 3. 전체 스택 배포 (chat-server + promtail + loki + prometheus + grafana)
kubectl apply -f k8s/

# 4. 접속
kubectl port-forward svc/chat-server 3800:3800   # 채팅 (client/client.py)
kubectl port-forward svc/grafana 3000:3000       # admin / chat-server-demo
```

## 6. 디렉토리 구조

```
server/
├── log.h / log.c          # 구조화 로깅 단일 진입점 (log_json)
├── metrics.h / metrics.c  # Prometheus 텍스트 포맷 메트릭 렌더링
├── Dockerfile              # 멀티스테이지 빌드 (gcc:13 -> debian:bookworm-slim)
├── Makefile                 # 컨테이너 빌드의 전제 조건
└── (그 외 소켓 서버 소스, README.md 참고)

k8s/
├── kind-config.yaml         # 로컬 단일 노드 클러스터 정의
├── chat-server.yaml         # Deployment(replicas=1) + Service + ConfigMap + probes
├── promtail.yaml            # DaemonSet + RBAC + ConfigMap
├── loki.yaml                # StatefulSet + PVC + Service
├── prometheus.yaml          # Deployment + PVC + ConfigMap(scrape) + RBAC
├── grafana.yaml             # Deployment + PVC + Secret + ConfigMap(datasource/dashboard)
└── grafana-dashboards/
    └── chat.json             # 대시보드 정의 (활성 접속 수 / 방 수 / 초당 메시지 / job queue depth / 에러 로그)
```
