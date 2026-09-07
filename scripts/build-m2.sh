#!/bin/sh
# Build the M2 harness (tools/playfile.c) for the RG353.
set -e
cd "$(dirname "$0")/.."
mkdir -p out
SRC=$(pwd -W 2>/dev/null || pwd)
MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src xcloud-arm64:latest sh -c '
  set -e
  gcc -march=armv8-a -mtune=cortex-a55 -O2 -Wall -Wextra \
      -o out/playfile \
      tools/playfile.c src/video/drm_output.c src/video/nv12.c src/media/decoder.c \
      $(pkg-config --cflags --libs libdrm libavcodec libavformat libavutil)
  sh scripts/check-abi.sh out/playfile'
echo "built out/playfile"
