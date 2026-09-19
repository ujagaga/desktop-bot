#!/usr/bin/env bash
#
# Build (and optionally flash) the ESP32-P4 camera firmware.
#
#   tools/build.sh            compile only
#   tools/build.sh flash      compile, then flash
#
# Overrides via env:
#   IDF_PATH=/path/to/esp-idf   ESP-IDF checkout (default: ~/esp/esp-idf)
#   PORT=/dev/ttyACM0           serial port for flashing
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
PORT="${PORT:-/dev/ttyACM0}"

[ -f "$IDF_PATH/export.sh" ] || {
  echo "ESP-IDF not found at $IDF_PATH. Set IDF_PATH to your esp-idf checkout." >&2
  exit 1
}
# shellcheck source=/dev/null
source "$IDF_PATH/export.sh" >/dev/null

cd "$PROJECT_DIR"
idf.py set-target esp32p4
idf.py build

if [ "${1:-}" = "flash" ]; then
  idf.py -p "$PORT" flash
fi
