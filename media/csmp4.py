#!/usr/bin/env python3
"""csmp4.py <in.h264> <out.mp4> <fps> [declared_seconds]

Muxes an Annex-B H.264 stream into a "constant sample size" MP4: every
frame is padded to the same size S with a filler NAL unit (type 12), so
the whole sample table is one entry per box (stts, stsc, stco, and stsz
with a constant size) and a frame's place in mdat is just k * S. The moov
is written first and can declare more frames than the file holds
(declared_seconds), so it can go out before a live stream's frames exist.
mdat has an 8-byte header: the exact size, or 0 (to the end of the file)
when the file is over-declared.

Why this layout: io-media's mp4_parser (efs lib/dll/mmedia/mp4_parser.so)
needs non-empty stts/stsc/stco/stsz tables in moov (validate_parsed_info),
ignores moof/trun (fragmented MP4 adds no samples), and with a constant
stsz size reads no per-sample table at all. docs/phone-mirroring.md 7.8.

AUD NAL units are dropped; SPS/PPS go into avcC and stay in-band too.
Prints S, the frame count and the largest frame.
"""
import struct
import sys

FILLER_MIN = 4 + 2  # length prefix + NAL header + rbsp trailing byte


def nal_units(data):
    """Split an Annex-B byte stream into NAL units (without start codes)."""
    starts = []
    i = 0
    n = len(data)
    while True:
        i = data.find(b'\x00\x00\x01', i)
        if i < 0:
            break
        starts.append(i + 3)
        i += 3
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        while e > s and data[e - 1] == 0:  # 4-byte start code / trailing zeros
            e -= 1
        yield data[s:e]


def access_units(nals):
    """Group NAL units into frames: a new frame starts at an AUD, SPS or
    PPS after a slice, or at a slice with first_mb_in_slice == 0 (the
    first bit of the slice header after the NAL header is 1 then)."""
    au, have_slice = [], False
    for nal in nals:
        t = nal[0] & 0x1f
        starts_new = False
        if t in (9, 7, 8, 6) and have_slice:
            starts_new = True
        elif t in (1, 5) and have_slice and nal[1] & 0x80:
            starts_new = True
        if starts_new:
            yield au
            au, have_slice = [], False
        if t in (1, 5):
            have_slice = True
        au.append(nal)
    if au:
        yield au


def box(kind, *payload):
    body = b''.join(payload)
    return struct.pack('>I4s', 8 + len(body), kind) + body


def fullbox(kind, version, flags, *payload):
    return box(kind, struct.pack('>I', version << 24 | flags), *payload)


MATRIX = struct.pack('>9I', 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)


