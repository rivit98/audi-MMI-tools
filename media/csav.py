#!/usr/bin/env python3
"""csav.py <in.h264> <in_audio> <aac|mp3> <out.mp4> <fps>

A constant-size MP4 (media/csmp4.py) with an audio track: the layout a live
stream with sound would use. The header has to be written before any frame
exists, so every sample of a track has the same size and the tables need no
per-sample entries:

  mdat = chunk 0 | chunk 1 | ...   (every chunk C bytes)
  chunk = nv video frames of Sv bytes | na audio frames of Sa bytes

nv video frames last exactly as long as na audio frames (AAC at 48 kHz, 25
fps: 8 and 15, 320 ms; MP3 at 48 kHz: 3 and 5, 120 ms). Each track's stco
lists every chunk, so the header grows by 8 bytes per chunk.

- Video frames are padded with a filler NAL, as csmp4.py does.
- AAC frames (raw, from ADTS) are padded with zero bytes after the frame's
  END element. Whether the DSP's decoder ignores them is one of the things
  hello9 tests.
- MP3 at 48 kHz has no padding slot: every CBR frame has the same size.

The input audio is an ADTS stream (aac) or a raw MP3 frame stream (mp3, no
ID3 tag, no Xing frame). Prints the layout.
"""
import math
import struct
import sys

from csmp4 import FILLER_MIN, MATRIX, access_units, box, fullbox, nal_units, sps_size

AAC_RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025,
             8000, 7350]
MP3_KBPS = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
MP3_RATES = [44100, 48000, 32000]


def video_frames(path):
    with open(path, 'rb') as f:
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
        sys.exit('csav: no SPS/PPS or no frames in ' + path)
    return sps, pps, frames


def adts_frames(data):
    """Raw AAC frames, the sample rate, channels and AudioSpecificConfig."""
    frames, i = [], 0
    rate = channels = asc = None
    while i + 7 <= len(data):
        h = data[i:i + 9]
        if h[0] != 0xff or h[1] & 0xf0 != 0xf0:
            sys.exit(f'csav: ADTS sync lost at {i}')
        crc = not h[1] & 1
        aot = (h[2] >> 6) + 1
        sfi = (h[2] >> 2) & 0xf
        ch = (h[2] & 1) << 2 | h[3] >> 6
        length = (h[3] & 3) << 11 | h[4] << 3 | h[5] >> 5
        hdr = 9 if crc else 7
        frames.append(data[i + hdr:i + length])
        if asc is None:
            rate, channels = AAC_RATES[sfi], ch
            asc = struct.pack('>H', aot << 11 | sfi << 7 | ch << 3)
        i += length
    return frames, rate, channels, asc


def mp3_frames(data):
    """MPEG-1 Layer III frames, the sample rate and channels."""
    frames, i = [], 0
    rate = channels = None
    while i + 4 <= len(data):
        b1, b2, b3 = data[i + 1], data[i + 2], data[i + 3]
        if data[i] != 0xff or b1 & 0xfe != 0xfa:  # MPEG-1 Layer III, CRC or not
            sys.exit(f'csav: not an MPEG-1 Layer III frame at {i}')
        kbps, sr = MP3_KBPS[b2 >> 4], MP3_RATES[(b2 >> 2) & 3]
        length = 144000 * kbps // sr + ((b2 >> 1) & 1)
        frames.append(data[i:i + length])
        if rate is None:
            rate, channels = sr, 1 if b3 >> 6 == 3 else 2
        i += length
    return frames, rate, channels


def descr(tag, *payload):
    """An MPEG-4 descriptor with ffmpeg's 4-byte length form."""
    body = b''.join(payload)
    n = len(body)
    return bytes([tag, 0x80 | (n >> 21) & 0x7f, 0x80 | (n >> 14) & 0x7f,
                  0x80 | (n >> 7) & 0x7f, n & 0x7f]) + body


def esds(oti, dsi, bitrate, buf_size):
    dec = descr(0x04, bytes([oti, 0x15]), struct.pack('>I', buf_size)[1:],
                struct.pack('>II', bitrate, bitrate), descr(0x05, dsi) if dsi else b'')
    return fullbox(b'esds', 0, 0, descr(0x03, struct.pack('>HB', 2, 0), dec,
                                        descr(0x06, b'\x02')))


def trak(track_id, tkhd_tail, mdhd, hdlr, media_header, stsd_entry, tables):
    stbl = box(b'stbl', fullbox(b'stsd', 0, 0, struct.pack('>I', 1), stsd_entry), *tables)
    minf = box(b'minf', media_header,
               box(b'dinf', fullbox(b'dref', 0, 0, struct.pack('>I', 1), fullbox(b'url ', 0, 1))),
               stbl)
    tkhd = fullbox(b'tkhd', 0, 3, struct.pack('>IIIII', 0, 0, track_id, 0, mdhd[2]),
                   b'\0' * 8, tkhd_tail)
    mdia = box(b'mdia', fullbox(b'mdhd', 0, 0, struct.pack('>IIIIHH', 0, 0, mdhd[0], mdhd[1],
                                                           0x55c4, 0)), hdlr, minf)
    return box(b'trak', tkhd, mdia)


