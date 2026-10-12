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
 * we register a layer manager
 * *video* displayable (cid 41) and show only it: flags LMGR_VIDEO plus the
 * capture unit bit (layermanagerV2 CVideoDisplayable, 0x08053238: bit 0 =
 * video, bit 8 = VCAP 0, bit 9 = VCAP 1). First half of the clip on VCAP 0,
 * second half on VCAP 1; which one carries the decoder is not known.
 *
 * The driver is asked to turn the knob once whenever the moving picture is
 * visible; the /dev/ipc/ch8 messages per half are the "seen" signal.
 * Evidence per clip in hello6/:
 *   play_<k>.log          testapp-file's io-media events (TIME = progress)
 *   hogs_<k>.txt          hbhogs CPU load per thread during playback
 * and io-media's own slog lines ("Configuring ... Width", "Acquiring
 * Layer ...") end up in var/hello6_slog.txt (run.sh).
 *
 * The two "growing" clips are written to the card while they play, at
 * real-time rate after a 2 s head start: can io-media read a file that is
 * still being written (the cheapest stand-in for a live stream)?
 */
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
#define HALF_MS      3500    /* START_MS + 2 halves < CLIP_S */
#define END_WAIT_MS  6000    /* for the player to finish by itself */
#define HEAD_S       2       /* growing clips: seconds written before playing */

struct result {
	int ran, url_style;          /* 0 plain path, 1 file:// */
	int output;                  /* index into outputs[], -1 = none attached */
	int player_status, stopped;
	int time_events, errors;
	long long max_pos_ms;
	unsigned knob[2];
	int reg_rc[2], reg_ok[2], show_rc[2];
	char error[100];
};

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

static void show_video(int vcap, struct result *r)
{
	const int flags = LMGR_VIDEO | (vcap ? LMGR_VCAP1 : LMGR_VCAP0);
	int cid = VCID, rc, ok = 0;

	lm_unregister_displayable(VCID);
	rc = lm_register_displayable(VCID, SCREEN_W, SCREEN_H, flags, &ok);
	log_msg("  register video displayable %d, flags 0x%03x (VCAP %d): rc=%d %s, result=%d",
	        VCID, flags, vcap, rc, rc ? strerror(rc) : "ok", ok);
	r->reg_rc[vcap] = rc;
	r->reg_ok[vcap] = rc == 0 && ok;
	r->show_rc[vcap] = -1;
	if (r->reg_ok[vcap]) {
		rc = lm_show_displayables(0, &cid, 1);
		r->show_rc[vcap] = rc;
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
	int status = 0, half, stop = 0, reaped = 0;

	memset(r, 0, sizeof(*r));
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

	if (lm_open() != 0) {
		log_msg("FAIL lm_open for the video displayable");
	} else {
		for (half = 0; half < 2 && !stop; half++) {
			unsigned k0;

			show_video(half, r);
			k0 = knob_events();
			stop = hold(HALF_MS, gp);
			r->knob[half] = knob_events() - k0;
			log_msg("  VCAP %d: %u knob events", half, r->knob[half]);
		}
		hide_video();
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
	int k, n = 0;

	memset(res, 0, sizeof(res));
	log_msg("== video tests (%d clips)", NCLIPS);
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

	summary_add("video: TIME = io-media progress events, knob = turns seen per VCAP");
	for (k = 0; k < n; k++) {
		const struct result *r = &res[k];

		if (!r->ran) {
			summary_add("%2d %-22s not run", k + 1, clips[k].file);
			continue;
		}
		summary_add("%2d %-22s TIME %2d pos %4.1fs knob %u/%u reg %s/%s%s",
		            k + 1, clips[k].file, r->time_events, r->max_pos_ms / 1000.0,
		            r->knob[0], r->knob[1], r->reg_ok[0] ? "ok" : "NO",
		            r->reg_ok[1] ? "ok" : "NO", r->url_style ? " file://" : "");
		log_msg("RESULT video_%d %s: time_events=%d pos_ms=%lld errors=%d knob_vcap0=%u "
		        "knob_vcap1=%u reg0=%d/%d reg1=%d/%d show0=%d show1=%d player=%s url=%s "
		        "output=%s",
		        k + 1, clips[k].file, r->time_events, r->max_pos_ms, r->errors,
		        r->knob[0], r->knob[1], r->reg_rc[0], r->reg_ok[0], r->reg_rc[1],
		        r->reg_ok[1], r->show_rc[0], r->show_rc[1],
		        proc_status_str(r->player_status), r->url_style ? "file://" : "path",
		        r->output < 0 ? "none" : outputs[r->output] ? outputs[r->output] : "default");
	}
}
