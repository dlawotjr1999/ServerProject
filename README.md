## 1. 개요

- epoll 기반 멀티 클라이언트 채팅 서버
- 네트워크 처리와 로직 처리를 분리한 2-스레드 구조이며, 스레드 간 통신은 job_queue로 진행됨
- 네트워크 스레드는 accept/recv/send와 epoll 이벤트 루프를 담당
- 수신 데이터는 protocol 파서가 패킷 단위로 파싱 진행
- 로직 스레드는 패킷을 처리하고 세션/룸 상태를 갱신하며, 브로드캐스트는 send 작업으로 변환되어 네트워크 스레드로 전달됨
- 송신 지연을 막기 위해 eventfd로 epoll을 깨움
- 현재는 입장/퇴장/채팅 브로드캐스트를 지원합니다

## 2. 실행 방법

- 서버는 ~/ServerProject/server에서 ./server로 실행
- 클라이언트는 다른 터미널에서 python3 ~/Project/client/client.py --host 127.0.0.1 --port 3800 --local-echo 커맨드로 실행

## 3. 디렉토리 구조

server/
├── common.h
├── main.c
├── net.h
├── net.c
├── job_queue.h
├── job_queue.c
├── logic.h
├── logic.c
├── state.h
├── state.c
└── protocol.c

client/
└── client.py

## 4. 모듈 별 설명

- common.h
- main.c
- net.c
- job_queue.c
- logic.c
- state.c
- protocol.c
- client.py
