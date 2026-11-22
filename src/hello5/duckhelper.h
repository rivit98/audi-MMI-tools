/*
 * duckhelper.h - hello5 step d's helper process: every HBDSI send to the
 * MMI, and nothing else. hello5 starts it as "hello5 <sd_path> @duck"
 * (ducklink.h has why and the protocol). It owns no screen, no ch8 and no
 * audio, so if a send never returns, only this process is stuck.
 *
 * Commands (stdin) and answers (fd 3):
 *   connect          -> ok <server pid> <chid> <server id> | err <why>
 *                       the broker attach, else straight to
 *                       MMI3GApplication on the chids the navigation uses
 *                       (DUCK_SKIP_CHIDS left out)
 *   focus <src> <on> -> ok <errno> <ms>: Request/ReleaseAudioFocus(src,
 *                       eSNK_SPEAKERS)
 *   rx               -> ok <msgs0> <msgs1> <pulses0> <pulses1>
 *   quit / EOF       -> detach and exit
 * Before anything that may block it says "wait <ms> <what>". It logs to
 * var/hello5_helper.log.
 */
#ifndef HELLO5_DUCKHELPER_H
#define HELLO5_DUCKHELPER_H

#define DUCK_HELPER_ARG   "@duck"       /* argv[2] that starts the helper */

#define DUCK_IFACE        "SGAudioManagement"
#define DUCK_ROLE         "AudioManagementDevCtrl"      /* its component */
#define DUCK_CONNECT_MS   1000          /* for a ConnectRequest reply */
#define DUCK_BACK_MS      600           /* for the server to connect back */
#define DUCK_TIMEOUT_MS   2000          /* for a DataRequest reply */
#define DUCK_BROKER_MS    3000          /* one broker read or attach */
#define DUCK_MAX_TRIES    8             /* ConnectRequests in all */

/* MMI3GApplication's channels never sent to: 6 replies all zeros (not the
 * HBDSI root channel), a ConnectRequest to 13 never returns
 * (docs/hbdsi.md section 6.1). */
#define DUCK_SKIP_CHIDS   { 6, 13 }

/* The helper's main: 0 after "quit". */
int duck_helper_main(const char *sd_path);

/* Upper bound of "connect" in ms, every "wait" counted, for hello5's
 * watchdog. */
unsigned duck_connect_max_ms(void);

#endif
