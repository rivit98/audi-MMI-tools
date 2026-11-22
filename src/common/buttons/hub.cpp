// hub.cpp - see hub.h.
#include "hub.h"

#include <cstdio>
#include <string>

extern "C" {
#include "log.h"
}

namespace buttons {

const char *Hub::name_of(const Sub &s, char *buf, usize size)
{
	if (s.name != nullptr)
		return s.name;
	std::snprintf(buf, size, "#%d", s.id);
	return buf;
}

int Hub::subscribe(Handler h, const char *name)
{
	int id;
	usize count;
	bool logging;
	{
		std::lock_guard<std::mutex> lk(mu_);
		id = next_id_++;
		subs_.push_back(Sub{id, std::move(h), name});
		count = subs_.size();
		logging = logging_;
	}
	if (logging) {
		char buf[16];
		log_msg("buttons: subscribe %s (id %d, %u subscribers)",
		        name_of(Sub{id, nullptr, name}, buf, sizeof(buf)), id, (unsigned)count);
	}
	return id;
}

void Hub::unsubscribe(int id)
{
	const char *name = nullptr;
	bool found = false, logging;
	usize count;
	{
		std::lock_guard<std::mutex> lk(mu_);
		for (auto it = subs_.begin(); it != subs_.end(); ++it)
			if (it->id == id) {
				name = it->name;
				subs_.erase(it);
				found = true;
				break;
			}
		count = subs_.size();
		logging = logging_;
	}
	if (!logging)
		return;
	char buf[16];
	if (found)
		log_msg("buttons: unsubscribe %s (id %d, %u subscribers)",
		        name_of(Sub{id, nullptr, name}, buf, sizeof(buf)), id, (unsigned)count);
	else
		log_msg("buttons: unsubscribe id %d: no such subscriber", id);
}

void Hub::feed(const u8 *buf, int n, u32 ms)
{
	// The log lines are built under the lock and written after it: a log
	// line on the SD card takes ~12 ms (fsync), and subscribers shouldn't
	// wait for it.
	std::vector<std::string> lines;
	{
		std::lock_guard<std::mutex> lk(mu_);
		messages_++;
		scratch_.clear();
		dec_.decode(buf, n, ms, scratch_);
		for (const Event &e : scratch_) {
			std::string line;
			if (logging_) {
				line = describe(e);
				line += subs_.empty() ? " -> nobody" : " ->";
			}
			bool first = true;
			for (const Sub &s : subs_) {
				if (logging_) {
					char nb[16];
					line += first ? " " : ", ";
					line += name_of(s, nb, sizeof(nb));
					first = false;
				}
				s.h(e);
			}
			if (logging_)
				lines.push_back(std::move(line));
		}
	}
	for (const std::string &l : lines)
		log_msg("buttons: %s", l.c_str());
}

void Hub::set_logging(bool on)
{
	std::lock_guard<std::mutex> lk(mu_);
	logging_ = on;
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

EventQueue::EventQueue(Hub &hub, usize capacity, const char *name)
	: hub_(hub), cap_(capacity ? capacity : 1), name_(name ? name : "queue")
{
	id_ = hub_.subscribe([this](const Event &e) { push(e); }, name_);
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
			// Logged at 1, 2, 4, 8, ... drops: an app that stopped reading
			// mustn't cost ~12 ms (an SD log line) per event.
			dropped_++;
			if ((dropped_ & (dropped_ - 1)) == 0)
				log_msg("buttons: %s full (%u), dropped %s (%u dropped so far)",
				        name_, (unsigned)cap_, describe(q_.front()).c_str(),
				        (unsigned)dropped_);
			q_.pop_front();
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
