/*
 * hello5 - sound from our app: which audio output reaches the speakers and
 * how it behaves there. (Its knob/button log, stage 2, is hello7 now.)
 *
 * hello5 runs a list of steps (argument "steps", default "mntv"), in
 * order. Steps m, n, v and t play a ping train that hello5 renders itself
 * (pulse.c: a 0.5 s ping every 2 s) on the one output heard so far, mixer
 * m2 DSPOUT_TDM1_20_SAD: m, v and t through the unit's testapp-file, n
 * through our own io-media client (common/iom.c). Each step first
 * shows what to do with a countdown, then a PLAYING screen with the
 * question to listen for; the silence between the pings is where the
 * answer is:
 *   m  radio on: is the radio still playing between the pings (our stream
 *      mixed in), or silent (our stream replaces it while it plays)?
 *   n  radio on, our own graph: 9 pings with our volume turned down to 0
 *      for pings 4-6 (does an app's own volume work?), then 3 one-ping
 *      files queued one after another the way testapp-file chains its
 *      URLs (do they keep the 2 s beat, i.e. can an app queue sound
 *      without gaps?). Every io-media event is logged with its time since
 *      play: the start latency, which testapp-file cannot give (its
 *      stdout is fully buffered).
 *   v  radio on, the volume knob turned 3 clicks down and 3 up during the
 *      pings: do they follow the car's volume? The clicks are counted on
 *      screen and every ch8 message is logged with its time.
 *   t  the driver switches to music (MEDIA): does our output still attach
 *      while the MME plays, and does the music keep playing between pings?
 * The train is written to /tmp (RAM) first: an app should not need the SD
 * card for its sound. If io-media does not play it from there, the step
 * retries from a copy on the card. The /tmp files are deleted at exit.
 *
 * Step g is the output survey of the earlier runs: a short chime is
 * played on each output in turn, group N playing N chimes with 3 s of
 * silence between groups.
 * Whoever is in the car counts the chimes they hear in each group, and
 * notes any group that cuts the radio, which says which output reaches the
 * speakers. An output is either
 *   mK  an io-media mixer output, "mixer:DSPOUT_*" (mixers[] below): the
 *       chime is played by the unit's testapp-file, as the MME plays music
 *   D   an io-audio playback device, dindex D from mmi3g-srv-starter.cfg
 *       (0 ent3, 1 navi, 2 SDS, 3 BT-Tr, 4 NAD, 5 tones1), written by us
 * Default order m2,m1,m6,m4,m7:
 *   group 1 = m2  DSPOUT_TDM1_20_SAD
 *   group 2 = m1  DSPOUT_TDM1_51
 *   group 3 = m6  DSPOUT_TDM3_20_SAD
 *   group 4 = m4  DSPOUT_TDM2_20_SAD
 *   group 5 = m7  DSPOUT_TDM3_20
 *
 * What the earlier runs showed (radio playing each time):
 *   13.10  all six io-audio devices took every chime in real time ("hb_fpga:
 *          Play buffer underrun strmN" as each ran dry): nothing heard.
 *   19.10  m0 TDM1_51_SAD: "Mixer entry for DSPOUT_TDM1_51_SAD: Already in
 *          use" - the MME's own player holds it (it is the MME's
 *          "defaultoutput"). m3 TDM1_20 and m5 TDM2_20 played every chime to
 *          INPUT_EOF; nothing heard, but TDM1_20 cut the radio while it
 *          played. tones1, navi, ent3 as on 13.10.
 * The sound leaves the MMI over MOST to the external amplifier. TDM1 is
 * the main entertainment stream: the radio (DSPIN_TDM1_20, routed by
 * MMI3GApplication's mixer graph) goes out on DSPOUT_TDM1_51_SAD, 5.1 with
 * a "SAD" header (io-media: AMixerSadHeaderSet). TDM1_20 is the same TDM
 * port in another format: switching it to 2.0 without the header broke the
 * stream the amplifier was decoding, so the radio stopped and the chime,
 * not in the expected format, was not played either [inferred]. The other
 * streams (TDM2/TDM3, the io-audio devices) reach the amplifier on channels
 * it only mixes in when MMI3GApplication's audio management switches that
 * source on (CAmpLC: announcement, navi, beep ...) [inferred].
 * So this run tries the TDM1 variants the amplifier may still decode (2.0
 * with the header, 5.1 without) and the "_SAD" forms of TDM2/TDM3, plus
 * TDM3 2.0, the one plain mixer output not yet tried. A group that cuts the
 * radio uses the radio's stream; one whose chime is heard is the way in.
 *   19.10 v2  all five (m2,m1,m6,m4,m7) attached and played every chime to
 *          INPUT_EOF. The listener thinks one chime was heard near the start
 *          (group 1, m2 TDM1 2.0 SAD, or group 2, m1 TDM1 5.1), and turned
 *          the volume knob ~1.3 s after group 1's chime. Groups 3-5 were not
 *          heard. The 600 ms chime was too short to place, so the chime is
 *          now 2.5 s long (sdk/mkbeep.py); same outputs, same order.
 *   21.10  same five outputs, 2.5 s chime, radio on: group 1 (m2 TDM1 2.0
 *          SAD) was HEARD. Group 2 (m1 TDM1 5.1) muted the car audio while
 *          it played, its chimes not heard. Groups 3-5 silent. io-media saw
 *          no difference between them (INPUT_EOF, exit after ~2.5 s each).
 *          That answered the survey, so g is no longer in the default
 *          steps: m, n, v and t ask what the TDM1 2.0 SAD output does
 *          next to the radio, the volume knob and the music, and whether
 *          our own client can drive it.
 *   25.10  steps m, n, v, t (the phone video's sound, 880 Hz band, is the
 *          better witness here than the listener, who heard only step m):
 *          m  all 6 pings recorded, ~15 dB over the cabin noise.
 *          n  all 12 pings recorded at step m's level: our own io-media
 *             client plays. Volume 30 for pings 4-6 was ~3 dB down at most
 *             and pings 7-9 did not come back up: not conclusive, so it
 *             is 0 now. Each queued file added ~350 ms of silence (pings
 *             10-12 2.35 s apart): no gapless queueing. hello5 then
 *             replayed the last file on FINISHED (fixed: one FINISHED per
 *             queue, not per file).
 *          v  ping 1 recorded; the knob was turned at once and counted 148
 *             clicks down, 37 up; pings 2-10 not recorded. So the pings
 *             follow the car volume, or the volume ended at 0.
 *          t  nothing recorded, probably because v left the volume down.
 *             So v now runs last (default "mntv").
 *   28.10  m: the radio is muted for the whole train, back ~0.2 s after.
 *          n: volume 0 changes nothing. The run ended in t's countdown.
 *   30.10  all four answered. m, n: radio muted, pings heard; pings 4-6
 *          as loud as the rest. t: Bluetooth music muted for the whole
 *          train, audible the moment it ended. v: the pings follow the
 *          volume knob (~21 dB down, then ~12 dB up again).
 *
 * For an io-audio device each chime also measures whether the hardware
 * consumed it: after the write, snd_pcm_plugin_prepare() is polled until it
 * stops returning EBUSY (on 12.10 v2 EBUSY lasted as long as the clip
 * played). About the clip's length means the device played it in real time,
 * audible or not. Polling, not drain, so a device that never plays cannot
 * block the test. Each group runs on its own thread: if a write blocks for
 * good, the group is written off after its deadline and the test goes on.
 *
 * The screen (h5ui.c) shows the group in its own colour, a huge group number,
 * "chime k of N" and a white PLAYING box while a chime sounds, plus the log
 * clock, so a phone video says which chime was heard. The PLAYING frame is
 * presented just before the write that starts the chime. For a mixer group
 * it stays up while testapp-file runs: ~100 ms of graph set-up, then the
 * chime.
 *
 * Every /dev/ipc/ch8 message is logged (raw) while hello5 runs, and step
 * v counts the volume knob's detents (byte 4 up, 5 down). The full knob
 * and button tester is hello7.
 *
 * Audio goes through QNX Sound Architecture (QSA / libasound). The plugin
 * layer (snd_pcm_plugin_*) resamples/upmixes the clip to whatever the FPGA
 * wants. beep.wav is rendered by sdk/mkbeep.py: a 2.5 s chime (a short A5,
 * then a held D6) at 44.1 kHz mono. The QSA struct ABI in <sys/asoundlib.h> is reconstructed
 * from the device's own libasound.so.2.
 *
 * Stops on SIGTERM/INT/HUP, SD removal, its own run timer, or the watchdog
 * (src/common/quit.c). No key or button stops it.
 *
 * usage: hello5 <sd_path> [steps] [outputs]
 *   steps: any of "mnvtg", run in that order (default mntv)
 *   outputs: step g's groups, comma-separated, one group each: mK =
 *   mixers[K], a number = an io-audio dindex (default m2,m1,m6,m4,m7)
 * Step g plays <sd_path>/beep.wav.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/asoundlib.h>

#include "log.h"
#include "proc.h"
#include "quit.h"
#include "h5ui.h"
#include "iom.h"
#include "pulse.h"

#define DEFAULT_STEPS   "mntv"
#define DEFAULT_OUTPUTS "m2,m1,m6,m4,m7"
#define MAX_GROUPS      6
#define LEAD_MS         2000    /* silence before the first group */
#define CHIME_GAP_MS    400     /* between the chimes of one group */
#define GROUP_GAP_MS    3000    /* between groups */
#define BUSY_CAP_MS     4000    /* stop waiting for one chime after this */
#define BUSY_POLL_MS    20
#define CHIME_BUDGET_MS 4500    /* group deadline: this per chime ... */
#define GROUP_EXTRA_MS  2000    /* ... plus this (open/params/close) */
#define MIXER_CAP_MS    5000    /* stop a testapp-file chime after this */
#define MIXER_BUDGET_MS 6000    /* per chime for a mixer group's deadline */
#define PLAY_POLL_MS    50      /* testapp-file exit / log check */
#define STEP_MIXER      2       /* mixers[2] TDM1 2.0 SAD, heard on 21.10 */
#define STEP_SLACK_MS   4000    /* testapp-file's cap: the train plus this */
#define STEP_DONE_MS    4000    /* the "step done" screen */
#define STEP_REDRAW_MS  250     /* the PLAYING screen, at most this often */
#define WATCHDOG_SLACK  20
#define POLL_MS         100

