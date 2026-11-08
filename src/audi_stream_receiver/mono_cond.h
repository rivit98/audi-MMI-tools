// mono_cond.h - a condition variable whose timed wait runs on CLOCK_MONOTONIC.
//
// std::condition_variable's timed waits end up on CLOCK_REALTIME here (QNX 6.3
// has no pthread_cond_clockwait), and the unit's wall clock starts at 1970 and
// jumps when something sets it: a wait across a jump backwards would not end.
#ifndef AUDI_STREAM_MONO_COND_H
#define AUDI_STREAM_MONO_COND_H

#include <cerrno>
#include <mutex>

#include <pthread.h>
#include <time.h>

class MonoCond {
public:
	MonoCond()
	{
		pthread_condattr_t ca;

		pthread_condattr_init(&ca);
		pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
		pthread_cond_init(&cond_, &ca);
		pthread_condattr_destroy(&ca);
	}
	~MonoCond() { pthread_cond_destroy(&cond_); }
	MonoCond(const MonoCond &) = delete;
	MonoCond &operator=(const MonoCond &) = delete;

	void notify() { pthread_cond_signal(&cond_); }

	// Until done() (checked with the lock held) or ms have passed.
	template <class Done>
	void wait_ms(std::unique_lock<std::mutex> &lock, unsigned ms, Done done)
	{
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_sec += ms / 1000;
		ts.tv_nsec += (long)(ms % 1000) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		while (!done())
			if (pthread_cond_timedwait(&cond_, lock.mutex()->native_handle(), &ts) == ETIMEDOUT)
				break;
	}

private:
	pthread_cond_t cond_;
};

#endif
