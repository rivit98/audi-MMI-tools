/*
 * hello5 - beep on every turn of the main rotary knob.
 *
 * The MMI's only user-input device is /dev/ipc/ch8 (same channel hello*'s
 * quit.c and qnx-audi read). Its 8-byte messages decode as: byte 7/6 =
 * main rotary knob left/right, 5/4 = volume knob left/right, all of 4..7
 * zero = a button press. quit.c owns the single reader; hello5 hooks it
 * with quit_init_ex() and plays a short PCM clip whenever byte 6 or 7 is
 * set (a main-rotary detent). Nothing on ch8 stops the run; it ends on its
 * own timer.
 *
 * Audio goes through QNX Sound Architecture (QSA / libasound). io-audio on
 * this unit runs the hb_fpga driver and exposes six playback devices; index
 * 5 is "tones1", the UI-tone mixer input (see mmi3g-srv-starter.cfg), so a
 * beep there mixes over media/nav instead of fighting the ent3 output.
 * The plugin layer (snd_pcm_plugin_*) resamples/upmixes the clip to whatever
 * the FPGA wants. beep.wav is rendered by sdk/mkbeep.py: a 600 ms two-note
 * chime at 44.1 kHz mono, so the plugin has no rate conversion to do.
 *
 * The QSA struct ABI in <sys/asoundlib.h> is reconstructed from the device's
 * own libasound.so.2; every snd_* return code, the negotiated format and the
 * driver's why_failed code are logged, so one car run shows which parameter
 * it rejects (watch the "params" and "setup" lines).
 *
 * Stops on a button press, SIGTERM/INT/HUP, SD removal, its own run timer,
 * or the watchdog (src/common/quit.c).
 *
 * usage: hello5 <sd_path> [run_seconds] [wav_path] [device_index]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/asoundlib.h>

#include "log.h"
#include "quit.h"

#define TONES1_DEVICE   5       /* dindex 5 = "tones1" in io-audio's args */
#define RUN_SECONDS     600     /* default session length */
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

static snd_pcm_t *pcm_handle;
static struct clip beep;
static unsigned beeps, beep_busy, beep_fail;

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

/* Open the tones device and negotiate the clip's format once. */
static int audio_open(int card, int device, const struct clip *c)
{
	snd_pcm_channel_params_t p;
	snd_pcm_channel_setup_t s;
	int fmt = sfmt_for(c->bits), rc;

	if (fmt < 0) {
		log_msg("audio: unsupported %u-bit sample", c->bits);
		return -1;
	}
	rc = snd_pcm_open(&pcm_handle, card, device, SND_PCM_OPEN_PLAYBACK);
	if (rc < 0) {
		log_msg("audio: snd_pcm_open(card=%d,dev=%d): %s", card, device,
		        snd_strerror(rc));
		pcm_handle = NULL;
		return -1;
	}
	/* The plugin layer builds an mmap plugin unless bit 1 of the disable
	 * mask is set (plugin_params tests `handle[0x9B0] & 2`); the standard
	 * QSA playback sequence turns mmap off before negotiating. */
	snd_pcm_plugin_set_disable(pcm_handle, PLUGIN_DISABLE_MMAP);
	/* let the plugin layer resample/upmix to the hardware's fixed format */
	snd_pcm_plugin_set_src_method(pcm_handle, SND_PCM_PLUGIN_CONV_AUTO);

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

	rc = snd_pcm_plugin_params(pcm_handle, &p);
	if (rc < 0) {
		/* why_failed is the driver's own reason, copied back into our
		 * struct by libasound; without it "Invalid argument" says
		 * nothing about which field was rejected. */
		log_msg("audio: snd_pcm_plugin_params: %s (why_failed=%d: %s)",
		        snd_strerror(rc), p.why_failed,
		        why_failed_name(p.why_failed));
		log_msg("audio: requested %u Hz, %u ch, %u-bit (sfmt=%d), "
		        "frag_size=%d frags %d..%d",
		        c->rate, c->voices, c->bits, fmt,
		        p.frag_size, p.frags_min, p.frags_max);
		snd_pcm_close(pcm_handle);
		pcm_handle = NULL;
		return -1;
	}
	log_msg("audio: params accepted (requested %u Hz, %u ch, sfmt=%d)",
	        c->rate, c->voices, fmt);

	/* negotiated read-back - for the log only */
	memset(&s, 0, sizeof(s));
	s.channel = SND_PCM_CHANNEL_PLAYBACK;
	if (snd_pcm_plugin_setup(pcm_handle, &s) >= 0)
		log_msg("audio: setup negotiated %d Hz, %d ch, sfmt=%d",
		        s.rate, s.voices, s.format);
	return 0;
}

