#!/usr/bin/env bash
# swdec_clips.sh <dir> - test_swdec's inputs, made in the mmi-media image (ffmpeg):
#   full.h264 / full.yuv   800x480 Baseline (Android Auto's size), 50 frames,
#                          one slice per picture, and ffmpeg's decode of it (I420)
#   crop.h264 / crop.yuv   360x200 (coded 368x208: SPS cropping), 4 slices per
#                          picture, and ffmpeg's decode (the visible part)
set -euo pipefail
d=${1:?usage: swdec_clips.sh <dir>}
x264() {
  ffmpeg -v error -y -f lavfi -i "testsrc2=size=$1:rate=25:duration=2" \
      -c:v libx264 -profile:v baseline -level:v 3.1 -pix_fmt yuv420p \
      -preset veryfast -b:v 1500k -g 25 -keyint_min 25 -sc_threshold 0 -bf 0 -refs 1 \
      -x264-params "slices=$2" -an -f h264 "$3"
  ffmpeg -v error -y -i "$3" -f rawvideo -pix_fmt yuv420p "${3%.h264}.yuv"
}
x264 800x480 1 "$d/full.h264"
x264 360x200 4 "$d/crop.h264"
