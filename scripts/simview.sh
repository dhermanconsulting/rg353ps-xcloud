#!/bin/sh
# The client in a browser: run the host simulator with its live view up.
#
#   sh scripts/simview.sh [port] live         the real client -- sign in,
#                                             browse the library, play a game
#   sh scripts/simview.sh [port] <recording>  replay a recording, on a loop
#
# "live" is the client exactly as the handheld runs it, with the panel
# simulated and nothing else changed: the sign-in code appears in the
# browser, the keyboard drives the menus, and a game streams from xCloud for
# real. No sound (ALSA's null device), and the token lands in out/host/state
# rather than on the handheld. It runs until the client quits or Ctrl-C.
#
# A replay ends when the recording does (there is no loop in the reader), so
# that mode restarts the client each time. The page reconnects on its own
# after about a second, which is why looping this way is fine.
#
# Deliberately passes NO -scale: the downscale filter then comes from
# <state>/options.json, i.e. from whatever was last chosen in the options
# overlay (SELECT+X, which the page maps to Backspace+C). Passing the flag
# would override the menu on every restart and make the setting look broken.
#
# Ctrl-C stops it; the container is named, so a stray one dies with
#   docker kill xcloud-view
set -e
cd "$(dirname "$0")/.."

PORT=${1:-8080}
REC=${2:-live}

[ "$REC" = "live" ] || [ -f "$REC" ] || {
	echo "no $REC -- make one with: sh scripts/mkstream.sh 20," >&2
	echo "pull one off the handheld with scripts/pull.sh," >&2
	echo "or pass 'live' for the real client" >&2
	exit 1
}
[ -f out/host/xcloud ] || {
	echo "out/host/xcloud missing -- run: sh scripts/build-host.sh" >&2
	exit 1
}

trap 'docker kill xcloud-view >/dev/null 2>&1; exit 0' INT TERM

# A previous run's container can still be shutting down, and docker refuses a
# duplicate name with a bare "Conflict" that looks nothing like the cause.
# Take the name back and wait for it to actually go.
docker rm -f xcloud-view >/dev/null 2>&1 || true
i=0
while docker ps -a --format '{{.Names}}' | grep -qx xcloud-view; do
	i=$((i + 1))
	[ "$i" -gt 20 ] && {
		echo "a container named xcloud-view will not go away" >&2
		exit 1
	}
	sleep 1
done

echo "live view: http://localhost:$PORT/   (Ctrl-C to stop)" >&2
if [ "$REC" = "live" ]; then
	echo "the real client; Backspace+C opens the options overlay" >&2
	# One run, no bound: the sign-in screen, the library and a stream are
	# all just the client drawing to the simulated panel, so they all
	# arrive in the browser like anything else.
	XCLOUD_SIM_VIEW="$PORT" XCLOUD_SIM_NAME=xcloud-view \
		sh scripts/sim.sh -nort
	exit 0
fi

echo "replaying $REC on a loop; Backspace+C opens the options overlay" >&2
while :; do
	XCLOUD_SIM_VIEW="$PORT" XCLOUD_SIM_NAME=xcloud-view \
		XCLOUD_SIM_TIMEOUT=120 \
		sh scripts/sim.sh -replay "$REC" -nort -quit-after 110 \
		>/dev/null 2>&1 || true
	sleep 1
done
