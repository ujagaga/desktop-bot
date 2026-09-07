#!/usr/bin/env bash
# Install in place on 64-bit Raspberry Pi OS. Run as your normal Pi user.
set -Eeuo pipefail
trap 'echo "Installation failed at line $LINENO. Fix the error above and rerun install.sh." >&2' ERR

APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_DIR=$(dirname -- "$APP_DIR")
SERVICE_NAME=face-recognition.service
SERVICE_USER=${SUDO_USER:-$(id -un)}
VENV_DIR="$PROJECT_DIR/.venv-server"

fail() { echo "Error: $*" >&2; exit 1; }
[[ "$SERVICE_USER" != root ]] || fail "Run bash install.sh as your normal Pi user (with sudo access), not from a root login."
[[ "$SERVICE_USER" =~ ^[a-z_][a-z0-9_-]*[$]?$ ]] || fail "Unsupported service user: $SERVICE_USER"
# Keep systemd paths unambiguous, including its percent and dollar expansion.
[[ "$APP_DIR" =~ ^/[a-zA-Z0-9_./-]+$ ]] || fail "Place the project in a path containing only letters, digits, /, _, . and -."
command -v apt-get >/dev/null || fail "This installer requires Raspberry Pi OS / Debian."
[[ -d /run/systemd/system ]] || fail "This installer requires a running systemd system."
[[ $(dpkg --print-architecture) == arm64 ]] || fail "Use 64-bit Raspberry Pi OS (arm64) for this installer."
[[ -f "$APP_DIR/server.py" && -f "$APP_DIR/requirements.txt" ]] || fail "Project files are missing."

as_root() {
    if [[ $EUID == 0 ]]; then "$@"; else sudo "$@"; fi
}
as_user() {
    if [[ $EUID == 0 ]]; then runuser -u "$SERVICE_USER" -- "$@"; else "$@"; fi
}
as_user test -w "$APP_DIR" || fail "$SERVICE_USER must own or have write access to $APP_DIR."
cd "$APP_DIR"

echo "Installing system dependencies..."
as_root apt-get update
as_root apt-get install -y python3 python3-venv python3-pip curl ca-certificates libgomp1

echo "Installing server Python packages..."
as_user python3 -m venv "$VENV_DIR"
as_user "$VENV_DIR/bin/python" -m pip install --upgrade pip
# Avoid an accidental multi-hour OpenCV source build on the Pi.
as_user "$VENV_DIR/bin/python" -m pip install --only-binary=:all: -r "$APP_DIR/requirements.txt"
echo "Migrating legacy settings, OAuth credentials, photos, and models..."
as_user "$VENV_DIR/bin/python" "$APP_DIR/migrate.py"
as_user mkdir -p "$APP_DIR/models" "$APP_DIR/faces"

for MODEL in face_detection_yunet/face_detection_yunet_2023mar.onnx face_recognition_sface/face_recognition_sface_2021dec.onnx; do
    DESTINATION="$APP_DIR/models/${MODEL##*/}"
    if [[ ! -s "$DESTINATION" ]]; then
        echo "Downloading ${MODEL##*/} from OpenCV Zoo..."
        as_user curl --fail --location --retry 3 --connect-timeout 20 \
            "https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/$MODEL" \
            --output "$DESTINATION.part"
        as_user mv "$DESTINATION.part" "$DESTINATION"
    fi
done

echo "Preparing local settings (existing settings are preserved)..."
as_user "$VENV_DIR/bin/python" - <<'PY'
import os
import pathlib
import secrets

path = pathlib.Path('appsettings.py')
if not path.exists():
    content = ('"""Local server credentials. Keep private."""\n'
               f'API_KEY = "{secrets.token_urlsafe(32)}"\n'
               f'SECRET_KEY = "{secrets.token_urlsafe(48)}"\n'
               'COOKIE_SECURE = False\n'
               'GOOGLE_CLIENT_SECRETS_FILE = "client_secret.json"\n'
               'ALLOWED_EMAILS = []\n'
               'GEMINI_API_KEY = ""\n'
               'GEMINI_MODEL = "gemini-3.1-flash-live-preview"\n')
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as output:
        output.write(content)