static void audio_close(void)
{
	if (pcm_handle != NULL) {
		snd_pcm_plugin_playback_drain(pcm_handle);
		snd_pcm_close(pcm_handle);
		pcm_handle = NULL;
	}
}

/* Play the whole clip once. Called from the key thread, serialized. */
static void beep_play(void)
{
	ssize_t w;
	int rc;

	if (pcm_handle == NULL)
		return;
	rc = snd_pcm_plugin_prepare(pcm_handle, SND_PCM_CHANNEL_PLAYBACK);
	if (rc == -EBUSY) {
		/* the previous chime is still playing: on 12.10 v2 every EBUSY
		 * came < 600 ms (the clip's length) after a beep that worked */
		beep_busy++;
		return;
	}
	if (rc < 0) {
		beep_fail++;
		log_msg("beep: prepare: %s", snd_strerror(rc));
		return;
	}
	w = snd_pcm_plugin_write(pcm_handle, beep.pcm, beep.len);
	if (w < (ssize_t)beep.len) {
		beep_fail++;
		log_msg("beep: short write %ld/%u", (long)w, beep.len);
		return;
	}
	beeps++;
	log_msg("beep %u: %u bytes written", beeps, beep.len);
}

/* quit.c calls this for every ch8 message. Byte 7/6 = main rotary turn. */
static void on_key(const unsigned char *buf, int n, void *arg)
{
	(void)arg;
	if (n >= 8 && (buf[6] | buf[7]) != 0)
		beep_play();
}

static void nap(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

int main(int argc, char *argv[])
{
	char logpath[256], wavpath[256];
	const char *sd_path, *wav;
	unsigned run_s = RUN_SECONDS, run_ms;
	int device = TONES1_DEVICE;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [run_seconds] [wav] [device]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	if (argc > 2 && atoi(argv[2]) > 0)
		run_s = (unsigned)atoi(argv[2]);
	if (argc > 3)
		wav = argv[3];
	else {
		snprintf(wavpath, sizeof(wavpath), "%s/beep.wav", sd_path);
		wav = wavpath;
	}
	if (argc > 4)
		device = atoi(argv[4]);
	run_ms = run_s * 1000;

	snprintf(logpath, sizeof(logpath), "%s/hello5.log", sd_path);
	if (log_open(logpath) != 0)
		fprintf(stderr, "hello5: cannot open %s, logging to stdout only\n", logpath);
	log_msg("hello5 start, pid=%d, run=%u s, wav=%s, device=%d",
	        getpid(), run_s, wav, device);

	if (clip_load(wav, &beep) != 0) {
		log_msg("hello5: no clip, nothing to play - exiting");
		log_close();
		return 1;
	}
	if (audio_open(0, device, &beep) != 0)
		log_msg("hello5: audio not ready; will still log knob turns");

	/* quit.c owns /dev/ipc/ch8; on_key() runs on its reader thread. */
	quit_init_ex(sd_path, run_s + WATCHDOG_SLACK, on_key, NULL);
	log_msg("hello5: ready - turn the main rotary knob to beep");

	for (;;) {
		if (log_ms() >= run_ms)
			quit_request(QUIT_TIMER);
		if (quit_poll() != QUIT_NONE)
			break;
		nap(POLL_MS);
	}

	audio_close();
	log_msg("hello5 exit (stop: %s), beeps=%u, busy (still playing)=%u, failures=%u",
	        quit_name(quit_poll()), beeps, beep_busy, beep_fail);
	log_close();
	free(beep.pcm);
	return 0;
}
