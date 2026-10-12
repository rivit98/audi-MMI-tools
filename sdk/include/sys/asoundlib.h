/*
 * sys/asoundlib.h - minimal QNX Sound Architecture (QSA) declarations for
 * the MMI 3G+ SDK.
 *
 * The openqnx (6.4.1) source leak we build the sysroot from does NOT ship
 * the audio DDK headers, so this file is a reconstruction of the QNX 6.3
 * QSA ABI: just the pieces hello5 uses to play a PCM clip.
 *
 * The struct layout below was recovered by disassembling the device's own
 * libasound.so.2 with IDA (there is no QSA header anywhere, and Hex-Rays has
 * no SH4 decompiler, so this is from the SH4 disassembly):
 *
 *   snd_pcm_channel_params()  packs the struct straight into
 *       devctl(fd, 0xC2F04134, params, ...)
 *   The QNX dcmd 0xC2F04134 encodes a transfer size of 0x2F0 bytes, so
 *       sizeof(snd_pcm_channel_params_t) == 752.                 [CONFIRMED]
 *   snd_pcm_plugin_params() reads:
 *       params->channel @0x00   (validated 0..1)                 [CONFIRMED]
 *       params->mode    @0x04   (must be 1 or 2, else -EINVAL)   [CONFIRMED]
 *       params->format.format @0x1C (validated <=15, width looked up)
 *                               => format sub-struct starts @0x18 [CONFIRMED]
 *       params->format.voices @0x24 (multiplied by sample bytes)  [CONFIRMED]
 *       the format sub-struct is memcpy'd into the handle with a
 *       length of 0x11 = 17 bytes, so it is
 *       {interleave, format, rate, voices} + a trailing int8     [CONFIRMED]
 *       start_mode @0x1D8, copied into handle[0x9A8 + channel*4]  [CONFIRMED]
 *       why_failed @0x1E4: zeroed on entry, and on a failed devctl the
 *       driver's reason is copied from the local copy back into the
 *       caller's struct before returning -errno                   [CONFIRMED]
 *       frag_size @0x1E8 (rate-scaled), frags_max @0x1F0 (read)   [CONFIRMED]
 *   stop_mode @0x1DC, the time/ust_time bitfield word @0x1E0 and frags_min
 *   @0x1EC are [INFERRED]: they fill the gaps between the confirmed fields
 *   and match QSA's documented field order
 *   (... start_mode, stop_mode, time:1, ust_time:1, why_failed, buf ...).
 *
 *   NOTE: an earlier revision of this header had start_mode @0x1E0 and
 *   stop_mode @0x1E4, both 8 bytes too far along, which silently wrote
 *   stop_mode over why_failed.
 *
 *   The enum values (start/stop modes, sample formats, why_failed codes)
 *   were first copied from ALSA 0.5 and were all wrong for QNX. The 12.10
 *   car run failed with why_failed=2 = BAD_START: start_mode 0 is invalid,
 *   QNX's SND_PCM_START_DATA is 1. The values below are read from the
 *   unit's io-audio (params check at 0x0804E780).                [CONFIRMED]
 *
 *   snd_pcm_plugin_set_disable() stores its mask at handle[0x9B0], and
 *   snd_pcm_plugin_params() tests `mask & 2` to decide whether to build the
 *   mmap plugin, so PLUGIN_DISABLE_MMAP == 2 here.                [CONFIRMED]
 *
 * The compile-time asserts at the bottom lock the confirmed offsets/size, so
 * a bad edit fails the build rather than the car. The setup-readback struct
 * (snd_pcm_channel_setup_t) has its own layout (no sync[], format @0x08)
 * and is over-sized so the library cannot overflow it.
 *
 * libasound.so.2 is linked via its mkstubs.py stub (see the Makefile); the
 * real library resolves on the unit.
 */
#ifndef RIVIT_SYS_ASOUNDLIB_H
#define RIVIT_SYS_ASOUNDLIB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- open() mode flags (snd_pcm_open) ---- */
#define SND_PCM_OPEN_PLAYBACK   0x00000001
#define SND_PCM_OPEN_CAPTURE    0x00000002

/* ---- channel direction (params->channel) ---- */
#define SND_PCM_CHANNEL_PLAYBACK 0
#define SND_PCM_CHANNEL_CAPTURE  1

/* ---- transfer mode (params->mode). libasound requires 1 or 2; 0 is
 *      rejected with -EINVAL, so BLOCK is 1 here (not 0 as some QSA docs
 *      show). The ==1 path does fragment-based transfer. ---- */
