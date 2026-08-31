#ifndef STATE_H
#define STATE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// 세션 정보 구조체
// fd는 더 이상 세션에 저장하지 않음 -> fd<->session_id 매핑은 net.c(net 스레드)만의 관심사
// refcount 필드는 없음 -> state.cpp 내부에서 std::shared_ptr가 대신 관리함(REDESIGN.md 참고)
typedef struct session {
	int session_id;
	int global_id;   /* 클러스터 전역 유일 id (3단계 수정) - PKT_JOIN_ROOM에서 발급받음, 로컬 session_id
	                  * 는 pod마다 독립적으로 증가해 클러스터 전역에서 유일하지 않으므로 cross-pod pub/sub
	                  * 자기 자신 제외 판정에는 이 필드를 쓴다. 로컬 addressing(JOB_SEND 등)은 여전히
	                  * session_id를 그대로 씀 - 이 필드는 그 용도를 대체하지 않는다 */
	int room_id;
	bool alive;
	pthread_mutex_t lock;    // alive / room_id 접근을 보호

	char send_buf[SEND_BUF_SIZE];
	size_t size_len;
	size_t size_offset;
} session_t;

// 방 정보 구조체
typedef struct room {
	int room_id;
	session_t* users[MAX_ROOM_USER];
	int user_count;
	bool live;              // false면 이 슬롯은 회수되어 재사용 대기 중(더 이상 유효한 방이 아님)
	pthread_mutex_t lock;
} room_t;

/*
* session API (session_id 기반, shared_ptr로 수명 관리)
* session_get_by_id()로 얻은 포인터는 다 쓴 뒤 반드시 session_release()로 반환해야 함
* (C 콜사이트는 shared_ptr을 직접 못 들고 있으므로, 이 pair가 내부적으로 shared_ptr 사본의
* 생성/소멸을 대신 수행함 -> state.cpp 참고)
*/
session_t* session_create(void);
session_t* session_get_by_id(int session_id);
void session_acquire(session_t* s);
void session_release(session_t* s);
void session_remove_by_id(int session_id);
bool session_is_alive(session_t* s);
int session_get_room_id(session_t* s);  /* room_id를 락 보호 하에 안전하게 읽음 */
int session_deactivate(session_t* s);   /* alive=false로 내리고, 그 순간의 room_id를 원자적으로 함께 반환 */
void session_remove_all(void);   /* 서버 종료 시 남아있는 모든 세션을 방에서 빼고 정리 */

/* room API */
void state_init(void);   /* 서버 시작 시 1회 호출 - 방 슬롯의 뮤텍스를 미리 전부 초기화 (3단계) */
room_t* room_get(int room_id);
room_t* room_get_or_init(int room_id);   /* Redis가 배정한 room_id에 대해 로컬 슬롯을 준비 (3단계) */

/*
* 방에 입장시킨다. 입장 자체의 성공/실패를 반환값으로(성공 0 / 실패 -1), "이 입장으로 로컬 인원이
* 0->1이 됐는지"는 출력 인자로 분리해서 알려준다(redis_join_room의 int 반환 + out 인자 패턴과 동일).
* 예전처럼 bool 하나로 first_local_member만 돌려주면 "입장은 했지만 첫 멤버가 아님"과 "아예 입장을
* 못 함(세션이 이미 죽었거나 방이 꽉 참)"이 둘 다 false로 뭉개져, 호출부가 실패를 감지할 수 없었다.
* out_first_local_member는 NULL이어도 되고, 실패 시에는 항상 false로 채워진다
*/
int room_join(room_t* room, session_t* s, bool* out_first_local_member);
void room_leave(session_t* s);
void room_broadcast_local(int room_id, int except_global_id, packet_t* pkt);   /* 3단계: id 기반, 로컬 전달만 담당 */

/* metrics API */
int state_count_active_sessions(void);
int state_count_active_rooms(void);

#ifdef __cplusplus
}
#endif

#endif
