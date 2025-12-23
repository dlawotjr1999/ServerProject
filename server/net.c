#include "common.h"
#include "net.h"
#include "protocol.h"

static int listen_fd = -1;
static int epfd = -1;

static connection_t* connections[MAX_CLIENTS];

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

static int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if(flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int net_init() {
	struct sockaddr_in addr;
	int opt = 1;

	if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket error");
		return 1;
	}

	if((setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
		perror("setsockopt error");
		return 1;
	}

	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(PORTNUM);

	if(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind error");
		return 1;
	}

	if(listen(listen_fd, 256) < 0) {
		perror("listen error");
		return 1;
	}

	set_nonblocking(listen_fd);

	epfd = epoll_create1(0);
	if(epfd < 0) {
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

			// 俊矾客 谗辫 贸府
			if (ev & (EPOLLERR | EPOLLHUP)) {
				close_connection(fd);
				continue;
			}

			// listen fd 贸府
			if (fd == listen_fd) {
				struct sockaddr_in client_addr;
				socklen_t clilen = sizeof(client_addr);

				int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &clilen);

				if (client_fd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
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

			// client fd 贸府
			else if (events[i].events & EPOLLIN) {
				int cfd = events[i].data.fd;
				connection_t* conn = connections[cfd];
				if (!conn) 
					continue;

				while (1) {
					ssize_t n = recv(cfd, conn->recv_buf + conn->recv_len, RECV_BUF_SIZE - conn->recv_len, 0);

					if (n > 0) {
						conn->recv_len += n;
						packet_t pkt;

						while (1) {
							int r = protocol_parse(conn, &pkt);
							if (r == 1) {
								printf("[PACKET] fd=%d type=%d len=%d\n",
									cfd, pkt.type, pkt.length);
							}
							else if (r == 0) {
								break;
							}
							else {
								/* protocol error */
								printf("[ERROR] protocol violation fd=%d\n", cfd);
								close_connection(cfd);
								break;
							}
						}
					}
					else if (n == 0) {
						close_connection(cfd);
						break;
					}
					else {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							break;
						perror("recv error");
						close_connection(cfd);
						break;
					}
				}
			}
		}
	}
}
