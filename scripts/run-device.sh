#!/bin/sh
# Push out/xcloud to the handheld and run it over SSH with EmulationStation
# stopped, then put ES back.
#
# ES holds DRM master while it is running, so the client cannot take the
# screen; stopping it releases master either way. Everything runs under
# `timeout` so a wedged modeset clears itself instead of needing the power
# button. Logs come back on stdout.
#
#   scripts/run-device.sh [seconds] [client args...]
#
#   scripts/run-device.sh 30 -list
#   scripts/run-device.sh 90 -title FORZAHORIZON5
set -e
cd "$(dirname "$0")/.."

SECS=${1:-60}
[ $# -gt 0 ] && shift
ARGS="$*"

. scripts/device.sh
device_require

PAD=${PAD:-/dev/input/event3}
# BIN overrides the binary to push, so a run can use a snapshot copy while
# another build is in progress.
BIN=${BIN:-out/xcloud}

sh scripts/push.sh "$BIN" /userdata/ports/xcloud/xcloud

device_run "
/etc/init.d/S31emulationstation stop >/dev/null 2>&1
sleep 2
cd /userdata/ports/xcloud
HOME=/userdata/ports/xcloud \
XDG_CONFIG_HOME=/userdata/ports/xcloud/config \
XDG_DATA_HOME=/userdata/ports/xcloud/data \
XDG_CACHE_HOME=/userdata/ports/xcloud/cache \
timeout $SECS ./xcloud -p1devicepath $PAD $ARGS 2>&1
RC=\$?
/etc/init.d/S31emulationstation start >/dev/null 2>&1 &
echo \"--- exit \$RC ---\"
echo '--- dmesg (display/vop) tail ---'
dmesg | grep -iE 'vop|drm|vblank|WARN|rkvdec|iommu' | tail -8
"
