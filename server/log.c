#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_BUF_SIZE 1024
#define LOG_STR_VAL_SIZE 256

/* net 스레드와 4개의 logic worker 스레드가 모두 동시에 log_json을 호출하므로, 한 줄 출력이 서로 섞이지 않도록 이 락으로 보호함 */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/*
* JSON 문자열 값 안에 그대로 들어가면 포맷을 깨뜨리는 문자(", \)와 제어문자를 이스케이프하는 함수
* dst에 이스케이프된 결과를 채우고, 채워진 길이를 반환
*/
static int json_escape(char* dst, size_t dst_size, const char* src)
{
	size_t di = 0;

	/* src를 한 글자씩 순회하며 dst에 옮겨 담음(널 종료 문자 + 이스케이프 시 최대 2바이트 소모를 감안해 di + 2로 여유를 둠) */
	for (size_t si = 0; src[si] != '\0' && di + 2 < dst_size; ++si) {
		unsigned char c = (unsigned char)src[si];

		/* 큰따옴표와 역슬래시는 JSON 문법상 반드시 역슬래시로 이스케이프해야 함 */
		if (c == '"' || c == '\\') {
			dst[di++] = '\\';
			dst[di++] = (char)c;
		}
		/* 개행 등 제어문자는 한 줄 JSON 포맷을 깨뜨리므로 \u00XX 형태로 이스케이프 */
		else if (c < 0x20) {
			int n = snprintf(dst + di, dst_size - di, "\\u%04x", c);
			if (n > 0) di += (size_t)n;
		}
		/* 그 외 일반 문자는 그대로 복사 */
		else {
			dst[di++] = (char)c;
		}
	}

	dst[di] = '\0';
	return (int)di;
}

/*
* level/event 및 가변 key-value 필드를 한 줄 JSON으로 직렬화해 stdout에 쓰는 로깅 단일 진입점
* 여러 스레드가 동시에 호출하므로, 완성된 한 줄 전체를 하나의 write() 호출로 내보내야
* 컨테이너 로그 수집기에서 서로 다른 스레드의 로그 줄이 섞이지 않음
*/
void log_json(const char* level, const char* event, ...)
{
	char buf[LOG_BUF_SIZE];
	int len;

	/* 로그 시각은 호출부가 매번 넘기지 않도록 이 함수 안에서 UTC로 직접 생성함 */
	time_t now = time(NULL);
	struct tm tm_utc;
	gmtime_r(&now, &tm_utc);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

	/* ts/level/event는 모든 로그 라인에 공통으로 들어가는 고정 필드이므로 먼저 채움 */
	len = snprintf(buf, sizeof(buf), "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":\"%s\"", ts, level, event);
	if (len < 0) return;

	/*
	* 이후 가변 인자는 (key, 타입, 값) 세 개씩 묶여 전달되며, key로 NULL이 오면 종료
	* 버퍼가 거의 찼으면(여유가 1바이트 이하) 더 채우지 않고 중단해 오버플로를 방지
	*/
	va_list ap;
	va_start(ap, event);
	for (;;) {
		if ((size_t)len >= sizeof(buf) - 1) break;

		const char* key = va_arg(ap, const char*);
		if (!key) break;

		log_arg_type_t type = (log_arg_type_t)va_arg(ap, int);
		int n;

		/* 정수형 필드는 그대로 숫자로 직렬화 */
		if (type == LOG_ARG_INT) {
			int val = va_arg(ap, int);
			n = snprintf(buf + len, sizeof(buf) - (size_t)len, ",\"%s\":%d", key, val);
		}
		/* 문자열 필드는 JSON을 깨뜨릴 수 있는 문자를 이스케이프한 뒤 직렬화 */
		else {
			const char* val = va_arg(ap, const char*);
			char escaped[LOG_STR_VAL_SIZE];
			json_escape(escaped, sizeof(escaped), val ? val : "");
			n = snprintf(buf + len, sizeof(buf) - (size_t)len, ",\"%s\":\"%s\"", key, escaped);
		}

		if (n < 0) break;
		len += n;
	}
	va_end(ap);

	/* 객체를 닫는 '}'와 한 줄을 구분하는 개행을 덧붙임(여유 3바이트를 미리 확보한 뒤 기록) */
	if ((size_t)len > sizeof(buf) - 3) len = (int)sizeof(buf) - 3;
	buf[len++] = '}';
	buf[len++] = '\n';

	/*
	* 완성된 한 줄 전체를 락으로 보호된 상태에서 단일 write()로 출력
	* fprintf처럼 여러 번 나눠 쓰면 중간에 다른 스레드의 출력이 끼어들어 줄이 섞일 수 있음
	*/
	pthread_mutex_lock(&g_log_lock);
	ssize_t written = write(STDOUT_FILENO, buf, (size_t)len);
	(void)written; /* 로그 출력 실패는 무시(로깅 자체가 서버 로직에 영향을 주면 안 됨) */
	pthread_mutex_unlock(&g_log_lock);
}
