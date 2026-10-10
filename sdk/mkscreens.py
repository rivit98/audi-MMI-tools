#!/usr/bin/env python3
"""mkscreens.py - pre-render the status screens run.sh shows with showScreen.

showScreen (core/bin/showScreen, DrGER's tool) can only display a PNG, so
every screen run.sh may need is drawn here, at `make sd` time, as an 800x480
PNG in build/sd/screens/:

    start.png                first: what the run is, how long, don't pull the card
    next_<app>.png           before <app>: what it tests, what you'll see
    <app>_<verdict>.png      after <app>, one per verdict (see VERDICTS)
    done_<k>.png             at the end: k of the n apps were OK (k = 0..n)

usage: mkscreens.py <out_dir> <apps.conf> <pre_s> <post_s> <final_s> <app>...

Pure Python (zlib only), so it runs in the mmi-sdk image. Text uses the
Terminus Font console fonts in sdk/fonts/ (SIL OFL 1.1, see OFL.txt).
"""
import gzip
import os
import re
import struct
import sys
import textwrap
import zlib

W, H = 800, 480
MARGIN = 24
FOOTER_H = 44
HERE = os.path.dirname(os.path.abspath(__file__))

BG, ACCENT, FOOTER_BG, BOX = 0x101820, 0x5AC8FA, 0x1C2836, 0x0A1016
COLOURS = {'w': 0xE8E8E8, 'd': 0x8A929E, 'g': 0x4CD964, 'r': 0xFF5A50,
           'y': 0xFFB020, 'c': ACCENT}

# verdict -> (colour, explanation). run.sh decides the verdict.
VERDICTS = {
    'OK':      ('g', 'exit code 0, no FAIL lines in its output'),
    'WARN':    ('y', 'exit code 0, but its output has FAIL lines'),
    'FAIL':    ('r', 'it exited with a non-zero exit code'),
    'CRASH':   ('r', 'killed by a signal: see its slog and core files in var/'),
    'TIMEOUT': ('y', 'it ran over its {timeout} s timeout and was stopped'),
    'MISSING': ('d', 'bin/{app} is not on the card'),
}


def load_psf(path):
    """PSF1/PSF2 console font -> (width, height, {char: rows of bits})."""
    raw = gzip.decompress(open(path, 'rb').read())
    if raw[:2] == b'\x36\x04':
        mode, height = raw[2], raw[3]
        count, width, size, off = (512 if mode & 1 else 256), 8, height, 4
        tab, psf2 = raw[off + count * size:] if mode & 6 else b'', False
    elif raw[:4] == b'\x72\xb5\x4a\x86':
        _, off, flags, count, size, height, width = struct.unpack_from('<7I', raw, 4)
        tab, psf2 = raw[off + count * size:] if flags & 1 else b'', True
    else:
        sys.exit('%s: not a PSF font' % path)
    cmap = {}
    if psf2:
        for slot, entry in enumerate(tab.split(b'\xff')[:count]):
            for ch in entry.split(b'\xfe')[0].decode('utf-8', 'replace'):
                cmap.setdefault(ch, slot)
    else:
        slot, i = 0, 0
        while slot < count and i + 1 < len(tab):
            u = struct.unpack_from('<H', tab, i)[0]
            i += 2
            if u == 0xFFFF:
                slot += 1
            elif u != 0xFFFE:
                cmap.setdefault(chr(u), slot)
    row = (width + 7) // 8
    glyphs = {}
    for c in range(0x20, 0x7F):
        slot = cmap.get(chr(c), c)
        g = raw[off + slot * size:off + slot * size + row * height]
        glyphs[chr(c)] = [int.from_bytes(g[y * row:(y + 1) * row], 'big') for y in range(height)]
    return width, height, glyphs


