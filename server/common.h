#ifndef COMMON_H
#define COMMON_H


#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>


#define PORTNUM 3800
#define MAX_EVENTS 64
#define MAX_CLIENTS 512

#define RECV_BUF_SIZE 4096
#define SEND_BUF_SIZE 4096
#define MAX_PACKET_SIZE 1024

#define MAX_ROOM_USER 8
#define MAX_ROOMS 256

#define WORKER_THREAD_NUM 4
#define JOB_QUEUE_SIZE 1024

typedef enum {
	PKT_CHAT = 1,        // 채팅
	PKT_JOIN_ROOM,       // 방 입장
	PKT_LEAVE_ROOM,      // 방 퇴장
	PKT_GAME_ACTION,     // 게임 입력
	PKT_GAME_RESULT,     // 게임 결과
} packet_type_t;

typedef struct {
	uint16_t type;					// 프로토콜 종류
	uint16_t length;				// payload 실제 길이
	char payload[MAX_PACKET_SIZE];	// 가변 데이터 영역
} packet_t;

typedef struct {
	int fd;

	// recv
	char recv_buf[RECV_BUF_SIZE];	// 수신 버퍼
	int recv_len;					// 현재 수신된 버퍼 길이	

	// send
	char send_buf[SEND_BUF_SIZE];	// 송신 버퍼
	int send_len;					// 송신해야 할 전체 데이터 길이
	int send_offset;				// 이미 전송된 바이트 수(부분 전송을 위해 필요)
} connection_t;

#endif
