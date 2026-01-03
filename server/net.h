#ifndef NET_H
#define NET_H

#include "common.h"

int packet_send(int fd, packet_t* pkt);

int net_init();
void net_run();

#endif
