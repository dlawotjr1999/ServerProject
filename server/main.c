#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"
#include "net.h"
#include "logic.h"
#include "job_queue.h"

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

	/* 로직 worker thread 생성 */
	for(int i = 0; i < WORKER_THREAD_NUM; ++i) {
		pthread_t tid;
		if (pthread_create(&tid, NULL, worker_thread, NULL) != 0) {
			perror("pthread_create");
			exit(1);
		}
		pthread_detach(tid);
	}

	/* 네트워크 모듈 초기화 */
	if (net_init() < 0) {
		fprintf(stderr, "net_init failed\n");
		exit(1);
	}

	/* 
	* 네트워크 이벤트 루프 실행 
	* net_run이 반환하면 종료 절차를 수행
	* 종료 절차는 각 worker thread가 shutdown을 하나씩 받을 수 있게 하도록 함
	*/
	net_run();
	for (int i = 0; i < WORKER_THREAD_NUM; i++) {
		job_queue_push_shutdown(&g_logic_q);
	}

	return 0;
}
