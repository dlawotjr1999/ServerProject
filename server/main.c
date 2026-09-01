/*
* pthread_tryjoin_np()는 POSIX 표준이 아니라 glibc(GNU) 확장이라 _GNU_SOURCE를 정의해야
* 선언이 보인다 - 반드시 다른 include보다 먼저(파일의 첫 줄) 와야 함, 아래 종료 시퀀스에서 씀
*/
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

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
	* 네트워크 이벤트 루프 실행. net_run()이 반환하면 종료 절차를 시작한다.
	*
	* net_run()이 반환하는 순간 g_io_q의 유일한 소비자가 사라진다. 그런데 아직 남은 job을 처리
	* 중인 worker가 disconnect->close나 broadcast->send 과정에서 job_queue_push_close()/
	* job_queue_push_send()(둘 다 blocking)로 g_io_q에 넣을 수 있다 - 아무도 안 비우면 큐가
	* 가득 찼을 때 그 worker가 영원히 멈추고, 그러면 이 worker는 이 함수가 몇 줄 아래에서 넣으려는
	* JOB_SHUTDOWN도 영영 못 받아 pthread_join()이 끝나지 않는다(k8s는 결국 SIGKILL로 강제 종료).
	* 최악의 경우 반대 방향으로도 막힐 수 있다 - g_logic_q가 이미 가득 찬 상태에서 모든 worker가
	* g_io_q push에 막혀 아무도 g_logic_q를 못 비우면, 아래 JOB_SHUTDOWN을 넣으려는 이 스레드의
	* push조차 블록될 수 있다(g_logic_q <-> g_io_q 상호 포화).
	*
	* 그래서 net_run() 종료 후에도 이 스레드(main)가 모든 worker가 join될 때까지 아래 두 가지를
	* "동시에" 계속한다:
	* 1) net_drain_io_queue()로 g_io_q를 계속 비워준다 - net_run()이 하던 소비자 역할을 이어받음
	* 2) 아직 못 보낸 JOB_SHUTDOWN을 non-blocking(job_queue_try_push_shutdown)으로 재시도한다 -
	*    blocking push였다면 위에서 설명한 상호 포화 상황에서 이 스레드 자신도 막혀버렸을 것이다
	* pthread_tryjoin_np()(glibc 확장)로 각 worker가 이미 끝났는지 논블로킹으로 확인하면서,
	* 아직 안 끝난 worker가 있으면 짧게 쉬었다가 위 두 가지를 반복한다
	*/
	net_run();

	bool shutdown_sent[WORKER_THREAD_NUM] = { false };
	int shutdown_sent_count = 0;
	bool joined[WORKER_THREAD_NUM] = { false };
	int joined_count = 0;

	while (joined_count < WORKER_THREAD_NUM) {
		net_drain_io_queue();

		for (int i = 0; i < WORKER_THREAD_NUM; i++) {
			if (!shutdown_sent[i] && job_queue_try_push_shutdown(&g_logic_q) == 0) {
				shutdown_sent[i] = true;
				shutdown_sent_count++;
			}
		}
		(void)shutdown_sent_count;   /* 디버깅/가독성용 카운터 - 조건문에는 안 씀 */

		for (int i = 0; i < WORKER_THREAD_NUM; i++) {
			if (joined[i]) continue;
			if (pthread_tryjoin_np(worker_tids[i], NULL) == 0) {
				joined[i] = true;
				joined_count++;
			}
		}

		if (joined_count < WORKER_THREAD_NUM) {
			struct timespec ts = { 0, 1000000L };   /* 1ms - 바쁜 대기(busy wait)를 피하기 위한 짧은 sleep */
			nanosleep(&ts, NULL);
		}
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
