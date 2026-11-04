/*
 * video.c - tests T1 and T2 of docs/phone-mirroring.md: does the unit's
 * DM642 decoder play our files, and can it be shown from our app?
 *
 * Each clip (made by media/mkclips.sh, CLIP_S seconds of a moving test
 * picture with its name and a frame counter burnt in) is played by the
 * unit's own io-media test client, testapp-file (graph type trackplayer).
 * Its default output, /dev/snd/pcmPreferredp, cannot be attached on this
 * unit (12.10 v2: iom_attach_output_to_graph iom_errno=0x10001 for every
 * clip and even with no URL), so it gets -o with the outputs the unit's
 * own MME uses (outputdevices table in efs-system's mme SQL): io-media
 * mixer inputs "mixer:DSPOUT_*?delay=130". They are tried in turn until
 * one attaches; the one that worked is kept for the next clips. Meanwhile
 * we register a layer manager *video* displayable (cid 41) and show only
 * it: flags LMGR_VIDEO plus the capture unit bit (layermanagerV2
 * CVideoDisplayable, 0x08053238: bit 0 = video, bit 8 = VCAP 0, bit 9 =
 * VCAP 1), then ask dev-videoctrl to select that capture unit's input and
 * enable it (see vc_capture). attempts[] are the two candidate paths, one
 * after the other while the clip plays.
 *
 * The driver is asked to turn the knob once whenever the moving picture is
 * visible; the /dev/ipc/ch8 messages per attempt are the "seen" signal.
 * Evidence per clip in hello6/:
 *   play_<k>.log          testapp-file's io-media events (TIME = progress)
 *   query_<k>.log         io-media's view of the playing graph, before the
 *                         attempts (see query_graph)
 *   hogs_<k>.txt          hbhogs CPU load per thread during playback
 * and io-media's / dev-videoctrl's own slog lines ("NEDL: Enabling Capture",
 * "VP2: vcap enabled", ...) end up in hello6/slog_video.txt (main.c).
 *
 * 13.10: every clip but the 800-wide, fragmented and MPEG-PS ones played to
 * its end (TIME events up to 8 s, INPUT_EOF) and both VCAPs stayed black.
 * 19.10: the query showed io-media did take clip 1 as video ("video ('MPG ',
 * 25Hz 720x576)", get_video_info 720x576), and every capture unit stayed
 * black again: capture was never enabled (vc_capture). 19.10 v2: clip 1
 * on screen through attempt 2 (VCAP 0 input 1), attempt 1 black. Now
 * every clip runs again, each through both attempts.
 *
 * The two "growing" clips are written to the card while they play, at
 * real-time rate after a 2 s head start: can io-media read a file that is
 * still being written (the cheapest stand-in for a live stream)?
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "hello6.h"
#include "lmgr.h"
#include "log.h"
#include "proc.h"
#include "quit.h"
#include "texts.h"

#define VCID         41
#define LMGR_VIDEO   0x001
#define LMGR_VCAP0   0x100
#define LMGR_VCAP1   0x200
#define LABEL_MS     6000
#define START_MS     1000    /* player start-up before the first displayable */
#define BIND_MS      500     /* layer manager binds the VCAP when it shows us */
#define ATTEMPT_MS   7000    /* START_MS + query + 2 attempts < the clip;
                                * 10 s until 30.10, knob turned 2.4-4 s in */
#define QUERY_MS     2500    /* testapp-cmdline: run the script and quit */
#define END_WAIT_MS  8000    /* for the player to finish by itself */
#define HEAD_S       2       /* growing clips: seconds written before playing */

/* Ways to put the decoder's picture on screen, tried in this order while
 * the clip plays. vp/source are dev-videoctrl's (/etc/videoctrl-Audi3G.cfg):
 * VP 1 = VCAP 0, whose input 1 is the DM642's video port; VP 2 = VCAP 1,
 * fed only by "Partial Decode". io-media runs with NMSDK_DSP_PD=1 (srv-starter
 * environment) and its render manager serves VCAP 1 itself (nMSDKIPC:
 * CaptureUnitAcquire, LayerAcquire, VideoEnable), so VCAP 1 goes first. */