#define SND_PCM_MODE_BLOCK   1
#define SND_PCM_MODE_STREAM  2

/* ---- start/stop conditions. QNX numbers these from 1 (ALSA 0.5 used 0);
 *      io-audio's params check (0x0804E780) rejects start_mode outside 1..3
 *      with why_failed 2 and stop_mode outside 1..2 with why_failed 3. ---- */
#define SND_PCM_START_DATA   1   /* start as soon as data is written */
#define SND_PCM_START_FULL   2
#define SND_PCM_START_GO     3

#define SND_PCM_STOP_STOP      1
#define SND_PCM_STOP_ROLLOVER  2

/* ---- sample formats (format.format). QNX order, not ALSA 0.5's: unsigned
 *      before signed. From io-audio's ado_pcm_format_str table (u8 s8 u16l
 *      u16b s16l s16b u24l ...) and its format fallback, which picks 4 for
 *      caps bit 0x10, 12 for 0x1000 and 0 for bit 0x1. The driver checks
 *      (1 << format) against the channel's format mask. ---- */
#define SND_PCM_SFMT_U8        0
#define SND_PCM_SFMT_S8        1
#define SND_PCM_SFMT_U16_LE    2
#define SND_PCM_SFMT_U16_BE    3
#define SND_PCM_SFMT_S16_LE    4
#define SND_PCM_SFMT_S16_BE    5
#define SND_PCM_SFMT_U24_LE    6
#define SND_PCM_SFMT_U24_BE    7
#define SND_PCM_SFMT_S24_LE    8
#define SND_PCM_SFMT_S24_BE    9
#define SND_PCM_SFMT_U32_LE    10
#define SND_PCM_SFMT_U32_BE    11
#define SND_PCM_SFMT_S32_LE    12
#define SND_PCM_SFMT_S32_BE    13
#define SND_PCM_SFMT_A_LAW     14
#define SND_PCM_SFMT_MU_LAW    15

/* plugin source conversion method (snd_pcm_plugin_set_src_method) */
#define SND_PCM_PLUGIN_CONV_AUTO    0

/* snd_pcm_plugin_set_disable() mask. Bit 1 is the one plugin_params tests
 * (`mask & 2`) before building the mmap plugin; the canonical QSA playback
 * recipe disables mmap before negotiating parameters. */
#define PLUGIN_DISABLE_MMAP         0x0002

/* params->why_failed after snd_pcm_plugin_params() returns an error: the
 * driver's own reason for the rejection. Values as io-audio's params check
 * (0x0804E780 / 0x0804E974) sets them. */
#define SND_PCM_PARAMS_BAD_MODE     1   /* mode, or interleave not supported */
#define SND_PCM_PARAMS_BAD_START    2
#define SND_PCM_PARAMS_BAD_STOP     3
#define SND_PCM_PARAMS_BAD_FORMAT   4
#define SND_PCM_PARAMS_BAD_RATE     5
#define SND_PCM_PARAMS_BAD_VOICES   6
#define SND_PCM_PARAMS_NO_CHANNEL   10

/* ---- opaque handle ---- */
typedef struct snd_pcm snd_pcm_t;

/*
 * snd_pcm_channel_params_t - 752 bytes, offsets fixed by the device's
 * libasound.so.2 (see the file header). Only the named fields are touched;
 * the _rsv* / digital gaps are left zeroed. Offsets are asserted below.
 */
typedef struct snd_pcm_channel_params {
	int32_t  channel;          /* 0x000  SND_PCM_CHANNEL_* */
	int32_t  mode;             /* 0x004  SND_PCM_MODE_* (1 or 2) */
	uint8_t  sync[16];         /* 0x008  hardware sync id (unused) */
	int32_t  interleave;       /* 0x018  format.interleave */
	int32_t  format;           /* 0x01C  format.format = SND_PCM_SFMT_* */
	int32_t  rate;             /* 0x020  format.rate (Hz) */
	int32_t  voices;           /* 0x024  format.voices (channels) */
	int8_t   special;          /* 0x028  format.special (ends the 17-byte fmt) */
	uint8_t  digital[0x1D8 - 0x29]; /* 0x029  IEC958 digital setup (zeroed) */
	int32_t  start_mode;       /* 0x1D8  SND_PCM_START_* */
	int32_t  stop_mode;        /* 0x1DC  SND_PCM_STOP_* (inferred offset) */
	int32_t  time_flags;       /* 0x1E0  time:1 / ust_time:1 (inferred) */
	int32_t  why_failed;       /* 0x1E4  SND_PCM_PARAMS_BAD_*, set by driver */
	int32_t  frag_size;        /* 0x1E8  buf.block.frag_size (bytes) */
	int32_t  frags_min;        /* 0x1EC  buf.block.frags_min (inferred) */
	int32_t  frags_max;        /* 0x1F0  buf.block.frags_max */
	uint8_t  tail[0x2F0 - 0x1F4];   /* 0x1F4  ust/reserved */
} snd_pcm_channel_params_t;

