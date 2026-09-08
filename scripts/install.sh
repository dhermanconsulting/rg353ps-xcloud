#!/bin/sh
# Install the port on the handheld so it appears in EmulationStation's Ports
# list with a name and a description, rather than as a bare shell script.
#
#   sh scripts/install.sh
#
# This is the DEVELOPER install, over the network from a checkout. It puts the
# binary on /userdata (ext4: faster, and it survives swapping the ROM card),
# which is the third location port/XCloud.sh looks in.
#
# What a user gets from a release is different on purpose: scripts/package.sh
# lays the binary out beside the launcher in the card's ports/ folder, because
# that install is "drag a folder onto the SD card" and nothing else. The
# launcher handles both, and scripts/test.sh exercises the card layout.
#
# What lands where:
#
#   /userdata/ports/xcloud/xcloud       the binary (out/xcloud), UNLESS a
#                                       card install is already there, in
#                                       which case /userdata/roms/ports/
#                                       xcloud/xcloud, because that is the
#                                       one the launcher runs -- see below
#   /userdata/roms/ports/Xbox Cloud.sh  the ES launcher (port/XCloud.sh)
#   /userdata/roms/ports/gamelist.xml   MERGED, never overwritten
#
# That last one is the whole reason this script exists rather than three
# push.sh calls. gamelist.xml is shared by every port on the device --
# PortMaster's own entries, DRM Probe.sh, PlayTest.sh -- so copying ours over
# it would silently delete all of theirs. The device's copy is read, our entry
# is merged into it by tools/gamelist-merge.py (matching on <path>, so
# re-installing replaces rather than duplicates), and the result goes back.
# A device gamelist that does not parse stops the install instead of being
# overwritten.
#
# Fonts are deliberately not bundled: the client reads the device's own
# /usr/share/fonts/dejavu (src/ui/text.h), which the stock firmware ships.
#
# EmulationStation reads gamelist.xml when it starts, so it is restarted at
# the end or the new entry does not appear. Set NO_ES_RESTART=1 to skip that.
set -e
cd "$(dirname "$0")/.."

. scripts/device.sh
device_require

BIN=${BIN:-out/xcloud}
PORTS=/userdata/roms/ports
# The launcher's filename, space and all: it is what is already on the device
# and what EmulationStation has play history against, and it must match the
# <path> in port/gamelist.xml exactly.
LAUNCHER=${LAUNCHER:-"Xbox Cloud.sh"}
GAMEDIR=/userdata/ports/xcloud
TMP=${TMPDIR:-/tmp}/xcloud-install.$$

give_up() {
	echo "install: FAILED -- $1" >&2
	rm -rf "$TMP"
	exit 1
}

[ -f "$BIN" ] || give_up "$BIN missing -- run: sh scripts/build-app.sh"
[ -f port/XCloud.sh ] || give_up "port/XCloud.sh missing"
[ -f port/gamelist.xml ] || give_up "port/gamelist.xml missing"
command -v python >/dev/null 2>&1 || give_up "python not found (needed to merge gamelist.xml)"

mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

echo "install: target $(device_target)" >&2
device_run "mkdir -p '$PORTS'" \
	|| give_up "cannot reach $(device_target)"

# ---- where the LAUNCHER will actually look -------------------------------
# port/XCloud.sh searches "$HERE/xcloud/xcloud" -- beside itself, the SD-card
# layout scripts/package.sh produces -- BEFORE /userdata/ports/xcloud/xcloud.
# So a device that has ever had a card install shadows this one, and pushing
# to the internal path alone is an install that changes nothing you can see.
#
# That is not hypothetical: it cost a whole round of "is this even the new
# version" on 2026-09-08, with three pushes verified by md5 against a binary
# the launcher was never going to run. Install where the launcher looks
# first, and say which path was used, every time.
if device_run "[ -f '$PORTS/xcloud/xcloud' ]" 2>/dev/null; then
	GAMEDIR=$PORTS/xcloud
	echo "install: SD-card layout found -- installing to $GAMEDIR/xcloud," >&2
	echo "install: which the launcher prefers over /userdata/ports/xcloud." >&2
fi
device_run "mkdir -p '$GAMEDIR'" || give_up "cannot create $GAMEDIR"

# ---- binary and launcher -------------------------------------------------
sh scripts/push.sh "$BIN" "$GAMEDIR/xcloud" >&2
sh scripts/push.sh port/XCloud.sh "$PORTS/$LAUNCHER" >&2
device_run "chmod 755 '$PORTS/$LAUNCHER' '$GAMEDIR/xcloud'"

# ---- gamelist.xml, read-modify-write -------------------------------------
# cat over plink rather than pull.sh: this is a few KB of text, and pull.sh
# fails hard when the remote file does not exist, which here is a normal
# first-install case rather than an error. CR is stripped because plink on
# Windows can hand back the stream with line endings translated.
echo "install: reading $PORTS/gamelist.xml" >&2
device_run "cat '$PORTS/gamelist.xml' 2>/dev/null || true" \
	| tr -d '\r' > "$TMP/device.xml" || give_up "could not read the device's gamelist.xml"

if [ -s "$TMP/device.xml" ]; then
	echo "install: device gamelist has $(grep -c '<game>' "$TMP/device.xml") entries" >&2
else
	echo "install: no gamelist on the device yet; creating one" >&2
fi

python tools/gamelist-merge.py \
	--entry port/gamelist.xml \
	--into "$TMP/device.xml" \
	--out "$TMP/merged.xml" \
	|| give_up "gamelist merge refused; nothing on the device was changed"

sh scripts/push.sh "$TMP/merged.xml" "$PORTS/gamelist.xml" >&2
device_run "chmod 644 '$PORTS/gamelist.xml'"

# ---- what actually ended up there ----------------------------------------
echo "install: on the device now:" >&2
device_run "ls -la '$GAMEDIR/xcloud' '$PORTS/$LAUNCHER' '$PORTS/gamelist.xml'"

if [ "$NO_ES_RESTART" = "1" ]; then
	echo "install: NO_ES_RESTART=1 -- restart EmulationStation yourself to see the entry" >&2
else
	echo "install: restarting EmulationStation so it re-reads gamelist.xml" >&2
	device_run "/etc/init.d/S31emulationstation restart >/dev/null 2>&1 &" || true
fi
echo "install: OK -- Ports -> Xbox Cloud Gaming" >&2