/* ---- a loaded PCM clip ---- */
struct clip {
	unsigned char *pcm;     /* interleaved sample data (malloc'd) */
	uint32_t       len;     /* bytes in pcm */
	uint32_t       rate;    /* Hz */
	uint16_t       voices;  /* channels */
	uint16_t       bits;    /* bits per sample */
};

static struct clip beep;
static unsigned clip_ms;
static const char *wav;         /* beep's path, for testapp-file */
static const char *player;      /* testapp-file, NULL if not found */
static const char *sd_path;

/* io-media mixer outputs: every DSPOUT_TDM* name io-media-nvidia's mixer
 * knows (its table at 0x237ed4, P1001 efs-system). m0, m5 and m7 are also
 * in the unit's MME outputdevices table (efs-system mme_data.sql), m3 in
 * MMI3GApplication's mixer strings. */
static const struct {
	const char *url, *name;
} mixers[] = {
	{ "mixer:DSPOUT_TDM1_51_SAD?delay=130", "TDM1 5.1 SAD" },  /* MME's  */
	{ "mixer:DSPOUT_TDM1_51?delay=130",     "TDM1 5.1" },
	{ "mixer:DSPOUT_TDM1_20_SAD?delay=130", "TDM1 2.0 SAD" },
	{ "mixer:DSPOUT_TDM1_20?delay=130",     "TDM1 2.0" },      /* cut radio */
	{ "mixer:DSPOUT_TDM2_20_SAD?delay=130", "TDM2 2.0 SAD" },
	{ "mixer:DSPOUT_TDM2_20?delay=130",     "TDM2 2.0" },
	{ "mixer:DSPOUT_TDM3_20_SAD?delay=130", "TDM3 2.0 SAD" },
	{ "mixer:DSPOUT_TDM3_20?delay=130",     "TDM3 2.0" },
};
#define NMIXERS ((int)(sizeof(mixers) / sizeof(mixers[0])))

