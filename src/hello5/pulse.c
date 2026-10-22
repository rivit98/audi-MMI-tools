/*
 * pulse.c - see pulse.h. The tone comes from a rotating phasor, so there is
 * no sin() per sample and no libm (the cross sysroot has no <math.h>).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pulse.h"

#define PEAK      0.62          /* as sdk/mkbeep.py: headroom for the mix */
#define ATTACK_MS 5
#define H2_GAIN   0.25          /* 2nd harmonic, for a less thin ping */

/* sin and cos of a small angle (|x| < 0.5 here) by Taylor series. */
static void sincos_small(double x, double *s, double *c)
{
	double x2 = x * x;

	*s = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
	*c = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56)));
}

uint32_t pulse_samples(unsigned n)
{
	return (uint32_t)n * (PULSE_RATE / 1000) * PULSE_PERIOD_MS;
}

/* One ping: a fresh phasor per ping, so every ping starts at phase 0 and
 * sounds the same. Linear attack, quadratic decay to exactly 0 at the end. */
static void ping(int16_t *out, uint32_t len)
{
	const double w = 2 * 3.14159265358979323846 * PULSE_HZ / PULSE_RATE;
	const uint32_t attack = PULSE_RATE / 1000 * ATTACK_MS;
	const double scale = PEAK * 32767.0 / (1 + H2_GAIN);
	double ws, wc, w2s, w2c, s1 = 0, c1 = 1, s2 = 0, c2 = 1, t;
	uint32_t i;

	sincos_small(w, &ws, &wc);
	sincos_small(2 * w, &w2s, &w2c);
	for (i = 0; i < len; i++) {
		double env, ns, nc;

		if (i < attack)
			env = (double)i / attack;
		else {
			t = 1 - (double)(i - attack) / (len - attack);
			env = t * t;
		}
		out[i] = (int16_t)(scale * env * (s1 + H2_GAIN * s2));
		/* advance both phasors */
		ns = s1 * wc + c1 * ws;
		nc = c1 * wc - s1 * ws;
		s1 = ns;
		c1 = nc;
		ns = s2 * w2c + c2 * w2s;
		nc = c2 * w2c - s2 * w2s;
		s2 = ns;
		c2 = nc;
	}
}

void pulse_render(int16_t *pcm, unsigned n)
{
	const uint32_t period = PULSE_RATE / 1000 * PULSE_PERIOD_MS;
	const uint32_t len = PULSE_RATE / 1000 * PULSE_PING_MS;
	unsigned k;

	memset(pcm, 0, (size_t)pulse_samples(n) * sizeof(*pcm));
	for (k = 0; k < n; k++)
		ping(pcm + (size_t)k * period, len);
}

static void put16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

void wav_header(unsigned char *h, uint32_t rate, uint16_t voices,
                uint16_t bits, uint32_t data_len)
{
	uint16_t block = (uint16_t)(voices * bits / 8);

	memcpy(h, "RIFF", 4);
	put32(h + 4, 36 + data_len);
	memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16);
	put16(h + 20, 1);               /* PCM */
	put16(h + 22, voices);
	put32(h + 24, rate);
	put32(h + 28, rate * block);
	put16(h + 32, block);
	put16(h + 34, bits);
	memcpy(h + 36, "data", 4);
	put32(h + 40, data_len);
}

int pulse_write_wav(const char *path, unsigned n)
{
	uint32_t ns = pulse_samples(n), i;
	unsigned char hdr[WAV_HEADER], *le;
	int16_t *pcm = malloc((size_t)ns * sizeof(*pcm));
	FILE *f;
	int rc = -1;

	if (pcm == NULL)
		return -1;
	pulse_render(pcm, n);
	/* little-endian on disk whatever the host (the SH4 is LE anyway) */
	le = (unsigned char *)pcm;
	for (i = 0; i < ns; i++)
		put16(le + i * 2, (uint16_t)pcm[i]);
	wav_header(hdr, PULSE_RATE, 1, 16, ns * 2);
	f = fopen(path, "wb");
	if (f != NULL) {
		if (fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
		    fwrite(le, 2, ns, f) == ns)
			rc = 0;
		if (fclose(f) != 0)
			rc = -1;
	}
	free(pcm);
	return rc;
}
