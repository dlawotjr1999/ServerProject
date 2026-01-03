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
	uint16_t type;
	uint16_t length;
	char payload[MAX_PACKET_SIZE];
} packet_t;

typedef struct {
	int fd;
	packet_t packet;
} job_t;

typedef struct {
	int fd;

	// recv
	char recv_buf[RECV_BUF_SIZE];
	int recv_len;

	// send
	char send_buf[SEND_BUF_SIZE];
	int send_len;
	int send_offset;
} connection_t;

#endif
