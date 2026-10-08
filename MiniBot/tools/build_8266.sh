#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH_DIR="$REPO_DIR/ESP_8266"
# ESP-01 charger: plain ESP8266 module with 1 MB flash.
FQBN="${FQBN:-esp8266:esp8266:generic}"
PORT="${PORT:-/dev/ttyUSB0}"
UPLOAD_BAUD="${UPLOAD_BAUD:-115200}"
arduino-cli compile --fqbn "$FQBN" \
  --build-path "$SKETCH_DIR/.cache" --output-dir "$SKETCH_DIR/build" "$SKETCH_DIR"
if [ "${1:-}" = upload ]; then
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" \
    --upload-property "upload.speed=$UPLOAD_BAUD" \
    --input-dir "$SKETCH_DIR/build" "$SKETCH_DIR"
fi
