#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
* Redis 서버에 연결한다(명령용 연결 1개 + 구독용 연결 1개), 매치메이킹/퇴장 Lua 스크립트를 로드한다.
* net_init()과 동일한 원칙 - 실패하면 -1을 반환하고, 호출부는 서버를 즉시 종료시킨다
*/
int redis_client_init(const char* host, int port);

/* 서버 종료 시 두 연결을 정리한다 */
void redis_client_shutdown(void);

/* epoll에 등록할 구독 전용 연결의 fd. redis_client_init() 성공 후에만 유효하다 */
int redis_client_sub_fd(void);

/*
* 빈 방을 찾거나 새로 만든다(클러스터 전역, 원자적).
* 성공하면 0을 반환하고 *out_room_id에 배정된 방 번호를 채운다.
* 방 정원(MAX_ROOMS) 소진 등으로 실패하면 -1을 반환한다
*/
int redis_join_room(int session_id, int* out_room_id);

/*
* room_id의 인원 카운트를 하나 줄인다. 0이 되면 Redis 쪽에서 자동으로 회수(freelist 반환)된다.
* 실패하면 -1을 반환한다
*/
int redis_leave_room(int session_id, int room_id);

/*
* room:<room_id> 채널로 패킷을 발행한다. except_session_id는 원 발신자(로컬 에코 제외용)다.
* 발신자 자신의 pod도 자신의 SUBSCRIBE를 통해 이 메시지를 받는다(로컬/원격 배송을 분리하지 않음).
* 실패하면 -1을 반환한다
*/
int redis_publish_chat(int room_id, int except_session_id, const packet_t* pkt);

/*
* room:<room_id> 채널 구독을 시작/중단한다. g_redis_sub는 net 스레드만 접근해야 하므로
* 이 두 함수도 net 스레드에서만 호출해야 한다(스레드 안전하지 않음)
*/
int redis_subscribe_room(int room_id);
int redis_unsubscribe_room(int room_id);

/*
* 구독 fd가 readable할 때 net 스레드가 반복 호출한다.
* 채팅 메시지를 하나 파싱했으면 1을 반환하고 out_room_id, out_except_id, *out_pkt를 채운다.
* 더 읽을 완전한 메시지가 없으면 0, 오류면 -1을 반환한다(SUBSCRIBE/UNSUBSCRIBE 확인 응답 등
* "message"가 아닌 pub/sub 응답은 조용히 소비하고 0을 반환한다)
*/
int redis_sub_read(int* out_room_id, int* out_except_id, packet_t* out_pkt);

/*
* 클러스터 전역에서 유일한 정수 id를 하나 발급받는다(Redis INCR 기반).
* 방에 입장하는 세션마다 하나씩 발급받아, cross-pod pub/sub 자기 자신 제외 판정에 쓴다
* (로컬 session_id는 pod마다 독립적으로 1부터 증가하므로 클러스터 전역에서 유일하지 않음 - 이 함수가
* 그 문제를 해결한다). 성공하면 0을 반환하고 *out_global_id를 채운다. 실패하면 -1을 반환한다
*/
int redis_next_global_id(int* out_global_id);

#ifdef __cplusplus
}
#endif

#endif
