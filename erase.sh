#!/bin/bash
# Full erase CYD — removes Bruce firmware
set -e
cd "$(dirname "$0")"
PORT=${1:-/dev/ttyUSB0}
~/.local/share/mise/installs/python/latest/bin/python -m esptool --chip esp32 -p "$PORT" -b 921600 erase_flash
echo "Erased. Bruce is gone."
