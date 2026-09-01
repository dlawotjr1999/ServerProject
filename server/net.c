#include <sys/eventfd.h>
#include <poll.h>

#include "common.h"
#include "net.h"
#include "protocol.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"
#include "redis_client.h"

static int listen_fd = -1;
static int metrics_listen_fd = -1;
static int epfd = -1;
static int wake_fd = -1;

/* 메트릭/헬스체크 전담 스레드(아래 metrics_thread_main 참고) - net_init()이 생성, net_run() 종료 시 join */
static pthread_t g_metrics_tid;

/* Redis room 하트비트 전담 스레드(아래 heartbeat_thread_main 참고) - net_init()이 생성, net_run() 종료 시 join */
static pthread_t g_heartbeat_tid;

/* net_init()이 리스닝 소켓 bind까지 전부 성공했는지 나타내는 플래그(readiness probe용) */
static volatile bool g_net_ready = false;

/* fd -> connection 객체 매핑 테이블 */
static connection_t* connections[MAX_CLIENTS];

extern job_queue_t g_io_q;
extern job_queue_t g_logic_q;

/* epoll_wait로 대기 중인 네트워크 스레드를 깨우는 함수 */
void net_wakeup(void) {
	if (wake_fd < 0) return;

	uint64_t one = 1;

	for (;;) {
		/* eventfd는 8바이트 정수 쓰기를 요구하며, 성공하면 epoll_wait가 즉시 깨어남 */
		ssize_t rc = write(wake_fd, &one, sizeof(one));
		
		/* 정상적으로 깨우는 경우 */
		if (rc == (ssize_t)sizeof(one)) {
			return;                 
		}
		if (rc < 0) {
			/* 
			* 시그널로 끊김 -> 재시도
			* non-block + 카운터 포화 -> 깨우기 실패해도 치명적 아님
			* 기타 오류는 조용히 종료
			*/  
			if (errno == EINTR) continue; 
			if (errno == EAGAIN) return;   
			return;                    
		}

		/* 부분 write 방어 */
		return; 
	}
}

/*
* fd에 대응하는 네트워크 연결을 실제로(close 시스템콜까지) 종료하는 함수
* net_disconnect()가 즉시 부르는 게 아니라, logic 스레드가 세션 정리를 끝내고
* JOB_CLOSE로 알려온 시점에만 호출됨 -> 그 전까지는 fd가 계속 열려있어서
* 커널이 이 fd 번호를 다른 연결에 재할당할 수 없음(fd 재사용 경쟁의 근본 차단)
*/
static void close_connection(int fd)
{
	connection_t* conn = connections[fd];
	if (!conn) return;

	/*
	* epoll에서 이미 빠져있겠지만(net_disconnect에서 먼저 제거), 혹시 몰라 한 번 더 시도해도 무해함
	* 실제 소켓 종료 후 connection 구조체 메모리 해제, 마지막으로 연결 테이블에서 제거
	*/
	epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
	close(fd);
	free(conn);
	connections[fd] = NULL;

	log_json("INFO", "close", "fd", LOG_ARG_INT, fd, NULL);
}

/*
* 네트워크 연결 종료를 감지했을 때 호출되는 함수
* fd를 즉시 close하지 않는 게 핵심: epoll 감시에서만 즉시 빼서 더 이상 이벤트를 받지 않게 하고,
* 실제 close()는 logic 스레드가 세션/룸 정리를 끝내고 JOB_CLOSE를 보내올 때까지 미룸.
* 그래야 정리가 끝나기 전에 커널이 이 fd 번호를 새 accept에 재할당하는 일이 없음
*/
static void net_disconnect(int fd)
{
	if (fd < 0 || fd >= MAX_CLIENTS) return;

	connection_t* conn = connections[fd];
	if (!conn || conn->closing) return;   /* 이미 처리 중이거나 없는 fd면 중복 처리 방지 */
	conn->closing = true;

	/* epoll 감시에서만 즉시 제거. close()는 아직 하지 않음 */
	epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);

	/* 세션 / 룸 상태 정리는 로직 스레드에서 처리하도록 DISCONNECT 작업을 큐에 전달 */
	job_queue_push_disconnect(&g_logic_q, conn->session_id, fd);
	log_json("INFO", "disconnect_queued", "fd", LOG_ARG_INT, fd, "session_id", LOG_ARG_INT, conn->session_id, NULL);

	/* 연결 종료 지표 누적 (메트릭 노출용) */
	metrics_inc_disconnects();
}

