// cxxrt-std.cpp - the C++ half of cxxrt-qnx.o (see cxxrt.c): libstdc++.a
// members that can't be used as built for glibc, rebuilt here against the
// QNX headers. Linked ahead of libstdc++.a, so its members are not pulled.
//
// std::condition_variable: libstdc++.a's constructor initialises a glibc
//   pthread_cond_t (48 bytes) inside an object the QNX headers size for
//   QNX's (8 bytes) - 40 bytes of someone else's memory zeroed, and a
//   condvar QNX doesn't recognise. These are the same members compiled
//   with QNX's layout and PTHREAD_COND_INITIALIZER. Everything else in
//   condition_variable.o (notify_all_at_thread_exit) stays unavailable: using
//   it pulls the member in and the link fails on duplicate symbols.
// std::call_once: libstdc++.a keeps the callable in __thread variables;
//   without TLS the headers use a global std::function under a global
//   mutex instead (GCC's own non-TLS mutex.cc, reproduced here). An
//   exception thrown by the callable can't unwind through QNX's
//   pthread_once (no unwind tables) and ends in std::terminate.
#include <condition_variable>
#include <functional>
#include <mutex>

namespace std {

condition_variable::condition_variable() noexcept = default;
condition_variable::~condition_variable() noexcept = default;

void condition_variable::wait(unique_lock<mutex>& lock)
{
	_M_cond.wait(*lock.mutex());
}

void condition_variable::notify_one() noexcept
{
	_M_cond.notify_one();
}

void condition_variable::notify_all() noexcept
{
	_M_cond.notify_all();
}

function<void()> __once_functor;

namespace {
unique_lock<mutex>* once_functor_lock_ptr;
}

mutex& __get_once_mutex()
{
	static mutex once_mutex;
	return once_mutex;
}

void __set_once_functor_lock_ptr(unique_lock<mutex>* ptr)
{
	once_functor_lock_ptr = ptr;
}

} // namespace std

// pthread_once's init routine for std::call_once: take the callable, let
// the next call_once in, run it.
extern "C" void __once_proxy()
{
	std::function<void()> once_call = std::move(std::__once_functor);

	if (std::unique_lock<std::mutex>* lock = std::once_functor_lock_ptr) {
		std::once_functor_lock_ptr = nullptr;
		lock->unlock();
	} else {
		std::__get_once_mutex().unlock();
	}
	once_call();
}
