#include "protocol.h"

int protocol_parse(connection_t* conn, packet_t* out)
{
    /* header 최소 크기 */
    if (conn->recv_len < 4)
        return 0;

    uint16_t pkt_len;
    memcpy(&pkt_len, conn->recv_buf, sizeof(uint16_t));
    pkt_len = ntohs(pkt_len);

    /* sanity check */
    if (pkt_len == 0 || pkt_len > MAX_PACKET_SIZE)
        return -1;

    /* 전체 패킷이 아직 안 들어옴 */
    if ((size_t)conn->recv_len < pkt_len + sizeof(uint16_t))
        return 0;

    uint16_t pkt_type;
    memcpy(&pkt_type, conn->recv_buf + 2, sizeof(uint16_t));
    pkt_type = ntohs(pkt_type);

    out->length = pkt_len;
    out->type = pkt_type;

    int payload_len = pkt_len - sizeof(uint16_t);
    if (payload_len > 0) {
        memcpy(out->payload,
            conn->recv_buf + 4,
            payload_len);
    }

    /* recv_buf 앞으로 당김 */
    int remain = conn->recv_len - (pkt_len + 2);
    memmove(conn->recv_buf,
        conn->recv_buf + pkt_len + 2,
        remain);
    conn->recv_len = remain;

    return 1;
}