/* 소켓을 nonblocking 모드로 설정하는 함수 */
static int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
* 패킷을 connection의 send 버퍼에 직렬화하여 적재하는 함수
* 실제 send는 epoll의 EPOLLOUT 이벤트에서 수행됨
*/
int packet_send(int fd, packet_t* pkt) {
	connection_t* conn = connections[fd];
	if (!conn)
		return -1;

	/* pkt->length는 (type + payload) 길이 */
	if (pkt->length < 2 || pkt->length > MAX_PACKET_SIZE + 2)
		return -1;

	int payload_len = pkt->length - 2;
	if (payload_len < 0)
		return -1;

	/* length(2) + (type + payload) */
	int total_len = 2 + pkt->length; 

	if (conn->send_len + total_len > SEND_BUF_SIZE)
		return -1;

	/* 안전한 직렬화 시작 */
	uint16_t net_len = htons(pkt->length);
	uint16_t net_type = htons(pkt->type);

	/* send 버퍼에 데이터 복사 후 send 버퍼 길이 갱신 */
	memcpy(conn->send_buf + conn->send_len, &net_len, 2);
	memcpy(conn->send_buf + conn->send_len + 2, &net_type, 2);
	if (payload_len > 0) {
		memcpy(conn->send_buf + conn->send_len + 4, pkt->payload, payload_len);
	}
	conn->send_len += total_len;

	/* EPOLLOUT을 활성화하여 전송 트리거 */
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLOUT;
	ev.data.fd = fd;
	epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);

	return 0;
}

/*
* session_id에 지금 대응하는 fd를 찾는 함수 (fd<->session_id 매핑은 net 스레드만 아는 정보이므로 여기서 조회)
* MAX_CLIENTS(4096)가 크지 않아 선형 탐색으로 충분함(핫 패스인 accept/recv/send 자체는 아님)
*/
static int find_fd_by_session_id(int session_id)
{
	for (int fd = 0; fd < MAX_CLIENTS; ++fd) {
		connection_t* conn = connections[fd];
		if (conn && !conn->closing && conn->session_id == session_id)
			return fd;
	}
	return -1;
}

/*
* IO 큐에서 전달된 SEND 작업을 처리하는 함수
* job은 fd가 아니라 session_id로 대상을 지정하므로, 실제 전송 직전에 그 session_id가
* 지금 어느 fd에 대응하는지 다시 조회함 -> 그 사이 대상이 끊기고 fd가 재사용됐어도
* session_id가 더 이상 안 맞으므로 엉뚱한 연결로 잘못 보내지 않고 조용히 스킵됨
*/
static void handle_send_job(job_t* job)
{
	int fd = find_fd_by_session_id(job->session_id);
	if (fd < 0)
		return;   /* 대상이 이미 끊겼거나, 다른 세션이 그 fd를 쓰고 있음 */

	/* 패킷 적재 실패 시 연결 종료 */
	if (packet_send(fd, &job->packet) < 0) {
		net_disconnect(fd);
	}
}

