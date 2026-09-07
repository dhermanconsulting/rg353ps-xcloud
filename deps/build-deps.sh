#!/bin/sh
# Build the libpeer WebRTC stack for the RG353 (arm64, glibc <= 2.32).
#
# Everything this builds now comes from third_party/libpeer, which is a
# LOCAL COPY inside this project -- not a clone of somebody else's repository
# and not a patch applied to a commit fetched at build time. See
# third_party/libpeer/PROVENANCE.md for where each tree came from and what was
# changed. There is no network step and no patch step: what you read in
# third_party/ is exactly what gets compiled.
#
# The behavioural changes to libpeer (SDP template, DTLS role, ICE pair
# ordering, RTCP feedback, per-stream SCTP reliability, the raw-RTP passthrough
# that replaced libpeer's own broken NALU assembly) are already applied in that
# tree. deps/patches/libpeer-rg353.patch is kept only as a record of what those
# changes were against pristine upstream; nothing applies it any more.
#
# Ported originally from green-nx's deps/build-switch.sh. Three Switch-only
# workarounds are deliberately absent here:
#
#   - No usrsctp patch. Its two hunks exist because devkitA64's newlib is
#     BSD-derived and needs sockaddr_conn to carry sa_len. glibc does not.
#   - No mbedtls patch file. On Switch libpeer had to disable platform entropy
#     and MBEDTLS_NET_C and supply its own hardware poll; glibc has
#     /dev/urandom, so the only setting we still need is DTLS-SRTP, which is
#     enabled in the vendored mbedtls_config.h and checked for below.
#   - No shim headers: ifaddrs/if_nametoindex/endian.h are all in glibc.
#
# Everything is built static and installed into deps/rg353/{lib,include}.
#
# Usage:  sh deps/build-deps.sh          (host side; runs the build in docker)
#         sh deps/build-deps.sh --inside (container side; not for direct use)
#
# The same script also builds the stack NATIVELY for the host simulator
# (scripts/build-host.sh does this when deps/host is missing):
#
#   XCLOUD_DEPS_IMAGE=xcloud-host:latest XCLOUD_DEPS_TARGET=host \
#   XCLOUD_DEPS_ARCH="-O2 -fPIC" sh deps/build-deps.sh
#
# TARGET names the output directory deps/<target> and the per-library build
# directory build-<target>, so the two targets share the one vendored tree
# without treading on each other. The defaults are the device build, unchanged.

set -e

IMAGE=${XCLOUD_DEPS_IMAGE:-xcloud-arm64:latest}
TARGET=${XCLOUD_DEPS_TARGET:-rg353}

# ---------------------------------------------------------------------------
# Container side.
# ---------------------------------------------------------------------------
if [ "$1" = "--inside" ]; then
  DEPS=/src/deps
  PREFIX="$DEPS/$TARGET"
  BUILD="build-$TARGET"
  SRC=/src/third_party/libpeer
  JOBS=$(nproc)
  ARCH=${XCLOUD_DEPS_ARCH:-"-march=armv8-a -mtune=cortex-a55 -O2 -fPIC"}

  build() { # name srcdir extra-cflags... -- cmake-args...
    name="$1"; dir="$2"; shift 2
    cflags=""
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do cflags="$cflags $1"; shift; done
    [ "$1" = "--" ] && shift
    echo "=== [$name] configure"
    rm -rf "$dir/$BUILD"
    CFLAGS="$ARCH $cflags" cmake -S "$dir" -B "$dir/$BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      "$@"
    echo "=== [$name] build"
    cmake --build "$dir/$BUILD" -j"$JOBS"
    echo "=== [$name] install"
    cmake --install "$dir/$BUILD"
  }

  # 1) mbedtls 3.4.0. DTLS-SRTP is off in pristine upstream and libpeer's DTLS
  #    layer does not build without it, so the vendored copy has it enabled.
  #    Checked rather than edited: if a future re-vendor loses the setting, the
  #    failure should be this line and not 200 lines of missing symbols.
  grep -q '^#define MBEDTLS_SSL_DTLS_SRTP' \
      "$SRC/third_party/mbedtls/include/mbedtls/mbedtls_config.h" \
      || { echo "MBEDTLS_SSL_DTLS_SRTP not enabled in the vendored mbedtls_config.h" >&2
           echo "  (third_party/libpeer/third_party/mbedtls/include/mbedtls/mbedtls_config.h)" >&2
           exit 1; }
  # The vendored mbedtls is pruned: tests/, programs/, visualc/ and docs/ are
  # gone, so those must stay switched off. GEN_FILES=OFF uses the pre-generated
  # error.c / version_features.c / ssl_debug_helpers_generated.c /
  # psa_crypto_driver_wrappers.c that are checked in under library/.
  build mbedtls "$SRC/third_party/mbedtls" -- \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON

  # 2) usrsctp. IPv4 only, matching libpeer's CONFIG_IPV6 0.
  build usrsctp "$SRC/third_party/usrsctp" -- \
    -Dsctp_werror=0 -Dsctp_build_programs=0 -Dsctp_build_shared_lib=0 \
    -Dsctp_debug=0 -Dsctp_inet6=0

  # 3) libsrtp against the mbedtls from step 1.
  build libsrtp "$SRC/third_party/libsrtp" "-I$PREFIX/include" -- \
    -DENABLE_MBEDTLS=ON -DTEST_APPS=OFF -DBUILD_SHARED_LIBS=OFF \
    -DMBEDTLS_ROOT_DIR="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX"

  # 4) libpeer core. LOG_REDIRECT routes its logs into our peer_log() (see
  #    src/net/peer_compat.c). LOG_LEVEL=2 (INFO) keeps the one-shot connection
  #    diagnostics but compiles out per-packet DEBUG logging, which on green-nx
  #    flooded storage and stalled the worker thread.
  build libpeer "$SRC" "-I$PREFIX/include" -DLOG_REDIRECT=1 -DLOG_LEVEL=2 -- \
    -DDISABLE_PEER_SIGNALING=ON -DCMAKE_PREFIX_PATH="$PREFIX"

  # 5) Link smoke test. The deps expect one app-provided symbol (peer_log);
  #    stub it so this checks only that the deps' own symbols resolve.
  echo "=== smoke test link"
  cat > /tmp/smoketest.c <<'EOF'