static const struct attempt {
	int vcap, vp, source;
	const char *what;
} attempts[] = {
	{ 1, 2, 0, "VCAP 1 = partial decode" },
	{ 0, 1, 1, "VCAP 0 input 1 = DM642 video port" },
};
#define NATTEMPTS ((int)(sizeof(attempts) / sizeof(attempts[0])))

struct result {
	int ran, url_style;          /* 0 plain path, 1 file:// */
	int output;                  /* index into outputs[], -1 = none attached */
	int player_status, stopped;
	int time_events, errors;
	long long max_pos_ms;
	unsigned knob[NATTEMPTS];
	int reg_rc[NATTEMPTS], reg_ok[NATTEMPTS], show_rc[NATTEMPTS];
	int vc_rc[NATTEMPTS];        /* first non-zero videoctrl devctl rc, -1 no fd */
	int video;                   /* from the query: 1 video stream, 0 none, -1 ? */
	unsigned vw, vh;             /* iom_get_video_info */
	char error[100];
};

/* ------------------------------------------------------------------ */
/* dev-videoctrl                                                       */

/* /dev/videoctrl, dev-videoctrl's resource manager. The HMI
 * (MMI3GApplication CVideoControlInterface) drives it with
 * DEVCTL_VP_SET_SOURCE, DEVCTL_VP_ENABLE and DEVCTL_UPDATE; codes and
 * payloads from dev-videoctrl's io_devctl (0x08042640, P1001):
 *   {int vp, int value}, exactly 8 bytes: each stores the value for the VP
 *   and marks it pending; UPDATE (4 bytes, 0) applies every pending value
 *   (VCAP VPs: gf_vcap_set_input_source / gf_vcap_enable or _disable, then
 *   gf_vcap_update) and slogs "VP<n>: source changed to <s>" / "VP<n>: vcap
 *   enabled".
 * The layer manager only attaches the VCAP and binds it to the video layer
 * (CVideoDisplayable::renderLayer); nobody enables capture unless the HMI
 * asks dev-videoctrl, which is why a video displayable alone stayed black on
 * 12.10 and 13.10. Note: a payload dev-videoctrl rejects is ignored and
 * still answered with EOK; only the slog lines confirm. */
#define VC_PATH            "/dev/videoctrl"
#define VC_DCMD_SET_SOURCE 0x80080501u /* vp, input                     */
#define VC_DCMD_UPDATE     0x80040507u /* 0: apply the pending values   */
#define VC_DCMD_ENABLE     0x8008050bu /* vp, 0 = disable, 1 = enable   */

static int vc_fd = -1;

static int vc_set(unsigned dcmd, int vp, int value)
{
	int data[2] = { vp, value };

	return vc_fd < 0 ? -1 : devctl(vc_fd, (int)dcmd, data, sizeof(data), NULL);
}

static int vc_update(void)
{
	int zero = 0;

	return vc_fd < 0 ? -1 : devctl(vc_fd, (int)VC_DCMD_UPDATE, &zero, sizeof(zero), NULL);
}

/* Select the attempt's input and switch its capture unit on (on = 1) or
 * off again (on = 0; VCAP 0 then goes back to input 0, the ADV7180, as
 * dev-videoctrl sets it up at boot). Returns the first non-zero rc. */
static int vc_capture(const struct attempt *a, int on)
{
	int rc_src = 0, rc_en, rc_up;

	if (vc_fd < 0)
		vc_fd = open(VC_PATH, O_RDWR);
	if (vc_fd < 0) {
		log_msg("  %s: %s", VC_PATH, strerror(errno));
		return -1;
	}
	if (on || a->source != 0)
		rc_src = vc_set(VC_DCMD_SET_SOURCE, a->vp, on ? a->source : 0);
	rc_en = vc_set(VC_DCMD_ENABLE, a->vp, on);
	rc_up = vc_update();
	log_msg("  videoctrl VP %d: source %d rc=%d, %s rc=%d, update rc=%d", a->vp,
	        on ? a->source : 0, rc_src, on ? "enable" : "disable", rc_en, rc_up);
	return rc_src ? rc_src : rc_en ? rc_en : rc_up;
}