class Font:
    def __init__(self, name):
        self.w, self.h, self.glyphs = load_psf(os.path.join(HERE, 'fonts', name))
        self.top_bit = 1 << (((self.w + 7) // 8) * 8 - 1)


BODY = Font('Lat15-TerminusBold16.psf.gz')        # 8x16
BIG = Font('Lat15-TerminusBold32x16.psf.gz')      # 16x32


class Screen:
    def __init__(self):
        self.px = bytearray(struct.pack('>I', BG)[1:] * (W * H))
        self.y = 20
        self.fill(0, 0, W, 6, ACCENT)

    def fill(self, x, y, w, h, rgb):
        x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, W), min(y + h, H)
        if x1 <= x0:
            return
        span = struct.pack('>I', rgb)[1:] * (x1 - x0)
        for j in range(y0, y1):
            self.px[(j * W + x0) * 3:(j * W + x1) * 3] = span

    def text(self, font, x, y, s, rgb):
        colour = struct.pack('>I', rgb)[1:]
        s = s[:(W - x - MARGIN) // font.w]
        for ch in s:
            rows = font.glyphs.get(ch, font.glyphs['?'])
            for gy, bits in enumerate(rows):
                if not bits or not 0 <= y + gy < H:
                    continue
                base = ((y + gy) * W + x) * 3
                for gx in range(font.w):
                    if bits & (font.top_bit >> gx):
                        self.px[base + gx * 3:base + gx * 3 + 3] = colour
            x += font.w
        return x

    def line(self, s='', colour='w', big=False):
        font = BIG if big else BODY
        if self.y + font.h <= H - FOOTER_H:
            self.text(font, MARGIN, self.y, s, COLOURS[colour])
        self.y += font.h + (6 if big else 0)

    def para(self, s, colour='w'):
        """Word-wrapped body text; "30 s" style numbers + units stay together."""
        s = re.sub(r'(\d) (?=\w)', '\\1\xa0', s)
        for ln in textwrap.wrap(s, (W - 2 * MARGIN) // BODY.w):
            self.line(ln.replace('\xa0', ' '), colour)

    def run_order(self, apps, current, done_all=False):
        """The run order, with the current app highlighted."""
        self.line('Run order:', 'd')
        for i, a in enumerate(apps):
            if done_all or i < current:
                self.line('    %-8s done' % a, 'd')
            elif i == current:
                self.line('  > %-8s now' % a, 'c')
            else:
                self.line('    %s' % a, 'w')

    def footer(self, s):
        y = H - FOOTER_H
        self.fill(0, y, W, FOOTER_H, FOOTER_BG)
        self.text(BODY, MARGIN, y + 14, s, COLOURS['w'])

    def save(self, path):
        raw = b''.join(b'\0' + bytes(self.px[y * W * 3:(y + 1) * W * 3]) for y in range(H))

        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
        with open(path, 'wb') as f:      # 8-bit RGB, like rePartitioningNeeded.png
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


DEFAULT = {'timeout': '120', 'args': '', 'title': '(not in apps.conf)', 'text': ''}


def read_conf(path):
    conf = {}
    for ln in open(path):
        ln = ln.rstrip('\n')
        if ln and not ln.startswith('#'):
            name, timeout, args, title, text = (ln.split('|', 4) + [''] * 4)[:5]
            conf[name] = {'timeout': timeout or '120', 'args': args,
                          'title': title or name, 'text': text}
    return conf


def main():
    if len(sys.argv) < 7:
        sys.exit(__doc__.split('\n\n')[2])
    out, conf_path, pre_s, post_s, final_s = sys.argv[1:6]
    apps = sys.argv[6:]
    conf = read_conf(conf_path)
    n = len(apps)
    os.makedirs(out, exist_ok=True)
    files = []

    s = Screen()
    s.line('rivit_app', 'c', big=True)
    s.line('MMI 3G+ native app tests', 'w', big=True)
    s.para('%d test programs run one after another, each with a screen before '
           'it (what it does, what you should see) and one after it (the '
           'result). This takes a few minutes.' % n)
    s.line()
    s.line('Do not remove the SD card until the "all done" screen', 'y')
    s.line('has gone. Results are written to var/ on the card.', 'y')
    s.line()
    s.run_order(apps, -1)
    s.footer('first test in %s s' % pre_s)
    files.append(('start.png', s))

    for i, app in enumerate(apps):
        c = conf.get(app, DEFAULT)

        s = Screen()
        s.line('rivit_app  %d/%d' % (i + 1, n), 'd', big=True)
        s.line('Next: %s' % app, 'c', big=True)
        s.line(c['title'], 'c')
        s.para(c['text'])
        s.line()
        s.line('runs: %s <sd> %s   timeout: %s s   output: var/%s_out.log'
               % (app, c['args'], c['timeout'], app), 'd')
        s.line()
        s.run_order(apps, i)
        s.footer('%s starts in %s s' % (app, pre_s))
        files.append(('next_%s.png' % app, s))

        for verdict, (colour, why) in VERDICTS.items():
            s = Screen()
            s.line('rivit_app  %d/%d' % (i + 1, n), 'd', big=True)
            s.line('%s: %s' % (app, verdict), colour, big=True)
            s.line(why.format(app=app, timeout=c['timeout']), colour)
            s.line(c['title'], 'd')
            s.line()
            s.line('exit code, run time, stdout + stderr:', 'd')
            s.line('    var/%s_out.log' % app)
            s.line('system log after the run:', 'd')
            s.line('    var/%s_slog.txt' % app)
            s.line()
            s.run_order(apps, i)
            s.footer('%s in %s s' % ('next app' if i + 1 < n else 'results', post_s))
            files.append(('%s_%s.png' % (app, verdict), s))

    for k in range(n + 1):
        s = Screen()
        s.line('rivit_app: all done', 'c', big=True)
        s.line('%d of %d apps OK' % (k, n), 'g' if k == n else 'y' if k else 'r', big=True)
        s.line('verdicts per app: var/summary.txt', 'd')
        s.line('each app\'s output: var/<app>_out.log', 'd')
        s.line('run.sh log: var/rivit_app.log', 'd')
        s.line()
        s.run_order(apps, n, done_all=True)
        s.footer('back to the MMI in %s s' % final_s)
        files.append(('done_%d.png' % k, s))

    for name, s in files:
        s.save(os.path.join(out, name))
    print('%d screens -> %s' % (len(files), out))


if __name__ == '__main__':
    main()