#include <peer.h>
#include <peer_connection.h>
#include <stddef.h>
void peer_log(char* lvl, const char* file, int line, const char* fmt, ...) {
  (void)lvl; (void)file; (void)line; (void)fmt;
}
int main(void) {
  PeerConfiguration config = {0};
  PeerConnection* pc;
  peer_init();
  pc = peer_connection_create(&config);
  peer_connection_create_offer(pc);
  peer_connection_loop(pc);
  peer_connection_destroy(pc);
  peer_deinit();
  return 0;
}
EOF
  gcc $ARCH -I"$PREFIX/include" /tmp/smoketest.c \
    -L"$PREFIX/lib" -lpeer -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 -lmbedcrypto \
    -lpthread -lm -o /tmp/smoketest
  sh /src/scripts/check-abi.sh /tmp/smoketest
  echo "=== smoke test link OK"
  exit 0
fi

# ---------------------------------------------------------------------------
# Host side: run the build in the container against the vendored tree.
# ---------------------------------------------------------------------------
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DEPS="$ROOT/deps"
LIBPEER_DIR="$ROOT/third_party/libpeer"

[ -f "$LIBPEER_DIR/src/peer_connection.c" ] || {
  echo "third_party/libpeer is missing or incomplete: $LIBPEER_DIR" >&2
  echo "This tree is part of the project, not a checkout -- restore it from" >&2
  echo "backup rather than re-cloning upstream, which would lose the" >&2
  echo "xCloud-specific changes. See third_party/libpeer/PROVENANCE.md." >&2
  exit 1
}
for sub in mbedtls usrsctp libsrtp; do
  [ -d "$LIBPEER_DIR/third_party/$sub" ] || {
    echo "third_party/libpeer/third_party/$sub is missing" >&2; exit 1; }
done

rm -rf "$DEPS/$TARGET"
SRCWIN=$(cd "$ROOT" && (pwd -W 2>/dev/null || pwd))
# An empty XCLOUD_DEPS_ARCH reads as unset inside (${VAR:-default}), so the
# device flags still apply when the caller did not give any.
MSYS_NO_PATHCONV=1 docker run --rm -v "$SRCWIN:/src" -w /src \
  -e XCLOUD_DEPS_TARGET="$TARGET" -e XCLOUD_DEPS_ARCH="${XCLOUD_DEPS_ARCH:-}" \
  "$IMAGE" sh /src/deps/build-deps.sh --inside

echo
echo "=== installed artifacts ==="
for lib in libpeer.a libsrtp2.a libusrsctp.a libmbedtls.a libmbedcrypto.a libmbedx509.a; do
  if [ -f "$DEPS/$TARGET/lib/$lib" ]; then
    echo "OK       deps/$TARGET/lib/$lib"
  else
    echo "MISSING  deps/$TARGET/lib/$lib" >&2; exit 1
  fi
done
