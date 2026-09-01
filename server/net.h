#ifndef NET_H
#define NET_H

#include "common.h"

void net_wakeup(void);
int packet_send(int fd, packet_t* pkt);

int net_init();
void net_run();

/* g_io_q에 지금 쌓여있는 작업만 한 번 훑어 처리(더 들어올 때까지 기다리지 않음).
 * main.c가 net_run() 반환 후 logic worker 종료를 기다리는 동안 계속 호출해 g_io_q 소비자
 * 역할을 이어가기 위해 노출됨 - net.c 주석 참고 */
void net_drain_io_queue(void);

#endif
