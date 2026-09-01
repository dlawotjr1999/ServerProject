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

/*
* 서버 종료 시 두 연결을 정리한다.
* 반드시 모든 logic worker를 pthread_join으로 회수한 "뒤에" 호출해야 한다 - worker의 종료 경로
* (handle_shutdown -> session_remove_all -> room_leave -> redis_leave_room)가 g_redis_cmd를
* 계속 쓰기 때문에, 그 전에 부르면 이미 free된 커넥션을 참조하게 된다(main.c의 종료 순서 참고)
*/
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
* room:<room_id> 채널로 패킷을 발행한다. except_global_id는 원 발신자의 클러스터 전역 id
* (redis_next_global_id로 발급받은 값)로, 로컬 에코 제외 판정에 쓴다 - pod마다 독립적으로 증가하는
* session_id를 쓰면 다른 pod의 동일 session_id를 자기 자신으로 오인해 메시지를 걸러버린다.
* 발신자 자신의 pod도 자신의 SUBSCRIBE를 통해 이 메시지를 받는다(로컬/원격 배송을 분리하지 않음).
* 실패하면 -1을 반환한다
*/
int redis_publish_chat(int room_id, int except_global_id, const packet_t* pkt);

/*
* room:<room_id> 채널 구독을 시작/중단한다. g_redis_sub는 net 스레드만 접근해야 하므로
* 이 두 함수도 net 스레드에서만 호출해야 한다(스레드 안전하지 않음)
*/
int redis_subscribe_room(int room_id);
int redis_unsubscribe_room(int room_id);

/*
* 구독 fd가 readable할 때 net 스레드가 반복 호출한다. 반환값 계약:
*   1  : 채팅 메시지를 하나 파싱했다. out_room_id / out_except_id / *out_pkt가 채워진다.
*   2  : 응답을 하나 소비했지만 채팅 메시지는 아니었다(SUBSCRIBE/UNSUBSCRIBE 확인 응답, 손상된
*        메시지 등). 같은 소켓 읽기에 뒤이어 진짜 메시지가 버퍼에 남아있을 수 있으므로,
*        호출부는 반드시 한 번 더 호출해야 한다.
*   0  : 더 읽을 완전한 응답이 없다(소켓에서 바이트를 더 받아와야 함) -> 드레인 루프 종료 지점.
*   -1 : 오류. 구독 연결이 끊어졌다는 뜻이므로 호출부는 스핀하지 말고 처리해야 한다(net.c 참고).
* 즉 호출부의 루프 조건은 "> 0인 동안 계속, 1일 때만 배송"이다
*/
int redis_sub_read(int* out_room_id, int* out_except_id, packet_t* out_pkt);

/*
* 클러스터 전역에서 유일한 정수 id를 하나 발급받는다(Redis INCR 기반).
* 방에 입장하는 세션마다 하나씩 발급받아, cross-pod pub/sub 자기 자신 제외 판정에 쓴다
* (로컬 session_id는 pod마다 독립적으로 1부터 증가하므로 클러스터 전역에서 유일하지 않음 - 이 함수가
* 그 문제를 해결한다). 성공하면 0을 반환하고 *out_global_id를 채운다. 실패하면 -1을 반환한다
*/
int redis_next_global_id(int* out_global_id);

/*
* 이 pod이 room_id에 대해 현재 갖고 있는 로컬 멤버 수(local_count)를 Redis에 하트비트로 알린다.
* local_count > 0이면: 이 pod을 room:{id}:pods SET에 등록하고, room:{id}:pod:{pod_id}:count를
* local_count로 갱신하고, room:{id}:pod:{pod_id}:lease를 30초 TTL로 (재)설정한다.
* local_count == 0이면(이 pod에서 이 방의 로컬 멤버가 전부 빠짐): 위 세 키를 즉시 정리한다(SET에서
* 빼고, count/lease 키를 지운다) - TTL 만료를 기다릴 필요 없이 곧바로 반납.
* 두 트리거 지점에서 호출된다: (1) state.cpp의 room_join()/room_leave()가 로컬 인원이 바뀔 때마다
* 즉시, (2) 인원 변화가 없어도 lease가 만료되지 않도록 주기적으로(net.c의 타이머).
* 실패해도 재시도하지 않는다(이 프로젝트의 기존 정책) - 로그만 남기고 그냥 다음 호출을 기다린다.
* 성공하면 0, 실패하면 -1을 반환한다
*/
int redis_room_heartbeat(int room_id, int local_count);

#ifdef __cplusplus
}
#endif

#endif
