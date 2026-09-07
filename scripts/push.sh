#!/bin/sh
# Copy a file to the handheld.
#
# History, because the obvious routes do not work here:
#   - pscp fails on some hosts with "Cannot assign requested address".
#   - Piping base64 through plink's stdin works but manages about 10 KB/s.
#     Tolerable for the 2 MB M3 binary, useless for a 19 MB debug build: over
#     ten minutes per push.
#
# So the device pulls over HTTP instead. It has BusyBox wget, the host is on
# the same subnet, and a short-lived python http.server serves a gzipped copy:
# about 30 seconds instead of ten minutes.
#
# Two rules learned the hard way, both of which cost real time:
#
#   1. NEVER fall back to base64 silently. A silent fallback looks exactly
#      like a hang -- twice it turned a 30 second push into a ten minute wait
#      that had to be killed by hand. The fallback is now opt-in via
#      PUSH_FALLBACK=1 and everything else is a loud, immediate failure.
#
#   2. NEVER assume a fixed port is free. Killing a run leaves the python
#      server alive (the EXIT trap does not run on SIGKILL), it keeps the port
#      bound, the next run's server exits "address already in use", and the
#      device's wget is refused. So: reap our own leftovers, then pick a port
#      that is actually free.
#
#   scripts/push.sh out/xcloud /userdata/ports/xcloud/xcloud
set -e
cd "$(dirname "$0")/.."
SRC="$1"; DST="$2"
[ -f "$SRC" ] && [ -n "$DST" ] || { echo "usage: push.sh <local> <remote>" >&2; exit 2; }

. scripts/device.sh
device_require

PIDFILE=${TMPDIR:-/tmp}/xcloud-push-httpd.pid

# The address the DEVICE should reach US on. A development machine usually has
# several (Wi-Fi, Ethernet, WSL, Docker, a VPN), and only the one sharing the
# device's subnet can be routed to from the handheld. So: take the device's
# /24 and look for a local address on it, rather than guessing an interface.
#
# Set HOST_IP yourself if this machine is on a different subnet and something
# routes between them.
host_ip_guess() {
	case "$DEVICE_HOST" in
	*[!0-9.]*) return 0 ;;   # a hostname; nothing to derive a prefix from
	esac
	prefix=${DEVICE_HOST%.*}.
	# ipconfig on Windows, ip/ifconfig elsewhere; whichever exists.
	{ ipconfig 2>/dev/null; ip -4 addr 2>/dev/null; ifconfig 2>/dev/null; } |
		grep -oE '(^|[^0-9.])'"$(printf '%s' "$prefix" | sed 's/\./\\./g')"'[0-9]+' |
		grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' |
		grep -v "^$DEVICE_HOST\$" | head -1
}
HOST_IP=${HOST_IP:-$(host_ip_guess)}

push_base64() {
	echo "push: using the base64 fallback -- expect this to take MINUTES" >&2
	gzip -9 -c "$SRC" | base64 -w0 | \
	device_run "cat > /tmp/.push.b64 && base64 -d /tmp/.push.b64 | gunzip > '$DST.new' \
		 && chmod 755 '$DST.new' && mv -f '$DST.new' '$DST' \
		 && rm -f /tmp/.push.b64 && ls -la '$DST'"
}

# A push that cannot use HTTP is a bug to fix, not a ten minute wait to sit
# through. Say why, and how to get the slow path deliberately.
give_up() {
	echo "push: FAILED -- $1" >&2
	if [ "$PUSH_FALLBACK" = "1" ]; then
		push_base64
		exit $?
	fi
	echo "push: not falling back automatically (it takes minutes)." >&2
	echo "push: re-run with PUSH_FALLBACK=1 to use the slow base64 path." >&2
	exit 1
}

# Reap a server this script leaked earlier. Killing the caller (Ctrl-C, a
# stopped task) skips the EXIT trap below, so the previous run's python can
# still be holding its port.
if [ -f "$PIDFILE" ]; then
	kill "$(cat "$PIDFILE")" 2>/dev/null || true
	rm -f "$PIDFILE"
fi

port_free() {
	! netstat -ano 2>/dev/null | grep -q "[:.]$1[^0-9]"
}

PUSH_PORT=""
p=${PUSH_PORT_BASE:-8899}
while [ "$p" -lt $((${PUSH_PORT_BASE:-8899} + 20)) ]; do
	if port_free "$p"; then PUSH_PORT=$p; break; fi
	p=$((p + 1))
done
[ -n "$PUSH_PORT" ] || give_up "no free port in $((${PUSH_PORT_BASE:-8899}))..+20"

command -v python >/dev/null 2>&1 || give_up "python not found (needed to serve the file)"
[ -n "$HOST_IP" ] || give_up "could not work out this host's address on the device's subnet"

STAGE=$(mktemp -d)
cleanup() {
	[ -n "$HTTPD" ] && kill "$HTTPD" 2>/dev/null
	rm -f "$PIDFILE"
	rm -rf "$STAGE" 2>/dev/null || true   # a served file can still be busy
}
trap cleanup EXIT
gzip -9 -c "$SRC" > "$STAGE/payload.gz"
echo "push: $SRC -> $DST ($(wc -c < "$STAGE/payload.gz") bytes gzipped, port $PUSH_PORT)" >&2

# Redirect the SUBSHELL, not just python: a backgrounded subshell keeps this
# script's stdout open, so anything piping our output hangs until the server
# dies, long after the push finished.
(cd "$STAGE" && exec python -m http.server "$PUSH_PORT" --bind 0.0.0.0) \
	>/dev/null 2>&1 </dev/null &
HTTPD=$!
echo "$HTTPD" > "$PIDFILE"

# Confirm OUR server is the one answering before sending the device at it.
i=0
until curl -fsS -m 2 "http://127.0.0.1:$PUSH_PORT/payload.gz" -o /dev/null 2>/dev/null; do
	kill -0 "$HTTPD" 2>/dev/null || give_up "the http server died on port $PUSH_PORT"
	i=$((i + 1))
	[ "$i" -lt 20 ] || give_up "the http server never answered on port $PUSH_PORT"
	sleep 0.25
done

# Write to .new and rename, so a half-written binary can never be the one run.
device_run "wget -q -T 30 -O '$DST.gz' 'http://$HOST_IP:$PUSH_PORT/payload.gz' \
	 && gunzip -c '$DST.gz' > '$DST.new' && chmod 755 '$DST.new' \
	 && mv -f '$DST.new' '$DST' && rm -f '$DST.gz' && ls -la '$DST'" \
	|| give_up "the device could not fetch http://$HOST_IP:$PUSH_PORT/ (firewall? wrong HOST_IP?)"
