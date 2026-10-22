#!/bin/bash
# mkclips.sh <outdir> - hello6's test videos (runs in the mmi-media image).
#
# Every clip is D seconds of ffmpeg's moving testsrc2 pattern with its own
# name and a frame counter burnt in, so the driver can tell what is on the
# screen. Silent audio where the container has audio. The H.264 clips are
# Baseline profile, level 3.0, no B-frames, one I-frame per second: what
# io-media-nvidia accepts ("Baseline profile H.264 bitstreams exceeding
# level 3 are not supported") and what a phone's MediaCodec would send.
#
# hello6 (src/hello6/texts.h) has the same list and D (CLIP_S), long
# enough for hello6's two capture-unit attempts per clip (~25 s).
set -euo pipefail

if [[ $# -ne 1 || -z "$1" ]]; then
  echo "usage: $0 <outdir>" >&2
  exit 2
fi
OUT=$1
D=30
FPS=25
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf

# video filter: testsrc2 + label + frame counter
vf() { # vf <w> <h> <label> [seconds]
  echo "testsrc2=size=$1x$2:rate=$FPS:duration=${4:-$D},\
drawtext=fontfile=$FONT:text='$3':x=(w-tw)/2:y=h/10:fontsize=h/12:fontcolor=white:box=1:boxcolor=black@0.7:boxborderw=6,\
drawtext=fontfile=$FONT:text='%{n}':x=(w-tw)/2:y=(h-th)/2:fontsize=h/4:fontcolor=yellow:box=1:boxcolor=black@0.7:boxborderw=8"
}

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

X264=(-c:v libx264 -profile:v baseline -level:v 3.0 -pix_fmt yuv420p -preset veryfast
  -b:v 1500k -maxrate 2000k -bufsize 2000k -g "$FPS" -keyint_min "$FPS" -sc_threshold 0
  -bf 0 -refs 1)
AAC=(-c:a aac -b:a 128k -ar 44100 -ac 2)
SIL44=(-f lavfi -t "$D" -i anullsrc=r=44100:cl=stereo)
SIL48=(-f lavfi -t "$D" -i anullsrc=r=48000:cl=stereo)

# c1: DVD-style control. MPEG-2 PAL 720x576 + MP2 in MPEG-PS.
ff -f lavfi -i "$(vf 720 576 'c1 MPEG-2 720x576')" -f lavfi -t "$D" -i anullsrc=r=48000:cl=stereo \
  -c:v mpeg2video -b:v 4000k -maxrate 6000k -bufsize 1835k -g 12 -bf 2 -pix_fmt yuv420p \
  -c:a mp2 -b:a 192k -shortest -f vob "$OUT/c1_mpeg2_576.mpg"

# c2: MPEG-4 Part 2 Simple Profile (XviD tag) 640x480 + MP3 in AVI.
ff -f lavfi -i "$(vf 640 480 'c2 MPEG-4 SP 640x480')" "${SIL44[@]}" \
  -c:v mpeg4 -vtag XVID -b:v 1500k -g "$FPS" -bf 0 -pix_fmt yuv420p \
  -c:a libmp3lame -b:a 128k -shortest "$OUT/c2_mpeg4_480.avi"

# c3-c6: H.264 Baseline in MP4 (moov first)
ff -f lavfi -i "$(vf 720 480 'c3 H.264 720x480')" "${SIL44[@]}" "${X264[@]}" "${AAC[@]}" \
  -shortest -movflags +faststart "$OUT/c3_h264_480.mp4"
ff -f lavfi -i "$(vf 800 480 'c4 H.264 800x480')" "${SIL44[@]}" "${X264[@]}" "${AAC[@]}" \
  -shortest -movflags +faststart "$OUT/c4_h264_800.mp4"
ff -f lavfi -i "$(vf 720 336 'c5 H.264 720x336')" "${SIL44[@]}" "${X264[@]}" "${AAC[@]}" \
  -shortest -movflags +faststart "$OUT/c5_h264_336.mp4"
ff -f lavfi -i "$(vf 720 336 'c6 H.264 720x336 no audio')" "${X264[@]}" -an \
  -movflags +faststart "$OUT/c6_h264_336_na.mp4"

# c7: fragmented MP4, the streamable layout (empty moov, one moof per I-frame)
ff -f lavfi -i "$(vf 720 336 'c7 H.264 720x336 fMP4')" "${SIL44[@]}" "${X264[@]}" "${AAC[@]}" \
  -shortest -movflags frag_keyframe+empty_moov+default_base_moof "$OUT/c7_h264_336_frag.mp4"

# c8: H.264 + MP2 in MPEG-PS, the other streamable container
ff -f lavfi -i "$(vf 720 336 'c8 H.264 720x336 MPEG-PS')" "${SIL48[@]}" "${X264[@]}" \
  -c:a mp2 -b:a 192k -shortest -f vob "$OUT/c8_h264_336.mpg"

# what came out
for f in "$OUT"/*.*; do
  printf '%-24s %8s bytes  ' "${f##*/}" "$(stat -c %s "$f")"
  ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,profile,level,width,height,r_frame_rate \
    -of csv=p=0 "$f"
done
