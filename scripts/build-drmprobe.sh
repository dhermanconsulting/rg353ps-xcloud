#!/bin/sh
# Build tools/drmprobe.c for the RG353 (aarch64, glibc 2.32 ceiling).
#
# One-time setup:
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
#   docker build --load -f docker/Dockerfile.arm64-bullseye -t xcloud-arm64 .
#
# Note --load: without it the buildx default builder keeps the image in the
# build cache and `docker run` cannot find it.
#
# Do NOT pass --platform to `docker run`: the image is already arm64, and the
# flag makes the daemon re-resolve the manifest and fail to find it locally.
set -e
cd "$(dirname "$0")/.."
mkdir -p out

# Docker on Windows needs a native path; MSYS_NO_PATHCONV stops git-bash
# rewriting the -v argument.
SRC=$(pwd -W 2>/dev/null || pwd)

MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src xcloud-arm64:latest sh -c '
  set -e
  gcc -march=armv8-a -mtune=cortex-a55 -O2 -Wall -Wextra \
      -o out/drmprobe tools/drmprobe.c $(pkg-config --cflags --libs libdrm)
  sh scripts/check-abi.sh out/drmprobe'
echo "built out/drmprobe"
