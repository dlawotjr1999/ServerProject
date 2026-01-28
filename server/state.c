#include "state.h"
#include "job_queue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 세션 관련 데이터 */
static session_t* sessions[MAX_CLIENTS];
static int next_session_id = 1;

/* 방 관련 데이터 */
static room_t rooms[MAX_ROOMS];
static int room_count = 0;

/* 세션, 방 정보에 대한 mutex */
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rooms_lock = PTHREAD_MUTEX_INITIALIZER;

extern job_queue_t g_io_q;
extern void net_wakeup(void);

/* 세션을 생성하는 함수 */
session_t* session_create(int fd)
{
    /* fd가 범위를 벗어나는 경우 NULL 반환 */
    if (fd < 0 || fd >= MAX_CLIENTS)
        return NULL;

    /* 세션 테이블은 공유 자원이므로 접근이 mutex로 보호되어 동기화됨 */
    pthread_mutex_lock(&g_sessions_lock);

    /*
    * 해당 fd에 대해 이미 세션이 존재하는지 확인(중복 세션 생성 방지)
    * 만약 존재한다면 그대로 반환
    */
    session_t* s = sessions[fd];
    if (s) {
        pthread_mutex_unlock(&g_sessions_lock);
        return s;
    }

    /*
    * 아직 세션이 없으므로 새 세션 메모리 할당(락을 잡은 상태에서 수행하여 경쟁 생성 방지)
    * 메모리 할당 실패 시 락 해제 후 실패 반환
    */
    s = malloc(sizeof(session_t));
    if (!s) {
        pthread_mutex_unlock(&g_sessions_lock);
        return NULL;
    }

    /*
    * 세션 정보 할당
    * 메모리 초기화 후 session id, fd, room id, 유효성 할당
    * 이후 세션 테이블에 생성된 세션 정보 저장
    */
    memset(s, 0, sizeof(*s));
    s->session_id = next_session_id++;
    s->fd = fd;
    s->room_id = -1;
    s->alive = true;
    sessions[fd] = s;

    pthread_mutex_unlock(&g_sessions_lock);

    printf("[SESSION] created session id=%d fd=%d\n", s->session_id, fd);
    return s;
}

/* 세션 정보를 가져오는 함수 */
session_t* session_get(int fd)
{
    /* fd가 범위를 벗어나는 경우 NULL 반환 */
    if (fd < 0 || fd >= MAX_CLIENTS)
        return NULL;

    /*
    * 세션 테이블 접근은 mutex로 보호되어 동기화됨
    * 세션 포인터의 일관성을 보장하기 위함
    */
    pthread_mutex_lock(&g_sessions_lock);
    session_t* s = sessions[fd];
    pthread_mutex_unlock(&g_sessions_lock);

    return s;
}

/* 세션을 제거하는 함수 */
void session_remove(int fd)
{
    /* fd가 범위를 벗어나는 경우 NULL 반환 */
    if (fd < 0 || fd >= MAX_CLIENTS)
        return;

    /* 
    * 세션 테이블은 공유 자원이므로 접근이 mutex로 보호되어 동기화됨
    * 세션 획득에 실패하면 그대로 return
    */
    pthread_mutex_lock(&g_sessions_lock);
    session_t* s = sessions[fd];
    if (!s) {
        pthread_mutex_unlock(&g_sessions_lock);
        return;
    }

    /* 
    * 테이블에서 먼저 제거 후 유효 플래그를 false로 설정
    * 세션의 발견 가능성을 먼저 끊어야 하기 때문
    * 즉, 먼저 더 이상 찾을 수 없게 만든 뒤 그 다음 내부 상태를 정리함
    */
    sessions[fd] = NULL;   
    s->alive = false;
    pthread_mutex_unlock(&g_sessions_lock);

    printf("[SESSION] removed sid=%d fd=%d\n", s->session_id, fd);
    free(s);
}

/* ============================ Room ============================ */

/* 방을 생성하는 함수 */
room_t* room_create(void)
{
    /* room 테이블은 공유 자원이므로 접근이 mutex로 보호되어 동기화됨 */
    pthread_mutex_lock(&g_rooms_lock);

    /* 최대 방 수보다 많은 방이 생성될 경우 방 생성 불가 */
    if (room_count >= MAX_ROOMS) {
        pthread_mutex_unlock(&g_rooms_lock);
        return NULL;
    }

    /* 
    * 새로운 방은 rooms 배열의 다음 인덱스에 생성
    * 이후 방 구조체 정보 초기화
    */
    room_t* r = &rooms[room_count];
    memset(r, 0, sizeof(*r));
    r->room_id = room_count;
    r->user_count = 0;
    pthread_mutex_init(&r->lock, NULL);

    /* 방 생성이 완료되었으므로 전역 방 갯수 증가 */
    room_count++;

    pthread_mutex_unlock(&g_rooms_lock);

    printf("[ROOM] created room_id=%d\n", r->room_id);
    return r;
}

/* 방 정보를 가져오는 함수 */
room_t* room_get(int room_id)
{
    /* room_count는 여러 스레드에서 동시에 변경될 수 있으므로 mutex로 보호 */
    pthread_mutex_lock(&g_rooms_lock);
    int max = room_count;
    pthread_mutex_unlock(&g_rooms_lock);

    /* 유효하지 않은 room_id의 경우 NULL 반환 */
    if (room_id < 0 || room_id >= max)
        return NULL;

    return &rooms[room_id];
}

