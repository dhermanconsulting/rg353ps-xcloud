#!/bin/sh
# Resolution research harness: run one live xCloud session per argument set
# and print what the server actually encoded.
#
#   sh scripts/resmatrix.sh "-tier 720 -alias 1080" "-tier 720 -alias Auto" ...
#
# Each argument is a full set of client flags. One live session is created per
# set, held for SECS seconds, and the sizes reported by "drm: source now" are
# collected. Output is one line per run:
#
#   RESULT <flags> -> <sizes seen> [alias=<what we sent>] [note]
#
# Why this exists: -tier moves five signals at once (the session fingerprint,
# the claimed display size, the resolutionAlias, the capability messages and
# the SDP decode caps), so nothing the server does can be attributed to one of
# them. -alias, -osname and -display move three of them independently, and
# this runs the combinations. See docs/RESOLUTION.md.
#
# Sessions are real and cost cloud time. Keep SECS short: the encode size is
# known within a few seconds of the first frame.
set -e
cd "$(dirname "$0")/.."

SECS=${SECS:-35}
TITLE=${TITLE:-WRECKFEST}
LOGDIR=${LOGDIR:-out/host/resmatrix}
mkdir -p "$LOGDIR"

[ $# -gt 0 ] || { echo "usage: resmatrix.sh \"<flags>\" [\"<flags>\" ...]" >&2; exit 2; }

n=0
for FLAGS in "$@"; do
	n=$((n + 1))
	log="$LOGDIR/run$n.log"
	echo "--- run $n: $FLAGS" >&2
	# The client's own exit is bounded by -quit-after; the container gets a
	# little longer so a wedged session is still killed rather than hanging
	# the whole matrix.
	XCLOUD_SIM_TIMEOUT=$((SECS + 60)) sh scripts/sim.sh \
		-quit-after "$SECS" -nort -title "$TITLE" $FLAGS \
		> "$log" 2>&1 || true

	sizes=$(grep -a "source now" "$log" \
		| sed -e 's/.*source now //' -e 's/ ->.*//' \
		| grep -v '^640x480$' | tr '\n' ' ')
	alias=$(grep -a "resolutionAlias:" "$log" | tail -1 | sed 's/.*resolutionAlias: //')
	note=""
	grep -qa "binding request timeout" "$log" && note="$note ICE-TIMEOUT"
	grep -qa "server ended the session" "$log" && \
		note="$note SERVER-ENDED($(grep -a 'server ended the session' "$log" | tail -1 | sed 's/.*session//'))"
	grep -qa "FAIL:" "$log" && note="$note FAILED"
	[ -z "$sizes" ] && sizes="(no video)"
	# Median-ish delivered bitrate: skip the first two seconds, which are
	# the encoder ramping up, and take the middle of what is left.
	kbps=$(grep -a "net| video" "$log" | sed 's/.*video //;s/ kbps.*//' \
		| tail -n +3 | sort -n | awk '{a[NR]=$1} END{if(NR)print a[int((NR+1)/2)]}')
	[ -z "$kbps" ] && kbps="?"

	echo "RESULT [$FLAGS] -> $sizes ${kbps}kbps alias=$alias$note"
done
