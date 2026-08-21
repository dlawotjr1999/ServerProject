#include <sys/eventfd.h>

#include "common.h"
#include "net.h"
#include "protocol.h"
#include "job_queue.h"
#include "state.h"
#include "log.h"
#include "metrics.h"

static int listen_fd = -1;
static int metrics_listen_fd = -1;
static int epfd = -1;
static int wake_fd = -1;

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

/* fd에 대응하는 네트워크 연결을 완전히 종료하는 함수 */
static void close_connection(int fd)
{
	connection_t* conn = connections[fd];
	if (!conn) return;

	/*
	* epoll 감시 대상 제거
	* 소켓 종료 후 connection 구조체 메모리 해제
	* 마지막으로 연결 테이블에서 제거
	*/
	epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
	close(fd);
	free(conn);
	connections[fd] = NULL;

	log_json("INFO", "close", "fd", LOG_ARG_INT, fd, NULL);
}

/*
* 네트워크 연결 종료 처리 함수
* 네트워크 리소스 정리 후, 논리적 상태 정리는 워커 스레드에 위임
*/
static void net_disconnect(int fd)
{
	if (fd < 0 || fd >= MAX_CLIENTS) return;

	/* 네트워크 리소스 정리 */
	if (connections[fd]) close_connection(fd);

	/* 세션 / 룸 상태 정리는 로직 스레드에서 처리하도록 DISCONNECT 작업을 큐에 전달 */
	job_queue_push_disconnect(&g_logic_q, fd);
	log_json("INFO", "disconnect_queued", "fd", LOG_ARG_INT, fd, NULL);

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

/* IO 큐에서 전달된 SEND 작업을 처리하는 함수 */
static void handle_send_job(job_t* job)
{
	int fd = job->fd;
	connection_t* conn = connections[fd];

	/* 이미 끊긴 연결의 경우 조용히 무시 */
	if (!conn)
		return;

	/* 패킷 적재 실패 시 연결 종료 */
	if (packet_send(fd, &job->packet) < 0) {
		net_disconnect(fd);
	}
}

/*
* 메트릭/헬스체크 스크레이퍼 연결을 처리하는 함수
* 채팅 클라이언트와 달리 요청/응답이 짧고 일회성이므로, epoll에 등록하지 않고
* accept 즉시 이 자리에서 동기적으로 처리 후 close (net thread를 오래 막지 않도록 타임아웃을 둠)
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
		/* readiness: 지금은 고정 OK. 1-4에서 net_init의 실제 bind 성공 여부를 반영하도록 교체 예정 */
		body_len = snprintf(body, sizeof(body), "ok\n");
		status_line = "200 OK";
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

	/* 위에서 쓴 ev 변수를 재사용해 metrics_listen_fd도 같은 epoll 인스턴스(epfd)에 등록 */
	ev.events = EPOLLIN;
	ev.data.fd = metrics_listen_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, metrics_listen_fd, &ev);

	/* 서버 초기화 완료 */
	log_json("INFO", "server_started", "port", LOG_ARG_INT, PORTNUM, "metrics_port", LOG_ARG_INT, METRICS_PORT, NULL);
	return 0;
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
		* IO 큐에 쌓인 SEND 작업 처리
		* 네트워크 전송은 항상 네트워크 스레드에서 수행
		*/
		job_t job;
		while (job_queue_pop(&g_io_q, &job, JOBQ_NONBLOCK)) {
			if (job.type == JOB_SEND) {
				handle_send_job(&job);
			}
		}

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

			/* 메트릭/헬스체크 스크레이퍼 연결 처리 (채팅 연결과 다른 경로이므로 이후 로직으로 흘러가지 않도록 continue) */
			if (fd == metrics_listen_fd) {
				handle_metrics_accept();
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

				conn->fd = client_fd;
				conn->recv_len = 0;
				conn->send_len = 0;
				conn->send_offset = 0;
				memset(conn->recv_buf, 0, RECV_BUF_SIZE);

				/* fd -> connection 매핑 */
				connections[client_fd] = conn;

				log_json("INFO", "accept",
					"fd", LOG_ARG_INT, client_fd,
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

							job.fd = cfd;
							job.packet = pkt;

							/* 파싱된 패킷을 로직 스레드로 전달 */
							job_queue_push_packet(&g_logic_q, cfd, &pkt);

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