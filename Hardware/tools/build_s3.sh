#!/usr/bin/env bash
#
# Build (and optionally flash) the ESP32-S3 LCD battery display firmware.
#
#   tools/build_s3.sh            compile only
#   tools/build_s3.sh upload     compile, then flash
#
# Overrides via env:
#   FQBN=esp32:esp32:esp32s3   target board
#   PORT=/dev/ttyACM0          serial port for upload
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH_DIR="$REPO_DIR/ESP32_S3"
BUILD_DIR="$SKETCH_DIR/build"
CACHE_DIR="$SKETCH_DIR/.cache"
[ -f "$REPO_DIR/.env" ] && source "$REPO_DIR/.env"
FQBN="${FQBN:-esp32:esp32:esp32s3}"
PORT="${PORT:-/dev/ttyACM0}"

command -v arduino-cli >/dev/null 2>&1 || {
  echo "arduino-cli not found on PATH. Install: https://arduino.github.io/arduino-cli/latest/installation/" >&2
  exit 1
}

echo "Compiling $SKETCH_DIR for $FQBN ..."
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" --build-path "$CACHE_DIR" "$SKETCH_DIR"

if [ "${1:-}" = "upload" ]; then
  echo "Uploading to $PORT ..."
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$BUILD_DIR" "$SKETCH_DIR"
fi
