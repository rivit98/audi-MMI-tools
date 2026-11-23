// conf.h - the runner's view of sd/apps.conf (the format: the file's header)
// and of an app's result. Pure, tested on the host
// (tests/test_runner.cpp).
#ifndef RUNNER_CONF_H
#define RUNNER_CONF_H

#include "types.h"

#include <string>
#include <vector>

namespace runner {

struct AppConf {
	std::string name;
	unsigned timeout_s = 120;
	std::vector<std::string> args;      // after <sd_path>
	std::string text;
};

// name|timeout s|arguments|text, one app per line; '#' lines and blank
// lines skipped. Like ksh's `IFS='|' read`, the text keeps any further '|'.
// A name listed twice: the later line wins, at the first line's place.
std::vector<AppConf> parse_conf(const std::string &text);

// Apps found on the card: every ELF file in <sd>/apps/ is one. An apps.conf
// line is optional and only describes it.
constexpr const char *kAppsDir = "apps";
// Timeout of an app with no apps.conf line. The apps stop themselves (quit.c's
// watchdog); this only catches one that hangs.
constexpr unsigned kDefaultTimeoutS = 3600;

// A file name the runner accepts as an app name: [A-Za-z0-9_.-], not
// starting with '.', at most 64 characters (it ends up in log names and in
// the ksh command that collects core files).
bool valid_name(const std::string &name);
// An ELF file starts with 7f 'E' 'L' 'F'.
bool is_elf(const u8 *head, usize n);

// The menu: the apps found (files), described by their apps.conf line when
// there is one (timeout, arguments, text), else name only with
// kDefaultTimeoutS. apps.conf's order first, then the rest by name.
std::vector<AppConf> menu_apps(const std::vector<std::string> &files,
                               const std::vector<AppConf> &conf);

// The verdicts. rc: the exit code, or 128 + the signal.
//   MISSING  not started      TIMEOUT  stopped at its timeout
//   CRASH    rc > 128         FAIL     rc != 0
//   WARN     rc 0, FAIL lines in its output        OK  rc 0, none
const char *verdict(bool started, bool timed_out, int rc, unsigned fail_lines);

// Lines with "FAIL" in them (`grep -c FAIL`).
unsigned count_fail_lines(const std::string &text);

} // namespace runner

#endif
