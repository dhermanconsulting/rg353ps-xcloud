#!/bin/sh
# Generate a synthetic .xcau recording for the host simulator, in the
# xcloud-host container (out/host/mkstream, built by scripts/build-host.sh;
# tools/mkstream.cpp says what the stream looks like and why).
#
#   sh scripts/mkstream.sh <seconds> [-jitter <ms>] [-keyint <frames>]
#
#   -jitter <ms>      Gaussian jitter on every arrival time, clamped so
#                     neither stream is ever reordered. Default 0: an ideal
#                     16.667 ms video / 20 ms audio cadence.
#   -keyint <frames>  IDR interval. Default 60 (one a second): after a
#                     simulated loss the replay feeder discards units until
#                     the next IDR in the file, so this is the length of
#                     every simulated freeze. xCloud's own IDRs are rarer
#                     (on request only); 600 turned a 1 % loss run into a
#                     black screen.
#
# writes recordings/synth-<seconds>s.xcau, with -j<ms> and/or -k<frames>
# appended for the options given (synth-20s-j4.xcau, synth-20s-k600.xcau),
# so a varied file can never be mistaken for the clean one scripts/test.sh
# measures against. Then, for example:
#
#   sh scripts/sim.sh -nort -replay recordings/synth-20s.xcau
#   sh scripts/sim.sh -nort -replay recordings/synth-20s.xcau \
#       -impair "jitter=4,gap=8000:150,loss=0.5,burst=12"
#
# About 1 MB per second of stream; 20 s encodes in about 40 s on one core.
# XCLOUD_SIM_NAME names the container, as for sim.sh, so scripts/test.sh
# can kill exactly its own generator if it is interrupted.
set -e
cd "$(dirname "$0")/.."

SECS=$1
case "$SECS" in
	''|*[!0-9]*)
		echo "usage: sh scripts/mkstream.sh <seconds> [-jitter <ms>] [-keyint <frames>]" >&2
		exit 2 ;;
esac
shift
SUFFIX=""
prev=""
for a in "$@"; do
	case "$prev" in
		-jitter) SUFFIX="$SUFFIX-j$a" ;;
		-keyint) SUFFIX="$SUFFIX-k$a" ;;
	esac
	prev=$a
done
OUT=recordings/synth-${SECS}s${SUFFIX}.xcau

IMAGE=${XCLOUD_HOST_IMAGE:-xcloud-host:latest}
# -f, not -x: see sim.sh.
[ -f out/host/mkstream ] || {
	echo "out/host/mkstream missing -- run: sh scripts/build-host.sh" >&2
	exit 1
}
SRC=$(pwd -W 2>/dev/null || pwd)
mkdir -p recordings
NAME=${XCLOUD_SIM_NAME:+--name $XCLOUD_SIM_NAME}
MSYS_NO_PATHCONV=1 exec docker run --rm $NAME -v "$SRC:/src" -w /src \
	"$IMAGE" out/host/mkstream "$OUT" "$SECS" "$@"
