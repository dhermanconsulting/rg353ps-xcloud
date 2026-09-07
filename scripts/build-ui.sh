#!/bin/sh
# Build the M3 UI test harness.
set -e
cd "$(dirname "$0")/.."
mkdir -p out
SRC=$(pwd -W 2>/dev/null || pwd)
MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src xcloud-arm64:latest sh -c '
  set -e
  gcc -march=armv8-a -mtune=cortex-a55 -O2 -Wall -Wextra \
      -o out/uitest \
      tools/uitest.c src/video/drm_output.c src/ui/text.c src/ui/screen.c \
      $(pkg-config --cflags --libs libdrm freetype2)
  sh scripts/check-abi.sh out/uitest'
echo "built out/uitest"
