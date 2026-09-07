#!/usr/bin/env python3

"""Face recognition server: browser UI, snapshot recognition, and enrollment.

Pipeline: YuNet detect -> 5-point align -> SFace embed -> cosine match.
Known faces live in faces/<name>/*.jpg and are all matched together.

GET  /          Google-login-protected face list and upload form
POST /recognize API key + image (raw body or multipart) -> recognized names
POST /enroll    API key or login session + CSRF token, image + name -> enrollment
"""

import io
import json
import pathlib
import re
import secrets
from collections import Counter
from functools import lru_cache, wraps
from hmac import compare_digest
from uuid import uuid4

import appsettings

import cv2
import numpy as np
from authlib.integrations.flask_client import OAuth
from flask import Flask, jsonify, render_template, request, session, redirect, url_for, abort, send_file

MODELS = pathlib.Path(__file__).parent / "models"
FACES = pathlib.Path(__file__).parent / "faces"
COSINE_THRESHOLD = 0.363  # OpenCV's recommended SFace threshold
NAME_RE = re.compile(r"[A-Za-z0-9_-]{1,32}")  # a name becomes a directory name

detector = cv2.FaceDetectorYN_create(
    str(MODELS / "face_detection_yunet_2023mar.onnx"), "", (320, 320),
    score_threshold=0.6, nms_threshold=0.3, top_k=5000,
)
recognizer = cv2.FaceRecognizerSF_create(
    str(MODELS / "face_recognition_sface_2021dec.onnx"), "",
)


def embed(img, face):
    """Align a detected face and return its L2-normalized 128-d embedding."""
    feature = recognizer.feature(recognizer.alignCrop(img, face))
    return feature.flatten() / np.linalg.norm(feature)


def detect(img):
    detector.setInputSize((img.shape[1], img.shape[0]))
    _, faces = detector.detect(img)
    return faces if faces is not None else np.empty((0, 15), np.float32)


def decode(blob):
    return cv2.imdecode(np.frombuffer(blob, np.uint8), cv2.IMREAD_COLOR) if blob else None


def image_from_request():
    # ESP32 posts the raw JPEG body; multipart is what the browser and curl send.
    # Only touch request.files for multipart -- reading it otherwise makes Werkzeug
    # form-parse the body and get_data() then comes back empty.
    if request.mimetype == "multipart/form-data":
        upload = request.files.get("image")
        return upload.read() if upload else b""
    return request.get_data()


def load_faces():
    """Embed every image under faces/<name>/ into a list of (name, vector)."""
    entries = []
    for person in sorted(FACES.glob("*")):
        if not person.is_dir():
            continue
        for path in sorted(person.iterdir()):
            img = cv2.imread(str(path))
            if img is None:
                print(f"skip {path}: not a readable image")
                continue
            faces = detect(img)
            if len(faces) != 1:
                print(f"skip {path}: found {len(faces)} faces, want exactly 1")
                continue
            entries.append((person.name, embed(img, faces[0])))
    print(f"{len(entries)} images across {len({n for n, _ in entries})} people")
    return entries


def best_match(vector):
    entries = KNOWN_FACES
    if not entries:
        return None
    scores = np.array([v for _, v in entries], np.float32) @ vector
    i = int(np.argmax(scores))
    if scores[i] < COSINE_THRESHOLD:
        return None
    return entries[i][0], float(scores[i])


KNOWN_FACES = load_faces()

app = Flask(__name__)
app.config.update(
    SECRET_KEY=appsettings.SECRET_KEY,
    MAX_CONTENT_LENGTH=10 * 1024 * 1024,
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Lax",
    SESSION_COOKIE_SECURE=getattr(appsettings, "COOKIE_SECURE", False),
)
if not all((appsettings.API_KEY, appsettings.SECRET_KEY)):
    raise RuntimeError("Configure API_KEY and SECRET_KEY in appsettings.py")

# Resolve relative paths against the project, independent of the working directory.
secrets_path = pathlib.Path(getattr(appsettings, "GOOGLE_CLIENT_SECRETS_FILE", "client_secret.json"))
if not secrets_path.is_absolute():
    secrets_path = pathlib.Path(__file__).parent / secrets_path
ALLOWED_EMAILS = {email.strip().casefold() for email in getattr(appsettings, "ALLOWED_EMAILS", [])}
oauth = OAuth(app)
google = None
if secrets_path.is_file():
    with secrets_path.open() as source:
        credentials = json.load(source)["web"]
    google = oauth.register(
        name="google",
        client_id=credentials["client_id"],
        client_secret=credentials["client_secret"],
        server_metadata_url="https://accounts.google.com/.well-known/openid-configuration",
        client_kwargs={"scope": "openid email profile"},
    )


def logged_in():
    user = session.get("user", {})
    return bool(user.get("sub") and user.get("email") in ALLOWED_EMAILS)



def csrf_token():
    if "csrf_token" not in session:
        session["csrf_token"] = secrets.token_urlsafe(32)
    return session["csrf_token"]


app.jinja_env.globals["csrf_token"] = csrf_token


def login_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not logged_in():
            if request.method == "GET":
                return redirect(url_for("login"))
            return jsonify(error="login required"), 401
        return view(*args, **kwargs)
    return wrapped


