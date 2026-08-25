#ifndef LOG_H
#define LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* log_json의 가변 key-value 인자 각각의 값 타입을 나타냄 */
typedef enum {
	LOG_ARG_INT,
	LOG_ARG_STR,
} log_arg_type_t;

/*
* 한 줄 JSON 로그를 stdout에 출력하는 단일 진입점
* 사용법: log_json("INFO", "accept", "fd", LOG_ARG_INT, fd, "session_id", LOG_ARG_INT, sid, NULL);
* key-value 쌍은 (const char* key, log_arg_type_t type, 값) 순서로 나열하고 NULL로 종료
*
* C 가변인자(...) 함수라 net.c/logic.c/main.c(순수 C) 콜사이트를 하나도 안 바꾸고 그대로 씀
*/
void log_json(const char* level, const char* event, ...);

#ifdef __cplusplus
}
#endif

#endif