/* ---- little-endian readers (the MMI is SH4 little-endian, as are WAVs) ---- */

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parse a canonical PCM .wav: walk chunks for "fmt " and "data". */
static int clip_load(const char *path, struct clip *c)
{
	unsigned char *buf;
	long size;
	uint32_t off, fmt_off = 0, data_off = 0, data_len = 0;
	uint16_t audio_fmt;
	FILE *f = fopen(path, "rb");

	if (f == NULL) {
		log_msg("clip: cannot open %s", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 44) {
		log_msg("clip: %s too small (%ld bytes)", path, size);
		fclose(f);
		return -1;
	}
	buf = malloc(size);
	if (buf == NULL || fread(buf, 1, size, f) != (size_t)size) {
		log_msg("clip: read %s failed", path);
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);

	if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
		log_msg("clip: %s is not a RIFF/WAVE file", path);
		free(buf);
		return -1;
	}
	/* chunks start at offset 12: id[4], size[4], payload */
	off = 12;
	while (off + 8 <= (uint32_t)size) {
		uint32_t clen = rd32(buf + off + 4);

		if (memcmp(buf + off, "fmt ", 4) == 0)
			fmt_off = off + 8;
		else if (memcmp(buf + off, "data", 4) == 0) {
			data_off = off + 8;
			data_len = clen;
		}
		off += 8 + clen + (clen & 1);   /* chunks are word-aligned */
	}
	if (fmt_off == 0 || data_off == 0 || data_off + data_len > (uint32_t)size) {
		log_msg("clip: %s missing/!bad fmt or data chunk", path);
		free(buf);
		return -1;
	}
	audio_fmt   = rd16(buf + fmt_off);
	c->voices   = rd16(buf + fmt_off + 2);
	c->rate     = rd32(buf + fmt_off + 4);
	c->bits     = rd16(buf + fmt_off + 14);
	if (audio_fmt != 1) {
		log_msg("clip: %s is not PCM (fmt=%u)", path, audio_fmt);
		free(buf);
		return -1;
	}
	/* keep just the sample bytes; move them to the front and shrink */
	memmove(buf, buf + data_off, data_len);
	c->pcm = buf;
	c->len = data_len;
	log_msg("clip: %s = PCM %u Hz, %u ch, %u-bit, %u bytes (%u ms)",
	        path, c->rate, c->voices, c->bits, c->len,
	        c->rate && c->voices && c->bits ?
	        c->len * 1000u / (c->rate * c->voices * (c->bits / 8)) : 0);
	return 0;
}

static int sfmt_for(uint16_t bits)
{
	switch (bits) {
	case 8:  return SND_PCM_SFMT_U8;        /* WAV 8-bit is unsigned */
	case 16: return SND_PCM_SFMT_S16_LE;
	case 24: return SND_PCM_SFMT_S24_LE;
	case 32: return SND_PCM_SFMT_S32_LE;
	}
	return -1;
}

/* params->why_failed after a rejected snd_pcm_plugin_params(): the driver's
 * own reason, which is the only thing that says which field it disliked. */
static const char *why_failed_name(int why)
{
	switch (why) {
	case 0:                             return "not set";
	case SND_PCM_PARAMS_BAD_MODE:       return "bad mode (or interleave unsupported)";
	case SND_PCM_PARAMS_BAD_START:      return "bad start_mode";
	case SND_PCM_PARAMS_BAD_STOP:       return "bad stop_mode";
	case SND_PCM_PARAMS_BAD_FORMAT:     return "bad format";
	case SND_PCM_PARAMS_BAD_RATE:       return "bad rate";
	case SND_PCM_PARAMS_BAD_VOICES:     return "bad voices";
	case SND_PCM_PARAMS_NO_CHANNEL:     return "no channel";
	}
	return "?";
}

static const char *dev_name(int device)
{
	static const char *const names[] = {
		"ent3", "navi", "SDS", "BT-Tr", "NAD", "tones1"
	};

	return device >= 0 && device < 6 ? names[device] : "?";
}