path.chmod(0o600)
PY

echo "Checking credentials, models, and application startup..."
as_user "$VENV_DIR/bin/python" - <<'PY'
import appsettings
for key in ('API_KEY', 'SECRET_KEY'):
    value = getattr(appsettings, key, None)
    if not isinstance(value, str) or not value:
        raise SystemExit(f'Set a nonempty {key} in appsettings.py, then rerun install.sh.')
import conversation
conversation.create_app()
import server
with server.app.test_client() as client:
    assert client.get('/login').status_code == 200, 'Login page check failed'
print('Application startup check passed.')
PY

UNIT_FILE=$(mktemp)
trap 'rm -f -- "$UNIT_FILE"' EXIT
cat > "$UNIT_FILE" <<EOF_UNIT
[Unit]
Description=Familiar face recognition server
After=network.target
StartLimitIntervalSec=120
StartLimitBurst=5

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$APP_DIR
Environment=PYTHONUNBUFFERED=1
ExecStart=$VENV_DIR/bin/gunicorn --bind 0.0.0.0:8040 --workers 1 --worker-class sync --threads 1 --timeout 120 --access-logfile - --error-logfile - server:app
Restart=on-failure
RestartSec=5
UMask=0077
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF_UNIT

as_root install -m 0644 "$UNIT_FILE" "/etc/systemd/system/$SERVICE_NAME"
cat > "$UNIT_FILE" <<EOF_UNIT
[Unit]
Description=Familiar Gemini conversation gateway
After=network-online.target face-recognition.service
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$APP_DIR
Environment=PYTHONUNBUFFERED=1
ExecStart=$VENV_DIR/bin/python $APP_DIR/conversation.py
Restart=on-failure
RestartSec=5
UMask=0077
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF_UNIT
as_root install -m 0644 "$UNIT_FILE" /etc/systemd/system/face-conversation.service
as_root systemctl daemon-reload
as_root systemctl enable "$SERVICE_NAME"
as_root systemctl reset-failed "$SERVICE_NAME"
as_root systemctl restart "$SERVICE_NAME"
as_root systemctl enable face-conversation.service
as_root systemctl reset-failed face-conversation.service
as_root systemctl restart face-conversation.service

echo "Waiting for the server to respond..."
READY=false
for ((attempt = 0; attempt < 60; attempt++)); do
    if as_root systemctl is-active --quiet "$SERVICE_NAME" && \
        curl --fail --silent --max-time 2 http://127.0.0.1:8040/login >/dev/null; then
        READY=true
        break
    fi
    sleep 2
done
if [[ "$READY" != true ]]; then
    as_root journalctl -u "$SERVICE_NAME" -n 40 --no-pager
    fail "Server did not become ready. Inspect the logs above."
fi

echo "Checking conversation gateway..."
as_user "$VENV_DIR/bin/python" - <<'PY_CHECK_GATEWAY'
import time
import urllib.request
import appsettings
for attempt in range(20):
    try:
        request = urllib.request.Request('http://127.0.0.1:8041/health',
            headers={'X-API-Key': appsettings.API_KEY})
        with urllib.request.urlopen(request, timeout=2) as response:
            assert response.status == 200
        break
    except OSError:
        time.sleep(1)
else:
    raise SystemExit('Gateway failed to start. Run: sudo journalctl -u face-conversation.service -n 40')
PY_CHECK_GATEWAY

echo "Installed and started $SERVICE_NAME; it will start automatically on boot."
echo "Copy client_secret.json into $APP_DIR and configure ALLOWED_EMAILS in appsettings.py."
echo "Restart $SERVICE_NAME after configuring Google login. See README.md for HTTPS setup."
echo "Logs: sudo journalctl -u $SERVICE_NAME -f"
echo "Restart: sudo systemctl restart $SERVICE_NAME"
echo "Keep this project at $APP_DIR: the service runs from this directory."

echo "Gemini gateway installed on port 8041 (face-conversation.service)."
echo "Set GEMINI_API_KEY in appsettings.py, then sudo systemctl restart face-conversation.service."
