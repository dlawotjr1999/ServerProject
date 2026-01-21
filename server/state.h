#ifndef STATE_H
#define STATE_H

#include "common.h"

// 세션 정보 구조체
typedef struct session {
	int session_id;
	int fd;
	int room_id;
	bool alive;

	char send_buf[SEND_BUF_SIZE];
	size_t size_len;
	size_t size_offset;
} session_t;

// 방 정보 구조체
typedef struct room {
	int room_id;
	session_t* users[MAX_ROOM_USER];
	int user_count;
	pthread_mutex_t lock;
} room_t;

/* session API */
session_t* session_get(int fd);       // 조회만 (생성 X)
session_t* session_create(int fd);    // 생성만 (없을 때만 생성)
void session_remove(int fd);

/* room API */
room_t* room_get(int room_id);  // 
room_t* room_create(void);      //     
room_t* room_find(void);

void room_join(room_t* room, session_t* s);
void room_leave(session_t* s);
void room_broadcast(room_t* room, session_t* sender, packet_t* pkt);

#endif
