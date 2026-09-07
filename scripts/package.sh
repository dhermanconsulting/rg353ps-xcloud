#!/bin/sh
# Build the release archive: the thing a person downloads, unzips, and copies
# onto their handheld's SD card.
#
#   sh scripts/package.sh            -> out/xcloud-rg353ps-<version>.zip
#
# Why this exists: everything else here assumes a checkout, Docker and SSH.
# That is right for developing and wrong for using. Somebody who just wants
# the client on their handheld should be able to take the SD card out, drag a
# folder onto it, put it back, and find the port in the menu -- no terminal,
# no network, no account on anything.
#
# The layout is built around that:
#
#   xcloud-rg353ps-<version>/
#     INSTALL.txt          read this first
#     install-on-device.sh optional, for people who would rather use SSH
#     ports/               <- drag THIS onto the ROM card, merging with the
#       Xbox Cloud.sh         ports folder already there
#       xcloud/
#         xcloud           the client
#         gamelist.xml     the port's name and description
#         gamelist-merge.py
#         LICENSE, THIRD-PARTY-NOTICES.md, README.md
#
# The card is the one holding the ROMs (exFAT, so Windows, macOS and Linux
# can all write it), mounted on the device at /userdata/roms. Its root already
# contains a `ports` folder, so dropping ours on top merges rather than
# replaces. port/XCloud.sh finds the binary relative to itself, which is what
# makes this work without an installer.
#
# The binary is whatever is in out/, and is NOT rebuilt here: packaging
# something other than the artefact that was tested is exactly the mistake
# this should not make.
set -e
cd "$(dirname "$0")/.."

BIN=${BIN:-out/xcloud}
VERSION=${XCLOUD_VERSION_STRING:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}
NAME=xcloud-rg353ps-$VERSION
STAGE=out/package/$NAME
PORTS=$STAGE/ports
OUT=out/$NAME.zip

die() { echo "package: $1" >&2; exit 1; }

[ -f "$BIN" ] || die "$BIN missing -- run: sh scripts/bootstrap.sh"

# The binary must be the aarch64 one. Packaging a host build would produce an
# archive that installs cleanly and then will not execute.
case "$(file -b "$BIN" 2>/dev/null)" in
*aarch64*) ;;
*) die "$BIN is not an aarch64 binary -- did you package the host build?" ;;
esac
sh scripts/check-abi.sh "$BIN" >/dev/null || die "ABI check failed on $BIN"

# A dirty tree means the archive corresponds to no commit, so nobody can
# reproduce it. Allowed -- packaging a work in progress to try on the device
# is a normal thing to do -- but said out loud.
case "$VERSION" in
*-dirty)
	echo "package: WARNING -- '$VERSION' has uncommitted changes." >&2
	echo "package:            This archive matches no commit." >&2
	;;
esac

rm -rf out/package
mkdir -p "$PORTS/xcloud"

cp "$BIN"                  "$PORTS/xcloud/xcloud"
cp port/XCloud.sh          "$PORTS/Xbox Cloud.sh"
cp port/gamelist.xml       "$PORTS/xcloud/gamelist.xml"
cp tools/gamelist-merge.py "$PORTS/xcloud/gamelist-merge.py"
cp LICENSE                 "$PORTS/xcloud/LICENSE"
cp THIRD-PARTY-NOTICES.md  "$PORTS/xcloud/THIRD-PARTY-NOTICES.md"
cp README.md               "$PORTS/xcloud/README.md"
chmod 755 "$PORTS/xcloud/xcloud" "$PORTS/Xbox Cloud.sh"

# ---------------------------------------------------------------------------
# The optional SSH installer.
# ---------------------------------------------------------------------------
# BusyBox ash on the device: no arrays, no [[ ]]. Does the same thing the card
# copy does, plus the gamelist merge and an EmulationStation restart, so the
# port appears without a reboot.
cat > "$STAGE/install-on-device.sh" <<'INSTALLER'
#!/bin/sh
# Install the xCloud port. Run this ON THE HANDHELD, over SSH:
#
#   cd /userdata/xcloud-rg353ps-*  &&  sh install-on-device.sh
#
# You do not need this if you copied the ports folder onto the SD card --
# that is already a complete install. This just does it over the network
# instead, and restarts EmulationStation so the entry appears immediately.
set -e
cd "$(dirname "$0")"

DEST=/userdata/roms/ports
die() { echo "install: FAILED -- $1" >&2; exit 1; }

[ -f "ports/xcloud/xcloud" ] || die "run this from inside the unzipped folder"
[ -d /userdata ] || die "no /userdata -- is this the handheld?"
mkdir -p "$DEST/xcloud"

echo "install: copying the port to $DEST"
cp "ports/Xbox Cloud.sh" "$DEST/Xbox Cloud.sh"
cp ports/xcloud/xcloud    "$DEST/xcloud/xcloud.new"
mv -f "$DEST/xcloud/xcloud.new" "$DEST/xcloud/xcloud"
for f in gamelist.xml gamelist-merge.py LICENSE THIRD-PARTY-NOTICES.md README.md; do
	cp "ports/xcloud/$f" "$DEST/xcloud/$f" 2>/dev/null || true
done
chmod 755 "$DEST/Xbox Cloud.sh" "$DEST/xcloud/xcloud" 2>/dev/null || true