/* ------------------------------------------------------------------ */
/* growing file                                                        */

struct grow {
	int fd;
	uint8_t *data;
	long size, done, head;
	unsigned t0;
};

static int grow_start(struct grow *g, const char *src, const char *dst)
{
	FILE *f = fopen(src, "rb");

	memset(g, 0, sizeof(*g));
	g->fd = -1;
	if (f == NULL)
		return -1;
	fseek(f, 0, SEEK_END);
	g->size = ftell(f);
	fseek(f, 0, SEEK_SET);
	g->data = malloc((size_t)g->size);
	if (g->data == NULL || fread(g->data, 1, (size_t)g->size, f) != (size_t)g->size) {
		fclose(f);
		free(g->data);
		g->data = NULL;
		return -1;
	}
	fclose(f);
	g->fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (g->fd < 0) {
		free(g->data);
		g->data = NULL;
		return -1;
	}
	g->head = g->size * HEAD_S / CLIP_S;
	g->done = write(g->fd, g->data, (size_t)g->head);
	if (g->done < 0)
		g->done = 0;
	g->t0 = log_ms();
	log_msg("  growing %s: %ld bytes, %ld written up front", dst, g->size, g->done);
	return 0;
}

static void grow_tick(struct grow *g)
{
	double frac;
	long want;
	ssize_t n;

	if (g == NULL || g->fd < 0 || g->done >= g->size)
		return;
	frac = (log_ms() - g->t0) / (CLIP_S * 1000.0);
	want = g->head + (long)(frac * (double)g->size);
	if (want > g->size)
		want = g->size;
	if (want <= g->done)
		return;
	n = write(g->fd, g->data + g->done, (size_t)(want - g->done));
	if (n > 0)
		g->done += n;
	if (g->done >= g->size)
		log_msg("  growing file complete after %u ms", log_ms() - g->t0);
}

static void grow_end(struct grow *g)
{
	if (g->fd >= 0)
		close(g->fd);
	free(g->data);
	g->fd = -1;
	g->data = NULL;
}

