server/
├── main.c
├── net.c // socket, epoll, accept, recv, send
├── net.h
├── logic.c // worker thread, job 처리
├── logic.h
├── state.c // session, room 관리
├── state.h
├── protocol.c // packet 파싱
├── protocol.h
└── common.h // 공통 struct, 상수

client/
├── main.c // 초기화, 스레드 생성
├── client.c // socket / connect / send
├── client.h
├── receiver.c // recv 전용 스레드
├── receiver.h
├── protocol.h // packet 정의 (서버와 공유)
└── common.h // 최소 매크로
