#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_BUF_SIZE 1024
#define LOG_STR_VAL_SIZE 256

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/* JSON 문자열 값 안의 " \ 및 제어문자를 이스케이프하여 dst에 담고 길이를 반환 */
static int json_escape(char* dst, size_t dst_size, const char* src)
{
	size_t di = 0;
	for (size_t si = 0; src[si] != '\0' && di + 2 < dst_size; ++si) {
		unsigned char c = (unsigned char)src[si];
		if (c == '"' || c == '\\') {
			dst[di++] = '\\';
			dst[di++] = (char)c;
		}
		else if (c < 0x20) {
			int n = snprintf(dst + di, dst_size - di, "\\u%04x", c);
			if (n > 0) di += (size_t)n;
		}
		else {
			dst[di++] = (char)c;
		}
	}
	dst[di] = '\0';
	return (int)di;
}

/*
* level/event 및 가변 key-value 필드를 한 줄 JSON으로 직렬화해 stdout에 씀
* net/logic 등 여러 스레드가 동시에 호출하므로, 완성된 한 줄을 mutex로 보호된
* 단일 write() 호출로 내보내 컨테이너 로그에서 줄이 서로 섞이지 않게 함
*/
void log_json(const char* level, const char* event, ...)
{
	char buf[LOG_BUF_SIZE];
	int len;

	time_t now = time(NULL);
	struct tm tm_utc;
	gmtime_r(&now, &tm_utc);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

	len = snprintf(buf, sizeof(buf), "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":\"%s\"", ts, level, event);
	if (len < 0) return;

	va_list ap;
	va_start(ap, event);
	for (;;) {
		if ((size_t)len >= sizeof(buf) - 1) break;

		const char* key = va_arg(ap, const char*);
		if (!key) break;

		log_arg_type_t type = (log_arg_type_t)va_arg(ap, int);
		int n;

		if (type == LOG_ARG_INT) {
			int val = va_arg(ap, int);
			n = snprintf(buf + len, sizeof(buf) - (size_t)len, ",\"%s\":%d", key, val);
		}
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

	if ((size_t)len > sizeof(buf) - 3) len = (int)sizeof(buf) - 3;
	buf[len++] = '}';
	buf[len++] = '\n';

	pthread_mutex_lock(&g_log_lock);
	ssize_t written = write(STDOUT_FILENO, buf, (size_t)len);
	(void)written; /* 로그 출력 실패는 무시(로깅 자체를 위해 서버 로직에 영향을 주지 않음) */
	pthread_mutex_unlock(&g_log_lock);
}
