#!/usr/bin/env bash
#
# One-time dev environment setup for a fresh Ubuntu machine: installs
# arduino-cli, the selected board core (ESP32 or ESP8266), and the Arduino
# libraries needed by tools/build.sh.
#
# Usage: tools/setup_env.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Which board are you targeting?"
select BOARD in esp8266 esp32; do
  [ -n "$BOARD" ] && break
done

read -rp "Serial port for upload [/dev/ttyACM0]: " PORT
PORT="${PORT:-/dev/ttyACM0}"
echo "PORT=$PORT" > "$REPO_DIR/.env"

echo "Installing VS Code settings for $BOARD ..."
mkdir -p "$REPO_DIR/.vscode"
cp "$SCRIPT_DIR/vscode_$BOARD"/*.json "$REPO_DIR/.vscode/"
sed -i "s#\"port\": \"\"#\"port\": \"$PORT\"#" "$REPO_DIR/.vscode/arduino.json"

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "Installing arduino-cli ..."
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh -s -- -b ~/.local/bin
  export PATH="$HOME/.local/bin:$PATH"
fi

command -v arduino-cli >/dev/null 2>&1 || {
  echo "arduino-cli still not on PATH. Add ~/.local/bin to PATH and re-run." >&2
  exit 1
}

# Also add a plugdev/dialout membership hint for serial upload permissions.
if ! groups "$USER" | grep -qw dialout; then
  echo "Note: adding $USER to the 'dialout' group for serial port access (needs re-login to take effect)."
  sudo usermod -aG dialout "$USER"
fi

arduino-cli config init --overwrite >/dev/null 2>&1 || true
if [ "$BOARD" = esp32 ]; then
  arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
else
  arduino-cli config set board_manager.additional_urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
fi

echo "Updating index and installing $BOARD core ..."
arduino-cli core update-index
arduino-cli core install "$BOARD:$BOARD"

echo "Installing libraries ..."
arduino-cli lib install \
  "Adafruit GFX Library" \
  "Adafruit ST7735 and ST7789 Library" \
  "ArduinoJson" \
  "NTPClient" \
  "Timezone" \
  "ESP_EEPROM" \
  "WebSockets"

echo "Done. Build with: tools/build.sh"
