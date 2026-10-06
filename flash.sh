#!/bin/bash
# Build + flash Space Invaders to CYD
set -e
cd "$(dirname "$0")"
PORT=${1:-/dev/ttyUSB0}
pio run -e cyd-2432s028
pio run -e cyd-2432s028 -t upload --upload-port "$PORT"
echo "Done. Tap screen to start."