static void nap(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Sleep ms, polling for a stop request. Returns non-zero if asked to stop. */
static int wait_ms(unsigned ms)
{
	unsigned end = log_ms() + ms;

	while (log_ms() < end) {
		if (quit_poll() != QUIT_NONE)
			return 1;
		nap(POLL_MS);
	}
	return quit_poll() != QUIT_NONE;
}

/* Open one playback device and negotiate the clip's format. */
static int audio_open(int card, int device, const struct clip *c, snd_pcm_t **out)
{
	snd_pcm_channel_params_t p;
	snd_pcm_channel_setup_t s;
	snd_pcm_t *h;
	int fmt = sfmt_for(c->bits), rc;

	*out = NULL;
	if (fmt < 0) {
		log_msg("audio: unsupported %u-bit sample", c->bits);
		return -1;
	}
	rc = snd_pcm_open(&h, card, device, SND_PCM_OPEN_PLAYBACK);
	if (rc < 0) {
		log_msg("audio: snd_pcm_open(card=%d,dev=%d %s): %s", card, device,
		        dev_name(device), snd_strerror(rc));
		return -1;
	}
	/* The plugin layer builds an mmap plugin unless bit 1 of the disable
	 * mask is set (plugin_params tests `handle[0x9B0] & 2`); the standard
	 * QSA playback sequence turns mmap off before negotiating. */
	snd_pcm_plugin_set_disable(h, PLUGIN_DISABLE_MMAP);
	/* let the plugin layer resample/upmix to the hardware's fixed format */
	snd_pcm_plugin_set_src_method(h, SND_PCM_PLUGIN_CONV_AUTO);

	memset(&p, 0, sizeof(p));
	p.channel     = SND_PCM_CHANNEL_PLAYBACK;
	p.mode        = SND_PCM_MODE_BLOCK;     /* = 1 (libasound rejects 0) */
	p.interleave  = 1;
	p.format      = fmt;
	p.rate        = (int)c->rate;
	p.voices      = c->voices;
	p.start_mode  = SND_PCM_START_DATA;     /* start on first write (one-shot) */
	p.stop_mode   = SND_PCM_STOP_STOP;
	p.frag_size   = 4096;
	p.frags_min   = 1;
	p.frags_max   = -1;

	rc = snd_pcm_plugin_params(h, &p);
	if (rc < 0) {
		/* why_failed is the driver's own reason, copied back into our
		 * struct by libasound; without it "Invalid argument" says
		 * nothing about which field was rejected. */
		log_msg("audio: dev %d: snd_pcm_plugin_params: %s (why_failed=%d: %s)",
		        device, snd_strerror(rc), p.why_failed,
		        why_failed_name(p.why_failed));
		log_msg("audio: requested %u Hz, %u ch, %u-bit (sfmt=%d), "
		        "frag_size=%d frags %d..%d",
		        c->rate, c->voices, c->bits, fmt,
		        p.frag_size, p.frags_min, p.frags_max);
		snd_pcm_close(h);
		return -1;
	}

	/* negotiated read-back - for the log only */
	memset(&s, 0, sizeof(s));
	s.channel = SND_PCM_CHANNEL_PLAYBACK;
	if (snd_pcm_plugin_setup(h, &s) >= 0)
		log_msg("audio: dev %d %s: params accepted, setup %d Hz, %d ch, sfmt=%d",
		        device, dev_name(device), s.rate, s.voices, s.format);
	else
		log_msg("audio: dev %d %s: params accepted (setup read-back failed)",
		        device, dev_name(device));
	*out = h;
	return 0;
}

/* ---- stage 1: one group of chimes per output ---- */

/* An output from the command line: an io-audio device or a mixer. */
struct output {
	int device;             /* dindex, or -1 */
	int mixer;              /* index into mixers[], or -1 */
};

static void output_label(const struct output *o, char *buf, size_t size)
{
	if (o->mixer >= 0)
		snprintf(buf, size, "m%d: %s", o->mixer, mixers[o->mixer].name);
	else
		snprintf(buf, size, "device %d: %s", o->device, dev_name(o->device));
}

/* Per chime, for the group deadline. A testapp-file run lasts the clip plus
 * ~100 ms of graph set-up (19.10 v2: 640-700 ms for the old 600 ms clip). */
static unsigned chime_budget(int mixer)
{
	return mixer >= 0 ? MIXER_BUDGET_MS : CHIME_BUDGET_MS;
}

struct group {
	int      index;         /* 1-based, = number of chimes */
	int      ngroups;
	int      device;        /* io-audio dindex, or -1: ... */
	int      mixer;         /* ... index into mixers[] (else -1) */
	char     label[40];     /* "device 5: tones1" / "m2: TDM1 2.0 SAD" */
	/* written by the group's thread, read by main once done is set */
	int      opened;        /* device opened / mixer output attached */
	unsigned written;       /* chimes written in full */
	unsigned played;        /* ... of which the device finished playing */
	unsigned total_ms;      /* sum of write + busy time over played chimes */
	unsigned step;          /* what it was doing, for a stuck group */
	volatile int done;
};

enum { STEP_OPEN, STEP_PREPARE, STEP_WRITE, STEP_WAIT, STEP_GAP, STEP_CLOSE };

static const char *step_name(unsigned step)
{
	switch (step) {
	case STEP_OPEN:    return "open/params";
	case STEP_PREPARE: return "prepare";
	case STEP_WRITE:   return "write";
	case STEP_WAIT:    return "waiting for playback";
	case STEP_GAP:     return "gap";
	case STEP_CLOSE:   return "close";
	}
	return "?";
}

/* Poll prepare until the device is no longer busy with the last chime.
 * Returns the prepare result (0 = idle again, -EBUSY = still busy at the
 * cap) and the time waited in *busy_ms. */
static int wait_played(snd_pcm_t *h, unsigned *busy_ms)
{
	unsigned t0 = log_ms();
	int rc;

	for (;;) {
		rc = snd_pcm_plugin_prepare(h, SND_PCM_CHANNEL_PLAYBACK);
		*busy_ms = log_ms() - t0;
		if (rc != -EBUSY || *busy_ms >= BUSY_CAP_MS)
			return rc;
		nap(BUSY_POLL_MS);
	}
}

static long file_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (f == NULL)
		return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

/* ---- playing a file through io-media (the unit's testapp-file) ---- */

struct play {
	int      status;        /* testapp-file's wait status */
	int      stopped;       /* we stopped it: cap reached or a stop request */
	int      eof;           /* played: testapp-file saw INPUT_EOF (io-media
	                           read the clip to the end); step n: every
	                           track FINISHED */
	int      refused;       /* the mixer output would not attach */
	unsigned run_ms;        /* start to exit */
};

/* What testapp-file wrote to its log since pos. Its exit status says
 * nothing: main has one epilogue, `mov #1,r0` (0x80412A4), so it is 1 on
 * every path. Its stdout is fully buffered (no setvbuf/fflush; QNX BUFSIZ
 * 1 KB), so the lines carry no timing either. */
static void scan_log(const char *log_file, long pos, struct play *r)
{
	char line[300];
	FILE *f = fopen(log_file, "r");

	if (f == NULL)
		return;
	fseek(f, pos, SEEK_SET);
	while (fgets(line, sizeof(line), f) != NULL) {
		r->eof |= strstr(line, "INPUT_EOF") != NULL;
		r->refused |= strstr(line, "iom_attach_output") != NULL;
	}
	fclose(f);
}

/* Play file on the mixer output url with testapp-file, its output appended
 * to log_file. tick (may be NULL) is called every poll with the ms since
 * the start, for a live screen. Stopped after cap_ms or on a stop request.
 * -1 if it could not start. */
static int play_clip(const char *url, const char *file, const char *log_file,
                     unsigned cap_ms, void (*tick)(unsigned ms), struct play *r)
{
	char *argv[] = { (char *)player, "-f", "hello5", "-n", "200", "-o",
	                 (char *)url, (char *)file, NULL };
	long pos = file_size(log_file);
	unsigned t0;
	pid_t pid;

	memset(r, 0, sizeof(*r));
	t0 = log_ms();
	pid = proc_start(log_file, argv);
	if (pid < 0)
		return -1;
	while (proc_wait(pid, 0, &r->status) != 0) {
		if (log_ms() - t0 >= cap_ms || quit_poll() != QUIT_NONE) {
			r->status = proc_stop(pid);
			r->stopped = 1;
			break;
		}
		if (tick != NULL)
			tick(log_ms() - t0);
		nap(PLAY_POLL_MS);
	}
	r->run_ms = log_ms() - t0;
	scan_log(log_file, pos, r);
	return 0;
}

/* The chimes of a mixer group: one testapp-file run per chime, its
 * io-media events appended to <sd>/hello5_m<K>.log. Played = the player
 * reported INPUT_EOF (it got to the end of the clip). */
static void mixer_chimes(struct group *g)
{
	char log_file[256];
	unsigned k, shown, screen_ms;
	struct play p;

	snprintf(log_file, sizeof(log_file), "%s/hello5_m%d.log", sd_path, g->mixer);
	g->opened = player != NULL;
	if (player == NULL)
		log_msg("group %d: no testapp-file, nothing to play %s with", g->index,
		        mixers[g->mixer].url);
	for (k = 1; g->opened && k <= (unsigned)g->index; k++) {
		shown = log_ms();
		screen_ms = ui_chime(g->index, g->ngroups, g->label, (int)k, 1);
		g->step = STEP_WAIT;
		if (play_clip(mixers[g->mixer].url, wav, log_file, MIXER_CAP_MS,
		              NULL, &p) != 0) {
			log_msg("group %d chime %u: could not start %s: %s", g->index, k,
			        player, strerror(errno));
			break;
		}
		g->written++;
		ui_chime(g->index, g->ngroups, g->label, (int)k, 0);
		if (p.eof) {
			g->played++;
			g->total_ms += p.run_ms;
		}
		log_msg("group %d chime %u/%d on %s: PLAYING on screen at t=%u (%u ms), "
		        "testapp-file %s after %u ms%s%s", g->index, k, g->index,
		        g->label, shown, screen_ms, proc_status_str(p.status), p.run_ms,
		        p.stopped ? " (stopped by us)" : "",
		        p.refused ? " - output would not attach" :
		        p.eof ? "" : " - no INPUT_EOF, clip not played to the end");
		if (p.refused) {
			if (k == 1)
				g->opened = 0;
			break;
		}
		g->step = STEP_GAP;
		nap(CHIME_GAP_MS);
	}
}

static void *group_thread(void *arg)
{
	struct group *g = arg;
	snd_pcm_t *h;
	unsigned k, t0, write_ms, busy_ms, shown, screen_ms;
	ssize_t w;
	int rc;

	if (g->mixer >= 0) {
		mixer_chimes(g);
		g->done = 1;
		return NULL;
	}
	g->step = STEP_OPEN;
	g->opened = audio_open(0, g->device, &beep, &h) == 0;
	for (k = 1; g->opened && k <= (unsigned)g->index; k++) {
		g->step = STEP_PREPARE;
		rc = snd_pcm_plugin_prepare(h, SND_PCM_CHANNEL_PLAYBACK);
		if (rc < 0) {
			log_msg("group %d chime %u: prepare: %s", g->index, k, snd_strerror(rc));
			break;
		}
		shown = log_ms();
		screen_ms = ui_chime(g->index, g->ngroups, g->label, (int)k, 1);
		g->step = STEP_WRITE;
		t0 = log_ms();
		w = snd_pcm_plugin_write(h, beep.pcm, beep.len);
		write_ms = log_ms() - t0;
		if (w < (ssize_t)beep.len) {
			log_msg("group %d chime %u: short write %ld/%u after %u ms",
			        g->index, k, (long)w, beep.len, write_ms);
			break;
		}
		g->written++;
		g->step = STEP_WAIT;
		rc = wait_played(h, &busy_ms);
		ui_chime(g->index, g->ngroups, g->label, (int)k, 0);
		if (rc == 0) {
			g->played++;
			g->total_ms += write_ms + busy_ms;
		}
		log_msg("group %d chime %u/%d on %s: PLAYING on screen at t=%u "
		        "(%u ms), write %u ms + busy %u ms = %u ms (clip %u ms)%s",
		        g->index, k, g->index, g->label, shown, screen_ms,
		        write_ms, busy_ms, write_ms + busy_ms, clip_ms,
		        rc == 0 ? "" : rc == -EBUSY ? " - still busy, not consumed in time"
		                                    : " - prepare failed after write");
		if (rc != 0)
			break;
		g->step = STEP_GAP;
		nap(CHIME_GAP_MS);
	}
	g->step = STEP_CLOSE;
	if (g->opened)
		snd_pcm_close(h);
	g->done = 1;
	return NULL;
}

/* Run one group on its own thread; give up on it after its deadline. */
static void run_group(struct group *g)
{
	pthread_t tid;
	unsigned deadline = log_ms() + g->index * chime_budget(g->mixer) + GROUP_EXTRA_MS;

	log_msg("group %d: %d chime(s) on %s%s%s", g->index, g->index, g->label,
	        g->mixer >= 0 ? " = " : "", g->mixer >= 0 ? mixers[g->mixer].url : "");
	if ((errno = pthread_create(&tid, NULL, group_thread, g)) != 0) {
		log_msg("FAIL group %d: thread: %s", g->index, strerror(errno));
		return;
	}
	while (!g->done && log_ms() < deadline && quit_poll() == QUIT_NONE)
		nap(BUSY_POLL_MS);
	if (g->done) {
		pthread_join(tid, NULL);
		return;
	}
	pthread_detach(tid);    /* left blocked; exit() ends it */
	if (quit_poll() == QUIT_NONE)
		log_msg("FAIL group %d: %s stuck in %s, written off",
		        g->index, g->label, step_name(g->step));
}

static void stage_sound(const struct output *outputs, int n)
{
	/* static: a stuck group's thread may still write to its entry */
	static struct group groups[MAX_GROUPS];
	char next[40];
	unsigned any = 0;
	int i;

	log_msg("stage 1: %d group(s) of chimes, group N = N chimes, %u s apart",
	        n, GROUP_GAP_MS / 1000);
	ui_lead(n, LEAD_MS);
	if (wait_ms(LEAD_MS))
		return;
	for (i = 0; i < n; i++) {
		struct group *g = &groups[i];

		g->index = i + 1;
		g->ngroups = n;
		g->device = outputs[i].device;
		g->mixer = outputs[i].mixer;
		output_label(&outputs[i], g->label, sizeof(g->label));
		run_group(g);
		if (quit_poll() != QUIT_NONE)
			return;
		if (i + 1 < n)
			output_label(&outputs[i + 1], next, sizeof(next));
		ui_gap(g->index, i + 1 < n ? g->index + 1 : 0, i + 1 < n ? next : "",
		       !g->done ? "output stuck - skipped" :
		       !g->opened ? "output would not open - nothing played" :
		       g->written < (unsigned)g->index ? "not all chimes were written" : NULL);
		if (i + 1 < n && wait_ms(GROUP_GAP_MS))
			return;
	}
	for (i = 0; i < n; i++) {
		struct group *g = &groups[i];

		any += g->written;
		log_msg("RESULT group %d (%d chime%s) %s: %s, written %u, "
		        "played %u, avg %u ms (clip %u ms)%s",
		        g->index, g->index, g->index == 1 ? "" : "s", g->label,
		        !g->done ? "STUCK" : g->opened ? "opened" : "open failed",
		        g->written, g->played,
		        g->played ? g->total_ms / g->played : 0, clip_ms,
		        g->done && g->opened && g->played < (unsigned)g->index ?
		        " - incomplete" : "");
	}
	if (any == 0)
		log_msg("FAIL stage 1: no output took a single chime");
}

/* ---- the volume knob (on_key below counts it) ---- */

static volatile unsigned vol_up_clicks, vol_down_clicks; /* byte 4/5 = clicks */

/* ---- steps m, v, t, n: a ping train on TDM1 2.0 SAD ---- */

struct step;
/* Plays the step's train: 0, or -1 if there was nothing to play with. */
typedef int (*step_player)(const struct step *s, const char *log_file,
                           struct play *p);

struct step {
	char        key;
	const char *title;
	const char *todo;       /* what to do: before and during the pings */
	const char *listen;     /* the question to listen for */
	unsigned    intro_s;    /* countdown before the pings */
	unsigned    pings;      /* one every PULSE_PERIOD_MS */
	unsigned    chain;      /* step n: one-ping files queued after them */
	step_player play;
};

/* RAM first: an app's sounds should not need the card. */
static const char *pulse_dir = "/tmp";
static unsigned pulse_made;     /* bit n: /tmp/hello5_pulse<n>.wav written */

/* Render the train of n pings into pulse_dir (falling back to the card if
 * that is not writable); its path in buf. */
static int pulse_file(unsigned n, char *buf, size_t size)
{
	unsigned t0 = log_ms();

	snprintf(buf, size, "%s/hello5_pulse%u.wav", pulse_dir, n);
	if (pulse_write_wav(buf, n) != 0) {
		log_msg("pulse: cannot write %s: %s", buf, strerror(errno));
		if (pulse_dir == sd_path)
			return -1;
		pulse_dir = sd_path;
		return pulse_file(n, buf, size);
	}
	log_msg("pulse: %s = %u pings (%u ms) rendered in %u ms", buf, n,
	        n * PULSE_PERIOD_MS, log_ms() - t0);
	if (pulse_dir != sd_path && n < sizeof(pulse_made) * 8)
		pulse_made |= 1u << n;
	return 0;
}

/* Delete the trains left in /tmp (the card copies stay, to inspect). */
static void pulse_cleanup(void)
{
	char path[64];
	unsigned n;

	for (n = 0; n < sizeof(pulse_made) * 8; n++)
		if (pulse_made & (1u << n)) {
			snprintf(path, sizeof(path), "/tmp/hello5_pulse%u.wav", n);
			unlink(path);
		}
}

/* The step being played, for step_tick(). */
static const struct step *cur_step;
static int cur_index, cur_count;
static unsigned cur_up0, cur_down0, cur_drawn;
static int cur_volume = -1;     /* step n's own volume while it plays */

/* The live line under the PLAYING box; empty if the step has none. */
static void step_note(const struct step *s, char *buf, size_t size)
{
	buf[0] = '\0';
	if (s->key == 'v')
		snprintf(buf, size, "volume knob: %u clicks down, %u up",
		         vol_down_clicks - cur_down0, vol_up_clicks - cur_up0);
	else if (s->key == 'n' && cur_volume >= 0)
		snprintf(buf, size, "hello5's own volume: %d", cur_volume);
}

/* The PLAYING screen, redrawn while the train plays. */
static void step_tick(unsigned ms)
{
	const struct step *s = cur_step;
	unsigned n = s->pings + s->chain, ping = ms / PULSE_PERIOD_MS + 1;
	char big[32], note[64];

	if (cur_drawn != 0 && ms - cur_drawn < STEP_REDRAW_MS)
		return;
	cur_drawn = ms ? ms : 1;
	snprintf(big, sizeof(big), "ping %u of %u", ping < n ? ping : n, n);
	step_note(s, note, sizeof(note));
	ui_step(cur_index, cur_count, s->title, s->todo, s->listen, big, 1,
	        note[0] ? note : NULL);
}

/* Steps m, v, t: the train through the unit's testapp-file. */
static int play_testapp(const struct step *s, const char *log_file, struct play *p)
{
	unsigned clip = s->pings * PULSE_PERIOD_MS;
	char file[256];

	if (player == NULL) {
		log_msg("step %c: no testapp-file to play with", s->key);
		return -1;
	}
	if (pulse_file(s->pings, file, sizeof(file)) != 0)
		return -1;
	log_msg("step %c: testapp-file plays %s on %s", s->key, file,
	        mixers[STEP_MIXER].url);
	if (play_clip(mixers[STEP_MIXER].url, file, log_file, clip + STEP_SLACK_MS,
	              step_tick, p) != 0) {
		log_msg("step %c: could not start %s: %s", s->key, player, strerror(errno));
		return -1;
	}
	log_msg("step %c: testapp-file %s after %u ms (train %u ms)%s%s", s->key,
	        proc_status_str(p->status), p->run_ms, clip,
	        p->stopped ? ", stopped by us" : "",
	        p->refused ? " - output would not attach" :
	        p->eof ? "" : " - no INPUT_EOF, not played to the end");
	return 0;
}

/* ---- step n: the train through our own io-media client (common/iom.c) ---- */

#define NATIVE_LOW_VOLUME 0     /* for pings 4-6: 30 was not clearly
                                   quieter on 2026-10-25, 0 is decisive */
#define NATIVE_TIME_MS    500   /* TIME event period */

static uint64_t rd64(const unsigned char *p)
{
	return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static void native_error(struct iom *g, const char *what, int rc)
{
	struct iom_err e;

	if (iom_error_info(g, &e) == EOK)
		log_msg("step n: %s: %s, iom_errno=0x%x type=%u value=%llu", what,
		        strerror(rc), (unsigned)e.iom_errno, (unsigned)e.value_type,
		        (unsigned long long)e.value);
	else
		log_msg("step n: %s: %s", what, strerror(rc));
}

/* One native play, for its event handler. */
struct native {
	struct iom  g;
	const char *chain_file;
	unsigned    playing_ms; /* first STATUS state 4 (playing), since play */
	unsigned    to_attach;  /* chain inputs still to attach */
	int         finished;   /* FINISHED: the whole queue has played */
	uint64_t    next_track;
};

static void native_event(struct native *n, uint32_t type,
                         const unsigned char *p, uint32_t len, unsigned t)
{
	int rc;

	switch (type) {
	case IOM_EV_STATUS:
		if (len < 16)
			break;
		log_msg("step n: +%u ms STATUS track %llu state %u speed %d", t,
		        (unsigned long long)rd64(p), (unsigned)rd32(p + 8),
		        (int)rd32(p + 12));
		if (rd32(p + 8) == 4 && n->playing_ms == 0)
			n->playing_ms = t ? t : 1;
		return;
	case IOM_EV_TIME:
		if (len < 24)
			break;
		log_msg("step n: +%u ms TIME track %llu at %lld of %llu ms", t,
		        (unsigned long long)rd64(p), (long long)rd64(p + 8),
		        (unsigned long long)rd64(p + 16));
		return;
	case IOM_EV_INPUT_EOF:
		log_msg("step n: +%u ms INPUT_EOF track %llu", t,
		        len >= 8 ? (unsigned long long)rd64(p) : 0ULL);
		if (n->to_attach == 0)
			return;
		/* as testapp-file: drop the input that was read, queue the next */
		rc = iom_detach_input(&n->g);
		if (rc != EOK)
			native_error(&n->g, "detach input", rc);
		rc = iom_attach_input(&n->g, n->next_track, n->chain_file);
		if (rc != EOK) {
			native_error(&n->g, "attach input", rc);
			n->to_attach = 0;               /* those never come */
			return;
		}
		log_msg("step n: +%u ms queued track %llu", t,
		        (unsigned long long)n->next_track);
		n->next_track++;
		n->to_attach--;
		return;
	case IOM_EV_FINISHED:
		/* One FINISHED for the whole queue: inputs attached on INPUT_EOF
		 * play on in the same session. (On 2026-10-25 hello5 expected one
		 * per track and played again on it, which replayed the last
		 * track until the cap.) */
		n->finished = 1;
		log_msg("step n: +%u ms FINISHED%s", t,
		        n->to_attach ? ", inputs never attached" : "");
		return;
	case IOM_EV_ERROR:
		native_error(&n->g, "ERROR event", EIO);
		return;
	}
	log_msg("step n: +%u ms event type %u, %u bytes", t, (unsigned)type,
	        (unsigned)len + 8);
}

/* Play file (pings), then chain one-ping files, on TDM1 2.0 SAD with our
 * own graph. Our volume goes down to NATIVE_LOW_VOLUME for pings 4-6.
 * Every event is logged with the time since play. */
static int native_play(const char *file, const char *chain_file,
                       unsigned pings, unsigned chain, struct play *r)
{
	static unsigned graphs;
	unsigned clip = (pings + chain) * PULSE_PERIOD_MS, t0, t = 0, ref;
	unsigned char buf[256];
	struct native n;
	char name[32];
	int rc, len, off, vstep = 0, blind = 0;

	memset(r, 0, sizeof(*r));
	memset(&n, 0, sizeof(n));
	n.chain_file = chain_file;
	n.to_attach = chain;
	n.next_track = 2;
	snprintf(name, sizeof(name), "hello5_%d_%u", getpid(), ++graphs);
	rc = iom_create(&n.g, name);
	if (rc != EOK) {
		log_msg("step n: create graph %s: %s", n.g.path, strerror(rc));
		return -1;
	}
	log_msg("step n: graph %s, playing %s then %u x %s", n.g.path, file,
	        chain, chain_file);
	if ((rc = iom_set_speed(&n.g, 1000)) != EOK)
		native_error(&n.g, "set speed", rc);
	if ((rc = iom_set_time_notify(&n.g, NATIVE_TIME_MS)) != EOK)
		native_error(&n.g, "set time notification", rc);
	rc = iom_attach_output(&n.g, mixers[STEP_MIXER].url);
	if (rc != EOK) {
		native_error(&n.g, "attach output", rc);
		r->refused = 1;
		iom_close(&n.g);
		return 0;
	}
	log_msg("step n: output %s = id %d", mixers[STEP_MIXER].url, n.g.output);
	cur_volume = 100;
	if ((rc = iom_set_volume(&n.g, cur_volume)) != EOK)
		native_error(&n.g, "set volume", rc);
	rc = iom_attach_input(&n.g, 1, file);
	if (rc != EOK) {
		native_error(&n.g, "attach input", rc);
		cur_volume = -1;
		iom_close(&n.g);
		return 0;       /* not played: run_step retries from the card */
	}
	cur_drawn = 0;
	t0 = log_ms();
	rc = iom_play(&n.g);
	if (rc != EOK) {
		native_error(&n.g, "play", rc);
		cur_volume = -1;
		iom_close(&n.g);
		return 0;
	}
	log_msg("step n: play");
	while (!n.finished) {
		t = log_ms() - t0;
		if (t >= clip + STEP_SLACK_MS || quit_poll() != QUIT_NONE) {
			r->stopped = 1;
			break;
		}
		len = blind ? 0 : iom_read_events(&n.g, buf, sizeof(buf));
		if (len < 0) {
			log_msg("step n: reading events: %s; playing on blind",
			        strerror(errno));
			blind = 1;
			len = 0;
		}
		for (off = 0; off + 8 <= len; ) {
			uint32_t size = rd32(buf + off);

			if (size < 8 || off + size > (uint32_t)len) {
				log_msg("step n: bad event size %u at %d of %d bytes",
				        (unsigned)size, off, len);
				break;
			}
			native_event(&n, rd32(buf + off + 4), buf + off + 8, size - 8,
			             log_ms() - t0);
			off += (int)size;
		}
		/* our volume: down in the gap before ping 4, up before ping 7 */
		ref = n.playing_ms + (vstep == 0 ? 3 : 6) * PULSE_PERIOD_MS - 500;
		if (vstep < 2 && t >= ref) {
			cur_volume = vstep == 0 ? NATIVE_LOW_VOLUME : 100;
			rc = iom_set_volume(&n.g, cur_volume);
			log_msg("step n: +%u ms volume %d: %s", t, cur_volume,
			        rc == EOK ? "ok" : strerror(rc));
			vstep++;
		}
		step_tick(t);
		if (len == 0)
			nap(20);
	}
	r->run_ms = log_ms() - t0;
	r->eof = n.finished && n.to_attach == 0 && !r->stopped;
	cur_volume = -1;
	iom_close(&n.g);
	log_msg("step n: done after %u ms (pings %u ms), STATUS playing at +%u ms%s",
	        r->run_ms, clip, n.playing_ms,
	        r->stopped ? ", stopped by us" : "");
	return 0;
}

static int play_native(const struct step *s, const char *log_file, struct play *p)
{
	char file[256], one[256];

	(void)log_file;         /* the events go to hello5.log, timed */
	if (pulse_file(s->pings, file, sizeof(file)) != 0 ||
	    pulse_file(1, one, sizeof(one)) != 0)
		return -1;
	return native_play(file, one, s->pings, s->chain, p);
}

static const struct step step_list[] = {
	{ 'm', "radio + our sound",
	  "Radio on at a normal volume. Then pings, one every 2 s, for 12 s.",
	  "LISTEN: is the radio still playing BETWEEN the pings?", 10, 6, 0,
	  play_testapp },
	{ 'n', "hello5's own player",
	  "Pings every 2 s for 24 s, played by hello5 itself this time. Pings "
	  "4, 5 and 6 should be SILENT: hello5 turns its own volume to 0.",
	  "LISTEN: are pings 4-6 missing? Do 10-12 keep the 2 s beat?", 8, 9, 3,
	  play_native },
	{ 'v', "volume knob",
	  "Pings every 2 s for 20 s. While they play, turn the VOLUME knob "
	  "slowly, ONE click at a time: 3 clicks DOWN, wait a few pings, then 3 "
	  "clicks UP.",
	  "LISTEN: do the pings get quieter and louder with the radio?", 10, 10, 0,
	  play_testapp },
	{ 't', "music",
	  "Press MEDIA now and start some music (USB, Bluetooth, Jukebox, CD). "
	  "When the Media screen comes up, this screen should come back within "
	  "a second. Then 6 pings, one every 2 s.",
	  "WATCH: is this screen back, without flicker? LISTEN: music BETWEEN "
	  "the pings?",
	  25, 6, 0, play_testapp },
};
#define NSTEPS ((int)(sizeof(step_list) / sizeof(step_list[0])))

static const struct step *find_step(char key)
{
	int i;

	for (i = 0; i < NSTEPS; i++)
		if (step_list[i].key == key)
			return &step_list[i];
	return NULL;
}

/* Worst case for one step, for the watchdog: countdown, two plays (the
 * /tmp one and the card retry) up to their cap, the done screen. */
static unsigned step_max_ms(const struct step *s)
{
	return s->intro_s * 1000 + STEP_DONE_MS + 2000 +
	       2 * ((s->pings + s->chain) * PULSE_PERIOD_MS + STEP_SLACK_MS);
}

static void run_step(const struct step *s, int index, int count)
{
	char log_file[256], big[32], note[64];
	const char *result;
	struct play p;
	unsigned i;
	int rc;

	log_msg("step %d/%d %c (%s): %u s to get ready, then %u pings on %s",
	        index, count, s->key, s->title, s->intro_s, s->pings + s->chain,
	        mixers[STEP_MIXER].name);
	snprintf(log_file, sizeof(log_file), "%s/hello5_%c.log", sd_path, s->key);
	for (i = s->intro_s; i > 0; i--) {
		snprintf(big, sizeof(big), "pings in %u s", i);
		ui_step(index, count, s->title, s->todo, s->listen, big, 0, NULL);
		if (wait_ms(1000))
			return;
	}

	cur_step = s;
	cur_index = index;
	cur_count = count;
	cur_up0 = vol_up_clicks;
	cur_down0 = vol_down_clicks;
	cur_drawn = 0;
	memset(&p, 0, sizeof(p));
	rc = s->play(s, log_file, &p);
	/* not played and not refused: maybe io-media cannot read /tmp */
	if (rc == 0 && !p.eof && !p.refused && !p.stopped && pulse_dir != sd_path) {
		log_msg("step %c: nothing played from %s, retrying from the card",
		        s->key, pulse_dir);
		pulse_dir = sd_path;
		rc = s->play(s, log_file, &p);
	}
	if (quit_poll() != QUIT_NONE)
		return;

	step_note(s, note, sizeof(note));
	if (s->key == 'v')
		log_msg("step v: %s during the pings", note);
	result = rc != 0 ? "nothing to play it with" :
	         p.refused ? "the output would not attach" :
	         p.eof ? "played to the end" : "NOT played to the end";
	log_msg("RESULT step %c (%s): %s", s->key, s->title, result);
	ui_step(index, count, s->title, s->listen, NULL, "done", 0,
	        s->key == 'v' ? note : result);
	wait_ms(STEP_DONE_MS);
}

/* ---- /dev/ipc/ch8 ---- */

/* quit.c calls this for every ch8 message, on its key thread: logged raw,
 * volume detents counted for step v (hello7 decodes everything). */
static void on_key(const unsigned char *buf, int n, void *arg)
{
	char hex[32 * 3 + 1];
	int i;

	(void)arg;
	for (i = 0; i < n && i < 32; i++)
		snprintf(hex + i * 3, 4, "%02x ", buf[i]);
	hex[i * 3] = '\0';
	if (n >= 8) {
		/* the value is the detents since the last message (21.10: one
		 * message said 2) */
		vol_up_clicks += buf[4];
		vol_down_clicks += buf[5];
	}
	log_msg("key: %d bytes: %s", n, hex);
}

/* "m0,m1,5,0": mK = mixers[K], a number = io-audio dindex. */
static int parse_outputs(const char *s, struct output *outputs)
{
	int n = 0;
	char *end;

	while (*s != '\0' && n < MAX_GROUPS) {
		int mixer = *s == 'm';
		long d = strtol(s + mixer, &end, 10);

		if (end == s + mixer || d < 0 || d > (mixer ? NMIXERS - 1 : 5))
			return -1;
		outputs[n].device = mixer ? -1 : (int)d;
		outputs[n].mixer = mixer ? (int)d : -1;
		n++;
		s = *end == ',' ? end + 1 : end;
		if (*end != ',' && *end != '\0')
			return -1;
	}
	return n;
}

/* The unit's testapp-file (efs-system, served by inflator), else the copy
 * `make sd` puts next to hello5 in bin/. */
static const char *find_player(const char *argv0, char *buf, size_t size)
{
	static const char unit[] = "/mnt/efs-system/bin/testapp-file";
	const char *slash = strrchr(argv0, '/');

	if (access(unit, X_OK) == 0)
		return unit;
	if (slash != NULL)
		snprintf(buf, size, "%.*s/testapp-file", (int)(slash - argv0), argv0);
	else
		snprintf(buf, size, "./testapp-file");
	return access(buf, X_OK) == 0 ? buf : NULL;
}

int main(int argc, char *argv[])
{
	char logpath[256], wavpath[256], playerpath[256];
	const char *outspec = DEFAULT_OUTPUTS, *steps = DEFAULT_STEPS, *c;
	unsigned stage1_max_ms = 0;
	struct output outputs[MAX_GROUPS];
	int nout, nsteps, i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [steps] [outputs]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	if (argc > 2)
		steps = argv[2];
	if (argc > 3)
		outspec = argv[3];
	snprintf(wavpath, sizeof(wavpath), "%s/beep.wav", sd_path);
	wav = wavpath;

	snprintf(logpath, sizeof(logpath), "%s/hello5.log", sd_path);
	if (log_open(logpath) != 0)
		fprintf(stderr, "hello5: cannot open %s, logging to stdout only\n", logpath);
	player = find_player(argv[0], playerpath, sizeof(playerpath));
	log_msg("hello5 start, pid=%d, steps=%s, outputs=%s, player=%s",
	        getpid(), steps, outspec, player ? player : "NOT FOUND");

	nout = parse_outputs(outspec, outputs);
	nsteps = (int)strlen(steps);
	for (c = steps; *c != '\0' && (*c == 'g' || find_step(*c) != NULL); c++)
		;
	if (nout <= 0 || *c != '\0') {
		log_msg("hello5: bad step list \"%s\" (any of mnvtg) or output list "
		        "\"%s\" (want e.g. %s)", steps, outspec, DEFAULT_OUTPUTS);
		log_close();
		return 2;
	}
	/* worst case: every step and group runs into its deadline */
	for (c = steps; *c != '\0'; c++) {
		if (*c != 'g') {
			stage1_max_ms += step_max_ms(find_step(*c));
			continue;
		}
		stage1_max_ms += LEAD_MS;
		for (i = 0; i < nout; i++)
			stage1_max_ms += (unsigned)(i + 1) * chime_budget(outputs[i].mixer) +
			                 GROUP_EXTRA_MS + GROUP_GAP_MS;
	}

	/* beep.wav is step g's clip */
	if (strchr(steps, 'g') != NULL) {
		if (clip_load(wav, &beep) == 0 && beep.rate && beep.voices && beep.bits >= 8)
			clip_ms = beep.len * 1000u / (beep.rate * beep.voices * (beep.bits / 8));
		else {
			log_msg("FAIL hello5: no usable clip, skipping step g");
			free(beep.pcm);
			beep.pcm = NULL;
		}
	}

	/* quit.c owns /dev/ipc/ch8; on_key() runs on its reader thread. */
	quit_init_ex(sd_path, stage1_max_ms / 1000 + WATCHDOG_SLACK,
	             on_key, NULL);

	ui_open();
	for (i = 0; i < nsteps && quit_poll() == QUIT_NONE; i++) {
		if (steps[i] != 'g')
			run_step(find_step(steps[i]), i + 1, nsteps);
		else if (beep.pcm != NULL)
			stage_sound(outputs, nout);
	}

	quit_request(QUIT_TIMER);
	ui_close();
	pulse_cleanup();
	log_msg("hello5 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	free(beep.pcm);
	return 0;
}