/* Sleep ms in 100 ms steps, feeding a growing file. Non-zero: stop. */
static int hold(unsigned ms, struct grow *g)
{
	unsigned t0 = log_ms();

	while (log_ms() - t0 < ms) {
		grow_tick(g);
		if (sleep_quit(100))
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* video displayable                                                   */

static void show_video(int i, struct result *r)
{
	const int vcap = attempts[i].vcap;
	const int flags = LMGR_VIDEO | (vcap ? LMGR_VCAP1 : LMGR_VCAP0);
	int cid = VCID, rc, ok = 0;

	lm_unregister_displayable(VCID);
	rc = lm_register_displayable(VCID, SCREEN_W, SCREEN_H, flags, &ok);
	log_msg("  register video displayable %d, flags 0x%03x (VCAP %d): rc=%d %s, result=%d",
	        VCID, flags, vcap, rc, rc ? strerror(rc) : "ok", ok);
	r->reg_rc[i] = rc;
	r->reg_ok[i] = rc == 0 && ok;
	r->show_rc[i] = -1;
	if (r->reg_ok[i]) {
		rc = lm_show_displayables(0, &cid, 1);
		r->show_rc[i] = rc;
		log_msg("  lm_show_displayables(0, {%d}): rc=%d %s", VCID, rc,
		        rc ? strerror(rc) : "ok");
	}
}

static void hide_video(void)
{
	int rc = lm_unregister_displayable(VCID);

	log_msg("  unregister video displayable %d: rc=%d", VCID, rc);
}

/* ------------------------------------------------------------------ */

static void parse_player_log(const char *path, struct result *r)
{
	char line[300];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		if (strstr(line, "TIME;") != NULL) {
			r->time_events++;
			p = strstr(line, "cur=");
			if (p != NULL) {
				long long cur = strtoll(p + 4, NULL, 10);

				if (cur > r->max_pos_ms)
					r->max_pos_ms = cur;
			}
		}
		if (strstr(line, "iom_errno") != NULL || strstr(line, "ERROR") != NULL ||
		    strstr(line, "failed") != NULL || strstr(line, "Could not") != NULL ||
		    strstr(line, "No such") != NULL) {
			if (r->errors++ == 0) {
				snprintf(r->error, sizeof(r->error), "%.*s",
				         (int)strcspn(line, "\r\n"), line);
			}
		}
	}
	fclose(f);
}

/* testapp-cmdline commands for query_graph, one per line. */
static const char query_script[] =
	"GetStatus\n"
	"GetTrackInfo\n"
	"GetVideoInfo\n"
	"GetErrorInfo\n"
	"quit\n";

/* Ask io-media about the clip's graph while it plays. testapp-cmdline
 * attaches to testapp-file's graph by name (-a hello6; graph type
 * trackplayer is its default) without an audio output (-O) or an event
 * reader (-e), runs query_script and quits. Quitting closes only its own
 * handle: testapp-file still holds the graph. Output in query_<k>.log;
 * r->video from GetTrackInfo (", video ('<codec>', ...)" or ", no video"),
 * r->vw/vh from GetVideoInfo. */
static void query_graph(int k, struct result *r)
{
	const char *cmdline = tool_path("testapp-cmdline");
	char script[256], qlog[256], name[32], line[300];
	pid_t pid;
	int status = 0;
	FILE *f;

	r->video = -1;
	if (cmdline == NULL)
		return;
	out_path(script, sizeof(script), "iomquery.txt");
	f = fopen(script, "w");
	if (f == NULL)
		return;
	fputs(query_script, f);
	fclose(f);
	snprintf(name, sizeof(name), "query_%d.log", k + 1);
	out_path(qlog, sizeof(qlog), name);
	{
		char *argv[] = { (char *)cmdline, "-a", "hello6", "-O", "-e", script, NULL };

		pid = proc_start(qlog, argv);
	}
	if (pid < 0) {
		log_msg("  query: could not start %s: %s", cmdline, strerror(errno));
		return;
	}
	if (proc_wait(pid, QUERY_MS, &status) != 0)
		status = proc_stop(pid);

	f = fopen(qlog, "r");
	while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		if (strstr(line, ", no video") != NULL)
			r->video = 0;
		else if (strstr(line, ", video ('") != NULL)
			r->video = 1;
		p = strstr(line, "get_video_info() returned w=");
		if (p != NULL)
			sscanf(p, "get_video_info() returned w=%u h=%u", &r->vw, &r->vh);
	}
	if (f != NULL)
		fclose(f);
	log_msg("  query (%s): video stream %s, video info %ux%u", proc_status_str(status),
	        r->video < 0 ? "unknown" : r->video ? "yes" : "NO", r->vw, r->vh);
}

/* testapp-file -o candidates, from the unit's MME outputdevices table.
 * TDM2/TDM3 first: secondary zones, so the clips' sound is least likely to
 * come out of the front speakers. NULL = testapp-file's default. */
static const char *const outputs[] = {
	"mixer:DSPOUT_TDM2_20?delay=130",
	"mixer:DSPOUT_TDM3_20?delay=130",
	"mixer:DSPOUT_TDM1_51_SAD?delay=130",
	NULL,
};
#define NOUTPUTS ((int)(sizeof(outputs) / sizeof(outputs[0])))
static int first_output;        /* the one that attached last time */

static pid_t start_player(const char *player, const char *output, const char *url,
                          const char *log_file)
{
	char *argv[] = { (char *)player, "-n", "500", "-f", "hello6", "-o", (char *)output,
	                 (char *)url, NULL };
	pid_t pid;

	if (output == NULL) {           /* drop "-o <output>" */
		argv[5] = (char *)url;
		argv[6] = NULL;
	}
	pid = proc_start(log_file, argv);
	log_msg("  %s -n 500 -f hello6%s%s %s: pid %d", player, output ? " -o " : "",
	        output ? output : "", url, (int)pid);
	return pid;
}

static long file_size(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 ? (long)st.st_size : 0;
}

