#!/usr/bin/env python3
"""check_png.py - decode tests/test_png.c's PNGs with zlib, compare pixels.

usage: check_png.py <dir>

For every <name>.png in <dir> there is <name>.rgb with the expected
pixels. Checks every chunk CRC, the zlib stream (zlib checks the Adler-32)
and that the decoded RGB rows, with the row filters undone, equal the
expected ones.
"""
import os
import struct
import sys
import zlib


def decode(data):
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'signature'
    pos, idat, ihdr = 8, b'', None
    while pos < len(data):
        n, = struct.unpack_from('>I', data, pos)
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        crc, = struct.unpack_from('>I', data, pos + 8 + n)
        assert zlib.crc32(kind + body) == crc, 'CRC of %r' % kind
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            assert pos + 12 + n == len(data), 'data after IEND'
            break
        pos += 12 + n
    w, h, depth, ctype, comp, filt, inter = ihdr
    assert (depth, ctype, comp, filt, inter) == (8, 2, 0, 0, 0), ihdr
    d = zlib.decompressobj()
    raw = d.decompress(idat)
    assert d.eof and not d.unused_data, 'zlib stream end'
    stride = w * 3 + 1
    assert len(raw) == stride * h, 'raw size'
    return w, h, unfilter(raw, w * 3, h)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def unfilter(raw, n, h, bpp=3):
    """Undo the per-row PNG filters (0 none, 1 sub, 2 up, 3 average, 4 paeth)."""
    out, prev = bytearray(), bytearray(n)
    for y in range(h):
        f = raw[y * (n + 1)]
        row = bytearray(raw[y * (n + 1) + 1:(y + 1) * (n + 1)])
        assert f <= 4, 'filter type %d' % f
        for i in range(n):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            row[i] = (row[i] + (0, a, b, (a + b) // 2, paeth(a, b, c))[f]) & 0xFF
        out += row
        prev = row
    return bytes(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    names = sorted(f[:-4] for f in os.listdir(d) if f.endswith('.png'))
    failed = 0
    for name in names:
        exp = open(os.path.join(d, name + '.rgb'), 'rb').read()
        ew, eh = struct.unpack_from('>II', exp)
        try:
            w, h, rgb = decode(open(os.path.join(d, name + '.png'), 'rb').read())
            assert (w, h) == (ew, eh), 'size %dx%d' % (w, h)
            assert rgb == exp[8:], 'pixels differ'
        except AssertionError as e:
            print('%s.png: FAIL: %s' % (name, e))
            failed += 1
    print('check_png.py: %d PNGs, %d failed' % (len(names), failed))
    sys.exit(1 if failed or not names else 0)


if __name__ == '__main__':
    main()
