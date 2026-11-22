// conf.h - the runner's view of sd/apps.conf (the format: the file's header)
// and of an app's result, as run.sh sees them. Pure, tested on the host
// (tests/test_runner.cpp).
#ifndef RUNNER_CONF_H
#define RUNNER_CONF_H

#include "types.h"

#include <string>
#include <vector>

namespace runner {

struct AppConf {
	std::string name;
	unsigned timeout_s = 120;           // run.sh's default
	std::vector<std::string> args;      // after <sd_path>
	std::string title;                  // the name if empty in the file
	std::string text;
};

// name|timeout s|arguments|title|text, one app per line; '#' lines and blank
// lines skipped. Like ksh's `IFS='|' read`, the text keeps any further '|'.
// A name listed twice: the later line wins, at the first line's place.
std::vector<AppConf> parse_conf(const std::string &text);

// run.sh's verdicts. rc: the exit code, or 128 + the signal.
//   MISSING  not started      TIMEOUT  stopped at its timeout
//   CRASH    rc > 128         FAIL     rc != 0
//   WARN     rc 0, FAIL lines in its output        OK  rc 0, none
const char *verdict(bool started, bool timed_out, int rc, unsigned fail_lines);

// Lines with "FAIL" in them (`grep -c FAIL`).
unsigned count_fail_lines(const std::string &text);

} // namespace runner

#endif
