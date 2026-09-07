#!/bin/sh
# M1 diagnostic port: runs both probes the way a real port runs, so we can see
# whether EmulationStation releases DRM master when it launches one.
# Output: /userdata/system/logs/drmprobe-port.log
mkdir -p /userdata/system/logs
LOG=/userdata/system/logs/drmprobe-port.log
{
  echo "=== launched from EmulationStation Ports ==="
  echo "argv: $0 $*"
  echo
  echo "--- processes holding card0 ---"
  fuser -v /dev/dri/card0 2>&1 || echo "(fuser unavailable)"
  echo
  echo "############ native probe (libdrm) ############"
  /userdata/drmprobe 2>&1
  echo
  echo "############ python probe (raw ioctls) ############"
  python3 /userdata/drmprobe.py 2>&1
  echo "=== end ==="
} > "$LOG" 2>&1
exit 0
