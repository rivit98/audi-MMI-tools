// conf.cpp - see conf.h.
#include "conf.h"

#include <cstdlib>

namespace runner {

namespace {

std::vector<std::string> split_ws(const std::string &s)
{
	std::vector<std::string> out;
	usize i = 0;
	while (i < s.size()) {
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
			i++;
		usize j = i;
		while (j < s.size() && s[j] != ' ' && s[j] != '\t')
			j++;
		if (j > i)
			out.push_back(s.substr(i, j - i));
		i = j;
	}
	return out;
}

} // namespace

std::vector<AppConf> parse_conf(const std::string &text)
{
	std::vector<AppConf> apps;
	usize pos = 0;

	while (pos < text.size()) {
		usize end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;

		std::string f[5];
		usize start = 0;
		for (int k = 0; k < 5; k++) {
			usize bar = k < 4 ? line.find('|', start) : std::string::npos;
			if (bar == std::string::npos) {
				f[k] = start <= line.size() ? line.substr(start) : "";
				start = line.size() + 1;
			} else {
				f[k] = line.substr(start, bar - start);
				start = bar + 1;
			}
		}
		if (f[0].empty())
			continue;

		AppConf a;
		a.name = f[0];
		if (!f[1].empty()) {
			long t = std::strtol(f[1].c_str(), nullptr, 10);
			if (t > 0)
				a.timeout_s = (unsigned)t;
		}
		a.args = split_ws(f[2]);
		a.title = f[3].empty() ? f[0] : f[3];
		a.text = f[4];

		bool replaced = false;
		for (AppConf &old : apps)
			if (old.name == a.name) {
				old = a;
				replaced = true;
			}
		if (!replaced)
			apps.push_back(a);
	}
	return apps;
}

const char *verdict(bool started, bool timed_out, int rc, unsigned fail_lines)
{
	if (!started)
		return "MISSING";
	if (timed_out)
		return "TIMEOUT";
	if (rc > 128)
		return "CRASH";
	if (rc != 0)
		return "FAIL";
	return fail_lines ? "WARN" : "OK";
}

unsigned count_fail_lines(const std::string &text)
{
	unsigned n = 0;
	usize pos = 0;

	while (pos < text.size()) {
		usize end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		usize hit = text.find("FAIL", pos);
		if (hit != std::string::npos && hit < end)
			n++;
		pos = end + 1;
	}
	return n;
}

} // namespace runner
