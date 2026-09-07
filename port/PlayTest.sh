#!/bin/sh
# M2 acceptance test, launched the real way: from the EmulationStation Ports
# menu, so DRM master handling is exercised exactly as the client will do it.
# Plays the test clip three times, then returns to ES.
# Log: /userdata/system/logs/playfile.log
mkdir -p /userdata/system/logs
LOG=/userdata/system/logs/playfile.log
{
  echo "=== launched from Ports at $(uptime) ==="
  echo "argv: $0 $*"
  for i in 1 2 3; do
    echo "--- pass $i ---"
    timeout 30 /userdata/playfile /userdata/test720.mp4
    echo "exit=$?"
  done
  echo "=== end ==="
} > "$LOG" 2>&1
exit 0
