#!/usr/bin/env python3
"""f3swalk.py <image.efs> <outdir> [path-substring]

Extract a QNX F3S flash image (efs-system.efs, efs-extended.efs) by
following its extents, so files come out whole even when their data is
split over several extents or units. The repo's scan-based
tools/extract_f3s_efs.py guesses file ends instead, which is how the old
io-media-nvidia carve got its data segment shifted by 0x1dc8.

Layout (openqnx utils/m/mkxfs/mkxfs/mk_flash_fsys.c, checked on P1001):
  unit (1 << unit_info.unit_pow2 bytes, 256 KB here):
    +0x00 unit_info: struct_size u16, endian 'L', ..., unit_pow2 u16 @4
    +0x10 unit_logi: ..., logi u16 @0x12 (logical unit number, 1-based)
    extent headers grow down from the unit's end, 32 bytes apart:
    header i at unit_end - 16 - 32*i:
      status u16, reserve u8, text_offset_hi u8, text_offset_lo u16,
      text_size u16, next extptr, super extptr
      text = unit + ((hi << 16 | lo) << 2), text_size bytes
  extptr = (logi u16, index u16); 0xffff = none
  boot_info at 0x28 of logical unit 1; root dirent extptr at 0x3c
  dirent text: struct_size u16, moves u8, namelen u8, first extptr,
    name (4-aligned), stat (mode u16 @2: 4 = dir, 8 = file, a = link)
  a dirent's extent 'next' = its next sibling; 'first' = first child (dir)
  or first data extent (file/link), data extents chained by 'next'.

Files are written under <outdir>; one line per file ("size path").
Compressed (UCL "iwlyfmbp") files come out as stored: inflate them with
tools/qnx_inflator.c.
"""
import os
import struct
import sys

FNULL = 0xffff


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        img = f.read()
    out = sys.argv[2]
    want = sys.argv[3] if len(sys.argv) > 3 else None

    usz = 1 << struct.unpack_from('<H', img, 4)[0]
    units = {}
    for p in range(len(img) // usz):
        base = p * usz
        if img[base + 2:base + 3] == b'L':
            units.setdefault(struct.unpack_from('<H', img, base + 0x12)[0], base)

    def text(ptr):
        base = units[ptr[0]]
        h = base + usz - 16 - 32 * ptr[1]
        _, _, hi, lo, size, nlu, nidx = struct.unpack_from('<HBBHHHH', img, h)
        off = base + (((hi << 16) | lo) << 2)
        return img[off:off + size], (nlu, nidx)

    def chain(ptr):
        data = b''
        while ptr[0] != FNULL:
            t, ptr = text(ptr)
            data += t
        return data

    def walk(ptr, path):
        while ptr[0] != FNULL:
            t, nxt = text(ptr)
            nl = t[3]
            first = struct.unpack_from('<HH', t, 4)
            name = t[8:8 + nl].split(b'\0')[0].decode('latin1')
            mode = struct.unpack_from('<H', t, 8 + ((nl + 3) & ~3) + 2)[0]
            p = os.path.join(path, name) if name not in ('/', '') else path
            kind = mode >> 12
            if kind == 0x4:
                os.makedirs(out + p, exist_ok=True)
                walk(first, p)
            elif kind == 0x8 and (want is None or want in p):
                d = chain(first) if first[0] != FNULL else b''
                os.makedirs(os.path.dirname(out + p), exist_ok=True)
                with open(out + p, 'wb') as f:
                    f.write(d)
                print(len(d), p)
            elif kind == 0xa:
                d = chain(first) if first[0] != FNULL else b''
                print('LINK', p, '->', d.split(b'\0')[0].decode('latin1'))
            ptr = nxt

    walk(struct.unpack_from('<HH', img, 0x3c), '/')


if __name__ == '__main__':
    main()