/* Did the player's log, from byte `from` on (this attempt), say the output
 * would not attach? */
static int output_failed(const char *log_file, long from)
{
	char line[300];
	int bad = 0;
	FILE *f = fopen(log_file, "r");

	if (f == NULL)
		return 0;
	fseek(f, from, SEEK_SET);
	while (!bad && fgets(line, sizeof(line), f) != NULL)
		bad = strstr(line, "iom_attach_output") != NULL;
	fclose(f);
	return bad;
}

/* One clip. Returns non-zero when hello6 should stop. */
static int run_clip(int k, const struct clip *c, struct result *r)
{
	const char *player = tool_path("testapp-file"), *hogs = tool_path("hbhogs");
	char src[256], url[300], play_log[256], hogs_log[256];
	char title[96], body[700], name[64];
	struct grow g, *gp = NULL;
	pid_t pid, hogs_pid = -1;
	int status = 0, i, stop = 0, reaped = 0;

	memset(r, 0, sizeof(*r));
	r->video = -1;
	snprintf(src, sizeof(src), "%s/clips/%s", sd_card_path(), c->file);
	log_msg("== video %d/%d: %s (%s)", k + 1, NCLIPS, c->title, c->file);
	if (player == NULL) {
		log_msg("FAIL testapp-file not found, no video tests");
		return 0;
	}
	if (access(src, R_OK) != 0) {
		log_msg("FAIL clip %s missing: %s", src, strerror(errno));
		return 0;
	}
	r->ran = 1;

	snprintf(title, sizeof(title), VIDEO_TITLE_FMT, k + 1, NCLIPS, c->title);
	snprintf(body, sizeof(body), VIDEO_BODY_FMT, c->what);
	label_show(title, body, FOOTER_VIDEO);
	if (hold(LABEL_MS, NULL)) {
		label_hide();
		return 1;
	}
	label_hide();

	snprintf(name, sizeof(name), "play_%d.log", k + 1);
	out_path(play_log, sizeof(play_log), name);
	snprintf(name, sizeof(name), "hogs_%d.txt", k + 1);
	out_path(hogs_log, sizeof(hogs_log), name);

	snprintf(url, sizeof(url), "%s", src);
	if (c->grow) {
		const char *ext = strrchr(c->file, '.');

		snprintf(name, sizeof(name), "grow_%d%s", k + 1, ext ? ext : "");
		out_path(url, sizeof(url), name);
		if (grow_start(&g, src, url) != 0) {
			log_msg("FAIL could not start growing %s from %s", url, src);
			return 0;
		}
		gp = &g;
	}

	if (hogs != NULL) {
		char *argv[] = { (char *)hogs, "-t", "2000", "-n", "12", NULL };

		hogs_pid = proc_start(hogs_log, argv);
	}
	/* Start the player; when it exits within START_MS without playing,
	 * try the next output (it said attach_output) or the file:// URL form
	 * (it failed some other way, presumably on the input). */
	{
		int o = first_output, tries = 0, furl_tried = 0;
		char furl[320];

		snprintf(furl, sizeof(furl), "file://%s", url);
		r->output = -1;
		for (;;) {
			const char *u = r->url_style ? furl : url;
			long from = file_size(play_log);

			pid = start_player(player, outputs[o], u, play_log);
			if (pid < 0) {
				log_msg("FAIL could not start %s: %s", player, strerror(errno));
				goto done;
			}
			r->output = o;
			stop = hold(START_MS, gp);
			if (stop || proc_wait(pid, 0, &status) != 0)
				break;                  /* still running: playing, we hope */
			reaped = 1;
			log_msg("  player exited early (%s)", proc_status_str(status));
			if (++tries >= NOUTPUTS + 1)
				break;
			if (output_failed(play_log, from)) {
				r->output = -1;
				if ((o = (o + 1) % NOUTPUTS) == first_output)
					break;          /* every output refused */
			} else if (!furl_tried) {
				furl_tried = r->url_style = 1;
			} else {
				break;
			}
			reaped = 0;
		}
		if (r->output >= 0)
			first_output = r->output;
	}

	if (!stop)
		query_graph(k, r);
	if (lm_open() != 0) {
		log_msg("FAIL lm_open for the video displayable");
	} else {
		/* Show the displayable first (the layer manager binds its VCAP
		 * to the video layer), then have dev-videoctrl select the input
		 * and enable capture, as the HMI does. Capture off again before
		 * the displayable goes. */
		for (i = 0; i < NATTEMPTS && !stop; i++) {
			const struct attempt *a = &attempts[i];
			unsigned k0;

			log_msg("  -- attempt %d/%d: %s", i + 1, NATTEMPTS, a->what);
			show_video(i, r);
			stop = hold(BIND_MS, gp);
			if (!stop) {
				r->vc_rc[i] = vc_capture(a, 1);
				k0 = knob_events();
				stop = hold(ATTEMPT_MS, gp);
				r->knob[i] = knob_events() - k0;
				log_msg("  %s: %u knob events", a->what, r->knob[i]);
				vc_capture(a, 0);
			}
			hide_video();
		}
		lm_close();
	}

	if (pid >= 0 && !reaped) {
		if (stop || proc_wait(pid, END_WAIT_MS, &status) != 0) {
			r->stopped = 1;
			status = proc_stop(pid);
		}
	}
	r->player_status = status;
	if (pid >= 0)
		log_msg("  player: %s%s", proc_status_str(status), r->stopped ? " (stopped by us)" : "");
done:
	if (hogs_pid >= 0)
		proc_stop(hogs_pid);
	if (gp != NULL) {
		grow_tick(gp);
		grow_end(gp);
	}
	parse_player_log(play_log, r);
	log_msg("  result: %d TIME events, position %lld ms, %d error lines%s%s",
	        r->time_events, r->max_pos_ms, r->errors, r->errors ? ": " : "", r->error);
	return stop;
}

