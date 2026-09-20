#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH_DIR="$REPO_DIR/ESP32_CAM"
# Classic AI-Thinker ESP32-CAM: PSRAM and two 1.875 MiB OTA slots in 4 MB flash.
FQBN="${FQBN:-esp32:esp32:esp32cam}"
PORT="${PORT:-/dev/ttyUSB0}"
UPLOAD_BAUD="${UPLOAD_BAUD:-115200}"
arduino-cli compile --fqbn "$FQBN" --build-property build.partitions=min_spiffs \
  --build-property upload.maximum_size=1966080 \
  --build-path "$SKETCH_DIR/.cache" --output-dir "$SKETCH_DIR/build" "$SKETCH_DIR"
if [ "${1:-}" = upload ]; then
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" \
    --upload-property "upload.speed=$UPLOAD_BAUD" \
    --input-dir "$SKETCH_DIR/build" "$SKETCH_DIR"
fi
