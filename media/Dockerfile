# mmi-media - ffmpeg for hello6's test videos (media/mkclips.sh).
# Built by `make clips` / `make sd SD_APPS=hello6` (Makefile: build/.media-image).
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends ffmpeg fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
