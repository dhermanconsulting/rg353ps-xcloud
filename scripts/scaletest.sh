#!/bin/sh
# Build and run tools/scaletest.c in the host image: reproduce VOP2's 2:1
# downscale in software and compare it with the filters we could run on the
# CPU instead. Writes PNGs to out/host/scaletest/.
#
# Needs only freetype and zlib, both in xcloud-host, and nothing from the
# device.
set -e
cd "$(dirname "$0")/.."

IMAGE=${XCLOUD_HOST_IMAGE:-xcloud-host:latest}
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
	echo "no $IMAGE image -- run:" >&2
	echo "  docker build --load -f docker/Dockerfile.host-bullseye -t xcloud-host ." >&2
	exit 1
}

mkdir -p out/host/scaletest
SRC=$(pwd -W 2>/dev/null || pwd)

MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src "$IMAGE" sh -c '
  set -e
  gcc -O2 -o out/host/scaletest-bin tools/scaletest.c src/ui/text.c \
      $(pkg-config --cflags freetype2) $(pkg-config --libs freetype2) -lz -lm
  ./out/host/scaletest-bin
'
