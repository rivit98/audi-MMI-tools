// watch.cpp - see watch.h.
#include "watch.h"

#include "ipc4.h"

#include <algorithm>
#include <set>

namespace watch {

bool Watch::value(unsigned ms, const std::string &key, const std::vector<u8> &v)
{
	auto it = last_.find(key);
	if (it == last_.end()) {
		last_.emplace(key, v);
		return false;
	}
	if (it->second == v)
		return false;
	changes_.push_back(Change{ms, key, it->second, v});
	it->second = v;
	return true;
}

void Watch::begin(unsigned ms, const std::string &label, const std::string &kind)
{
	end(ms);
	Step s;
	s.label = label;
	s.kind = kind;
	s.begin_ms = ms;
	s.end_ms = ms;
	steps_.push_back(s);
	open_ = true;
}

void Watch::end(unsigned ms)
{
	if (!open_)
		return;
	steps_.back().end_ms = ms;
	open_ = false;
}

std::vector<std::string> Watch::changed_in(unsigned b, unsigned e) const
{
	std::set<std::string> keys;
	for (const Change &c : changes_)
		if (c.ms >= b && c.ms < e)
			keys.insert(c.key);
	return std::vector<std::string>(keys.begin(), keys.end());
}

std::map<std::string, std::vector<std::string>> Watch::candidates() const
{
	std::set<std::string> noisy;
	std::map<std::string, std::vector<std::set<std::string>>> by_kind;
	for (const Step &s : steps_) {
		std::vector<std::string> keys = changed_in(s.begin_ms, s.end_ms);
		if (s.kind.empty())
			noisy.insert(keys.begin(), keys.end());
		else
			by_kind[s.kind].emplace_back(keys.begin(), keys.end());
	}
	std::map<std::string, std::vector<std::string>> out;
	for (const auto &[kind, sets] : by_kind) {
		std::vector<std::string> &keys = out[kind];
		for (const std::string &k : sets.front()) {
			if (noisy.count(k))
				continue;
			bool everywhere = std::all_of(sets.begin(), sets.end(),
			                              [&](const std::set<std::string> &s) { return s.count(k) != 0; });
			if (everywhere)
				keys.push_back(k);
		}
	}
	return out;
}

std::vector<std::string> Watch::describe(const std::string &key, unsigned b, unsigned e) const
{
	std::vector<std::string> out;
	for (const Change &c : changes_)
		if (c.key == key && c.ms >= b && c.ms < e)
			out.push_back("[" + ipc4::hex(c.from, 12) + "] -> [" + ipc4::hex(c.to, 12) + "]");
	return out;
}

} // namespace watch
