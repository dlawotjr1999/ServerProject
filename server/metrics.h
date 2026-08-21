#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

/* 카운터형 지표 증가 함수 (호출부에서 이벤트가 발생할 때마다 호출) */
void metrics_inc_messages(void);
void metrics_inc_disconnects(void);

/* 현재 지표를 Prometheus 텍스트 포맷으로 buf에 렌더링하고, 쓰여진 길이를 반환 */
int metrics_render(char* buf, size_t bufsize);

#endif
