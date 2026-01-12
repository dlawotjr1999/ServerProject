#include "common.h"
#include "net.h"
#include "protocol.h"
#include "job_queue.h"
#include "state.h"

static int listen_fd = -1;
static int epfd = -1;

static connection_t* connections[MAX_CLIENTS];

extern job_queue_t g_job_queue;

static void close_connection(int fd)
{
	connection_t* conn = connections[fd];
	if (!conn) return;

	session_t* s = session_get_or_create(fd);
	if (s && s->room_id >= 0)
		room_leave(s);

	epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
	session_remove(fd);
	close(fd);
	free(conn);
	connections[fd] = NULL;

	printf("[INFO] Connection closed fd=%d\n", fd);
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

	int total_len = pkt->length + sizeof(uint16_t);

	if (conn->send_len + total_len > SEND_BUF_SIZE)
		return -1;

	uint16_t net_len = htons(pkt->length);
	memcpy(conn->send_buf + conn->send_len, &net_len, 2);
	memcpy(conn->send_buf + conn->send_len + 2, &pkt->type, pkt->length);

	conn->send_len += total_len;

	// EPOLLOUT 활성화
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLOUT;
	ev.data.fd = fd;
	epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);

	return 0;
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

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;

	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

	printf("Server is operating on port %d\n", PORTNUM);
	return 0;
}

void net_run() {
	struct epoll_event events[MAX_EVENTS];

	while (1) {
		int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (n < 0) {
			perror("epoll wait error");
			continue;
		}

		for (int i = 0; i < n; ++i) {
			int fd = events[i].data.fd;
			uint32_t ev = events[i].events;

			// 에러와 끊김 처리
			if (ev & (EPOLLERR | EPOLLHUP)) {
				job_queue_push_disconnect(&g_job_queue, fd);
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
					ssize_t n = recv(fd, conn->recv_buf + conn->recv_len, RECV_BUF_SIZE - conn->recv_len, 0);

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
								close_connection(cfd);
								connection_closed = true;
								break;
							}
							if (connection_closed)
								break;

							job_t job;
							job.fd = cfd;
							job.packet = pkt;

							job_queue_push(&g_job_queue, &job);

							printf("[PACKET] fd=%d type=%d len=%d\n", cfd, pkt.type, pkt.length);
						}
					}
					else if (n == 0) {
						// 정상 종료
						job_queue_push_disconnect(&g_job_queue, fd);
						break;
					}
					else {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							break;
						}
						else {
							job_queue_push_disconnect(&g_job_queue, fd);
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
						job_queue_push_disconnect(&g_job_queue, fd);
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
}