/* 방을 조회하는 함수 */
room_t* room_find(void)
{
    /* 
    * 최대 방 수만큼 반복을 진행
    * 반복문 내에서 존재하는 방을 찾으면 해당 방의 정보를 반환
    */
    pthread_mutex_lock(&g_rooms_lock);
    for (int i = 0; i < room_count; i++) {
        if (rooms[i].user_count < MAX_ROOM_USER) {
            room_t* r = &rooms[i];
            pthread_mutex_unlock(&g_rooms_lock);
            return r;
        }
    }
    pthread_mutex_unlock(&g_rooms_lock);
    return NULL;
}

/* 방에 입장하는 함수 */
void room_join(room_t* room, session_t* s)
{
    if (!room || !s) return;

    pthread_mutex_lock(&room->lock);

    /* 이미 세션에 방에 존재하면 무시(중복 추가 방지) */ 
    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i] == s) {
            pthread_mutex_unlock(&room->lock);
            return;
        }
    }

    /* 방의 유저 수가 방의 최대 인원보다 많은 경우에도 무시 */
    if (room->user_count >= MAX_ROOM_USER) {
        pthread_mutex_unlock(&room->lock);
        return;
    }

    /* 
    * 현재 방에 현재 세션(인원)을 추가 후 인원 수 증가
    * 현재 세션의 room_id를 현재 방의 room_id로 저장
    */
    room->users[room->user_count++] = s;
    s->room_id = room->room_id;

    printf("[ROOM] sid=%d joined room=%d\n", s->session_id, room->room_id);

    pthread_mutex_unlock(&room->lock);
}

/* 방에서 떠나는 함수 */
void room_leave(session_t* s)
{
    if (!s) return;
    if (s->room_id < 0) return;

    /* 세션이 속한 방 조회 */ 
    room_t* room = room_get(s->room_id);
    if (!room) {

        /* 방이 존재하지 않더라도 세션의 방 소속 정보는 초기화 */
        s->room_id = -1;
        return;
    }

    pthread_mutex_lock(&room->lock);

    /* 반복문을 돌며 현재 세션이 존재하는 방을 탐색
    * 제거할 자리를 마지막 사용자로 덮어써 배열 유지
    * 마지막 칸은 더 이상 사용하지 않으므로 NULL로 변경
    * 이후 방의 인원수 감소
    */
    for (int i = 0; i < room->user_count; i++) {
        if (room->users[i] == s) {
            room->users[i] = room->users[room->user_count - 1];
            room->users[room->user_count - 1] = NULL;
            room->user_count--;
            break;
        }
    }

    /* 현재 세션의 방 소속 정보 초기화 */
    printf("[ROOM] sid=%d left room=%d\n", s->session_id, s->room_id);
    s->room_id = -1;

    pthread_mutex_unlock(&room->lock);
}

/* 방에 채팅을 전파하는 함수 */
void room_broadcast(room_t* room, session_t* sender, packet_t* pkt)
{
    if (!room || !pkt) return;

    /*
    * 전송 대상 fd 목록을 임시로 저장
    * room->lock을 잡은 상태에서 직접 send하지 않기 위해 사용
    * count 변수에 브로드캐스팅으로 전송할 세션 수 저장
    */
    int fds[MAX_ROOM_USER];
    int count = 0;

    /* 송신자를 제외하기 위한 fd (없으면 -1) */
    int except_fd = sender ? sender->fd : -1;

    /* 방 내부 사용자 목록 접근은 다른 스레드와의 경쟁을 막기 위해 방 단위 mutex로 보호
    * 세션이 없거나, 종료된 세션이거나, 송신자인 경우에는 무시
    * 전송 대상 fd만 별도 배열에 수집 
    */
    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->user_count; ++i) {
        session_t* s = room->users[i];
        if (!s) continue;
        if (!s->alive) continue;
        if (s->fd == except_fd) continue;
        fds[count++] = s->fd;
    }
    pthread_mutex_unlock(&room->lock);

    /* 
    * pkt->length는 (type + payload)의 길이
    * payload의 길이가 최대 패킷길이보다 긴 경우 최대 패킷길이로 고정
    */
    int payload_len = (int)pkt->length - 2;
    if (payload_len <= 0) return;
    if (payload_len > MAX_PACKET_SIZE) payload_len = MAX_PACKET_SIZE;

    /* 브로드캐스트용 출력 패킷 생성 */
    packet_t out;
    memset(&out, 0, sizeof(out));

    /*
    * payload를 안전하게 복사하며 개행 추가
    * snprintf를 사용해 버퍼 오버플로 방지
    */
    int n = snprintf(out.payload, MAX_PACKET_SIZE, "%.*s\n", payload_len, pkt->payload);
    if (n <= 0 || n >= MAX_PACKET_SIZE)
        return;

    /*
    * 채팅 패킷 타입 설정
    * 전체 패킷 길이 = type(2바이트) + payload 길이 
    */
    out.type = PKT_CHAT;
    out.length = 2 + (uint16_t)n;

    /*
    * 수집된 fd 목록을 기반으로 각 대상에게 SEND 작업을 IO 큐에 등록
    * 이후 전송 작업을 IO 큐에 추가
    */
    for (int i = 0; i < count; ++i) {
        job_t job = { 0 };
        job.type = JOB_SEND;
        job.fd = fds[i];
        job.packet = out;
        job_queue_push(&g_io_q, &job);
    }

    /* IO 스레드를 깨워 큐에 쌓인 작업 처리 유도 */
    net_wakeup();
}