/*
* 메트릭/헬스체크 스크레이퍼 연결을 처리하는 함수.
* 채팅 클라이언트와 달리 요청/응답이 짧고 일회성이라고 보고, 원래는 채팅용 epoll 루프 안에서 accept
* 직후 동기 recv/send(각 2초 타임아웃)로 처리했었다. 하지만 이 함수가 채팅 epoll 루프와 같은
* 스레드에서 돌면, 응답을 늦게 받거나 안 받는 스크레이퍼 하나가 최대 4초(recv 2초 + send 2초) 동안
* 그 스레드가 담당하는 모든 걸 - 채팅 accept/recv/send, Redis pub/sub 처리, I/O job 처리, 심지어
* 다른 health/metrics 요청까지 - 전부 막아버린다. 연결만 반복하면 아주 적은 비용으로 서버 전체를
* 사실상 멈출 수 있는 구멍이었다. 그래서 이 함수 자체(동기 recv/send + 타임아웃)는 그대로 두되,
* 이제는 채팅 epoll 루프와 완전히 분리된 metrics_thread_main() 전용 스레드에서만 호출한다 -
* 여기서 최대 4초가 걸려도 채팅 쪽은 전혀 영향받지 않는다
*/
static void handle_metrics_accept(void)
{
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);

	/* metrics_listen_fd는 nonblocking이므로 대기 중인 연결이 없으면 -1과 함께 즉시 반환됨 */
	int cfd = accept(metrics_listen_fd, (struct sockaddr*)&addr, &alen);
	if (cfd < 0)
		return;

	/*
	* accept로 얻은 소켓은 기본적으로 blocking 상태로 생성됨
	* 응답을 늦게 받거나 안 받는 스크레이퍼가 붙어도 net 스레드가 무한정 멈추지 않도록
	* 수신/송신 각각에 짧은 타임아웃을 걸어둠
	*/
	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	/* HTTP 요청의 첫 줄(GET /path HTTP/1.1)만 보면 충분하므로 헤더 전체를 파싱하지 않고 일부만 수신 */
	char req[512];
	ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
	if (n <= 0) {
		close(cfd);
		return;
	}
	req[n] = '\0';

	char body[4096];
	int body_len;
	const char* status_line;

	/* 요청 라인 접두어만으로 경로를 판별 (쿼리스트링 없는 단순 스크레이핑 요청만 가정) */
	if (strncmp(req, "GET /metrics ", 13) == 0) {
		body_len = metrics_render(body, sizeof(body));
		status_line = "200 OK";
	}
	else if (strncmp(req, "GET /healthz ", 13) == 0) {
		/* liveness: 이 코드가 실행 중이라는 사실 자체가 프로세스 생존의 증거이므로 항상 OK */
		body_len = snprintf(body, sizeof(body), "ok\n");
		status_line = "200 OK";
	}
	else if (strncmp(req, "GET /readyz ", 12) == 0) {
		/*
		* readiness: net_init()에서 리스닝 소켓 bind까지 전부 성공해야 g_net_ready가 true가 됨
		* k8s가 이 값을 보고 아직 준비 안 된 pod로 트래픽을 보내지 않도록 함
		*/
		if (g_net_ready) {
			body_len = snprintf(body, sizeof(body), "ok\n");
			status_line = "200 OK";
		}
		else {
			body_len = snprintf(body, sizeof(body), "not ready\n");
			status_line = "503 Service Unavailable";
		}
	}
	else {
		body_len = snprintf(body, sizeof(body), "not found\n");
		status_line = "404 Not Found";
	}

	/* Content-Length를 정확히 채워야 curl/Prometheus 등 클라이언트가 응답의 끝을 올바르게 인식함 */
	char resp[4096 + 256];
	int resp_len = snprintf(resp, sizeof(resp),
		"HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
		status_line, body_len, body);

	ssize_t sent = send(cfd, resp, (size_t)resp_len, 0);
	(void)sent; /* 스크레이퍼가 응답을 끝까지 못 받아도 서버 자체 동작에는 영향이 없으므로 무시 */
	close(cfd);
}

/*
* metrics_listen_fd 전담 스레드의 본체. 채팅용 epoll 루프(net_run)와 완전히 분리된 자기만의
* accept 루프를 돈다 - metrics_listen_fd는 nonblocking이라 accept() 자체는 즉시 반환하지만,
* "연결이 들어올 때까지 기다리는" 부분이 필요해서 poll()로 최대 1초까지 대기한다.
* 1초로 짧게 잡은 건 blocking accept 대신 쓰는 것뿐이라 그 자체는 별 의미가 없고, g_terminate를
* 주기적으로 다시 확인해서 종료 신호를 놓치지 않고 이 루프를 빠져나가기 위함이다(순수 blocking
* accept였다면 종료 시점에 아무도 연결하지 않는 한 이 스레드가 영원히 poll에 막혀 join이 끝나지
* 않았을 것이다)
*/
static void* metrics_thread_main(void* arg)
{
	(void)arg;

	while (!g_terminate) {
		struct pollfd pfd;
		pfd.fd = metrics_listen_fd;
		pfd.events = POLLIN;

		int pr = poll(&pfd, 1, 1000);
		if (pr <= 0) continue;   /* 타임아웃(0) 또는 오류(-1) - g_terminate 재확인하러 루프 처음으로 */

		if (pfd.revents & POLLIN) {
			handle_metrics_accept();
		}
	}

	return NULL;
}

