#!/bin/sh
# Copy a file FROM the handheld.
#
# The mirror of push.sh, under the same constraints: pscp can fail outright
# with "Cannot assign requested address", and base64 through plink runs at
# about 10 KB/s, which for a 49 MB recording is well over an hour. So the
# DEVICE serves the file over HTTP -- it has python3 -- and the host curls it.
# About 20 s for 49 MB on this LAN.
#
# Same rule as push.sh, learned the same expensive way: no silent slow
# fallback. If the fast path cannot work, say why and stop.
#
#   scripts/pull.sh /userdata/rec-wreckfest.xcau recordings/rec-wreckfest.xcau
#
# The remote name is used in a URL as-is, so spaces and such are not handled.
set -e
cd "$(dirname "$0")/.."
REMOTE="$1"; LOCAL="$2"
[ -n "$REMOTE" ] && [ -n "$LOCAL" ] || { echo "usage: pull.sh <remote> <local>" >&2; exit 2; }

. scripts/device.sh
device_require

PULL_PORT=${PULL_PORT:-8898}
# Where the device remembers its server, so a run that was killed before its
# EXIT trap (Ctrl-C, a stopped task) is reaped by the next one instead of
# holding the port.
PIDFILE=/tmp/xcloud-pull-httpd.pid

give_up() {
	echo "pull: FAILED -- $1" >&2
	exit 1
}

command -v curl >/dev/null 2>&1 || give_up "curl not found on this host"

REMOTE_DIR=$(dirname "$REMOTE")
REMOTE_NAME=$(basename "$REMOTE")

# Size first, which doubles as the existence check, and is what the
# transfer is verified against at the end. Not piped through tr: a pipe
# would report tr's status, and a device that has moved address (it is on
# DHCP, and has) would read as "file not readable" instead of "unreachable".
REMOTE_SIZE=$(device_run "wc -c < '$REMOTE'" 2>&1) \
	|| give_up "cannot reach $(device_target): $REMOTE_SIZE"
REMOTE_SIZE=$(printf '%s' "$REMOTE_SIZE" | tr -d ' \r\n')
case "$REMOTE_SIZE" in
	''|*[!0-9]*) give_up "$REMOTE is not readable on the device: $REMOTE_SIZE" ;;
esac

# Serve the file's directory. nohup with ALL THREE fds redirected: plink
# waits for the remote stdout to close, so a server still holding it would
# keep this call from ever returning. The device is BusyBox, so the previous
# server is reaped by pid file rather than pkill -f.
device_run "[ -f $PIDFILE ] && kill \$(cat $PIDFILE) 2>/dev/null; \
	cd '$REMOTE_DIR' && nohup python3 -m http.server $PULL_PORT --bind 0.0.0.0 \
		>/dev/null 2>&1 </dev/null & \
	echo \$! > $PIDFILE; sleep 1; kill -0 \$(cat $PIDFILE)" \
	|| give_up "could not start python3 -m http.server on the device"

cleanup() {
	device_run "kill \$(cat $PIDFILE) 2>/dev/null; rm -f $PIDFILE" >/dev/null 2>&1 || true
	rm -f "$LOCAL.part"
}
trap cleanup EXIT

mkdir -p "$(dirname "$LOCAL")"
echo "pull: $REMOTE ($REMOTE_SIZE bytes) -> $LOCAL via http://$DEVICE_HOST:$PULL_PORT/" >&2
# A few retries with a short delay cover the server's start-up only; a
# server that never answers is a failure, not a reason to try another route.
curl -fsS --retry 5 --retry-delay 1 --retry-connrefused -m 900 \
	"http://$DEVICE_HOST:$PULL_PORT/$REMOTE_NAME" -o "$LOCAL.part" \
	|| give_up "curl could not fetch http://$DEVICE_HOST:$PULL_PORT/$REMOTE_NAME"

LOCAL_SIZE=$(wc -c < "$LOCAL.part" | tr -d ' ')
[ "$LOCAL_SIZE" = "$REMOTE_SIZE" ] \
	|| give_up "size mismatch: got $LOCAL_SIZE bytes, device has $REMOTE_SIZE"
mv -f "$LOCAL.part" "$LOCAL"
echo "pull: OK $LOCAL ($LOCAL_SIZE bytes)" >&2
