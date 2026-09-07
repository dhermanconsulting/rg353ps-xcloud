#!/bin/sh
# Check the NEON 2:1 downscale kernels against plain C.
#
# Two runs, because each proves a different half:
#   host   x86-64 in xcloud-host, which takes nv12.c's scalar path -- proves
#          the reference and the edge handling, not the intrinsics.
#   arm64  the aarch64 binary from the cross image, run under QEMU in
#          xcloud-arm64, which takes the NEON path. This is the one that
#          matters. Needs binfmt: if it reports "exec format error", run
#            docker run --privileged --rm tonistiigi/binfmt --install arm64
#          and try again (the registration is lost when Docker's VM
#          restarts). Skipped, loudly, if the arm64 image is missing.
set -e
cd "$(dirname "$0")/.."

SRC=$(pwd -W 2>/dev/null || pwd)
mkdir -p out/host

HOST_IMAGE=${XCLOUD_HOST_IMAGE:-xcloud-host:latest}
CROSS_IMAGE=${XCLOUD_CROSS_IMAGE:-xcloud-cross:latest}
ARM_IMAGE=${XCLOUD_ARM_IMAGE:-xcloud-arm64:latest}

echo "== host (scalar) =="
MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src "$HOST_IMAGE" sh -c '
  gcc -O2 -o out/host/nv12check tools/nv12check.c src/video/nv12.c
  ./out/host/nv12check
'

# The arm64 half is the one that matters -- it is the only thing anywhere
# that executes the NEON intrinsics away from the device. Skipping it quietly
# would leave a green result that proved only the scalar reference, so say so
# loudly, and let NV12CHECK_STRICT=1 turn the skip into a failure for callers
# (CI, a release check) that must not accept a half-run.
docker image inspect "$ARM_IMAGE" >/dev/null 2>&1 || {
	echo >&2
	echo "== arm64 (NEON) == SKIPPED: no $ARM_IMAGE image." >&2
	echo "   The scalar reference passed, but THE NEON KERNELS WERE NOT TESTED." >&2
	echo "   Build the image to run them:" >&2
	echo "     docker run --privileged --rm tonistiigi/binfmt --install arm64" >&2
	echo "     docker build --load -f docker/Dockerfile.arm64-bullseye -t xcloud-arm64 ." >&2
	if [ "$NV12CHECK_STRICT" = "1" ]; then
		echo "   NV12CHECK_STRICT=1, so this is a failure." >&2
		exit 1
	fi
	exit 0
}

echo "== arm64 (NEON) =="
MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src "$CROSS_IMAGE" sh -c '
  aarch64-linux-gnu-gcc -O2 -march=armv8-a -static \
      -o out/nv12check-arm64 tools/nv12check.c src/video/nv12.c
'
MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src "$ARM_IMAGE" \
	./out/nv12check-arm64
