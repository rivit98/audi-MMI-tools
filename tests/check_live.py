#!/usr/bin/env python3
"""check_live.py prepare|check <dir>  (make test, in the mmi-media image)

prepare: <dir>/in.h264, a 2 s Baseline clip encoded like media/mkclips.sh's
         CS264, and <dir>/ref.mp4, media/csmp4.py's CS-MP4 of it (declared
         600 s). build/test_live then checks that the MMI's header builder
         writes the same bytes, and writes a stream with a filled gap.
check:   decodes that stream (<dir>/gap.h264, and the same frames as a
         CS-MP4 built on the MMI side, <dir>/gap.mp4) with ffmpeg: no
         decode errors, every frame comes out, and every skip frame
         (<dir>/gap_skips.txt) is the picture before it, both ways.
         And the clip with its SPS cropped on the MMI side (<dir>/crop.h264,
         <dir>/crop.mp4): CROP in size, and every frame the same as
         ffmpeg's own crop of the original.
"""
import subprocess
import sys

CROP = (288, 208)    # test_live.cpp's kCropW, kCropH


def run(*args):
    subprocess.run(args, check=True)


def prepare(d):
    run('ffmpeg', '-v', 'error', '-y', '-f', 'lavfi',
        '-i', 'testsrc2=size=320x240:rate=25:duration=2',
        '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.0', '-pix_fmt', 'yuv420p',
        '-preset', 'veryfast', '-b:v', '800k', '-maxrate', '800k', '-bufsize', '200k',
        '-g', '25', '-keyint_min', '25', '-sc_threshold', '0', '-bf', '0', '-refs', '1',
        '-an', '-f', 'h264', f'{d}/in.h264')
    run('python3', '-I', 'media/csmp4.py', f'{d}/in.h264', f'{d}/ref.mp4', '25', '600')


def md5s(path, vf=None):
    out = subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', path, '-map', '0:v',
                          *(['-vf', vf] if vf else []),
                          '-vsync', 'passthrough', '-f', 'framemd5', '-'],
                         check=True, capture_output=True, text=True)
    if out.stderr.strip():
        sys.exit(f'check_live: ffmpeg complained about {path}:\n{out.stderr}')
    return [line.split(',')[-1].strip() for line in out.stdout.splitlines()
            if line and not line.startswith('#')]


def check(d):
    with open(f'{d}/gap_skips.txt') as f:
        lines = f.read().split()
    total, skips = int(lines[0]), [int(x) for x in lines[1:]]
    if not skips:
        sys.exit('check_live: the gap stream has no skip frames')
    for name in ('gap.h264', 'gap.mp4'):
        m = md5s(f'{d}/{name}')
        if len(m) != total:
            sys.exit(f'check_live: {name}: {len(m)} frames decoded, {total} in the stream')
        bad = [k for k in skips if m[k] != m[k - 1]]
        if bad:
            sys.exit(f'check_live: {name}: skip frames {bad} differ from the frame before')
        print(f'check_live: {name}: {total} frames decoded, the {len(skips)} skips repeat '
              f'the frame before them')
    if md5s(f'{d}/gap.h264') != md5s(f'{d}/gap.mp4'):
        sys.exit('check_live: gap.mp4 decodes differently from gap.h264')
    check_crop(d)


def check_crop(d):
    w, h = CROP
    ref = md5s(f'{d}/in.h264', f'crop={w}:{h}:0:0')
    for name in ('crop.h264', 'crop.mp4'):
        size = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                               '-show_entries', 'stream=width,height', '-of', 'csv=p=0',
                               f'{d}/{name}'], check=True, capture_output=True, text=True)
        if size.stdout.strip() != f'{w},{h}':
            sys.exit(f'check_live: {name}: {size.stdout.strip()}, expected {w},{h}')
        if md5s(f'{d}/{name}') != ref:
            sys.exit(f'check_live: {name}: frames differ from ffmpeg\'s crop of in.h264')
        print(f'check_live: {name}: {w}x{h}, {len(ref)} frames = the original cropped')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('prepare', 'check'):
        sys.exit(__doc__)
    {'prepare': prepare, 'check': check}[sys.argv[1]](sys.argv[2])
