#!/bin/sh
# Build the xcloud client.
#
# Everything runs under QEMU (the container is arm64 on an amd64 host), so
# compiling is roughly 20x slower than native and the two things that matter
# are not doing the same work twice and using more than one core:
#
#   - Incremental. An object is rebuilt only when its source or one of our
#     headers is newer. The five green-nx core files never change and cost
#     about a minute each, which is most of a normal edit-build cycle.
#   - Parallel. One job per core instead of a serial for-loop.
#
# Pass -B to force a full rebuild (after changing compiler flags, which the
# timestamp check cannot see).
#
# C and C++ are compiled separately and linked with g++: passing -x c mid
# command line to g++ does not reliably switch the frontend.
#
# The WebRTC stack (libpeer, libsrtp, usrsctp, mbedTLS) is static and comes
# from deps/rg353, built by deps/build-deps.sh. That one is genuinely slow but
# only changes when the pinned libpeer commit does.
#
# The source list lives in scripts/sources.sh, shared with the host
# simulator build (scripts/build-host.sh) so a file added for one is built
# for both.
set -e
cd "$(dirname "$0")/.."

FORCE=0
[ "$1" = "-B" ] && FORCE=1

[ -f deps/rg353/lib/libpeer.a ] || {
	echo "deps/rg353/lib/libpeer.a missing -- run: sh deps/build-deps.sh" >&2
	exit 1
}
mkdir -p out/obj
SRC=$(pwd -W 2>/dev/null || pwd)

# Version string, baked into the binary and printed at startup and by
# -version. Worked out here rather than in the container because git is on
# this side, and the container has no .git. A tarball build with no git at all
# falls back to "dev", which is honest.
VERSION=${XCLOUD_VERSION_STRING:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}
echo "version: $VERSION" >&2

# Prefer the real cross-compiler: it runs the compiler natively on amd64 and
# only emits aarch64, which is roughly 20x faster than the emulated arm64
# image. Both produce the same code -- identical Debian bullseye, gcc 10.2.1,
# same FFmpeg 4.3.x headers -- so this is purely a speed choice and either
# image is correct.
#
# The container script below works in both because it uses $CC/$CXX. The cross
# image sets those to the aarch64 tools; the arm64 image leaves them unset, so
# they fall back to plain gcc/g++, which are already aarch64 there.
IMAGE=${XCLOUD_IMAGE:-}
if [ -z "$IMAGE" ]; then
	if docker image inspect xcloud-cross:latest >/dev/null 2>&1; then
		IMAGE=xcloud-cross:latest
	else
		IMAGE=xcloud-arm64:latest
		echo "note: no cross image, building under emulation (~20x slower)." >&2
		echo "      docker build --load -f docker/Dockerfile.cross-bullseye -t xcloud-cross ." >&2
	fi
fi
echo "build image: $IMAGE" >&2

MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src \
	-e FORCE="$FORCE" -e VERSION="$VERSION" "$IMAGE" sh -c '
  set -e
  . scripts/sources.sh   # CORE, XCLOUD_C_SOURCES, XCLOUD_CXX_SOURCES
  # -g costs nothing at runtime and makes the crash handler in main.cpp
  # useful: without symbols a backtrace is only addresses.
  ARCH="-march=armv8-a -mtune=cortex-a55 -O2 -g"
  CFLAGS=$(pkg-config --cflags libdrm freetype2 libcurl libavcodec libavutil)
  LIBS=$(pkg-config --libs libdrm freetype2 libcurl)
  PEER_CFLAGS="-I deps/rg353/include"
  # Link order matters for static archives: peer pulls srtp2/usrsctp, both of
  # which pull mbedtls. mbedtls itself is -tls before -x509 before -crypto.
  PEER_LIBS="-L deps/rg353/lib -lpeer -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 -lmbedcrypto"
  JOBS=$(nproc)

  # Rebuild when the source or ANY header we own is newer than the object.
  # Coarse on purpose: the headers are few, so a spurious rebuild costs one
  # file while a missed one costs a debugging session.
  # third_party/ is deliberately excluded: json.hpp is a megabyte of header
  # that never changes, and listing it here would make it the "newest header"
  # after every fresh checkout and force one full rebuild for nothing.
  NEWEST_HDR=$(ls -t $(find src -name "*.h" -o -name "*.hpp") \
                  2>/dev/null | head -1)

  stale() {  # stale <src> <obj>
    [ "$FORCE" = "1" ] && return 0
    [ -f "$2" ] || return 0
    [ "$1" -nt "$2" ] && return 0
    [ -n "$NEWEST_HDR" ] && [ "$NEWEST_HDR" -nt "$2" ] && return 0
    return 1
  }

  # Run queued compiles JOBS at a time. Any failure fails the build: plain sh
  # has no "wait -n", so statuses are collected after each batch.
  PIDS=""; NRUN=0
  reap() {
    rc=0
    for pid in $PIDS; do wait "$pid" || rc=1; done
    PIDS=""; NRUN=0
    [ "$rc" = "0" ] || { echo "build failed" >&2; exit 1; }
  }
  spawn() {
    "$@" & PIDS="$PIDS $!"
    NRUN=$((NRUN + 1))
    [ "$NRUN" -ge "$JOBS" ] && reap
    return 0
  }

  # Both images: the cross one exports the aarch64 tools, the emulated one
  # leaves these unset and plain gcc/g++ are already aarch64 there.
  CC=${CC:-gcc}; CXX=${CXX:-g++}
  cc()  { echo "  CC  $1"; $CC $ARCH -Wall -Wextra -c "$1" $CFLAGS -o "$2"; }
  cxx() { echo "  CXX $1"; $CXX -std=c++17 $ARCH -Wall -Wextra \
                               $PEER_CFLAGS \
                               -c "$1" $CFLAGS -o "$2"; }
  # main.cpp additionally carries the version string.
  cxxv() { echo "  CXX $1 ($VERSION)"; $CXX -std=c++17 $ARCH -Wall -Wextra \
                               $PEER_CFLAGS -DXCLOUD_VERSION=\"$VERSION\" \
                               -c "$1" $CFLAGS -o "$2"; }

  for f in src/video/drm_output.c $XCLOUD_C_SOURCES; do
    o="out/obj/$(basename ${f%.c}).o"
    stale "$f" "$o" && spawn cc "$f" "$o"
  done

  for f in $XCLOUD_CXX_SOURCES; do
    o="out/obj/$(basename ${f%.cpp}).oxx"
    case "$f" in
    # Always rebuilt: the version can change (a commit, a tag, the tree
    # going clean) without main.cpp changing, and a binary that misreports
    # its own version is worse than one that has none. One file, ~1 s.
    */main.cpp) spawn cxxv "$f" "$o" ;;
    *)          stale "$f" "$o" && spawn cxx "$f" "$o" ;;
    esac
  done
  reap

  echo "  LD  out/xcloud"
  # libavformat is only used by the file-playback path in decoder.c, but it
  # shares the translation unit, so the client links it too.
  $CXX -o out/xcloud out/obj/*.o out/obj/*.oxx $PEER_LIBS $LIBS \
      -lavcodec -lavformat -lavutil -lasound -lpthread -ldl \
      -static-libstdc++ -static-libgcc -rdynamic
  sh scripts/check-abi.sh out/xcloud'
echo "built out/xcloud"
