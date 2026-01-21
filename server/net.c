#include <sys/eventfd.h>

#include "common.h"
#include "net.h"
#include "protocol.h"
#include "job_queue.h"
#include "state.h"

static int listen_fd = -1;
static int epfd = -1;
static int wake_fd = -1;

static connection_t* connections[MAX_CLIENTS];

extern job_queue_t g_io_q;
extern job_queue_t g_logic_q;

void net_wakeup(void) {
	if (wake_fd < 0) return;

	uint64_t one = 1;

	for (;;) {
		ssize_t rc = write(wake_fd, &one, sizeof(one));
		if (rc == (ssize_t)sizeof(one)) {
			return;                 // 정상
		}
		if (rc < 0) {
			if (errno == EINTR) continue;  // 시그널로 끊김 -> 재시도
			if (errno == EAGAIN) return;   // 논블록 + 카운터 포화 -> 깨우기 실패해도 치명적 아님
			return;                         // 필요하면 perror/log
		}
		return; // (이론상) 부분 write 방어
	}
}

static void close_connection(int fd)
{
	connection_t* conn = connections[fd];
	if (!conn) return;

	epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
	close(fd);
	free(conn);
	connections[fd] = NULL;

	printf("[INFO] Connection closed fd=%d\n", fd);
}

static void net_disconnect(int fd)
{
	if (fd < 0 || fd >= MAX_CLIENTS) return;

	// 네트워크 리소스 정리
	if (connections[fd]) close_connection(fd);

	// 상태 정리는 워커에게 맡김
	job_queue_push_disconnect(&g_logic_q, fd);
}

static int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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

	int total_len = 2 + pkt->length; // length(2) + (type + payload)

	if (conn->send_len + total_len > SEND_BUF_SIZE)
		return -1;

	/* === 안전한 직렬화 시작 === */
	uint16_t net_len = htons(pkt->length);
	uint16_t net_type = htons(pkt->type);

	/* length */
	memcpy(conn->send_buf + conn->send_len, &net_len, 2);

	/* type */
	memcpy(conn->send_buf + conn->send_len + 2, &net_type, 2);

	/* payload */
	if (payload_len > 0) {
		memcpy(conn->send_buf + conn->send_len + 4,
			pkt->payload,
			payload_len);
	}

	conn->send_len += total_len;

	// EPOLLOUT 활성화
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLOUT;
	ev.data.fd = fd;
	epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);

	return 0;
}

static void handle_send_job(job_t* job)
{
	int fd = job->fd;
	connection_t* conn = connections[fd];

	// 이미 끊긴 경우 → 조용히 무시
	if (!conn)
		return;

	if (packet_send(fd, &job->packet) < 0) {
		net_disconnect(fd);
	}
}


int net_init() {
	struct sockaddr_in addr;
	int opt = 1;

	if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket error");
		return 1;
	}

	if ((setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
		perror("setsockopt error");
		return 1;
	}

	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(PORTNUM);

	if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind error");
		return 1;
	}

	if (listen(listen_fd, 256) < 0) {
		perror("listen error");
		return 1;
	}

	set_nonblocking(listen_fd);

	epfd = epoll_create1(0);
	if (epfd < 0) {
		perror("epoll error");
		return -1;
	}

	wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (wake_fd < 0) {
		perror("eventfd error");
		return -1;
	}

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

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;

	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

	printf("Server is operating on port %d\n", PORTNUM);
	return 0;
}

void net_run() {
	struct epoll_event events[MAX_EVENTS];

	while (!g_terminate) {
		int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll wait error");
			break;
		}

		for (int i = 0; i < n; ++i) {
			if (events[i].data.fd == wake_fd && (events[i].events & EPOLLIN)) {
				uint64_t v;
				while (read(wake_fd, &v, sizeof(v)) > 0) {}
				break;
			}
		}

		job_t job;
		while (job_queue_pop(&g_io_q, &job, JOBQ_NONBLOCK)) {
			if (job.type == JOB_SEND) {
				handle_send_job(&job);
			}
		}

		for (int i = 0; i < n; ++i) {
			int fd = events[i].data.fd;
			uint32_t ev = events[i].events;

			if (fd == wake_fd) {
				// 위에서 이미 드레인 했더라도, 혹시 남았으면 한 번 더 비움
				if (ev & EPOLLIN) {
					uint64_t v;
					while (read(wake_fd, &v, sizeof(v)) > 0) {}
				}
				continue;
			}

			// 에러와 끊김 처리
			if (ev & (EPOLLERR | EPOLLHUP)) {
				net_disconnect(fd);
				continue;
			}

			// listen fd 처리
			if (fd == listen_fd) {
				struct sockaddr_in client_addr;
				socklen_t clilen = sizeof(client_addr);

				int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &clilen);

				if (client_fd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						continue;
					perror("accept error");
					break;
				}

				if (client_fd >= MAX_CLIENTS) {
					printf("warning: fd=%d exceeds MAX_CLIENTS\n", client_fd);
					close(client_fd);
					continue;
				}

				set_nonblocking(client_fd);

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

				connections[client_fd] = conn;

				printf("Client info : %s:%d (fd=%d)\n", inet_ntoa(client_addr.sin_addr),
					ntohs(client_addr.sin_port), client_fd);

				struct epoll_event cev;
				cev.events = EPOLLIN;
				cev.data.fd = client_fd;
				epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);
			}

			// EPOLLIN 처리
			if (events[i].events & EPOLLIN) {
				int cfd = events[i].data.fd;
				connection_t* conn = connections[cfd];
				if (!conn)
					continue;

				bool connection_closed = false;

				while (1) {
					ssize_t n = recv(cfd, conn->recv_buf + conn->recv_len, RECV_BUF_SIZE - conn->recv_len, 0);

					if (n > 0) {
						conn->recv_len += n;
						packet_t pkt;

						while (1) {
							int r = protocol_parse(conn, &pkt);

							if (r == 0)
								break;
							if (r < 0) {
								/* protocol error */
								printf("[ERROR] protocol violation fd=%d\n", cfd);
								net_disconnect(cfd);
								connection_closed = true;
								break;
							}
							if (connection_closed)
								break;

							job.fd = cfd;
							job.packet = pkt;
							job_queue_push_packet(&g_logic_q, cfd, &pkt);

							printf("[PACKET] fd=%d type=%d len=%d\n", cfd, pkt.type, pkt.length);
						}
					}
					else if (n == 0) {
						// 정상 종료
						net_disconnect(cfd);
						break;
					}
					else {
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

			// EPOLLOUT 처리
			if (events[i].events & EPOLLOUT) {
				connection_t* conn = connections[fd];
				if (!conn) continue;

				while (conn->send_offset < conn->send_len) {
					ssize_t n = send(
						fd,
						conn->send_buf + conn->send_offset,
						conn->send_len - conn->send_offset,
						0
					);

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

				/* 다 보냈으면 */
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

	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}

	for (int fd = 0; fd < MAX_CLIENTS; fd++) {
		if (connections[fd]) {
			close_connection(fd);
		}
	}

	if (epfd >= 0) {
		close(epfd);
		epfd = -1;
	}
}