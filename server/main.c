#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "net.h"
#include "logic.h"
#include "job_queue.h"
#include "state.h"
#include "redis_client.h"

/*
* g_logic_q : net -> logic(수신 패킷/끊김/종료 같은 "이벤트 전달")
* g_io_q : logic -> net(send 요청 같은 "I/O 수행 요청")
* 
* net thread는 epoll loop를 돌며 I/O에 집중해야 하고, logic thread는 상태 갱신과 브로드캐스트 결정을 담당해야 하므로 큐를 분리함
* 즉 방향성이 다른 작업을 분리해서 책임과 흐름을 명확히 하기 위해 큐를 분리함
*/
job_queue_t g_logic_q;
job_queue_t g_io_q;

/*
* g_terminate는 시그널 핸들러에서 비동기적으로 변경되므로, 컴파일러가 루프에서 값을 레지스터에 캐시하거나 읽기를 생략하는 최적화를 하면 변경을 못 보고 무한 루프가 될 수 있음
* volatile 키워드를 통해 이런 최적화를 막아 매번 메모리에서 값을 다시 읽게 해서, 시그널로 바뀐 종료 플래그를 놓치지 않게 함
*/
volatile sig_atomic_t g_terminate = 0;

/*
* Redis 구독 연결이 런타임에 끊겨서(재시작/네트워크 단절) 종료하는 경우 net.c가 1로 올린다
* -> 시그널에 의한 정상 종료와 구분해서 종료 코드를 다르게 돌려주기 위한 플래그
*/
volatile sig_atomic_t g_redis_fatal = 0;

/* SIGINT(Ctrl+C) / SIGTERM(종료 요청) 수신 시 종료 플래그 설정 */
void handle_sigint(int sig) {
	if (sig == SIGINT || sig == SIGTERM)
		g_terminate = 1;
}

int main() {

	/* 종료 시그널 처리 */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);

	/* 스레드 간 작업 큐 초기화 */
	job_queue_init(&g_logic_q);
	job_queue_init(&g_io_q);

	/* 방 슬롯의 뮤텍스를 미리 전부 초기화 (3단계) - worker가 방을 건드리기 전에 끝나야 함 */
	state_init();

	/*
	* 로직 worker thread 생성
	* detach하지 않고 tid를 배열에 보관 -> 종료 시 pthread_join으로 정리 완료를 기다리기 위함
	* (k8s가 pod 삭제 시 보내는 SIGTERM에 대해 graceful termination을 보장해야 함)
	*/
	pthread_t worker_tids[WORKER_THREAD_NUM];
	for(int i = 0; i < WORKER_THREAD_NUM; ++i) {
		if (pthread_create(&worker_tids[i], NULL, worker_thread, NULL) != 0) {
			perror("pthread_create");
			exit(1);
		}
	}

	/* 네트워크 모듈 초기화 */
	if (net_init() < 0) {
		fprintf(stderr, "net_init failed\n");
		exit(1);
	}

	/*
	* 네트워크 이벤트 루프 실행
	* net_run이 반환하면 종료 절차를 수행
	* 종료 절차는 각 worker thread가 shutdown을 하나씩 받게 한 뒤,
	* 전부 정리를 마칠 때까지 join으로 대기함 -> worker 정리 전에 프로세스가 먼저 죽는 것을 방지
	*/
	net_run();
	for (int i = 0; i < WORKER_THREAD_NUM; i++) {
		job_queue_push_shutdown(&g_logic_q);
	}
	for (int i = 0; i < WORKER_THREAD_NUM; i++) {
		pthread_join(worker_tids[i], NULL);
	}

	/*
	* Redis 연결 정리는 반드시 위 join이 전부 끝난 뒤에 해야 함
	* worker의 종료 경로(handle_shutdown -> session_remove_all -> room_leave -> redis_leave_room)가
	* 명령용 Redis 커넥션을 계속 사용하므로, 그보다 먼저 정리하면 SIGTERM 시점에 방에 남아있던
	* 유저가 있을 때 이미 해제된 커넥션을 참조하게 됨(use-after-free)
	* -> "worker 정리가 끝난 뒤에 net 소유 자원을 해제한다"는 기존 종료 순서 원칙과 동일한 자리
	*/
	redis_client_shutdown();

	/* 시그널에 의한 정상 종료면 0, Redis 구독 연결 유실로 인한 종료면 1 (k8s가 구분해서 보게 함) */
	return g_redis_fatal ? 1 : 0;
}
