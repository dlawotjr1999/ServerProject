#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include <stdio.h>

extern job_queue_t g_logic_q;

static void handle_packet(session_t* s, packet_t* pkt);
static void handle_disconnect(int fd);
static void handle_shutdown(void);

void* worker_thread(void* arg)
{
    (void)arg;
    job_t job;

    while (1) {
        job_queue_pop(&g_logic_q, &job, JOBQ_BLOCK);

        switch (job.type) {

        case JOB_PACKET: {
            session_t* s = session_get(job.fd);
            if (!s) s = session_create(job.fd);

            if (!s || !s->alive) {
                printf("[ERROR] session create failed fd=%d\n", job.fd);
                break;
            }

            handle_packet(s, &job.packet);

            printf("[WORKER] sid=%d fd=%d type=%d len=%d\n",
                s->session_id,
                job.fd,
                job.packet.type,
                job.packet.length);
            break;
        }

        case JOB_DISCONNECT: {
            handle_disconnect(job.fd);
            break;
        }

        case JOB_SHUTDOWN: {
            handle_shutdown();
            return NULL;   // worker 종료
        }

        default:
            break;
        }
    }

    return NULL;
}

static void handle_packet(session_t* s, packet_t* pkt) {
    if (!s || !s->alive)
        return;

    switch (pkt->type) {

    case PKT_JOIN_ROOM: {
        if (s->room_id >= 0)
            break;

        room_t* r = room_find();
        if (!r) r = room_create();
        room_join(r, s);
        break;
    }

    case PKT_CHAT: {
        if (s->room_id < 0)
            break;

        room_t* r = room_get(s->room_id);
        if (!r)
            break;
        room_broadcast(r, s, pkt);
        break;
    }

	case PKT_LEAVE_ROOM : {
		if(s->room_id < 0)
			break;

		room_leave(s);
		break;
	}

    default:
        break;
    }
}

static void handle_disconnect(int fd)
{
    session_t* s = session_get(fd);
    if (!s) {
        // 이미 정리됐거나, 세션 생성 전 끊긴 경우
        return;
    }

    printf("[LOGIC] fd=%d disconnect event\n", fd);

    /* 방에 들어가 있었다면 방에서 제거 */
    if (s->room_id >= 0) {
        room_leave(s);
    }

    /* 세션 제거 */
    session_remove(fd);
}

static void handle_shutdown(void)
{
    printf("[LOGIC] graceful shutdown started\n");

    for (int fd = 0; fd < MAX_CLIENTS; ++fd) {
        session_t* s = session_get(fd);
        if (!s)
            continue;

        if (s->room_id >= 0) {
            room_leave(s);
        }

        session_remove(fd);
    }

    printf("[LOGIC] graceful shutdown completed\n");
}