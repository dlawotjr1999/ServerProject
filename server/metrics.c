#include "metrics.h"
#include "common.h"
#include "state.h"
#include "job_queue.h"

#include <stdatomic.h>
#include <stdio.h>

extern job_queue_t g_logic_q;
extern job_queue_t g_io_q;

/* 누적 카운터형 지표. net/logic 여러 스레드에서 동시에 증가시키므로 atomic으로 선언 */
static atomic_long g_messages_total = 0;
static atomic_long g_disconnects_total = 0;

/* 채팅 메시지가 방에 브로드캐스트될 때마다 logic 스레드에서 호출되어 누적 카운터를 증가시키는 함수 */
void metrics_inc_messages(void)
{
	atomic_fetch_add(&g_messages_total, 1);
}

/* 클라이언트 연결이 끊길 때마다 net 스레드에서 호출되어 누적 카운터를 증가시키는 함수 */
void metrics_inc_disconnects(void)
{
	atomic_fetch_add(&g_disconnects_total, 1);
}

/*
* 현재 서버 상태를 Prometheus 텍스트 포맷으로 buf에 렌더링하는 함수
* 스크레이핑 주기가 수 초~수십 초로 길기 때문에, 게이지 값은 별도 카운터를 두지 않고
* 매 호출마다 세션/방/큐 테이블을 스캔해서 계산함(핫 패스인 accept/recv/send 경로에 부담을 주지 않기 위함)
*/
int metrics_render(char* buf, size_t bufsize)
{
	/* 접속 수 / 방 수는 세션·방 테이블을 스캔해서 매번 새로 계산(state.c 참조) */
	int connections_active = state_count_active_sessions();
	int rooms_active = state_count_active_rooms();

	/* net -> logic, logic -> net 두 큐 각각의 적체량을 조회 (net/logic 스레드 간 병목 파악용) */
	int logic_depth = job_queue_depth(&g_logic_q);
	int io_depth = job_queue_depth(&g_io_q);

	/* Prometheus 텍스트 포맷(# HELP, # TYPE 주석 + "지표명 값") 규격에 맞춰 한 번에 렌더링 */
	return snprintf(buf, bufsize,
		"# HELP chat_connections_active Number of active client connections\n"
		"# TYPE chat_connections_active gauge\n"
		"chat_connections_active %d\n"
		"# HELP chat_rooms_active Number of rooms with at least one member\n"
		"# TYPE chat_rooms_active gauge\n"
		"chat_rooms_active %d\n"
		"# HELP chat_messages_total Total chat messages broadcast\n"
		"# TYPE chat_messages_total counter\n"
		"chat_messages_total %ld\n"
		"# HELP chat_disconnects_total Total client disconnects handled\n"
		"# TYPE chat_disconnects_total counter\n"
		"chat_disconnects_total %ld\n"
		"# HELP chat_jobqueue_depth Pending jobs per queue\n"
		"# TYPE chat_jobqueue_depth gauge\n"
		"chat_jobqueue_depth{queue=\"logic\"} %d\n"
		"chat_jobqueue_depth{queue=\"io\"} %d\n",
		connections_active,
		rooms_active,
		atomic_load(&g_messages_total),
		atomic_load(&g_disconnects_total),
		logic_depth,
		io_depth);
}
