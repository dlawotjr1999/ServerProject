#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

int protocol_parse(connection_t* conn, packet_t* out);

#endif
