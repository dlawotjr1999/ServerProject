#include <sys/eventfd.h>

#include "common.h"
#include "net.h"
#include "protocol.h"
#include "job_queue.h"
#include "state.h"

static int listen_fd = -1;
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

	printf("[INFO] Connection closed fd=%d\n", fd);
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

	/* 서버 초기화 완료 */
	printf("Server is operating on port %d\n", PORTNUM);
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
					printf("warning: fd=%d exceeds MAX_CLIENTS\n", client_fd);
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

				printf("Client info : %s:%d (fd=%d)\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), client_fd);

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
								printf("[ERROR] protocol violation fd=%d\n", cfd);
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

							printf("[PACKET] fd=%d type=%d len=%d\n", cfd, pkt.type, pkt.length);
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