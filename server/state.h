#ifndef STATE_H
#define STATE_H

#include "common.h"
#include "net.h"

typedef struct session {
	int session_id;
	int fd;
	int room_id;
	bool alive;

	char send_buf[SEND_BUF_SIZE];
	size_t size_len;
	size_t size_offset;
} session_t;

typedef struct room {
	int room_id;
	session_t* users[MAX_ROOM_USER];
	int user_count;
	pthread_mutex_t lock;
} room_t;

/* session API */
session_t* session_get_or_create(int fd);
void session_remove(int fd);

/* room API */
room_t* room_find_or_create(void);
room_t* room_get(int room_id);
void room_join(room_t* room, session_t* s);
void room_leave(session_t* s);
void room_broadcast(room_t* room, session_t* sender, packet_t* pkt);

#endif
