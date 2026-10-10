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
 *       stop_mode @0x1E4 (zeroed by the lib), frag_size @0x1E8
 *       (rate-scaled), frags_max @0x1F0 (read)                    [CONFIRMED]
 *   start_mode @0x1E0 and frags_min @0x1EC sit between those and are
 *   [INFERRED] from the gaps; they are driver-consumed, not read by the lib.
 *
 * The compile-time asserts at the bottom lock the confirmed offsets/size, so
 * a bad edit fails the build rather than the car. The setup-readback struct
 * (snd_pcm_channel_setup_t) could not be re-checked and is over-sized so the
 * library cannot overflow it; hello5 treats its fields as approximate.
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

/* ---- start/stop conditions ---- */
#define SND_PCM_START_DATA   0   /* start as soon as data is written */
#define SND_PCM_START_FULL   1
#define SND_PCM_START_GO     2

#define SND_PCM_STOP_STOP      0
#define SND_PCM_STOP_ROLLOVER  1

/* ---- sample formats (format.format); all <= 15 ---- */
#define SND_PCM_SFMT_S8        0
#define SND_PCM_SFMT_U8        1
#define SND_PCM_SFMT_S16_LE    2
#define SND_PCM_SFMT_S16_BE    3
#define SND_PCM_SFMT_U16_LE    4
#define SND_PCM_SFMT_U16_BE    5
#define SND_PCM_SFMT_S24_LE    6
#define SND_PCM_SFMT_S24_BE    7
#define SND_PCM_SFMT_S32_LE    10
#define SND_PCM_SFMT_S32_BE    11

/* plugin source conversion method (snd_pcm_plugin_set_src_method) */
#define SND_PCM_PLUGIN_CONV_AUTO    0

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
	uint8_t  digital[0x1E0 - 0x28]; /* 0x028  IEC958 digital setup (zeroed) */
	int32_t  start_mode;       /* 0x1E0  SND_PCM_START_* (inferred offset) */
	int32_t  stop_mode;        /* 0x1E4  SND_PCM_STOP_* */
	int32_t  frag_size;        /* 0x1E8  buf.block.frag_size (bytes) */
	int32_t  frags_min;        /* 0x1EC  buf.block.frags_min (inferred) */
	int32_t  frags_max;        /* 0x1F0  buf.block.frags_max */
	uint8_t  tail[0x2F0 - 0x1F4];   /* 0x1F4  time/ust/why_failed/reserved */
} snd_pcm_channel_params_t;

/*
 * snd_pcm_channel_setup_t - read-back of the negotiated setup. snd_pcm_
 * channel_setup() memcpys the driver's cached setup block (same channel_
 * params-shaped prefix: format @0x18) into this struct, and its devctl
 * 0x42D84138 is sized 0x2D8 = 728 bytes. So the prefix offsets match
 * channel_params, and the struct is padded past 0x2D8 so the library
 * cannot write past it. hello5 reads format.* for a log line only.
 */
typedef struct snd_pcm_channel_setup {
	int32_t  channel;          /* 0x000 */
	int32_t  mode;             /* 0x004 */
	uint8_t  sync[16];         /* 0x008 */
	int32_t  interleave;       /* 0x018  format.interleave */
	int32_t  format;           /* 0x01C  format.format */
	int32_t  rate;             /* 0x020  format.rate */
	int32_t  voices;           /* 0x024  format.voices */
	uint8_t  _pad[0x320 - 0x28];    /* cover the 0x2D8 the driver writes */
} snd_pcm_channel_setup_t;

/* ---- the handful of entry points hello5 calls ---- */
int  snd_pcm_open(snd_pcm_t **handle, int card, int device, int mode);
int  snd_pcm_open_preferred(snd_pcm_t **handle, int *card, int *device, int mode);
int  snd_pcm_close(snd_pcm_t *handle);
int  snd_pcm_plugin_set_src_method(snd_pcm_t *handle, int method);
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
_Static_assert(offsetof(snd_pcm_channel_params_t, start_mode) == 0x1E0, "start_mode");
_Static_assert(offsetof(snd_pcm_channel_params_t, stop_mode)  == 0x1E4, "stop_mode");
_Static_assert(offsetof(snd_pcm_channel_params_t, frag_size)  == 0x1E8, "frag_size");
_Static_assert(offsetof(snd_pcm_channel_params_t, frags_max)  == 0x1F0, "frags_max");
_Static_assert(sizeof(snd_pcm_channel_params_t)               == 0x2F0, "sizeof 752");
_Static_assert(offsetof(snd_pcm_channel_setup_t, rate)        == 0x020, "setup.rate");
_Static_assert(sizeof(snd_pcm_channel_setup_t)                >= 0x2D8, "setup fits driver");
#endif

#if defined(__cplusplus)
}
#endif

#endif /* RIVIT_SYS_ASOUNDLIB_H */