# gamelist.xml is shared with PortMaster and every other port, so it is
# merged, never overwritten. A gamelist that will not parse stops the merge
# with the device's copy untouched.
echo "install: registering the port's name"
if command -v python3 >/dev/null 2>&1; then
	touch "$DEST/gamelist.xml"
	if python3 "$DEST/xcloud/gamelist-merge.py" \
		--entry "$DEST/xcloud/gamelist.xml" \
		--into  "$DEST/gamelist.xml" \
		--out   "$DEST/gamelist.xml.new"; then
		mv -f "$DEST/gamelist.xml.new" "$DEST/gamelist.xml"
	else
		rm -f "$DEST/gamelist.xml.new"
		echo "install: gamelist merge refused; nothing was changed." >&2
		echo "install: the port still works, named after its script." >&2
	fi
else
	echo "install: no python3; the port will be named after its script." >&2
fi

echo "install: restarting EmulationStation"
/etc/init.d/S31emulationstation restart >/dev/null 2>&1 &

echo "install: OK -- Ports -> Xbox Cloud Gaming"
echo "install: log at /userdata/system/logs/xcloud.log"
INSTALLER
chmod 755 "$STAGE/install-on-device.sh"

# ---------------------------------------------------------------------------
cat > "$STAGE/INSTALL.txt" <<EOF
xcloud-rg353ps $VERSION
Xbox Cloud Gaming for the Anbernic RG353PS and RG353-series handhelds,
on the stock firmware.


INSTALL -- SD CARD (easiest, no terminal needed)
------------------------------------------------
1. Turn the handheld off and take out the SD card that holds your ROMs.
   On a two-card device that is the second slot. Put it in your computer.

2. You will see folders like "ports", "snes", "psx" in the card's root.

3. Copy the "ports" folder from THIS archive into the card's root, so it
   merges with the "ports" folder already there. Say yes to merging.
   Nothing of yours is replaced -- the only new items are
   "Xbox Cloud.sh" and a folder called "xcloud".

4. Eject the card properly, put it back, and turn the handheld on.

5. It is in the PORTS section. The first launch names it
   "Xbox Cloud Gaming"; before that it may show as "Xbox Cloud".


INSTALL -- OVER SSH (if you prefer)
-----------------------------------
Copy this whole folder to /userdata on the handheld, then:

    cd /userdata/$NAME
    sh install-on-device.sh


FIRST RUN
---------
It shows a code and a microsoft.com/link address. Open that on your phone
or computer, sign in, and enter the code. The handheld picks it up within
a few seconds. You only do this once.


CONTROLS
--------
    A               select
    B               back
    START           settings (in the library)
    SELECT + X      options (during a stream)
    SELECT + START  quit

Settings are on the device -- stream quality, face-button layout, picture
mode, console language. You never need a computer again after sign-in.


WHAT YOU NEED
-------------
  * Xbox Game Pass Ultimate, for cloud streaming; or
  * your own Xbox on the same network, for remote play (which is lower
    latency and does not use your cloud hours).
  * A 5 GHz Wi-Fi network. The link is the main limit on picture quality.


IF SOMETHING GOES WRONG
-----------------------
The log is at /userdata/system/logs/xcloud.log and its first line names
the exact version, which is the first thing to quote in a bug report.

  Nothing in the Ports menu
      The "xcloud" folder must sit next to "Xbox Cloud.sh", both inside
      the card's "ports" folder. Copying only the .sh file is the usual
      mistake.

  It starts and immediately returns to the menu
      Read the log. "no client binary found" means the xcloud folder did
      not come across.

  Stutter or a soft picture
      Wi-Fi, almost always. Get closer to the router, prefer 5 GHz. The
      panel is 640x480 and the service encodes 720p, so the picture is
      halved -- see "Picture mode" and "Stream quality" in settings.


LICENCE
-------
GPL-3.0. See ports/xcloud/LICENSE and ports/xcloud/THIRD-PARTY-NOTICES.md.
Source and full documentation: see ports/xcloud/README.md.

Not affiliated with, endorsed by, or sponsored by Microsoft or Anbernic.
It uses your own account and your own subscription.
EOF

# ---------------------------------------------------------------------------
# zip(1) if it is here, otherwise python's zipfile -- python is already needed
# by scripts/install.sh, so this adds no dependency, and it is present on far
# more development machines than zip is (notably not on a stock Git for
# Windows). Executable bits are set explicitly in the python path, because
# zipfile does not carry them over and a launcher that arrives non-executable
# is a confusing first experience.
if command -v zip >/dev/null 2>&1; then
	( cd out/package && zip -q -r "../$NAME.zip" "$NAME" ) || die "zip failed"
elif command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1; then
	PY=$(command -v python3 || command -v python)
	"$PY" - "$STAGE" "$OUT" <<'PYZIP' || die "python zipfile failed"
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
root = os.path.dirname(stage)
executable = {"install-on-device.sh", "xcloud", "Xbox Cloud.sh"}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dirpath, _, names in os.walk(stage):
        for n in sorted(names):
            full = os.path.join(dirpath, n)
            arc = os.path.relpath(full, root).replace(os.sep, "/")
            info = zipfile.ZipInfo.from_file(full, arc)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if n in executable else 0o644) << 16
            with open(full, "rb") as f:
                z.writestr(info, f.read())
print("wrote", out)
PYZIP
else
	die "neither zip nor python found; archive out/package/$NAME yourself"
fi

echo >&2
echo "package: $OUT" >&2
ls -la "$OUT" >&2
