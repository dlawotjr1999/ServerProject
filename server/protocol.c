#include "protocol.h"

int protocol_parse(connection_t* conn, packet_t* out)
{
    /*
    * 최소 헤더 크기 검사
    * legnth와 type 모두 각각 uint16_t
    * length(2) + type(2) = 최소 4바이트 필요
    */
    if (conn->recv_len < 4)
        return 0;

    /*
    * 패킷 길이 필드 추출 : recv_buf[0~1]
    * pkt_len = (type + payload)의 길이
    * 이후 호스트 순서로 변환
    */
    uint16_t pkt_len;
    memcpy(&pkt_len, conn->recv_buf, sizeof(uint16_t));
    pkt_len = ntohs(pkt_len);

    /*
    * 정상 패킷 여부 확인
    * 길이가 0이거나 최대 payload 크기를 초과하면 비정상 패킷으로 판단
    */
    if (pkt_len == 0 || pkt_len > MAX_PACKET_SIZE)
        return -1;

    /* 
    * 전체 패킷이 아직 안 들어옴 
    * 즉, 현재 수신된 바이트 수 < (type + payload) + length(2)인 경우
    */
    if ((size_t)conn->recv_len < pkt_len + sizeof(uint16_t))
        return 0;
    
    /*
    * 패킷 타입 필드 추출 : recv_buf[2~3]
    * 이후 호스트 순서로 변환
    */
    uint16_t
    uint16_t pkt_type;
    memcpy(&pkt_type, conn->recv_buf + 2, sizeof(uint16_t));
    pkt_type = ntohs(pkt_type);

    /* 파싱 결과를 out 패킷에 저장 */
    out->length = pkt_len;
    out->type = pkt_type;

    /*
    * 전체 패킷이 수신되었는지 확인
    * payload_len = pkt_len(type + payload의 길이) - type 필드의 길이(2)
    * 한 번의 recv로 패킷 전체가 들어올거라는 보장이 없기 때문
    */
    int payload_len = pkt_len - sizeof(uint16_t);
    if (payload_len > 0) {
        memcpy(out->payload, conn->recv_buf + 4, payload_len);
    }

    /*
    * recv 버퍼 정리
    * 남은 바이트 수 = 현재 수신된 전체 바이트 수 - (pkt_len + length 필드의 길이(2))
    * 현재 패킷의 끝 부분(다음 패킷의 시작 위치) = conn->recv_buf + pkt_len + 2
    * 현재 패킷을 제거하고, 뒤에 남은 데이터(다음 패킷 또는 일부 패킷)를 버퍼 앞으로 당김
    */
    int remain = conn->recv_len - (pkt_len + 2);
    memmove(conn->recv_buf, conn->recv_buf + pkt_len + 2, remain);
    conn->recv_len = remain;

    /* 파싱 성공 */
    return 1;
}
