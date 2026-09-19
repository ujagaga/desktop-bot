#!/usr/bin/env python3
"""Reads JPEG frames from the ESP32-P4 over its USB-Serial-JTAG port
(FUSB) and displays them. Frame format: u32 magic, u16 width, u16 height,
u32 length, then `length` bytes of JPEG data (see main.c)."""
import sys
import struct

import numpy as np
import cv2
import serial

MAGIC = struct.pack("<I", 0x55AA55AA)
PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"

# Baud is ignored by USB-CDC devices like this one; pyserial still requires a value.
ser = serial.Serial(PORT, 115200, timeout=5)


def read_exact(n):
    data = b""
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            raise IOError("serial timeout waiting for data")
        data += chunk
    return data


def sync_to_magic():
    buf = read_exact(4)
    while buf != MAGIC:
        buf = buf[1:] + read_exact(1)


while True:
    sync_to_magic()
    width, height, length = struct.unpack("<HHI", read_exact(8))
    jpg = np.frombuffer(read_exact(length), dtype=np.uint8)
    bgr = cv2.imdecode(jpg, cv2.IMREAD_COLOR)
    if bgr is None:
        continue
    cv2.imshow("camera", bgr)
    if cv2.waitKey(1) & 0xFF == ord("q"):
        break

ser.close()
cv2.destroyAllWindows()
