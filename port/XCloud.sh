#!/bin/sh
# Xbox Cloud Gaming - EmulationStation port launcher.
#
# EmulationStation releases DRM master for ports, which is what lets the
# client take the screen. It passes -p1devicepath /dev/input/eventN among
# other arguments; the client parses that to find the pad.
#
# This script finds the client RELATIVE TO ITSELF, so the port works wherever
# it is dropped -- which is the whole point of being able to install by
# copying a folder onto the SD card. It looks for xcloud/xcloud beside this
# file first, then the location scripts/install.sh uses.

# Where this script actually is, following the symlink ES may have made.
HERE=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
[ -n "$HERE" ] || HERE=/userdata/roms/ports

# The client. First hit wins.
BIN=""
for cand in "$HERE/xcloud/xcloud" "$HERE/xcloud" /userdata/ports/xcloud/xcloud; do
	[ -f "$cand" ] && { BIN=$cand; break; }
done

# State -- token, settings, catalog cache, box art -- always lives on
# /userdata, never beside the binary. /userdata is ext4 and can hold the
# 0600 the sign-in token wants; the ROM card is exFAT, which has no
# permission bits at all. Keeping it here also means moving or replacing the
# port does not sign you out.
#
# The two overrides are a test hook -- scripts/test.sh points them at a
# sandbox so this launcher can be exercised on a development machine instead
# of only on hardware. Both are unset on the device.
STATE="${XCLOUD_PORT_STATE:-/userdata/ports/xcloud}"
LOGDIR="${XCLOUD_PORT_LOGS:-/userdata/system/logs}"

mkdir -p "$LOGDIR"
exec > "$LOGDIR/xcloud.log" 2>&1
set -x

if [ -z "$BIN" ]; then
	echo "xcloud: no client binary found."
	echo "xcloud: looked beside this script ($HERE/xcloud/xcloud) and in"
	echo "xcloud: /userdata/ports/xcloud/xcloud."
	echo "xcloud: copy the whole port folder, not just this launcher."
	exit 1
fi

# The ROM card can be mounted without the execute bit. Try to fix it; if the
# filesystem will not take it, exFAT still runs the binary through the
# interpreter, so this is a best effort rather than a requirement.
[ -x "$BIN" ] || chmod 755 "$BIN" 2>/dev/null

mkdir -p "$STATE"
# Absolute before anything cd's: every use of $STATE below happens after the
# cd into it, so a relative value would resolve against itself and quietly
# miss -- which is how the run-once marker went missing.
STATE=$(cd "$STATE" 2>/dev/null && pwd) || {
	echo "xcloud: cannot use state directory $STATE"
	exit 1
}

cd "$STATE" || exit 1
export HOME="$STATE"
export XDG_CONFIG_HOME="$STATE/config"
export XDG_DATA_HOME="$STATE/data"
export XDG_CACHE_HOME="$STATE/cache"
BINDIR=$(dirname "$BIN")
export LD_LIBRARY_PATH="$STATE/libs:$BINDIR/libs:$LD_LIBRARY_PATH"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"

# Register the port's name and description in EmulationStation's gamelist, so
# it reads as "Xbox Cloud Gaming" rather than the script's filename. Done here
# because installing by copying files onto the card cannot run anything.
#
# It MERGES -- that gamelist belongs to every port on the device -- runs once
# (a marker file), and gives up quietly on any problem. ES reads the gamelist
# at start-up, so the name appears next time it launches.
GAMELIST_DIR=$(dirname "$HERE")/ports
[ -d "$GAMELIST_DIR" ] || GAMELIST_DIR=$HERE
if [ ! -f "$STATE/.gamelist-registered" ] && [ -f "$HERE/xcloud/gamelist.xml" ]; then
	if command -v python3 >/dev/null 2>&1 && [ -f "$HERE/xcloud/gamelist-merge.py" ]; then
		touch "$GAMELIST_DIR/gamelist.xml" 2>/dev/null
		if python3 "$HERE/xcloud/gamelist-merge.py" \
			--entry "$HERE/xcloud/gamelist.xml" \
			--into  "$GAMELIST_DIR/gamelist.xml" \
			--out   "$GAMELIST_DIR/gamelist.xml.new" 2>/dev/null; then
			mv -f "$GAMELIST_DIR/gamelist.xml.new" "$GAMELIST_DIR/gamelist.xml" 2>/dev/null
		fi
		rm -f "$GAMELIST_DIR/gamelist.xml.new" 2>/dev/null
	fi
	# Marked either way: one attempt, not one per launch.
	touch "$STATE/.gamelist-registered" 2>/dev/null
fi

[ -w /sys/class/graphics/fbcon/cursor_blink ] && echo 0 > /sys/class/graphics/fbcon/cursor_blink
[ -c /dev/tty0 ] && printf '\033[?25l\033[2J\033[H' > /dev/tty0

# Run the client as a child and forward terminations to it.
#
# Without this the client is ORPHANED whenever the launcher is killed rather
# than the client -- EmulationStation terminating the port, a `timeout`
# wrapped round it during development, a shutdown. The shell dies, ES comes
# back over the top, and the client carries on holding the audio device and
# the DRM plane. The symptom is unmistakable: the menu returns and the game's
# music keeps playing.
"$BIN" "$@" &
CHILD=$!
trap 'kill -TERM $CHILD 2>/dev/null' INT TERM HUP
wait $CHILD
RC=$?

if [ "$RC" -gt 128 ]; then
	# We were signalled rather than the client exiting. Make sure it is
	# really gone before this script returns, escalating if it will not go
	# quietly -- a client left behind is worse than a slow exit.
	kill -TERM $CHILD 2>/dev/null
	i=0
	while kill -0 $CHILD 2>/dev/null && [ "$i" -lt 5 ]; do
		sleep 1
		i=$((i + 1))
	done
	kill -KILL $CHILD 2>/dev/null
	wait $CHILD 2>/dev/null
fi
trap - INT TERM HUP

[ -c /dev/tty0 ] && printf '\033[?25h' > /dev/tty0
exit $RC
