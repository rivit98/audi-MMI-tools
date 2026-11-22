// hub.cpp - see hub.h.
#include "hub.h"

extern "C" {
#include "log.h"
}

namespace buttons {

int Hub::subscribe(Handler h)
{
	std::lock_guard<std::mutex> lk(mu_);
	int id = next_id_++;
	subs_.push_back(Sub{id, std::move(h)});
	return id;
}

void Hub::unsubscribe(int id)
{
	std::lock_guard<std::mutex> lk(mu_);
	for (auto it = subs_.begin(); it != subs_.end(); ++it)
		if (it->id == id) {
			subs_.erase(it);
			return;
		}
}

void Hub::feed(const u8 *buf, int n, u32 ms)
{
	std::lock_guard<std::mutex> lk(mu_);
	messages_++;
	scratch_.clear();
	dec_.decode(buf, n, ms, scratch_);
	for (const Event &e : scratch_)
		for (const Sub &s : subs_)
			s.h(e);
}

void Hub::quit_cb(const u8 *buf, int n, void *arg)
{
	static_cast<Hub *>(arg)->feed(buf, n, log_ms());
}

u32 Hub::messages() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return messages_;
}

u8 Hub::held(int byte) const
{
	std::lock_guard<std::mutex> lk(mu_);
	return dec_.held(byte);
}

EventQueue::EventQueue(Hub &hub, usize capacity)
	: hub_(hub), cap_(capacity ? capacity : 1)
{
	id_ = hub_.subscribe([this](const Event &e) { push(e); });
}

EventQueue::~EventQueue()
{
	hub_.unsubscribe(id_);
}

void EventQueue::push(const Event &e)
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (q_.size() >= cap_) {
			q_.pop_front();
			dropped_++;
		}
		q_.push_back(e);
	}
	cond_.notify();
}

bool EventQueue::wait(Event &e, unsigned timeout_ms)
{
	std::unique_lock<std::mutex> lk(mu_);
	if (q_.empty() && timeout_ms > 0)
		cond_.wait_ms(lk, timeout_ms, [this] { return !q_.empty(); });
	if (q_.empty())
		return false;
	e = q_.front();
	q_.pop_front();
	return true;
}

bool EventQueue::poll(Event &e)
{
	return wait(e, 0);
}

void EventQueue::clear()
{
	std::lock_guard<std::mutex> lk(mu_);
	q_.clear();
}

usize EventQueue::size() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return q_.size();
}

u32 EventQueue::dropped() const
{
	std::lock_guard<std::mutex> lk(mu_);
	return dropped_;
}

} // namespace buttons