def main():
    if len(sys.argv) != 6 or sys.argv[3] not in ('aac', 'mp3'):
        sys.exit(__doc__)
    vsrc, asrc, codec, dst, fps = sys.argv[1:4] + [sys.argv[4], int(sys.argv[5])]
    sps, pps, vframes = video_frames(vsrc)
    width, height = sps_size(sps)
    with open(asrc, 'rb') as f:
        adata = f.read()
    if codec == 'aac':
        aframes, rate, channels, dsi = adts_frames(adata)
        spf, oti = 1024, 0x40
    else:
        aframes, rate, channels = mp3_frames(adata)
        dsi, spf, oti = None, 1152, 0x6b
        if len({len(a) for a in aframes}) != 1:
            sys.exit('csav: MP3 frames of different sizes (use 48 kHz CBR)')

    # nv video frames last as long as na audio frames
    g = math.gcd(rate, spf * fps)
    nv, na = spf * fps // g, rate // g
    nchunks = min(len(vframes) // nv, len(aframes) // na)
    vframes, aframes = vframes[:nchunks * nv], aframes[:nchunks * na]
    sv = (max(len(f) for f in vframes) + FILLER_MIN + 4095) // 4096 * 4096
    sa = max(len(a) for a in aframes)
    chunk = nv * sv + na * sa
    nvid, naud = len(vframes), len(aframes)
    vts = fps * 512
    dur_ms = nvid * 1000 // fps

    avcc = box(b'avcC', bytes([1, sps[1], sps[2], sps[3], 0xff, 0xe1]),
               struct.pack('>H', len(sps)), sps, b'\x01', struct.pack('>H', len(pps)), pps)
    avc1 = box(b'avc1', b'\0' * 6, struct.pack('>H', 1), b'\0' * 16,
               struct.pack('>HHIIIH', width, height, 0x480000, 0x480000, 0, 1),
               b'\0' * 32, struct.pack('>Hh', 0x18, -1), avcc)
    mp4a = box(b'mp4a', b'\0' * 6, struct.pack('>H', 1), b'\0' * 8,
               struct.pack('>HHHHI', channels, 16, 0, 0, rate << 16),
               esds(oti, dsi, sa * 8 * rate // spf, sa))

    def moov(data_off):
        vco = struct.pack(f'>I{nchunks}I', nchunks, *(data_off + k * chunk for k in range(nchunks)))
        aco = struct.pack(f'>I{nchunks}I', nchunks,
                          *(data_off + k * chunk + nv * sv for k in range(nchunks)))
        vtrak = trak(1, struct.pack('>hhhH', 0, 0, 0, 0) + MATRIX +
                     struct.pack('>II', width << 16, height << 16),
                     (vts, nvid * 512, dur_ms),
                     fullbox(b'hdlr', 0, 0, struct.pack('>I4s', 0, b'vide'), b'\0' * 12,
                             b'VideoHandler\0'),
                     fullbox(b'vmhd', 0, 1, b'\0' * 8), avc1,
                     [fullbox(b'stts', 0, 0, struct.pack('>III', 1, nvid, 512)),
                      fullbox(b'stsc', 0, 0, struct.pack('>IIII', 1, 1, nv, 1)),
                      fullbox(b'stsz', 0, 0, struct.pack('>II', sv, nvid)),
                      fullbox(b'stco', 0, 0, vco)])
        atrak = trak(2, struct.pack('>hhhH', 0, 1, 0x100, 0) + MATRIX + struct.pack('>II', 0, 0),
                     (rate, naud * spf, dur_ms),
                     fullbox(b'hdlr', 0, 0, struct.pack('>I4s', 0, b'soun'), b'\0' * 12,
                             b'SoundHandler\0'),
                     fullbox(b'smhd', 0, 0, b'\0' * 4), mp4a,
                     [fullbox(b'stts', 0, 0, struct.pack('>III', 1, naud, spf)),
                      fullbox(b'stsc', 0, 0, struct.pack('>IIII', 1, 1, na, 1)),
                      fullbox(b'stsz', 0, 0, struct.pack('>II', sa, naud)),
                      fullbox(b'stco', 0, 0, aco)])
        mvhd = fullbox(b'mvhd', 0, 0, struct.pack('>IIII', 0, 0, 1000, dur_ms),
                       struct.pack('>IH', 0x10000, 0x100), b'\0' * 10, MATRIX,
                       b'\0' * 24, struct.pack('>I', 3))
        return box(b'moov', mvhd, vtrak, atrak)

    ftyp = box(b'ftyp', b'isom', struct.pack('>I', 0x200), b'isomiso2avc1mp41')
    data_off = len(ftyp + moov(0)) + 8
    head = ftyp + moov(data_off)
    assert len(head) + 8 == data_off
    with open(dst, 'wb') as f:
        f.write(head)
        f.write(struct.pack('>I4s', 8 + nchunks * chunk, b'mdat'))
        for k in range(nchunks):
            for fr in vframes[k * nv:(k + 1) * nv]:
                pad = sv - len(fr)
                f.write(fr + struct.pack('>I', pad - 4) + b'\x0c' + b'\xff' * (pad - 6) + b'\x80')
            for a in aframes[k * na:(k + 1) * na]:
                f.write(a + b'\0' * (sa - len(a)))

    print(f'{dst}: {width}x{height} {nvid} frames of {sv} B, {codec} {rate} Hz {channels} ch '
          f'{naud} frames of {sa} B; chunk = {nv} video + {na} audio = {chunk} B, '
          f'{nv * 1000 // fps} ms; {nchunks} chunks, data at {data_off}')


if __name__ == '__main__':
    main()
