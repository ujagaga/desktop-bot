"""Webcam test client for the face recognition server.

Live preview; SPACE takes a snapshot, POSTs it to /recognize and draws the
returned boxes and names on the frozen frame. E enrolls a single detected face;
Space resumes, Q or ESC quits. Enrollment prompts for a name in
the terminal where the client was launched.

Point it elsewhere with:  SERVER=http://pi:8000/recognize python client.py
"""

import json
import re
import os
import sys
import urllib.error
import urllib.request
import urllib.parse
from uuid import uuid4

os.environ.setdefault("QT_QPA_PLATFORM", "xcb")  # cv2's wheel ships no wayland plugin

import cv2

import appsettings

SERVER = os.environ.get("SERVER", "http://127.0.0.1:8000/recognize")
API_KEY = os.environ.get("API_KEY", appsettings.API_KEY)
WINDOW = "face test client"
GREEN, RED, WHITE = (0, 200, 0), (0, 0, 255), (255, 255, 255)


def post(jpeg):
    request = urllib.request.Request(
        SERVER, data=jpeg, headers={"Content-Type": "image/jpeg", "X-API-Key": API_KEY},
    )
    return json_request(urllib.request.urlopen, request)


def json_request(open_request, request):
    try:
        with open_request(request, timeout=10) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        try:
            return json.load(error)
        except (ValueError, UnicodeError):
            return {"error": f"Server returned HTTP {error.code}."}
        finally:
            error.close()
    except (OSError, ValueError, UnicodeError) as error:
        return {"error": f"Request failed: {error}"}


class EnrollmentClient:
    """Enroll snapshots using the configured client API key."""
    def enroll(self, jpeg, name):
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", name):
            return {"error": "Name must be 1-32 letters, digits, underscores, or hyphens."}
        boundary = uuid4().hex
        parts = []
        for field, value in (("name", name),):
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{field}"\r\n\r\n{value}\r\n'.encode())
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="image"; filename="snapshot.jpg"\r\nContent-Type: image/jpeg\r\n\r\n'.encode())
        parts.extend((jpeg, f"\r\n--{boundary}--\r\n".encode()))
        request = urllib.request.Request(
            urllib.parse.urljoin(SERVER, "enroll"), data=b"".join(parts),
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}", "X-API-Key": API_KEY},
        )
        return json_request(urllib.request.urlopen, request)


def onboard(enrollment, jpeg, result):
    if "error" in result or len(result.get("faces", [])) != 1:
        return {"error": "Enrollment needs exactly one detected face. Take a new snapshot with one person."}
    try:
        name = input("Name to save (blank cancels; existing name adds a photo): ").strip()
        if not name:
            return {"error": "Enrollment cancelled."}
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", name):
            return {"error": "Name must be 1-32 letters, digits, underscores, or hyphens."}
        return enrollment.enroll(jpeg, name)
    except (EOFError, KeyboardInterrupt):
        return {"error": "Enrollment cancelled."}


def label(frame, text, origin, color, scale=0.7):
    cv2.putText(frame, text, origin, cv2.FONT_HERSHEY_SIMPLEX, scale, color, 2)


def annotate(frame, result):
    for face in result.get("faces", []):
        x, y, w, h = face["box"]
        known = face["score"] is not None
        color = GREEN if known else RED
        cv2.rectangle(frame, (x, y), (x + w, y + h), color, 2)
        text = f'{face["name"]} {face["score"]:.2f}' if known else face["name"]
        label(frame, text, (x, max(y - 8, 20)), color)
    if "error" in result:
        label(frame, result["error"][:70], (10, 30), RED)
    elif not result.get("faces"):
        label(frame, "no face detected", (10, 30), RED)
    return frame


def main():
    camera = cv2.VideoCapture(int(os.environ.get("CAMERA", 0)))
    if not camera.isOpened():
        sys.exit("could not open camera")
    enrollment = EnrollmentClient()
    try:
        while True:
            ok, frame = camera.read()
            if not ok:
                sys.exit("camera read failed")
            preview = frame.copy()
            label(preview, "SPACE snapshot   Q quit", (10, 30), WHITE, 0.6)
            cv2.imshow(WINDOW, preview)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break
            if key != ord(" "):
                continue
            jpeg = cv2.imencode(".jpg", frame)[1].tobytes()
            result = post(jpeg)
            print(json.dumps(result))
            while True:
                display = annotate(frame.copy(), result)
                label(display, "E enroll (terminal)   SPACE resume   Q quit", (10, display.shape[0] - 15), WHITE, 0.5)
                cv2.imshow(WINDOW, display)
                key = cv2.waitKey(0) & 0xFF
                if key in (ord("q"), 27):
                    return
                if key == ord(" "):
                    break
                if key == ord("e"):
                    print("Enrollment: enter the name in this terminal.")
                    saved = onboard(enrollment, jpeg, result)
                    if "error" in saved:
                        print(saved["error"])
                        notice = saved["error"]
                    else:
                        notice = f"Saved {saved['name']} ({saved['photos']} photos)."
                        print(notice)
                        result = post(jpeg)
                    display = annotate(frame.copy(), result)
                    label(display, notice[:80], (10, display.shape[0] - 40), WHITE, 0.5)
                    label(display, "Press any key to continue", (10, display.shape[0] - 15), WHITE, 0.5)
                    cv2.imshow(WINDOW, display)
                    if cv2.waitKey(0) & 0xFF in (ord("q"), 27):
                        return
    finally:
        camera.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