@app.before_request
def protect_requests():
    if request.endpoint == "recognize" or (
        request.endpoint == "enroll" and "X-API-Key" in request.headers
    ):
        supplied = request.headers.get("X-API-Key", "")
        if not compare_digest(supplied.encode(), appsettings.API_KEY.encode()):
            return jsonify(error="invalid or missing API key"), 401
    elif request.method == "POST":
        supplied = request.headers.get("X-CSRF-Token") or request.form.get("csrf_token", "")
        expected = session.get("csrf_token", "")
        if not expected or not compare_digest(supplied.encode(), expected.encode()):
            return jsonify(error="invalid CSRF token; reload the page and try again"), 403


@app.errorhandler(413)
def too_large(error):
    return jsonify(error="image must be smaller than 10 MB"), 413


@app.get("/login")
def login():
    if logged_in():
        return redirect(url_for("index"))
    error = None
    if google is None or not ALLOWED_EMAILS:
        error = "Google login is not configured. Set up client_secret.json and ALLOWED_EMAILS on the server."
    return render_template("login.html", error=error)


@app.get("/auth/google")
def google_login():
    if google is None or not ALLOWED_EMAILS:
        return render_template("login.html", error="Google login is not configured."), 503
    session.clear()
    return google.authorize_redirect(
        url_for("oauth2callback", _external=True,
                _scheme="http" if request.host.split(":", 1)[0] in {"localhost", "127.0.0.1"} else "https"),
        prompt="select_account",
    )


@app.get("/oauth2callback")
def oauth2callback():
    if google is None or not ALLOWED_EMAILS:
        return render_template("login.html", error="Google login is not configured."), 503
    try:
        # Authlib validates OAuth state and the ID token's signature, issuer,
        # audience, expiry and nonce before providing these identity claims.
        token = google.authorize_access_token()
        user = token.get("userinfo", {})
        email = user.get("email", "").strip().casefold()
        if not user.get("sub") or user.get("email_verified") is not True or email not in ALLOWED_EMAILS:
            session.clear()
            return render_template("login.html", error="This Google account is not authorized."), 403
    except Exception:
        session.clear()
        return render_template("login.html", error="Google login failed. Please try again."), 400
    session.clear()
    session["user"] = {"sub": user["sub"], "email": email}
    return redirect(url_for("index"))


@app.post("/logout")
@login_required
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.get("/")
@login_required
def index():
    people = [dict(name=name, photos=count)
              for name, count in sorted(Counter(name for name, _ in KNOWN_FACES).items())]
    for person in people:
        person["images"] = [path.name for path in sorted((FACES / person["name"]).glob("*"))
                            if path.is_file() and path.suffix.lower() in {".jpg", ".jpeg", ".png", ".webp", ".bmp"}]
    return render_template("index.html", people=people)


@lru_cache(maxsize=128)
def thumbnail_bytes(path, modified_ns):
    """Cache small previews; a file modification invalidates its cached version."""
    img = cv2.imread(path)
    if img is None:
        abort(404)
    height, width = img.shape[:2]
    scale = min(160 / width, 160 / height, 1)
    if scale < 1:
        img = cv2.resize(img, (max(1, round(width * scale)), max(1, round(height * scale))), interpolation=cv2.INTER_AREA)
    ok, encoded = cv2.imencode(".jpg", img)
    if not ok:
        abort(404)
    return encoded.tobytes()


@app.get("/photos/<name>/<filename>")
@login_required
def photo(name, filename):
    path = (FACES / name / filename).resolve()
    if (not NAME_RE.fullmatch(name) or not path.is_relative_to(FACES.resolve())
            or path.suffix.lower() not in {".jpg", ".jpeg", ".png", ".webp", ".bmp"}
            or not path.is_file()):
        abort(404)
    if request.args.get("thumbnail") == "1":
        response = send_file(io.BytesIO(thumbnail_bytes(str(path), path.stat().st_mtime_ns)), mimetype="image/jpeg")
    else:
        response = send_file(path)
    response.headers["Cache-Control"] = "private, no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    return response


@app.post("/recognize")
def recognize():
    img = decode(image_from_request())
    if img is None:
        return jsonify(error="could not decode image"), 400

    results = []
    for face in detect(img):
        vector = embed(img, face)
        match = best_match(vector)
        name, score = match if match else ("unknown", None)
        results.append({
            "name": name,
            "score": score,
            "box": [int(v) for v in face[:4]],
        })

    return jsonify(faces=results, names=[f["name"] for f in results])


@app.post("/enroll")
def enroll():
    # API keys are validated above; browser uploads require a session and CSRF.
    if "X-API-Key" not in request.headers and not logged_in():
        return jsonify(error="login or API key required"), 401
    name = request.form.get("name", "").strip()
    if not NAME_RE.fullmatch(name):
        return jsonify(error="name must be 1-32 chars of letters, digits, _ or -"), 400

    blob = image_from_request()
    img = decode(blob)
    if img is None:
        return jsonify(error="could not decode image"), 400
    faces = detect(img)
    if len(faces) != 1:
        return jsonify(error=f"found {len(faces)} faces, need exactly 1 to enroll"), 400

    person = FACES / name
    person.mkdir(parents=True, exist_ok=True)
    path = person / f"{uuid4().hex}.jpg"
    feature = embed(img, faces[0])
    if not cv2.imwrite(str(path), img):
        return jsonify(error="could not save image"), 500

    # Append to the live index so the new face matches without a restart.
    KNOWN_FACES.append((name, feature))
    return jsonify(
        saved=str(path.relative_to(FACES.parent)),
        name=name,
        photos=sum(1 for n, _ in KNOWN_FACES if n == name),
    )


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000, threaded=False)
