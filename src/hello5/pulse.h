/*
 * pulse.h - hello5's listening signal: a short ping at the start of every
 * period, then silence, rendered by the app itself into a WAV file.
 *
 * The silence is the point: between the pings you hear whatever else the
 * car is playing, so one listen says whether our stream mixes with the
 * radio/music or replaces it. Rendering it on the unit (no bundled file,
 * no libm) is also the smallest "the app makes its own sound" test.
 */
#ifndef HELLO5_PULSE_H
#define HELLO5_PULSE_H

#include <stdint.h>

#define PULSE_RATE      44100
#define PULSE_PERIOD_MS 2000    /* one ping every this */
#define PULSE_PING_MS   500     /* length of the ping, the rest is silence */
#define PULSE_HZ        880     /* A5, with a little 2nd harmonic */
#define WAV_HEADER      44

/* Samples (mono) in a train of n pings. */
uint32_t pulse_samples(unsigned n);

/* Render n pings into pcm (pulse_samples(n) samples, mono S16). */
void pulse_render(int16_t *pcm, unsigned n);

/* The canonical 44-byte PCM WAV header. */
void wav_header(unsigned char *hdr, uint32_t rate, uint16_t voices,
                uint16_t bits, uint32_t data_len);

/* Write a train of n pings to path as a WAV file. 0, or -1. */
int pulse_write_wav(const char *path, unsigned n);

#endif
