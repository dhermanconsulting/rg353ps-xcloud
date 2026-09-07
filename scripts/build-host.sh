#!/bin/sh
# Build the HOST SIMULATOR: out/host/xcloud, an x86-64 Linux binary of the
# same client source, which scripts/sim.sh runs inside the xcloud-host image
# with no hardware. For exercising the decode thread, presenter and audio
# player against a recording, and for looking at UI screens, without the
# handheld.
#
# Same shape as build-app.sh -- the source list from scripts/sources.sh, the
# same flags minus -march, the same incremental and parallel scheme, -B for a
# full rebuild -- with these differences:
#
#   - src/video/drm_output_sim.c is linked INSTEAD of drm_output.c: a
#     simulated panel and vblank clock, and a zlib PNG writer, so no libdrm
#     and an extra -lz.
#   - The WebRTC stack comes from deps/host, the same deps/build-deps.sh run
#     natively in the host image (done here, once, when it is missing; a
#     few minutes).
#   - No ABI check: this binary never leaves the container, and the glibc
#     it links against is the one it runs on.
#
# Everything is runtime-configured (XCLOUD_* environment, see sim.sh); there
# is no XCLOUD_SIM compile-time switch, so every object here is built from
# exactly the source the device runs.
set -e
cd "$(dirname "$0")/.."

FORCE=0
[ "$1" = "-B" ] && FORCE=1

IMAGE=${XCLOUD_HOST_IMAGE:-xcloud-host:latest}
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
	echo "no $IMAGE image -- run:" >&2
	echo "  docker build --load -f docker/Dockerfile.host-bullseye -t xcloud-host ." >&2
	exit 1
}
[ -f deps/host/lib/libpeer.a ] || {
	echo "deps/host/lib/libpeer.a missing -- building the WebRTC stack natively (once)" >&2
	XCLOUD_DEPS_IMAGE="$IMAGE" XCLOUD_DEPS_TARGET=host \
		XCLOUD_DEPS_ARCH="-O2 -fPIC" sh deps/build-deps.sh
}
mkdir -p out/host/obj
SRC=$(pwd -W 2>/dev/null || pwd)
echo "build image: $IMAGE" >&2

# Same version string as the device build, so a simulator log and a handheld
# log name the same build. See scripts/build-app.sh.
VERSION=${XCLOUD_VERSION_STRING:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}

MSYS_NO_PATHCONV=1 docker run --rm -v "$SRC:/src" -w /src \
	-e FORCE="$FORCE" -e VERSION="$VERSION" "$IMAGE" sh -c '
  set -e
  . scripts/sources.sh   # CORE, XCLOUD_C_SOURCES, XCLOUD_CXX_SOURCES
  OBJ=out/host/obj
  # -g for the same reason as the device build: the crash handler prints a
  # backtrace, and addresses without symbols are useless.
  ARCH="-O2 -g"
  # No libdrm: drm_output_sim.c makes no DRM call and drm_output.h includes
  # no DRM header.
  CFLAGS=$(pkg-config --cflags freetype2 libcurl libavcodec libavutil)
  LIBS=$(pkg-config --libs freetype2 libcurl)
  PEER_CFLAGS="-I deps/host/include"
  # Link order matters for static archives: peer pulls srtp2/usrsctp, both of
  # which pull mbedtls. mbedtls itself is -tls before -x509 before -crypto.
  PEER_LIBS="-L deps/host/lib -lpeer -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 -lmbedcrypto"
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

  CC=${CC:-gcc}; CXX=${CXX:-g++}
  cc()  { echo "  CC  $1"; $CC $ARCH -Wall -Wextra -c "$1" $CFLAGS -o "$2"; }
  cxx() { echo "  CXX $1"; $CXX -std=c++17 $ARCH -Wall -Wextra \
                               $PEER_CFLAGS \
                               -c "$1" $CFLAGS -o "$2"; }
  # main.cpp additionally carries the version string.
  cxxv() { echo "  CXX $1 ($VERSION)"; $CXX -std=c++17 $ARCH -Wall -Wextra \
                               $PEER_CFLAGS -DXCLOUD_VERSION=\"$VERSION\" \
                               -c "$1" $CFLAGS -o "$2"; }

  for f in src/video/drm_output_sim.c src/video/sim_view.c $XCLOUD_C_SOURCES; do
    o="$OBJ/$(basename ${f%.c}).o"
    stale "$f" "$o" && spawn cc "$f" "$o"
  done

  for f in $XCLOUD_CXX_SOURCES; do
    o="$OBJ/$(basename ${f%.cpp}).oxx"
    case "$f" in
    # Always rebuilt; see scripts/build-app.sh.
    */main.cpp) spawn cxxv "$f" "$o" ;;
    *)          stale "$f" "$o" && spawn cxx "$f" "$o" ;;
    esac
  done
  reap

  echo "  LD  out/host/xcloud"
  # libavformat is only used by the file-playback path in decoder.c, but it
  # shares the translation unit, so the client links it too. -lz is the
  # simulator PNG writer.
  $CXX -o out/host/xcloud $OBJ/*.o $OBJ/*.oxx $PEER_LIBS $LIBS \
      -lavcodec -lavformat -lavutil -lasound -lz -lpthread -ldl \
      -static-libstdc++ -static-libgcc -rdynamic

  # HOST ONLY: out/host/mkstream, the synthetic-recording generator
  # (tools/mkstream.cpp, run by scripts/mkstream.sh). It borrows the
  # client'"'"'s text renderer and .xcau reader objects built above and
  # encodes with libavcodec'"'"'s libx264 and libopus. Its object lives in a
  # subdirectory so the $OBJ/*.oxx glob that links the client never sees a
  # second main(). Never in the device build: scripts/sources.sh does not
  # know it.
  mkdir -p $OBJ/tools
  o="$OBJ/tools/mkstream.oxx"
  if stale tools/mkstream.cpp "$o"; then
    echo "  CXX tools/mkstream.cpp"
    $CXX -std=c++17 $ARCH -Wall -c tools/mkstream.cpp $CFLAGS -o "$o"
  fi
  echo "  LD  out/host/mkstream"
  $CXX -o out/host/mkstream "$o" $OBJ/text.o $OBJ/au_recorder.oxx \
      $(pkg-config --libs freetype2) -lavcodec -lavutil -lpthread \
      -static-libstdc++ -static-libgcc'
echo "built out/host/xcloud"
echo "built out/host/mkstream"
