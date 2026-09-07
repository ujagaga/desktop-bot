"""Image, upload, and browser helpers for the face recognition server.

Model instances, configuration, and the live face index belong to server.py.
Request and session helpers require an active Flask request context.
"""

import secrets
from collections import Counter
from functools import lru_cache

import cv2
import numpy as np
from flask import abort, request, session


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


def csrf_token():
    if "csrf_token" not in session:
        session["csrf_token"] = secrets.token_urlsafe(32)
    return session["csrf_token"]


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


def list_people(faces_dir, known_faces):
    """Build gallery entries from the current index and saved reference photos."""
    people = [dict(name=name, photos=count)
              for name, count in sorted(Counter(name for name, _ in known_faces).items())]
    for person in people:
        person["images"] = [path.name for path in sorted((faces_dir / person["name"]).glob("*"))
                            if path.is_file() and path.suffix.lower() in {".jpg", ".jpeg", ".png", ".webp", ".bmp"}]
    return people
