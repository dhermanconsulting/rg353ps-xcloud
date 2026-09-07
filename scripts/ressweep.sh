#!/bin/sh
# Which titles honour our declared display size?
#
#   sh scripts/ressweep.sh TITLEID [TITLEID ...]
#   sh scripts/ressweep.sh -f titles.txt
#
# Custom resolution is a title-side decision: the client sends its display
# details and the game chooses whether to call XGameStreamingSetResolution
# (docs/RESOLUTION.md). A title that does drops to a native 640x360 encode
# on this handheld -- no downscale, a third of the bitrate, a quarter of the
# decode -- which is a different class of experience, not a marginal gain. So
# it is worth knowing which ones do.
#
# One live session per title, held SECS seconds, which is long enough: the
# switch lands within ~10 s of the first frame. Prints one line per title:
#
#   NATIVE  <title>  640x360  <kbps>      honours it
#   FIXED   <title>  1280x720 <kbps>      ignores it
#   ERROR   <title>  <why>                session never produced video
#
# Sessions are real. This is minutes per dozen titles, not seconds.
set -e
cd "$(dirname "$0")/.."

SECS=${SECS:-40}
LOGDIR=${LOGDIR:-out/host/ressweep}
mkdir -p "$LOGDIR"

if [ "$1" = "-f" ]; then
	[ -f "$2" ] || { echo "ressweep: no such file: $2" >&2; exit 2; }
	set -- $(grep -v '^[[:space:]]*#' "$2" | tr -s ' \t\n' ' ')
fi
[ $# -gt 0 ] || { echo "usage: ressweep.sh TITLEID [...] | -f file" >&2; exit 2; }

for T in "$@"; do
	log="$LOGDIR/$T.log"
	XCLOUD_SIM_TIMEOUT=$((SECS + 60)) sh scripts/sim.sh \
		-quit-after "$SECS" -nort -title "$T" \
		-res 640x360 -display 640x360 > "$log" 2>&1 || true

	# The LAST size the stream settled on is the answer: an adopting title
	# starts at 720p and switches once the title has read our details.
	last=$(grep -a "stream is now" "$log" | tail -1 | sed 's/.*stream is now //')
	kbps=$(grep -a "net| video" "$log" | sed 's/.*video //;s/ kbps.*//' \
		| tail -n +3 | sort -n | awk '{a[NR]=$1} END{if(NR)print a[int((NR+1)/2)]}')
	[ -z "$kbps" ] && kbps="?"

	case "$last" in
	"")	why=$(grep -a "FAIL:" "$log" | tail -1 | sed 's/.*FAIL: //')
		printf 'ERROR   %-28s %s\n' "$T" "${why:-no video (see $log)}" ;;
	1280x720|1920x1080|2560x1440)
		printf 'FIXED   %-28s %-9s %s kbps\n' "$T" "$last" "$kbps" ;;
	*)	printf 'NATIVE  %-28s %-9s %s kbps\n' "$T" "$last" "$kbps" ;;
	esac
done