/*
 * snd_pcm_channel_setup_t - read-back of the negotiated setup. Unlike
 * channel_params there is no sync[] field: snd_pcm_plugin_setup()
 * (libasound 0xC378) checks mode @0x04 (1 or 2, filled in by the driver's
 * setup devctl 0x42D84138, sized 0x2D8 = 728 bytes) and memcpys the
 * plugin's 17-byte format block to @0x08.                     [CONFIRMED]
 * The 12.10 v2 run read it at @0x18 (the params offset) and logged
 * "0 Hz, 0 ch". Padded past 0x2D8 so the library cannot write past it.
 */
typedef struct snd_pcm_channel_setup {
	int32_t  channel;          /* 0x000 */
	int32_t  mode;             /* 0x004 */
	int32_t  interleave;       /* 0x008  format.interleave */
	int32_t  format;           /* 0x00C  format.format */
	int32_t  rate;             /* 0x010  format.rate */
	int32_t  voices;           /* 0x014  format.voices */
	int8_t   special;          /* 0x018  format.special */
	uint8_t  _pad[0x320 - 0x19];    /* cover the 0x2D8 the driver writes */
} snd_pcm_channel_setup_t;

/* ---- the handful of entry points hello5 calls ---- */
int  snd_pcm_open(snd_pcm_t **handle, int card, int device, int mode);
int  snd_pcm_open_preferred(snd_pcm_t **handle, int *card, int *device, int mode);
int  snd_pcm_close(snd_pcm_t *handle);
int  snd_pcm_plugin_set_src_method(snd_pcm_t *handle, int method);
int  snd_pcm_plugin_set_disable(snd_pcm_t *handle, int mask);
int  snd_pcm_plugin_params(snd_pcm_t *handle, snd_pcm_channel_params_t *params);
int  snd_pcm_plugin_setup(snd_pcm_t *handle, snd_pcm_channel_setup_t *setup);
int  snd_pcm_plugin_prepare(snd_pcm_t *handle, int channel);
int  snd_pcm_plugin_playback_drain(snd_pcm_t *handle);
ssize_t snd_pcm_plugin_write(snd_pcm_t *handle, const void *buffer, size_t size);
const char *snd_strerror(int errnum);

/* Lock the offsets/size recovered from libasound.so.2. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(snd_pcm_channel_params_t, channel)    == 0x000, "channel");
_Static_assert(offsetof(snd_pcm_channel_params_t, mode)       == 0x004, "mode");
_Static_assert(offsetof(snd_pcm_channel_params_t, interleave) == 0x018, "format@0x18");
_Static_assert(offsetof(snd_pcm_channel_params_t, format)     == 0x01C, "format.format");
_Static_assert(offsetof(snd_pcm_channel_params_t, rate)       == 0x020, "rate");
_Static_assert(offsetof(snd_pcm_channel_params_t, voices)     == 0x024, "voices");
_Static_assert(offsetof(snd_pcm_channel_params_t, special)    == 0x028, "format.special");
_Static_assert(offsetof(snd_pcm_channel_params_t, start_mode) == 0x1D8, "start_mode");
_Static_assert(offsetof(snd_pcm_channel_params_t, stop_mode)  == 0x1DC, "stop_mode");
_Static_assert(offsetof(snd_pcm_channel_params_t, why_failed) == 0x1E4, "why_failed");
_Static_assert(offsetof(snd_pcm_channel_params_t, frag_size)  == 0x1E8, "frag_size");
_Static_assert(offsetof(snd_pcm_channel_params_t, frags_max)  == 0x1F0, "frags_max");
_Static_assert(sizeof(snd_pcm_channel_params_t)               == 0x2F0, "sizeof 752");
_Static_assert(offsetof(snd_pcm_channel_setup_t, interleave)  == 0x008, "setup.format@0x08");
_Static_assert(offsetof(snd_pcm_channel_setup_t, rate)        == 0x010, "setup.rate");
_Static_assert(sizeof(snd_pcm_channel_setup_t)                >= 0x2D8, "setup fits driver");
#endif

#if defined(__cplusplus)
}
#endif

#endif /* RIVIT_SYS_ASOUNDLIB_H */
