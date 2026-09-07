# Familiar — face recognition server

A Python face recognition server intended for a Raspberry Pi 4, with a browser
interface for managing known faces and a webcam client for recognition and enrollment.
It uses OpenCV YuNet for face detection and SFace for recognition.

## Install as a Raspberry Pi service

Use **64-bit Raspberry Pi OS (arm64)** with internet access and a normal user
account that has sudo access. Keep this project in a permanent, user-writable
folder, for example `/home/pi/desktop-bot`. Paths must not contain spaces or
special characters.

From the project folder, run:

```bash
bash install.sh
```

The installer asks for sudo access for system packages and service setup. It:

- Installs Python and required system libraries.
- Creates `.venv-server/` and installs `requirements-server.txt` (including Gunicorn).
- Downloads missing [YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet)
  and [SFace](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface)
  model files from OpenCV Zoo.
- Generates private credentials in `appsettings.py` if it does not exist.
  Existing credentials and face photos are preserved. Empty credentials must be
  filled in before installation can finish.
- Checks application startup, then installs and starts `face-recognition.service`
  and enables it at boot.

Open `http://<pi-address>:8000` and read your login password from `appsettings.py`.
The webcam client runs separately; it is not installed as a service on the Pi.

```bash
sudo systemctl status face-recognition.service
sudo journalctl -u face-recognition.service -f
sudo systemctl restart face-recognition.service
# Stop the service and disable startup at boot:
sudo systemctl disable --now face-recognition.service
```

The service runs as the installing user from this project directory. Do not move
the folder after installation. It uses one synchronous Gunicorn worker because
the OpenCV objects and live recognition index are shared in memory. Do not
increase the worker or thread count without changing that architecture.

After updating the project or dependencies, rerun `bash install.sh`. It preserves
settings and photos and restarts the service. Stop any manually started server
using port 8000 before installing. Package installation requires binary wheels;
if none are available, it stops instead of building OpenCV from source.

## Manual server setup

Create a Python environment and install the server dependencies:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements-server.txt
```

Place these model files in `models/` before starting the server:

- `face_detection_yunet_2023mar.onnx`
- `face_recognition_sface_2021dec.onnx`

If you do not already have local settings, copy the example:

```bash
cp appsettings.example.py appsettings.py
```

Edit `appsettings.py` and set the following values:

| Setting | Purpose |
| --- | --- |
| `PASSWORD` | Browser login password; no username is needed. |
| `API_KEY` | Shared client credential for recognition **and enrollment**. |
| `SECRET_KEY` | Private random key for signing browser sessions. |
| `COOKIE_SECURE` | Set to `True` when serving over HTTPS; keep `False` for local HTTP. |

Use separate values for the password, API key, and session key. Generate random
values as needed with:

```bash
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

Start the server:

```bash
python server.py
```

Open `http://localhost:8000` on the server, or `http://<pi-address>:8000` from
another computer. The server listens on all interfaces on port 8000. Restart it
after changing settings. Use HTTPS when accessing remotely.

## Browser face library

Log in with `PASSWORD` to manage the library:

- See known people, reference-photo counts, and saved-image thumbnails.
- Click a thumbnail to view a larger image in the right-hand preview panel.
  On narrow screens, the preview appears below the library and upload form.
- Upload a photo containing exactly one face and enter the person's name.
- Reuse a name to add another reference photo for that person.
- Log out when finished. Saved images also require a browser login to view.

Names must contain 1–32 letters, digits, underscores, or hyphens. Upload requests
are limited to 10 MB. Newly enrolled faces are available immediately and persist
across restarts.

Photos are grouped by person in a single library:

```text
faces/
├── Alex/
│   ├── photo1.jpg
│   └── photo2.jpg
└── Sam/
    └── photo1.jpg
```

All reference photos are compared together; there are no daily or rare groups.
At startup, the server loads usable reference photos into memory. Photos added
manually to these folders require a restart; browser and client uploads update
the recognition index immediately.

## Webcam client

Run the client on a computer with a webcam, graphical desktop, and a terminal.
Use a separate environment from the server: the client needs OpenCV's GUI package,
while the server uses the headless package.

```bash
python3 -m venv .venv-client
source .venv-client/bin/activate
pip install -r requirements-client.txt
```

On a separate client computer, create an `appsettings.py` containing only the
matching server API key:

```python
API_KEY = "your-server-api-key"
```

Start the client, pointing it at the server:

```bash
SERVER=http://<pi-address>:8000/recognize python client.py
```

| Environment variable | Default | Purpose |
| --- | --- | --- |
| `SERVER` | `http://127.0.0.1:8000/recognize` | Recognition endpoint; enrollment uses `/enroll` on the same server. |
| `API_KEY` | `appsettings.API_KEY` | Override the configured client key. |
| `CAMERA` | `0` | Webcam device index. |

The client still requires `appsettings.py` with an `API_KEY` value when using an
environment override.

### Recognition and enrollment

1. Press **Space** in the live preview to capture and recognize a snapshot.
2. With exactly one detected face, press **E** to enroll it.
3. Enter a name in the terminal; a blank name cancels. No password is needed:
   the API key authorizes enrollment. An existing name adds a reference photo.
4. The client saves the original photo without drawn labels and recognizes the
   snapshot again. A status message reports success or failure; press any key to
   return to the snapshot controls.
5. Press **Space** to resume the live preview, or **Q** / **Escape** to quit.

If multiple people are detected, take another snapshot with just the person you
want to enroll.

## HTTP API

Clients authenticate with the `X-API-Key` header for both endpoints.

| Endpoint | Request | Response |
| --- | --- | --- |
| `POST /recognize` | Raw image bytes, or multipart file field `image` | `faces` containing name, score, and bounding box; plus a `names` list. |
| `POST /enroll` | Multipart fields `image` and `name`; exactly one face required | Saved path, name, and reference-photo count. |

Unrecognized faces have name `unknown` and a null score. Bounding boxes contain
`[x, y, width, height]`. Errors return an `error` message with an HTTP error status.
Browser enrollment instead uses a login session and the `csrf_token` form field.

## Local files and checks

`.gitignore` excludes Python virtual environments, bytecode caches, the local
`tests/` directory, `appsettings.py`, and downloaded ONNX model weights. Keep
`appsettings.example.py` as the shareable configuration template. Reference photos
under `faces/` are not excluded by these rules.

If the local test directory is present, run checks in the server environment:

```bash
python -m unittest discover -s tests -v
```

The checks require server dependencies and model files. They use synthetic images
and mocked face detections; they do not validate physical webcam interaction or
recognition accuracy on the Pi. Tests are local and are not included in new Git
checkouts.
