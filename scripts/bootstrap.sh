#!/bin/sh
# One command from a fresh checkout to a binary that runs on the handheld.
#
#   sh scripts/bootstrap.sh              device build (out/xcloud)
#   sh scripts/bootstrap.sh --host       also build the host simulator
#   sh scripts/bootstrap.sh --host-only  simulator only, no device toolchain
#
# It exists because the device build has four prerequisites that must happen in
# order, three of which are one-off, and getting them wrong produces errors
# that do not name their cause -- "exec format error" for a lost binfmt
# registration, "image not found" for a build cache that was never --load-ed.
# Each step is skipped when its output is already there, so re-running this is
# cheap and is the right thing to do after a pull.
#
# Nothing here reaches the network except `docker build`, which installs the
# Debian packages the images need. The WebRTC stack is compiled from
# third_party/ in this repository; no source is fetched.
set -e
cd "$(dirname "$0")/.."

WANT_DEVICE=1
WANT_HOST=0
case "$1" in
--host)      WANT_HOST=1 ;;
--host-only) WANT_HOST=1; WANT_DEVICE=0 ;;
--help|-h)   sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
"")          ;;
*)           echo "bootstrap: unknown option '$1' (try --help)" >&2; exit 2 ;;
esac

step() { echo; echo "=== $* ===" >&2; }
have_image() { docker image inspect "$1" >/dev/null 2>&1; }

command -v docker >/dev/null 2>&1 || {
	echo "bootstrap: docker not found. It is the only prerequisite; install it" >&2
	echo "           and re-run. Nothing else needs to be on this machine." >&2
	exit 1
}
docker info >/dev/null 2>&1 || {
	echo "bootstrap: docker is installed but not running." >&2
	exit 1
}

if [ "$WANT_DEVICE" = 1 ]; then
	# ---- 1. the cross image: amd64 host, aarch64 output ----------------
	# The fast path. Roughly 20x quicker than compiling under emulation,
	# and identical output: same Debian bullseye, same gcc 10.2.1, same
	# FFmpeg headers, so its objects link against the emulated image's.
	if have_image xcloud-cross:latest; then
		echo "bootstrap: xcloud-cross image present, skipping" >&2
	else
		step "Building the cross-compiler image (a few minutes, once)"
		# --load or the image stays in the build cache where `docker run`
		# cannot find it.
		docker build --load -f docker/Dockerfile.cross-bullseye -t xcloud-cross .
	fi

	# ---- 2. the emulated arm64 image, for the dependency build ---------
	# Only deps/build-deps.sh needs this, and only once: the WebRTC stack
	# changes when the vendored sources do, which is approximately never.
	if [ ! -f deps/rg353/lib/libpeer.a ]; then
		if ! have_image xcloud-arm64:latest; then
			step "Registering binfmt so this host can run arm64 containers"
			# Lost whenever Docker's VM restarts, and the symptom is
			# "exec /bin/sh: exec format error" from an unrelated step.
			docker run --privileged --rm tonistiigi/binfmt --install arm64 \
				>/dev/null || {
				echo "bootstrap: binfmt registration failed. Without it the arm64" >&2
				echo "           image cannot run. On Docker Desktop, enable" >&2
				echo "           'Use containerd for pulling and storing images'." >&2
				exit 1
			}
			step "Building the arm64 image (emulated, ~7 minutes, once)"
			docker build --load -f docker/Dockerfile.arm64-bullseye -t xcloud-arm64 .
		fi
		step "Building the WebRTC stack for the device (~7 minutes, once)"
		sh deps/build-deps.sh
	else
		echo "bootstrap: deps/rg353 present, skipping the WebRTC build" >&2
	fi

	# ---- 3. the client ------------------------------------------------
	step "Building the client"
	sh scripts/build-app.sh
fi

if [ "$WANT_HOST" = 1 ]; then
	if have_image xcloud-host:latest; then
		echo "bootstrap: xcloud-host image present, skipping" >&2
	else
		step "Building the host image (a few minutes, once)"
		docker build --load -f docker/Dockerfile.host-bullseye -t xcloud-host .
	fi
	# build-host.sh builds deps/host itself when it is missing.
	step "Building the host simulator"
	sh scripts/build-host.sh
fi

echo
echo "bootstrap: done." >&2
[ "$WANT_DEVICE" = 1 ] && echo "  out/xcloud       -> sh scripts/install.sh" >&2
[ "$WANT_HOST" = 1 ]   && echo "  out/host/xcloud  -> sh scripts/sim.sh, sh scripts/test.sh" >&2
exit 0