/*
* 방 하트비트(4단계 lease/heartbeat 복구)를 주기적으로 트리거하는 전담 스레드.
* state_heartbeat_local_rooms()가 하는 일(g_redis_cmd_lock 아래 blocking Redis 호출, 방 개수만큼
* 최대 3번씩)을 net 스레드(채팅 epoll 루프)에서 절대 하면 안 되므로 - 느려진 Redis 하나가 이
* 스레드를 오래 막으면 채팅 accept/recv/send/g_io_q 드레인/SIGTERM 감지까지 전부 같이 막힌다
* (metrics_thread_main()과 정확히 같은 이유로 별도 스레드 분리). 1초 단위로 g_terminate를
* 확인하면서 10초마다 한 번씩 실제 하트비트를 수행한다
*/
static void* heartbeat_thread_main(void* arg)
{
	(void)arg;

	while (!g_terminate) {
		for (int waited = 0; waited < 10 && !g_terminate; ++waited) {
			sleep(1);
		}
		if (g_terminate) break;

		state_heartbeat_local_rooms();
	}

	return NULL;
}

/*
* 네트워크 서버 초기화 함수
* 리스닝 소켓 생성 -> 인스턴스 생성 -> eventfd 기반 wakeup 메커니즘 등록
*
* 반환값: 성공 0, 실패 -1로 통일 (호출부인 main.c가 `< 0`으로만 검사하므로 모든 실패 경로가 이를 지켜야 함)
*/
int net_init() {
	struct sockaddr_in addr;

	/* SO_REUSEADDR 옵션 값 */
	int opt = 1;

	/* TCP 리스닝 소켓 생성 */
	if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket error");
		return -1;
	}

	/* SO_REUSEADDR 설정; TIME_WAIT 상태에서도 포트 재사용 가능 */
	if ((setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
		perror("setsockopt error");
		return -1;
	}

	/* 서버 주소 구조체 조기화 */
	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(PORTNUM);

	/* 소켓에 주소 바인딩 */
	if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind error");
		return -1;
	}

	/* listening 상태로 전환 */
	if (listen(listen_fd, 256) < 0) {
		perror("listen error");
		return -1;
	}

	/* listening 소켓을 nonblokcing 모드로 설정(epoll 기반 서버에서는 필수) */
	set_nonblocking(listen_fd);

	/* epoll 인스턴스 생성 */
	epfd = epoll_create1(0);
	if (epfd < 0) {
		perror("epoll error");
		return -1;
	}

	/* 
	* event fd 생성 
	* 네트워크 스레드가 epoll_wait 중일 때 외부에서 깨우기 용도
	*/
	wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (wake_fd < 0) {
		perror("eventfd error");
		return -1;
	}

	/*
	* eventfd를 epoll에 등록
	* EPOLLIN 이벤트 발생 시 epoll_wait가 즉시 깨어남
	*/
	struct epoll_event wev;
	memset(&wev, 0, sizeof(wev));
	wev.events = EPOLLIN;
	wev.data.fd = wake_fd;

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, wake_fd, &wev) < 0) {
		perror("epoll_ctl add wake_fd error");
		close(wake_fd);
		wake_fd = -1;
		return -1;
	}

	/*
	* 리스닝 소켓을 epoll에 등록
    * 새로운 클라이언트 연결 요청을 감지하기 위함
	*/
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;

	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

	/*
	* 메트릭/헬스체크용 두 번째 리스닝 소켓 생성
	* listen_fd와 동일한 패턴(생성 -> SO_REUSEADDR -> bind -> listen -> nonblocking -> epoll 등록)으로 구성
	*/
	if ((metrics_listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("metrics socket error");
		return -1;
	}

	if (setsockopt(metrics_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		perror("metrics setsockopt error");
		return -1;
	}

	/* 채팅용 주소 구조체(addr)와 별개로, 포트만 METRICS_PORT로 다르게 구성 */
	struct sockaddr_in metrics_addr;
	memset(&metrics_addr, 0x00, sizeof(metrics_addr));
	metrics_addr.sin_family = AF_INET;
	metrics_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	metrics_addr.sin_port = htons(METRICS_PORT);

	if (bind(metrics_listen_fd, (struct sockaddr*)&metrics_addr, sizeof(metrics_addr)) < 0) {
		perror("metrics bind error");
		return -1;
	}

	/* 스크레이퍼 전용 소켓이므로 채팅 리스닝 소켓(256)보다 훨씬 작은 backlog로 충분함 */
	if (listen(metrics_listen_fd, 16) < 0) {
		perror("metrics listen error");
		return -1;
	}

	set_nonblocking(metrics_listen_fd);

	/*
	* metrics_listen_fd는 채팅용 epoll 인스턴스(epfd)에 등록하지 않는다 - 대신 이 fd를 전담하는
	* metrics_thread_main() 스레드를 따로 띄운다(위 handle_metrics_accept()의 주석 참고: 느린
	* 스크레이퍼 하나가 채팅 스레드 전체를 최대 4초씩 막을 수 있던 문제를 스레드 분리로 없앰)
	*/
	if (pthread_create(&g_metrics_tid, NULL, metrics_thread_main, NULL) != 0) {
		perror("metrics thread create error");
		return -1;
	}

	/*
	* Redis room 하트비트(4단계 lease/heartbeat 복구) 전담 스레드도 함께 띄운다 - 위
	* heartbeat_thread_main()의 주석 참고. redis_client_init()이 아직 안 끝난 시점이지만, 이 스레드는
	* state_heartbeat_local_rooms() 안에서 매번 자체적으로 Redis 호출을 하므로 여기서 미리 띄워도
	* 문제 없다 (g_terminate가 아직 false인 동안은 최초 10초를 그냥 대기만 하다가 첫 하트비트를 보냄)
	*/
	if (pthread_create(&g_heartbeat_tid, NULL, heartbeat_thread_main, NULL) != 0) {
		perror("heartbeat thread create error");
		return -1;
	}

	/*
	* Redis 연결(명령용 + 구독용) 초기화 (3단계)
	* net_init()의 다른 실패 경로와 동일하게, 실패하면 -1을 반환해 서버가 즉시 종료되게 함
	*/
	if (redis_client_init(REDIS_HOST, REDIS_PORT) < 0) {
		fprintf(stderr, "redis_client_init failed\n");
		return -1;
	}

	/* 구독 전용 연결의 fd를 같은 epoll 인스턴스에 등록 - metrics_listen_fd와 동일한 패턴 */
	ev.events = EPOLLIN;
	ev.data.fd = redis_client_sub_fd();
	epoll_ctl(epfd, EPOLL_CTL_ADD, redis_client_sub_fd(), &ev);

	/* 채팅용/메트릭용 리스닝 소켓이 모두 bind에 성공했으므로 이 시점부터 준비 완료로 표시 */
	g_net_ready = true;

	/* 서버 초기화 완료 */
	log_json("INFO", "server_started", "port", LOG_ARG_INT, PORTNUM, "metrics_port", LOG_ARG_INT, METRICS_PORT, NULL);
	return 0;
}

/*
* g_io_q에 쌓인 작업(SEND/CLOSE/REDIS_SUBSCRIBE/REDIS_UNSUBSCRIBE)을 지금 당장 큐에 있는 만큼만
* 한 번 훑어 처리한다(더 들어올 때까지 기다리지 않고 즉시 반환). net_run()의 메인 루프뿐 아니라,
* net_run()이 반환한 뒤 main.c가 logic worker들의 종료를 기다리는 동안에도 호출된다 -
* g_io_q의 유일한 소비자가 net_run()이라 net_run()이 먼저 끝나버리면, 아직 종료 처리 중인 worker가
* JOB_CLOSE/JOB_SEND를 blocking push로 넣다가(job_queue_push_close/_send) 큐가 가득 찼을 때
* 영원히 멈추고 pthread_join도 끝나지 않을 수 있다(main.c의 종료 시퀀스 참고) -
* 그래서 net_run() 종료 후에도 이 함수를 계속 호출해 소비자 역할을 이어가게 한다
*/
void net_drain_io_queue(void)
{
	job_t job;
	while (job_queue_pop(&g_io_q, &job, JOBQ_NONBLOCK)) {
		if (job.type == JOB_SEND) {
			handle_send_job(&job);
		}
		else if (job.type == JOB_CLOSE) {
			close_connection(job.fd);
		}
		else if (job.type == JOB_REDIS_SUBSCRIBE) {
			/* 실패를 무시하면 이 pod만 그 방의 cross-pod 메시지를 못 듣게 되는데 아무 흔적도
			* 남지 않는다. 재시도는 하지 않고(프로젝트 정책) 로그만 남긴다 */
			if (redis_subscribe_room(job.room_id) != 0) {
				log_json("ERROR", "redis_subscribe_failed", "room_id", LOG_ARG_INT, job.room_id, NULL);
			}
		}
		else if (job.type == JOB_REDIS_UNSUBSCRIBE) {
			if (redis_unsubscribe_room(job.room_id) != 0) {
				log_json("ERROR", "redis_unsubscribe_failed", "room_id", LOG_ARG_INT, job.room_id, NULL);
			}
		}
	}
}

/*
* 네트워크 스레드 메인 루프
* - epoll 기반 이벤트 감시
* - accept / recv / send 처리
* - IO 큐에서 전달된 SEND 작업 처리
* - 연결 종료 및 리소스 정리
*/
void net_run() {
	struct epoll_event events[MAX_EVENTS];

	/* 종료 신호(g_terminate)가 오기 전까지 무한 루프 */
	while (!g_terminate) {

		/* 이벤트(timeout = -1)가 올 때까지 epoll 대기 */
		int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (n < 0) {
			/* 시그널로 깨어난 경우 재시도*/
			if (errno == EINTR)
				continue;
			perror("epoll wait error");
			break;
		}

		/*
		* wake_fd 이벤트 선처리
		* IO 큐에 작업이 들어왔음을 알리는 용도
		* 카운터를 비워 epoll 이벤트를 소거
		*/
		for (int i = 0; i < n; ++i) {
			if (events[i].data.fd == wake_fd && (events[i].events & EPOLLIN)) {
				uint64_t v;
				/* eventfd는 누적 카운터이므로 모두 드레인 */
				while (read(wake_fd, &v, sizeof(v)) > 0) {}
				break;
			}
		}

		/*
		* IO 큐에 쌓인 작업 처리 (SEND: 실제 전송, CLOSE: logic 스레드가 정리를 끝낸 fd를 이제 close)
		* 네트워크 전송/종료는 항상 네트워크 스레드에서 수행
		*/
		net_drain_io_queue();

		/* epoll로 전달된 각 이벤트 처리 */
		for (int i = 0; i < n; ++i) {
			int fd = events[i].data.fd;
			uint32_t ev = events[i].events;

			/* wake_fd는 이미 위에서 처리해으므로 건너뜀 */
			if (fd == wake_fd) {
				/* 위에서 이미 드레인 했더라도, 혹시 남았으면 한 번 더 비움 */
				if (ev & EPOLLIN) {
					uint64_t v;
					while (read(wake_fd, &v, sizeof(v)) > 0) {}
				}
				continue;
			}

			/* Redis 구독 연결에 pub/sub 메시지가 도착 (3단계) - 완전한 메시지를 전부 소진할 때까지 반복 */
			if (fd == redis_client_sub_fd()) {
				int room_id, except_id;
				packet_t pkt;
				int rc;

				/*
				* 종료 조건은 "0(더 읽을 것 없음)" 또는 "음수(오류)"뿐이다.
				* 2는 "확인 응답 같은 비-메시지 응답을 하나 소비했다"는 뜻이라 계속 읽어야 한다 -
				* 여기서 멈추면 같은 소켓 읽기에 뒤따라온 진짜 채팅 메시지가 hiredis reader 버퍼에
				* 남고, OS 소켓은 이미 비어 level-trigger epoll도 다시 안 깨워주므로 무관한 다음
				* publish가 올 때까지 그 메시지가 무한정 지연된다 (redis_client.h의 반환값 계약 참고)
				*/
				while ((rc = redis_sub_read(&room_id, &except_id, &pkt)) > 0) {
					if (rc == 1) {
						/* g_logic_q가 가득 차면(non-blocking push) 이 메시지 하나만 드롭된다 -
						* 이 스레드(net 스레드)를 블록시키지 않는 게 더 중요함. job_queue.h 참고 */
						if (job_queue_push_room_deliver(&g_logic_q, room_id, except_id, &pkt) != 0) {
							log_json("ERROR", "room_deliver_job_dropped", "room_id", LOG_ARG_INT, room_id, NULL);
						}
					}
				}

				/*
				* 오류면 구독 연결이 끊어진 것이다(Redis 재시작/네트워크 단절 등).
				* fd는 계속 epoll에서 readable로 남아있어 그냥 continue하면 100% CPU를 태우며
				* 같은 에러 로그만 무한히 쏟아내고, 그동안 cross-pod 배송은 완전히 죽어있다.
				* 이 프로젝트는 Redis 연결 실패에 대해 재연결/서킷브레이커를 두지 않고 fail-fast
				* (net_init() 실패와 동일)로 처리하므로, 여기서도 fd를 감시에서 빼고 not-ready로
				* 내린 뒤 기존 graceful shutdown 경로를 그대로 타서 프로세스를 끝낸다
				* -> k8s가 pod을 재시작하면서 두 연결 모두 새로 맺는다
				*/
				if (rc < 0) {
					log_json("ERROR", "redis_sub_read_error", "action", LOG_ARG_STR, "terminate", NULL);
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					g_net_ready = false;   /* readiness probe가 즉시 실패해 트래픽이 끊김 */
					g_redis_fatal = 1;     /* main이 종료 코드 1로 빠져나가게 함 */
					g_terminate = 1;       /* 아래 epoll 루프 조건에서 정상 종료 절차 시작 */
				}
				continue;
			}

			/* 에러와 끊김 처리 */
			if (ev & (EPOLLERR | EPOLLHUP)) {
				net_disconnect(fd);
				continue;
			}

			/* listen fd 처리(새로운 클라이언트 연결 수락) */
			if (fd == listen_fd) {
				struct sockaddr_in client_addr;
				socklen_t clilen = sizeof(client_addr);

				int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &clilen);

				/* nonblocking accept 특성상 더 이상 없으면 무시 */
				if (client_fd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						continue;
					perror("accept error");
					break;
				}

				/* fd 범위 초과 방어 */
				if (client_fd >= MAX_CLIENTS) {
					log_json("WARN", "fd_limit_exceeded", "fd", LOG_ARG_INT, client_fd, NULL);
					close(client_fd);
					continue;
				}

				/* 새 클라이언트 소켓을 논블로킹으로 설정 */
				set_nonblocking(client_fd);

				/* connection 구조체 생성 및 초기화 */
				connection_t* conn = malloc(sizeof(connection_t));
				if (!conn) {
					close(client_fd);
					continue;
				}

				/*
				* 세션을 accept 즉시 생성함(기존에는 첫 패킷이 올 때까지 지연 생성했음)
				* disconnect가 패킷 한 번 못 받고 발생해도 job에 실을 session_id가 이미 있어야 하기 때문
				* fd<->session_id 매핑은 이 connection_t 안에서만 관리하는, net 스레드만의 관심사임
				*/
				session_t* s = session_create();
				if (!s) {
					free(conn);
					close(client_fd);
					continue;
				}

				conn->fd = client_fd;
				conn->session_id = s->session_id;
				conn->closing = false;
				conn->recv_len = 0;
				conn->send_len = 0;
				conn->send_offset = 0;
				memset(conn->recv_buf, 0, RECV_BUF_SIZE);

				/* fd -> connection 매핑 */
				connections[client_fd] = conn;

				log_json("INFO", "accept",
					"fd", LOG_ARG_INT, client_fd,
					"session_id", LOG_ARG_INT, s->session_id,
					"ip", LOG_ARG_STR, inet_ntoa(client_addr.sin_addr),
					"port", LOG_ARG_INT, ntohs(client_addr.sin_port),
					NULL);

				/* 새 클라이언트 fd를 epoll에 등록 */
				struct epoll_event cev;
				cev.events = EPOLLIN;
				cev.data.fd = client_fd;
				epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);
			}

			/* EPOLLIN(수신 이벤트) 처리 */
			if (events[i].events & EPOLLIN) {
				int cfd = events[i].data.fd;
				connection_t* conn = connections[cfd];
				if (!conn)
					continue;

				bool connection_closed = false;

				/* nonblocking recv 루프 : 읽을 수 있는 데이터가 없을 때까지 반복 */
				while (1) {
					ssize_t n = recv(cfd, conn->recv_buf + conn->recv_len, RECV_BUF_SIZE - conn->recv_len, 0);

					if (n > 0) {
						conn->recv_len += n;
						packet_t pkt;

						/*
						* 수신 버퍼에서 패킷 단위로 파싱
						* 하나의 recv로 여러 패킷이 들어올 수 있음
						*/
						while (1) {
							int r = protocol_parse(conn, &pkt);

							/* 아직 패킷이 완성되지 않음 */
							if (r == 0)
								break;
							if (r < 0) {
								/* 프로토콜 위반 */
								log_json("ERROR", "protocol_violation", "fd", LOG_ARG_INT, cfd, NULL);
								net_disconnect(cfd);
								connection_closed = true;
								break;
							}
							if (connection_closed)
								break;

							/*
							* 파싱된 패킷을 로직 스레드로 전달 (fd가 아니라 session_id로 대상을 지정)
							* CHAT처럼 "잃어도 그만"인 패킷은 non-blocking push로 넘긴다 - g_logic_q가
							* 가득 차면 이 패킷 하나만 드롭된다. 네트워크 자체가 혼잡할 때 패킷을 잃는
							* 것과 같은 급의 트레이드오프이고, 이 스레드(유일한 epoll 스레드)를 블록시켜
							* 다른 모든 클라이언트의 accept/recv/send까지 같이 멈추는 것보다는 낫다.
							* 반면 JOIN/LEAVE처럼 상태를 바꾸는 저빈도 요청은 blocking push로 넘긴다 -
							* 이 프로토콜엔 ACK/재시도가 없어서, 드롭되면 클라이언트가 응답 없이 영원히
							* 기다리게 된다. job_queue.h 참고
							*/
							if (pkt.type == PKT_JOIN_ROOM || pkt.type == PKT_LEAVE_ROOM) {
								job_queue_push_packet_blocking(&g_logic_q, conn->session_id, &pkt);
							}
							else if (job_queue_push_packet(&g_logic_q, conn->session_id, &pkt) != 0) {
								log_json("ERROR", "packet_job_dropped", "fd", LOG_ARG_INT, cfd, "session_id", LOG_ARG_INT, conn->session_id, NULL);
							}

							log_json("INFO", "packet_received",
								"fd", LOG_ARG_INT, cfd,
								"type", LOG_ARG_INT, pkt.type,
								"len", LOG_ARG_INT, pkt.length,
								NULL);
						}
					}
					else if (n == 0) {
						/* 정상 종료 */
						net_disconnect(cfd);
						break;
					}
					else {
						/* 더 이상 읽을 데이터 없음 */
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							break;
						}
						else {
							net_disconnect(cfd);
							break;
						}
					}
				}

			}

			/* 
			* EPOLLOUT(송신 이벤트) 처리
			* send 버퍼에 쌓인 데이터를 실제 소켓으로 전송
			*/
			if (events[i].events & EPOLLOUT) {
				connection_t* conn = connections[fd];
				if (!conn) continue;

				while (conn->send_offset < conn->send_len) {
					ssize_t n = send(fd, conn->send_buf + conn->send_offset, conn->send_len - conn->send_offset, 0);

					if (n > 0) {
						conn->send_offset += n;
					}
					else if (n < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							break;
						net_disconnect(fd);
						break;
					}
				}

				/* 모든 데이터를 전송한 경우 EPOLLOUT 비활성화 */
				if (conn->send_offset == conn->send_len) {
					conn->send_offset = 0;
					conn->send_len = 0;

					/* EPOLLOUT 제거 */
					struct epoll_event ev;
					ev.events = EPOLLIN;
					ev.data.fd = fd;
					epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
				}
			}

		}
	}

	/*
	* metrics 전담 스레드가 g_terminate를 보고 자기 poll 루프를 빠져나올 때까지 기다린다(최대 1초).
	* 이 fd를 아직 쓰고 있을 수도 있는 스레드보다 먼저 close(metrics_listen_fd)를 하면 안 되므로
	* 반드시 아래 close보다 앞에 와야 한다
	*/
	pthread_join(g_metrics_tid, NULL);

	/* 하트비트 전담 스레드도 같은 방식으로 join한다(4단계 lease/heartbeat 복구) - 이 스레드는
	* 자신이 소유한 fd가 없으므로(state_heartbeat_local_rooms()가 g_redis_cmd_lock 아래 Redis
	* 커넥션만 쓴다) fd close 순서와는 무관하게 아무 때나 join해도 안전하다 */
	pthread_join(g_heartbeat_tid, NULL);

	/* 서버 종료 시 listening 소켓 정리 */
	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}
	/* 메트릭/헬스체크용 리스닝 소켓도 함께 정리 */
	if (metrics_listen_fd >= 0) {
		close(metrics_listen_fd);
		metrics_listen_fd = -1;
	}

	/*
	* redis_client_shutdown()은 여기서 부르지 않는다.
	* net_run()이 반환한 시점에는 logic worker들이 아직 JOB_SHUTDOWN을 받지도 못한 상태이고,
	* 그들의 종료 경로(handle_shutdown -> session_remove_all -> room_leave -> redis_leave_room)가
	* g_redis_cmd를 계속 쓴다. 여기서 커넥션을 free하면 SIGTERM 시점에 방에 남아있던 유저가
	* 한 명이라도 있을 때 워커가 이미 해제된 커넥션을 참조한다(use-after-free).
	* 그래서 정리는 main.c가 모든 워커를 pthread_join으로 회수한 뒤에 수행한다
	*/

	/* 모든 연결 정리 */
	for (int fd = 0; fd < MAX_CLIENTS; fd++) {
		if (connections[fd]) {
			close_connection(fd);
		}
	}

	/* epoll 인스턴스 종료 */
	if (epfd >= 0) {
		close(epfd);
		epfd = -1;
	}
}