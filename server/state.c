#include "state.h"
#include <stdlib.h>
#include <stdio.h>

static session_t* sessions[MAX_CLIENTS];
static int next_session_id = 1;

static room_t rooms[MAX_ROOMS];
static int room_count = 0;

session_t* session_get_or_create(int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS)
        return NULL;

    session_t* s = sessions[fd];
    if (s)
        return s;

    s = malloc(sizeof(session_t));
    if (!s)
        return NULL;

    s->session_id = next_session_id++;
    s->fd = fd;
    s->room_id = -1;
    s->alive = true;

    sessions[fd] = s;

    printf("[SESSION] created sid=%d fd=%d\n",
        s->session_id, fd);

    return s;
}

void session_remove(int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS)
        return;

    session_t* s = sessions[fd];
    if (!s)
        return;
    s->alive = false;

    if(s->room_id >= 0)
        room_leave(s);

    printf("[SESSION] removed sid=%d fd=%d\n",
        s->session_id, fd);

    free(s);
    sessions[fd] = NULL;
}

room_t* room_find_or_create(void)
{
    for (int i = 0; i < room_count; i++) {
        if (rooms[i].user_count < MAX_ROOM_USER)
            return &rooms[i];
    }

    if (room_count >= MAX_ROOMS)
        return NULL;

    room_t* r = &rooms[room_count];
    r->room_id = room_count;
    r->user_count = 0;
    pthread_mutex_init(&r->lock, NULL);

    room_count++;
    printf("[ROOM] created room_id=%d\n", r->room_id);
    return r;
}

room_t* room_get(int room_id)
{
    if (room_id < 0 || room_id >= room_count)
        return NULL;

    return &rooms[room_id];
}

void room_join(room_t* room, session_t* s) {
    pthread_mutex_lock(&room->lock);

    if (room->user_count >= MAX_ROOM_USER) {
        pthread_mutex_unlock(&room->lock);
        return;
    }

    room->users[room->user_count++] = s;
    s->room_id = room->room_id;

    printf("[ROOM] sid=%d joined room=%d\n",
        s->session_id, room->room_id);

    pthread_mutex_unlock(&room->lock);
}

void room_leave(session_t* s) {
    if (s->room_id < 0)
        return;

    room_t* room = &rooms[s->room_id];

    pthread_mutex_lock(&room->lock);

    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i] == s) {
            room->users[i] = room->users[room->user_count - 1];
            room->user_count--;
            break;
        }
    }

    printf("[ROOM] sid=%d left room=%d\n",
        s->session_id, s->room_id);

    s->room_id = -1;

    pthread_mutex_unlock(&room->lock);
}

void room_broadcast(room_t* room, session_t* sender, packet_t* pkt)
{
    int fds[MAX_ROOM_USER];
    int count = 0;
    int except_fd = sender ? sender->fd : -1;

    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->user_count; ++i) {
        session_t* s = room->users[i];
        if (!s) continue;
        if (s->fd == except_fd) continue;
        fds[count++] = s->fd;
    }
    pthread_mutex_unlock(&room->lock);

    // I/O는 lock 밖에서
    for (int i = 0; i < count; ++i) {
        packet_send(fds[i], pkt);
    }
}