def sps_size(sps):
    """Width and height from an SPS (Baseline: no chroma_format_idc etc.)."""
    rbsp = bytearray()
    zeros = 0
    for b in sps[1:]:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        rbsp.append(b)
        zeros = zeros + 1 if b == 0 else 0
    bits = ''.join(f'{b:08b}' for b in rbsp)
    pos = 0

    def u(n):
        nonlocal pos
        v = int(bits[pos:pos + n], 2)
        pos += n
        return v

    def ue():
        nonlocal pos
        z = 0
        while bits[pos] == '0':
            z += 1
            pos += 1
        pos += 1
        return (1 << z) - 1 + (u(z) if z else 0)

    profile = u(8)
    u(16)  # constraint flags + level
    ue()  # sps id
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128):
        sys.exit('csmp4: only Baseline/Main SPS supported')
    ue()  # log2_max_frame_num
    poc_type = ue()
    if poc_type == 0:
        ue()
    elif poc_type == 1:
        u(1)
        ue()
        ue()
        for _ in range(ue()):
            ue()
    ue()  # max_num_ref_frames
    u(1)
    w_mbs = ue() + 1
    h_map = ue() + 1
    frame_mbs_only = u(1)
    if not frame_mbs_only:
        u(1)
    u(1)  # direct_8x8_inference
    w, h = w_mbs * 16, h_map * 16 * (2 - frame_mbs_only)
    if u(1):  # frame_cropping
        l, r, t, b = ue(), ue(), ue(), ue()
        w -= 2 * (l + r)
        h -= 2 * (t + b) * (2 - frame_mbs_only)
    return w, h


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    src, dst, fps = sys.argv[1], sys.argv[2], int(sys.argv[3])
    declared_s = float(sys.argv[4]) if len(sys.argv) == 5 else None

    with open(src, 'rb') as f:
        data = f.read()
    sps = pps = None
    frames = []
    for au in access_units(nal_units(data)):
        keep = []
        for nal in au:
            t = nal[0] & 0x1f
            if t == 9:
                continue
            if t == 7 and sps is None:
                sps = nal
            if t == 8 and pps is None:
                pps = nal
            keep.append(nal)
        if any(n[0] & 0x1f in (1, 5) for n in keep):
            frames.append(b''.join(struct.pack('>I', len(n)) + n for n in keep))
    if sps is None or pps is None or not frames:
        sys.exit('csmp4: no SPS/PPS or no frames in ' + src)
    width, height = sps_size(sps)

    biggest = max(len(f) for f in frames)
    size = (biggest + FILLER_MIN + 4095) // 4096 * 4096
    nframes = len(frames)
    ndeclared = max(nframes, round(declared_s * fps)) if declared_s else nframes

    timescale = fps * 512  # 25 fps -> 12800, ffmpeg's choice
    delta = 512
    dur_ms = ndeclared * 1000 // fps
    dur_media = ndeclared * delta

    avcc = box(b'avcC', bytes([1, sps[1], sps[2], sps[3], 0xff, 0xe1]),
               struct.pack('>H', len(sps)), sps, b'\x01',
               struct.pack('>H', len(pps)), pps)
    avc1 = box(b'avc1', b'\0' * 6, struct.pack('>H', 1), b'\0' * 16,
               struct.pack('>HHIIIH', width, height, 0x480000, 0x480000, 0, 1),
               b'\0' * 32, struct.pack('>Hh', 0x18, -1), avcc)
    ftyp = box(b'ftyp', b'isom', struct.pack('>I', 0x200), b'isomiso2avc1mp41')

    def moov(chunk_offset):
        stbl = box(b'stbl',
                   fullbox(b'stsd', 0, 0, struct.pack('>I', 1), avc1),
                   fullbox(b'stts', 0, 0, struct.pack('>III', 1, ndeclared, delta)),
                   fullbox(b'stsc', 0, 0, struct.pack('>IIII', 1, 1, ndeclared, 1)),
                   fullbox(b'stsz', 0, 0, struct.pack('>II', size, ndeclared)),
                   fullbox(b'stco', 0, 0, struct.pack('>II', 1, chunk_offset)))
        minf = box(b'minf', fullbox(b'vmhd', 0, 1, b'\0' * 8),
                   box(b'dinf', fullbox(b'dref', 0, 0, struct.pack('>I', 1),
                                        fullbox(b'url ', 0, 1))),
                   stbl)
        mdia = box(b'mdia',
                   fullbox(b'mdhd', 0, 0, struct.pack('>IIIIHH', 0, 0, timescale,
                                                      dur_media, 0x55c4, 0)),
                   fullbox(b'hdlr', 0, 0, struct.pack('>I4s', 0, b'vide'), b'\0' * 12,
                           b'VideoHandler\0'),
                   minf)
        tkhd = fullbox(b'tkhd', 0, 3, struct.pack('>IIIII', 0, 0, 1, 0, dur_ms),
                       b'\0' * 8, struct.pack('>hhhH', 0, 0, 0, 0), MATRIX,
                       struct.pack('>II', width << 16, height << 16))
        mvhd = fullbox(b'mvhd', 0, 0, struct.pack('>IIII', 0, 0, 1000, dur_ms),
                       struct.pack('>IH', 0x10000, 0x100), b'\0' * 10, MATRIX,
                       b'\0' * 24, struct.pack('>I', 2))
        return box(b'moov', mvhd, box(b'trak', tkhd, mdia))

    # mdat: an 8-byte header with a 32-bit size. io-media's own MP4 demuxer
    # walks the top-level boxes reading 32-bit sizes only; a 64-bit
    # "largesize" header (size field 1) makes it give up at open
    # (docs/phone-mirroring.md 7.9). It also refuses a box that ends past
    # the file's current size, so an over-declared file (growing, or a live
    # stream) gets size 0, "to the end of the file". The whole stream must
    # stay under 4 GB either way.
    head = ftyp + moov(0)
    mdat_hdr_len = 8
    chunk_offset = len(head) + mdat_hdr_len
    head = ftyp + moov(chunk_offset)
    assert len(head) + mdat_hdr_len == chunk_offset
    if chunk_offset + ndeclared * size >= 1 << 32:
        sys.exit('csmp4: the declared stream is 4 GB or more; declare fewer seconds')
    mdat_size = 0 if declared_s else mdat_hdr_len + ndeclared * size

    with open(dst, 'wb') as f:
        f.write(head)
        f.write(struct.pack('>I4s', mdat_size, b'mdat'))
        for fr in frames:
            pad = size - len(fr)
            # filler NAL: forbidden 0, nal_ref_idc 0, type 12; ff..ff 80
            f.write(fr + struct.pack('>I', pad - 4) + b'\x0c' + b'\xff' * (pad - 6) + b'\x80')

    print(f'{dst}: {width}x{height} {nframes} frames (declared {ndeclared}), '
          f'sample size {size} (largest frame {biggest}), '
          f'{size * fps * 8 // 1000} kbit/s on the wire')


if __name__ == '__main__':
    main()