void video_tests(void)
{
	struct result res[NCLIPS];
	int k, i, n = 0;

	memset(res, 0, sizeof(res));
	log_msg("== video tests (%d of %d clips)", NCLIPS, NCLIPS);
	for (k = 0; k < NCLIPS; k++) {
		n = k + 1;
		if (run_clip(k, &clips[k], &res[k]))
			break;
	}
	/* in case a stop left it registered */
	if (lm_open() == 0) {
		lm_unregister_displayable(VCID);
		lm_close();
	}
	if (vc_fd >= 0)
		close(vc_fd);
	vc_fd = -1;

	summary_add("video: TIME = io-media progress events, knob = turns seen per attempt");
	for (i = 0; i < NATTEMPTS; i++)
		summary_add("  attempt %d: %s", i + 1, attempts[i].what);
	for (k = 0; k < n; k++) {
		const struct result *r = &res[k];

		if (!r->ran) {
			summary_add("%2d %-22s not run", k + 1, clips[k].file);
			continue;
		}
		summary_add("%2d %-22s TIME %2d pos %4.1fs vid %s %ux%u knob %u/%u%s",
		            k + 1, clips[k].file, r->time_events, r->max_pos_ms / 1000.0,
		            r->video < 0 ? "?" : r->video ? "yes" : "NO", r->vw, r->vh,
		            r->knob[0], r->knob[1], r->url_style ? " file://" : "");
		log_msg("RESULT video_%d %s: time_events=%d pos_ms=%lld errors=%d video=%d "
		        "video_info=%ux%u player=%s url=%s output=%s",
		        k + 1, clips[k].file, r->time_events, r->max_pos_ms, r->errors,
		        r->video, r->vw, r->vh, proc_status_str(r->player_status),
		        r->url_style ? "file://" : "path",
		        r->output < 0 ? "none" : outputs[r->output] ? outputs[r->output] : "default");
		for (i = 0; i < NATTEMPTS; i++)
			log_msg("RESULT video_%d attempt %d (%s): knob=%u reg=%d/%d show=%d "
			        "videoctrl=%d", k + 1, i + 1, attempts[i].what, r->knob[i],
			        r->reg_rc[i], r->reg_ok[i], r->show_rc[i], r->vc_rc[i]);
	}
}
