#ifndef POSIX_LOCK_HPP
#define POSIX_LOCK_HPP

#include <pthread.h>

/*
* pthread_mutex_t 위에 RAII를 씌운 락 가드
* std::lock_guard가 std::mutex에 해주는 일을, 그대로 POSIX 뮤텍스에 직접 구현함
* (std::mutex로 바꾸지 않고 pthread_mutex_t를 유지하기로 한 결정 때문 - REDESIGN.md 참고)
*
* 생성자에서 lock, 소멸자에서 unlock -> 함수 어느 경로로 빠져나가든(조기 return 포함)
* 언락을 깜빡하는 게 구조적으로 불가능해짐. C 버전 state.c에서 실제로 겪었던
* "이 return 경로에서 unlock을 안 했다"류의 버그를 여기서 원천 차단하는 게 목적
*/
class PosixLockGuard {
public:
	explicit PosixLockGuard(pthread_mutex_t& mutex) : mutex_(mutex) {
		pthread_mutex_lock(&mutex_);
	}

	~PosixLockGuard() {
		pthread_mutex_unlock(&mutex_);
	}

	PosixLockGuard(const PosixLockGuard&) = delete;
	PosixLockGuard& operator=(const PosixLockGuard&) = delete;

private:
	pthread_mutex_t& mutex_;
};

#endif
