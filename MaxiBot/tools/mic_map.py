#!/usr/bin/env python3
"""Find which audio channel (mic1-mic4) each physical microphone is.

Runs `audio levels` on the P4 console and asks you to tap (scratch) each mic
in turn; the channel that jumps most in that window is that mic.

    python3 tools/mic_map.py [label ...]     (default: corners, seen from above,
                                              camera = front)
"""
import re
import sys
import time

import serial

PORT = "/dev/ttyACM0"
LABELS = sys.argv[1:] or ["front-left", "front-right", "back-right", "back-left"]
READY_S = 5
TAP_S = 6

s = serial.Serial(PORT, 115200, timeout=0.05)
s.read(100000)
total = READY_S + TAP_S * len(LABELS) + 2
s.write(f"audio levels {total}\r\n".encode())

print(f"Get ready: tap or scratch each mic for {TAP_S} s when asked.")
start = time.time()
windows = [[] for _ in LABELS]
baseline = []
buf = ""
shown = None
while time.time() - start < total + 1:
    t = time.time() - start
    slot = int((t - READY_S) // TAP_S) if t >= READY_S else -1
    if slot != shown and slot < len(LABELS):
        print(f">>> tap {LABELS[slot].upper()} now" if slot >= 0 else f"... quiet, starting in {READY_S} s")
        shown = slot
    buf += s.read(4096).decode(errors="replace")
    *lines, buf = buf.split("\n")
    for line in lines:
        values = re.findall(r"-?\d+\.\d", line)
        if len(values) == 4:
            peaks = [float(v) for v in values]
            if slot < 0:
                baseline.append(peaks)
            elif slot < len(LABELS):
                windows[slot].append(peaks)

if not baseline or not all(windows):
    sys.exit("no level data received; is the robot on /dev/ttyACM0?")
base = [sum(row[c] for row in baseline) / len(baseline) for c in range(4)]
print("\nquiet level dBFS: " + "  ".join(f"mic{c + 1} {base[c]:.1f}" for c in range(4)))
for label, rows in zip(LABELS, windows):
    rise = [max(row[c] for row in rows) - base[c] for c in range(4)]
    best = max(range(4), key=lambda c: rise[c])
    detail = "  ".join(f"mic{c + 1} +{rise[c]:.0f}" for c in range(4))
    print(f"{label:12s} -> mic{best + 1}   ({detail} dB)")
