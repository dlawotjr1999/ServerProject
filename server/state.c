#include "state.h"
#include "job_queue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static session_t* sessions[MAX_CLIENTS];
static int next_session_id = 1;

static room_t rooms[MAX_ROOMS];
static int room_count = 0;

static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rooms_lock = PTHREAD_MUTEX_INITIALIZER;

extern job_queue_t g_io_q;
extern void net_wakeup(void);

session_t* session_get(int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS)
        return NULL;

    pthread_mutex_lock(&g_sessions_lock);
    session_t* s = sessions[fd];
    pthread_mutex_unlock(&g_sessions_lock);

    return s;
}

session_t* session_create(int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS)
        return NULL;

    pthread_mutex_lock(&g_sessions_lock);

    // 이미 있으면 그대로 반환 (중복 생성 방지)
    session_t* s = sessions[fd];
    if (s) {
        pthread_mutex_unlock(&g_sessions_lock);
        return s;
    }

    s = malloc(sizeof(session_t));
    if (!s) {
        pthread_mutex_unlock(&g_sessions_lock);
        return NULL;
    }
    memset(s, 0, sizeof(*s));

    s->session_id = next_session_id++;
    s->fd = fd;
    s->room_id = -1;
    s->alive = true;

    sessions[fd] = s;

    pthread_mutex_unlock(&g_sessions_lock);

    printf("[SESSION] created sid=%d fd=%d\n", s->session_id, fd);
    return s;
}

void session_remove(int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS)
        return;

    pthread_mutex_lock(&g_sessions_lock);
    session_t* s = sessions[fd];
    if (!s) {
        pthread_mutex_unlock(&g_sessions_lock);
        return;
    }

    sessions[fd] = NULL;   // 먼저 테이블에서 제거
    s->alive = false;
    pthread_mutex_unlock(&g_sessions_lock);

    printf("[SESSION] removed sid=%d fd=%d\n", s->session_id, fd);
    free(s);
}

/* ---------------- room ---------------- */
room_t* room_get(int room_id)
{
    pthread_mutex_lock(&g_rooms_lock);
    int max = room_count;
    pthread_mutex_unlock(&g_rooms_lock);

    if (room_id < 0 || room_id >= max)
        return NULL;

    return &rooms[room_id];
}

/* 생성만: 새 방 생성 */
room_t* room_create(void)
{
    pthread_mutex_lock(&g_rooms_lock);

    if (room_count >= MAX_ROOMS) {
        pthread_mutex_unlock(&g_rooms_lock);
        return NULL;
    }

    room_t* r = &rooms[room_count];
    memset(r, 0, sizeof(*r));
    r->room_id = room_count;
    r->user_count = 0;
    pthread_mutex_init(&r->lock, NULL);

    room_count++;

    pthread_mutex_unlock(&g_rooms_lock);

    printf("[ROOM] created room_id=%d\n", r->room_id);
    return r;
}

/* 조회만: 빈 자리 있는 방 찾기 */
room_t* room_find(void)
{
    pthread_mutex_lock(&g_rooms_lock);
    for (int i = 0; i < room_count; i++) {
        if (rooms[i].user_count < MAX_ROOM_USER) {
            room_t* r = &rooms[i];
            pthread_mutex_unlock(&g_rooms_lock);
            return r;
        }
    }
    pthread_mutex_unlock(&g_rooms_lock);
    return NULL;
}

void room_join(room_t* room, session_t* s)
{
    if (!room || !s) return;

    pthread_mutex_lock(&room->lock);

    // 이미 이 방에 있으면 중복 추가 방지
    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i] == s) {
            pthread_mutex_unlock(&room->lock);
            return;
        }
    }

    if (room->user_count >= MAX_ROOM_USER) {
        pthread_mutex_unlock(&room->lock);
        return;
    }

    room->users[room->user_count++] = s;
    s->room_id = room->room_id;

    printf("[ROOM] sid=%d joined room=%d\n", s->session_id, room->room_id);

    pthread_mutex_unlock(&room->lock);
}

void room_leave(session_t* s)
{
    if (!s) return;
    if (s->room_id < 0) return;

    // room_id 범위 체크
    room_t* room = room_get(s->room_id);
    if (!room) {
        s->room_id = -1;
        return;
    }

    pthread_mutex_lock(&room->lock);

    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i] == s) {
            room->users[i] = room->users[room->user_count - 1];
            room->users[room->user_count - 1] = NULL;
            room->user_count--;
            break;
        }
    }

    printf("[ROOM] sid=%d left room=%d\n", s->session_id, s->room_id);

    s->room_id = -1;

    pthread_mutex_unlock(&room->lock);
}

void room_broadcast(room_t* room, session_t* sender, packet_t* pkt)
{
    if (!room || !pkt) return;

    int fds[MAX_ROOM_USER];
    int count = 0;
    int except_fd = sender ? sender->fd : -1;

    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->user_count; ++i) {
        session_t* s = room->users[i];
        if (!s) continue;
        if (!s->alive) continue;
        if (s->fd == except_fd) continue;
        fds[count++] = s->fd;
    }
    pthread_mutex_unlock(&room->lock);

    // pkt->length는 (type + payload)
    int payload_len = (int)pkt->length - 2;
    if (payload_len <= 0) return;
    if (payload_len > MAX_PACKET_SIZE) payload_len = MAX_PACKET_SIZE;

    packet_t out;
    memset(&out, 0, sizeof(out));

    int n = snprintf(out.payload, MAX_PACKET_SIZE, "%.*s\n", payload_len, pkt->payload);
    if (n <= 0 || n >= MAX_PACKET_SIZE)
        return;

    out.type = PKT_CHAT;
    out.length = 2 + (uint16_t)n;

    for (int i = 0; i < count; ++i) {
        job_t job = { 0 };
        job.type = JOB_SEND;
        job.fd = fds[i];
        job.packet = out;
        job_queue_push(&g_io_q, &job);
    }
    net_wakeup();
}