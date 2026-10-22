#!/usr/bin/env python3
"""mkbeep.py - render src/hello5/beep.wav, hello5's sound-check chime.

The first clip was 100 ms at 22.05 kHz, short enough to read as a click.
The second, a 600 ms two-note chime, was heard at most once in the 19.10 v2
run, and the listener could not tell which group it came from: too short to
notice and place in a running car. This renders one long chime instead:

    A5 (880 Hz) strike, then D6 (1174.66 Hz) held for ~2 s, 2.5 s total

It is still one sound per chime, so "group N = N chimes" can be counted.
44.1 kHz so the plugin layer has no resampling to do (22.05 kHz needed a
2x rate conversion, and the tones device is very unlikely to run natively
below 44.1 kHz). Mono 16-bit PCM, the format hello5's WAV loader expects.

Each note is a sine with a little second and third harmonic for body, under
a raised-cosine attack and an exponential decay, so it starts and ends at
zero: a clip that begins or ends mid-waveform produces an audible click on
every repeat.

usage: mkbeep.py <out.wav>

Pure Python (stdlib only), so it runs in the mmi-sdk image.
"""

import math
import struct
import sys

RATE = 44100
BITS = 16
VOICES = 1
PEAK = 0.62  # headroom: the tones mixer adds our output to media/nav

# (start seconds, length seconds, Hz, gain, decay: e-folds over the note)
NOTES = [
    (0.00, 0.60, 880.00, 1.00, 6.0),  # A5, a short strike
    (0.25, 2.25, 1174.66, 0.90, 1.2),  # D6, held: ~10 dB down by its end
]
LENGTH = 2.50
ATTACK = 0.006

# relative amplitude of partials 1..3
PARTIALS = (1.0, 0.28, 0.10)


def envelope(t, length, decay):
    """Raised-cosine attack, exponential decay, forced to zero at the end."""
    if t < 0.0 or t > length:
        return 0.0
    if t < ATTACK:
        a = 0.5 - 0.5 * math.cos(math.pi * t / ATTACK)
    else:
        a = 1.0
    a *= math.exp(-decay * t / length)
    # taper the last 8 ms to silence so the clip ends on a zero crossing
    tail = 0.008
    if t > length - tail:
        a *= (length - t) / tail
    return a


def render():
    n = int(RATE * LENGTH)
    samples = [0.0] * n
    for start, length, freq, gain, decay in NOTES:
        for i in range(n):
            t = i / RATE - start
            env = envelope(t, length, decay)
            if env == 0.0:
                continue
            v = 0.0
            for k, amp in enumerate(PARTIALS, start=1):
                v += amp * math.sin(2.0 * math.pi * freq * k * t)
            samples[i] += gain * env * v

    peak = max(abs(v) for v in samples) or 1.0
    scale = PEAK * 32767.0 / peak
    return b''.join(struct.pack('<h', int(round(v * scale))) for v in samples)


def wav(pcm):
    block = VOICES * BITS // 8
    fmt = struct.pack(
        '<4sIHHIIHH', b'fmt ', 16, 1, VOICES, RATE, RATE * block, block, BITS
    )
    data = struct.pack('<4sI', b'data', len(pcm)) + pcm
    body = b'WAVE' + fmt + data
    return struct.pack('<4sI', b'RIFF', len(body)) + body


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: mkbeep.py <out.wav>')
    pcm = render()
    out = wav(pcm)
    with open(sys.argv[1], 'wb') as f:
        f.write(out)
    ms = len(pcm) * 1000 // (RATE * VOICES * BITS // 8)
    print(
        '%s: %d Hz, %d ch, %d-bit, %d bytes (%d ms)'
        % (sys.argv[1], RATE, VOICES, BITS, len(pcm), ms)
    )


if __name__ == '__main__':
    main()
