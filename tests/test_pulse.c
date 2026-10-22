/*
 * test_pulse.c - src/hello5/pulse.c: the WAV header, the train's layout
 * (n pings, silence between), clean ping edges, the level and the pitch.
 */
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "pulse.h"

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static void test_header(void)
{
	unsigned char h[WAV_HEADER];

	wav_header(h, 44100, 1, 16, 1000);
	CHECK(memcmp(h, "RIFF", 4) == 0);
	CHECK_EQ(rd32(h + 4), 1036);
	CHECK(memcmp(h + 8, "WAVEfmt ", 8) == 0);
	CHECK_EQ(rd32(h + 16), 16);
	CHECK_EQ(h[20] | h[21] << 8, 1);
	CHECK_EQ(h[22] | h[23] << 8, 1);
	CHECK_EQ(rd32(h + 24), 44100);
	CHECK_EQ(rd32(h + 28), 88200);
	CHECK_EQ(h[32] | h[33] << 8, 2);
	CHECK_EQ(h[34] | h[35] << 8, 16);
	CHECK(memcmp(h + 36, "data", 4) == 0);
	CHECK_EQ(rd32(h + 40), 1000);
}

static void test_train(void)
{
	const unsigned n = 3;
	const uint32_t period = PULSE_RATE / 1000 * PULSE_PERIOD_MS;
	const uint32_t len = PULSE_RATE / 1000 * PULSE_PING_MS;
	uint32_t ns = pulse_samples(n), i, k;
	int16_t *pcm = malloc(ns * sizeof(*pcm));
	int peak = 0, ok_silence = 1, ok_edges = 1, ok_same = 1;

	CHECK_EQ(ns, n * period);
	pulse_render(pcm, n);
	for (k = 0; k < n; k++) {
		const int16_t *p = pcm + k * period;
		unsigned up = 0;

		ok_edges &= p[0] == 0 && abs(p[len - 1]) < 50;
		for (i = 0; i < len; i++) {
			if (abs(p[i]) > peak)
				peak = abs(p[i]);
			ok_same &= p[i] == pcm[i];      /* every ping identical */
			if (i > 0 && p[i - 1] < 0 && p[i] >= 0)
				up++;
		}
		for (i = len; i < period; i++)
			ok_silence &= p[i] == 0;
		/* 880 Hz for 0.5 s = 440 upward zero crossings (the 2nd
		 * harmonic is too weak to add any) */
		CHECK(up >= 435 && up <= 441);
	}
	CHECK(ok_edges);
	CHECK(ok_silence);
	CHECK(ok_same);
	CHECK(peak > 32767 * 0.5 && peak < 32767 * 0.66);
	free(pcm);
}

int main(void)
{
	test_header();
	test_train();
	CHECK_DONE();
}
