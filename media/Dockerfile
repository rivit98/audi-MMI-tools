# mmi-media - ffmpeg for hello6's test videos (media/mkclips.sh), python3
# for media/csmp4.py.
# Built by `make clips` / `make sd SD_APPS=hello6` (Makefile: build/.media-image).
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends ffmpeg fonts-dejavu-core python3-minimal \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
