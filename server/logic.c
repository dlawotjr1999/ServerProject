#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include <stdio.h>

extern job_queue_t g_job_queue;

static void handle_packet(session_t* s, packet_t* pkt);

void* worker_thread(void* arg) {
	(void) arg;
	job_t job;

	while(1) {
		job_queue_pop(&g_job_queue, &job);
	
		session_t* s = session_get_or_create(job.fd);
        handle_packet(s, &job.packet);

		if(!s) {
			printf("[ERROR] session create failed fd = %d\n", job.fd);
			continue;
		}

		printf("[WORKER] sid = %d fd = %d type = %d len = %d\n", s->session_id, job.fd, job.packet.type, job.packet.length);
	}

	return NULL;
}

void handle_packet(session_t* s, packet_t* pkt)
{
    switch (pkt->type) {

    case PKT_JOIN_ROOM: {
        if (s->room_id >= 0)
            break;

        room_t* r = room_find_or_create();
        if (r)
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

    default:
        break;
    }
}
