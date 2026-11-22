/*
 * duckhelper.c - hello5 step d's helper process (duckhelper.h).
 */
#include "duckhelper.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "types.h"
#include "hbdsi.h"
#include "log.h"
#include "ducklink.h"
#include "ipcprobe.h"
#include "svcid.h"
#include "svcscan.h"

#define MAX_CHIDS   8           /* channels of the server we collect */
#define MAX_IDS     (SVC_MAX_IDS + 2)
#define MAX_LISTED  8           /* broker candidates tried */
#define N_VERSIONS  4

static const char *out_dir;

/* One line to hello5, logged here too. */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	usize n;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	for (n = 0; line[n] != '\0'; n++)
		if (line[n] == '\n')
			line[n] = ' ';
	log_msg("helper says: %s", line);
	line[n++] = '\n';
	(void)!write(DUCK_LINK_FD, line, n);
}

static void nap(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

unsigned duck_connect_max_ms(void)
{
	return 2 * DUCK_BROKER_MS + (MAX_LISTED + N_VERSIONS) * DUCK_BROKER_MS +
	       ipc_find_max_ms() + svc_scan_max_ms() +
	       DUCK_MAX_TRIES * (DUCK_CONNECT_MS + DUCK_BACK_MS + 2 * DUCK_LINK_SLACK_MS);
}

/* The broker's list, saved to var/, and the attach. Every candidate the
 * list names is tried, then the plain name at the usual versions. */
static int duck_attach(struct hbdsi *c)
{
	static const u16 versions[N_VERSIONS][2] = { { 1, 0 }, { 1, 1 }, { 2, 0 }, { 0, 1 } };
	static char text[16384];
	char path[256], name[HBDSI_NAME_MAX];
	u16 major, minor;
	unsigned i;
	int n, rc, status;
	FILE *f;

	say("wait %u broker list", DUCK_BROKER_MS);
	n = hbdsi_broker_list(text, sizeof(text));
	snprintf(path, sizeof(path), "%s/hello5_broker.txt", out_dir);
	f = fopen(path, "w");
	if (f != NULL) {
		fputs(text, f);
		fclose(f);
	}
	log_msg("step d: %s: %d bytes of status (saved to %s)", HBDSI_BROKER_PATH,
	        n, path);
	for (i = 0; i < MAX_LISTED &&
	            hbdsi_listing_find(text, DUCK_IFACE, i, name, sizeof(name),
	                               &major, &minor); i++) {
		if (strstr(name, "Client") != NULL)
			continue;
		say("wait %u broker attach", DUCK_BROKER_MS);
		rc = hbdsi_attach(c, name, major, minor, &status);
		log_msg("step d: attach \"%s\" %u.%u (listed): %s, broker: %d %s", name,
		        major, minor, strerror(rc), status, hbdsi_broker_status_name(status));
		if (rc == EOK && status == HBDSI_BROKER_OK) {
			say("note attached %.60s %u.%u", name, major, minor);
			return 0;
		}
	}
	for (i = 0; i < N_VERSIONS; i++) {
		say("wait %u broker attach", DUCK_BROKER_MS);
		rc = hbdsi_attach(c, DUCK_IFACE, versions[i][0], versions[i][1], &status);
		log_msg("step d: attach \"%s\" %u.%u: %s, broker: %d %s", DUCK_IFACE,
		        versions[i][0], versions[i][1], strerror(rc), status,
		        hbdsi_broker_status_name(status));
		if (rc != EOK)
			return -1;      /* no broker at all */
		if (status == HBDSI_BROKER_OK) {
			say("note attached %s %u.%u", DUCK_IFACE, versions[i][0],
			    versions[i][1]);
			return 0;
		}
		if (status != HBDSI_BROKER_VERSION_MISMATCH && i > 0)
			break;
	}
	return -1;
}

static int skipped(int chid)
{
	static const int skip[] = DUCK_SKIP_CHIDS;
	unsigned i;

	for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
		if (skip[i] == chid)
			return 1;
	return 0;
}

/* The other way in, when the broker does not know the interface: send the
 * ConnectRequest straight to MMI3GApplication, the way its own child
 * processes do (docs/hbdsi.md section 4). Three things are needed and
 * none of them comes from the broker:
 *   the server's pid         /proc, else sloginfo / pidin arg (ipcprobe.c)
 *   the channel to send to   the channels MMI3GNavigation is connected to
 *                            (ipcprobe.c), DUCK_SKIP_CHIDS left out; never
 *                            the ones MMI3GApplication only receives on:
 *                            each untried channel can hang the sender
 *   the service's server id  the component id from the bootstrap table,
 *                            looked for in the process's own memory
 *                            (svcscan.c), with the id read from the
 *                            binary as the fallback
 * The server answers every ConnectRequest, found or not, with its own
 * channels, so the reply says nothing about success. What does: it only
 * connects back to our channels when it found the service. */
static int duck_internal(struct hbdsi *c)
{
	pid_t app = 0;
	int all[MAX_CHIDS], chids[MAX_CHIDS];
	u32 ids[MAX_IDS];
	unsigned nids = 0, tries = 0, n_nav = 0, k;
	struct svc_result scan;
	char path[256];
	int n_all, n = 0, i, rc;

	say("wait %u finding MMI3GApplication's channels", ipc_find_max_ms());
	n_all = ipc_app_channels(out_dir, &app, all, MAX_CHIDS, &n_nav);
	for (i = 0; i < n_all && i < (int)n_nav; i++) {
		if (skipped(all[i]))
			log_msg("step d: chid %d left out (DUCK_SKIP_CHIDS)", all[i]);
		else
			chids[n++] = all[i];
	}
	if (n <= 0) {
		log_msg("step d: no untried channel of MMI3GApplication that the "
		        "navigation uses (%d found, %u of them the navigation's)",
		        n_all, n_nav);
		return -1;
	}
	snprintf(path, sizeof(path), "%s/hello5_services.txt", out_dir);
	say("wait %u reading MMI3GApplication's memory", svc_scan_max_ms());
	if (svc_scan(app, DUCK_ROLE, path, &scan) == 0)
		for (k = 0; k < scan.nids && nids < MAX_IDS; k++)
			ids[nids++] = scan.ids[k];
	/* the ids the binary's bootstrap table gives, in case the scan found
	 * nothing: they are what the table says audio management is */
	for (k = 0; k < 2; k++) {
		u32 id = k == 0 ? SVC_ID_AUDIO_DEVCTRL : SVC_ID_AUDIO_PRESCTRL;
		unsigned j;

		for (j = 0; j < nids; j++)
			if (ids[j] == id)
				break;
		if (j == nids && nids < MAX_IDS)
			ids[nids++] = id;
	}
	svc_ids_rank(ids, nids);

	/* the best id on every channel first, then the next id: an answer on
	 * the wrong channel costs a ConnectRequest timeout at most */
	for (k = 0; k < nids && tries < DUCK_MAX_TRIES; k++) {
		for (i = 0; i < n && tries < DUCK_MAX_TRIES; i++) {
			unsigned waited = 0;
			int back = 0;

			tries++;
			say("note chid %d, server id 0x%08x", chids[i], (unsigned)ids[k]);
			say("wait %u ConnectRequest to chid %d, server id 0x%08x",
			    DUCK_CONNECT_MS + DUCK_BACK_MS, chids[i], (unsigned)ids[k]);
			hbdsi_target(c, (u32)app, (u32)chids[i], ids[k]);
			rc = hbdsi_connect(c, DUCK_CONNECT_MS);
			if (rc != EOK) {
				log_msg("step d: chid %d, server id 0x%08x: ConnectRequest: %s",
				        chids[i], (unsigned)ids[k], strerror(rc));
				hbdsi_close(c);
				hbdsi_init(c);
				continue;
			}
			/* it may connect back after its reply: give it a moment.
			 * Two signs, each enough: its connection to our channel, or
			 * any message or pulse on our channels. */
			for (;;) {
				back = ipc_connected_to(app, getpid(), c->chid[0]) ||
				       ipc_connected_to(app, getpid(), c->chid[1]);
				if (back || c->rx_msgs[0] + c->rx_msgs[1] +
				            c->rx_pulses[0] + c->rx_pulses[1] > 0 ||
				    waited >= DUCK_BACK_MS)
					break;
				nap(50);
				waited += 50;
			}
			log_msg("step d: chid %d, server id 0x%08x: answered (its chids "
			        "%u/%u pid %u), after %u ms: connected back %s, rx %u+%u "
			        "messages %u+%u pulses", chids[i], (unsigned)ids[k],
			        (unsigned)c->srv.chid[0], (unsigned)c->srv.chid[1],
			        (unsigned)c->srv.pid, waited, back ? "YES" : "no",
			        c->rx_msgs[0], c->rx_msgs[1], c->rx_pulses[0],
			        c->rx_pulses[1]);
			if (back || c->rx_msgs[0] + c->rx_msgs[1] > 0) {
				log_msg("RESULT step d: %s answered on chid %d with server id "
				        "0x%08x and %s", DUCK_ROLE, chids[i], (unsigned)ids[k],
				        back ? "connected back" : "sent a message (no "
				        "connection seen)");
				return 0;
			}
			hbdsi_close(c);
			hbdsi_init(c);
		}
	}
	log_msg("step d: %u ConnectRequest(s), none reached %s", tries, DUCK_ROLE);
	say("note %s: no answer on %d channel(s)", DUCK_IFACE, n);
	return -1;
}

static void log_attach(const struct hbdsi_attach_reply *r)
{
	log_msg("step d: server nd %u pid %u chid %u, word0 0x%x, server id <%u.%u>, "
	        "client id <%u.%u>", (unsigned)r->nd, (unsigned)r->pid,
	        (unsigned)r->chid, (unsigned)r->word0, (unsigned)r->server_id[0],
	        (unsigned)r->server_id[1], (unsigned)r->client_id[0],
	        (unsigned)r->client_id[1]);
}

static void cmd_connect(struct hbdsi *c, int *connected)
{
	int rc;

	if (*connected) {
		say("err already connected");
		return;
	}
	if (duck_attach(c) == 0) {
		log_attach(&c->r);
		say("wait %u ConnectRequest", DUCK_TIMEOUT_MS);
		rc = hbdsi_connect(c, DUCK_TIMEOUT_MS);
		if (rc != EOK) {
			log_msg("RESULT step d: ConnectRequest: %s", strerror(rc));
			say("err ConnectRequest: %s", strerror(rc));
			return;
		}
	} else {
		/* the broker does not know it: the way its own children go */
		log_msg("step d: no attach to %s through the broker, going straight to "
		        "MMI3GApplication", DUCK_IFACE);
		hbdsi_close(c);
		hbdsi_init(c);
		if (duck_internal(c) != 0) {
			say("err %s: no connection", DUCK_IFACE);
			return;
		}
	}
	*connected = 1;
	say("ok %u %u 0x%08x.%u", (unsigned)c->r.pid, (unsigned)c->r.chid,
	    (unsigned)c->r.server_id[0], (unsigned)c->r.server_id[1]);
}

/* RequestAudioFocus / ReleaseAudioFocus(src, eSNK_SPEAKERS). */
static void cmd_focus(struct hbdsi *c, int connected, const char *arg)
{
	i32 args[2] = { 0, SGAM_SNK_SPEAKERS };
	int on = 0, rc;
	unsigned t0;
	u32 id;

	if (sscanf(arg, "%d %d", &args[0], &on) != 2) {
		say("err usage: focus <src> <0|1>");
		return;
	}
	if (!connected) {
		say("err not connected");
		return;
	}
	id = on ? SGAM_REQ_AUDIO_FOCUS : SGAM_REQ_RELEASE_FOCUS;
	say("wait %u %s(%d)", DUCK_TIMEOUT_MS,
	    on ? "RequestAudioFocus" : "ReleaseAudioFocus", (int)args[0]);
	t0 = log_ms();
	rc = hbdsi_request(c, id, args, 2, DUCK_TIMEOUT_MS);
	log_msg("step d: %s(%d, %d) = request %u: %s after %u ms",
	        on ? "RequestAudioFocus" : "ReleaseAudioFocus", (int)args[0],
	        (int)args[1], (unsigned)id, strerror(rc), log_ms() - t0);
	say("ok %d %u", rc, log_ms() - t0);
}

int duck_helper_main(const char *sd_path)
{
	char logpath[256], line[256];
	struct hbdsi c;
	int connected = 0;

	out_dir = var_dir(sd_path);
	snprintf(logpath, sizeof(logpath), "%s/hello5_helper.log", out_dir);
	log_open(logpath);
	log_msg("hello5 helper start, pid=%d", (int)getpid());
	hbdsi_init(&c);
	while (fgets(line, sizeof(line), stdin) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		log_msg("helper got: %s", line);
		if (strcmp(line, "quit") == 0)
			break;
		if (strcmp(line, "connect") == 0)
			cmd_connect(&c, &connected);
		else if (strncmp(line, "focus ", 6) == 0)
			cmd_focus(&c, connected, line + 6);
		else if (strcmp(line, "rx") == 0)
			say("ok %u %u %u %u", c.rx_msgs[0], c.rx_msgs[1], c.rx_pulses[0],
			    c.rx_pulses[1]);
		else
			say("err unknown command");
	}
	hbdsi_close(&c);
	log_msg("hello5 helper exit");
	log_close();
	return 0;
}
