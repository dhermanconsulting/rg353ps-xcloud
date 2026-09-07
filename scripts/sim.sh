#!/bin/sh
# Run the host simulator: out/host/xcloud (scripts/build-host.sh) inside the
# xcloud-host image, with no display, no sound card and no pad.
#
#   scripts/sim.sh -replay recordings/rec-wreckfest.xcau
#       the pacing harness: same pace|, audio| and replay| SUMMARY lines the
#       device prints (docs/VIDEO-PACING.md), against a simulated 60.3 Hz
#       panel and ALSA's null device.
#   XCLOUD_SIM_SHOTS=out/host/shots scripts/sim.sh -replay ...
#       the same, writing frame-NNNNNN.png of every UI screen and every
#       60th video frame (XCLOUD_SIM_SHOT_EVERY) into that directory.
#   XCLOUD_SIM_VIEW=8080 scripts/sim.sh -replay ...
#       watch it live at http://localhost:8080/ and drive it from the
#       keyboard: every presented frame as a lossless PNG, capped at 30 fps
#       (src/video/sim_view.c). This is the way to judge a downscale filter
#       away from the handheld -- open the options overlay with SELECT+X,
#       which the page maps to Backspace+C.
#   scripts/sim.sh
#       the client proper: with XCLOUD_SIM_SHOTS set, a picture of every UI
#       screen it paints. Sign-in needs a browser elsewhere as on the
#       device, and the token lands in out/host/state/tokens.json.
#
# The client's own arguments pass through unchanged; -p1devicepath is
# optional (EmulationStation supplies it on the device; here there is no pad,
# so the UI paints but cannot be driven).
#
# Environment passed through when set: XCLOUD_SIM_SHOTS (a path relative to
# the repository, which is /src inside the container, or absolute inside it),
# XCLOUD_SIM_SHOT_EVERY (default 60), XCLOUD_SIM_HZ (default 60.3, the
# panel's measured rate). SCHED_FIFO is refused in the container and logged;
# pass -nort to silence that.
#
# For unattended runs (scripts/test.sh): XCLOUD_SIM_NAME names the container
# so the caller can kill exactly its own run and nobody else's;
# XCLOUD_SIM_TIMEOUT=<s> wraps the client in coreutils timeout INSIDE the
# container -- SIGINT, which the client turns into a clean stop with its
# replay| SUMMARY still printed, then SIGKILL 10 s later -- so a wedged run
# can never keep a container alive. The client's own -quit-after <s> is the
# soft bound; this is the hard one.
set -e
cd "$(dirname "$0")/.."

IMAGE=${XCLOUD_HOST_IMAGE:-xcloud-host:latest}
# -f, not -x: the binary is a Linux ELF on a Windows-mounted tree, and Git
# Bash's -x does not consider it executable even though the container can
# run it.
[ -f out/host/xcloud ] || {
	echo "out/host/xcloud missing -- run: sh scripts/build-host.sh" >&2
	exit 1
}
SRC=$(pwd -W 2>/dev/null || pwd)
mkdir -p out/host/state
[ -n "$XCLOUD_SIM_SHOTS" ] && mkdir -p "$XCLOUD_SIM_SHOTS"

# XCLOUD_ALSA_DEVICE and XCLOUD_STATE_DIR replace the two things the client
# would otherwise expect from the device (audio_player.cpp, main.cpp); the
# XCLOUD_SIM_* variables are read by drm_output_sim.c. `-e NAME` with no
# value forwards a variable only when the caller set it, so the client's
# defaults apply otherwise.
NAME=${XCLOUD_SIM_NAME:+--name $XCLOUD_SIM_NAME}
BOUND=""
[ -n "$XCLOUD_SIM_TIMEOUT" ] && BOUND="timeout -s INT -k 10 $XCLOUD_SIM_TIMEOUT"
# XCLOUD_SIM_VIEW=<port>: the live browser view. The port has to be published
# out of the container as well as passed in, or the page is only reachable
# from inside it.
VIEW=""
if [ -n "$XCLOUD_SIM_VIEW" ]; then
	VIEW="-p $XCLOUD_SIM_VIEW:$XCLOUD_SIM_VIEW"
	echo "sim: live view on http://localhost:$XCLOUD_SIM_VIEW/" >&2
fi
MSYS_NO_PATHCONV=1 exec docker run --rm $NAME $VIEW -v "$SRC:/src" -w /src \
	-e XCLOUD_ALSA_DEVICE=null \
	-e XCLOUD_STATE_DIR=/src/out/host/state \
	-e XCLOUD_SIM_SHOTS -e XCLOUD_SIM_SHOT_EVERY -e XCLOUD_SIM_HZ \
	-e XCLOUD_SIM_VIEW \
	"$IMAGE" $BOUND out/host/xcloud "$@"
