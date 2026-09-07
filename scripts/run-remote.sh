#!/bin/sh
# Run the client ALREADY on the handheld, without pushing. Same shape as
# run-device.sh otherwise: EmulationStation stopped, `timeout`, ES restarted,
# dmesg tail. For back-to-back A/B runs of one binary with different flags.
#
#   scripts/run-remote.sh [seconds] [client args...]
#
# PROBE=<seconds> snapshots the sound card's real hw_params and pointer
# status that many seconds into the run, from the device side, so the rate
# the codec is actually clocked at is on record next to the app's own view.
set -e
cd "$(dirname "$0")/.."

SECS=${1:-60}
[ $# -gt 0 ] && shift
ARGS="$*"

. scripts/device.sh
device_require

PAD=${PAD:-/dev/input/event3}
PROBE=${PROBE:-0}

device_run "
/etc/init.d/S31emulationstation stop >/dev/null 2>&1
sleep 2
cd /userdata/ports/xcloud
if [ $PROBE -gt 0 ]; then
  (sleep $PROBE; echo '--- probe: pcm0p hw_params ---'; cat /proc/asound/card0/pcm0p/sub0/hw_params; echo '--- probe: governor ---'; cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq; for i in 1 2 3; do echo \"--- probe: pcm0p status +\$i ---\"; cat /proc/asound/card0/pcm0p/sub0/status; sleep 20; done) &
fi
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
