#!/usr/bin/env python3
"""
Webcam → NeOS HDMI live stream.

Captures from a host webcam, downsamples to 160x120, quantizes each frame
to 16 colors, and sends frame after frame to the FPGA over UART. Lives
inside the FPGA's `cam` command (cmd_cam loops reading 'img1' headers).

At 115200 baud each frame is ~9.7 KB → ~0.1 s line time. With fixed-palette
dither (no per-frame median-cut) the host can usually feed 5-10 fps. Press
Ctrl-C to stop; an ESC byte is sent so the FPGA exits cam mode cleanly.

Deps:  pip install opencv-python numpy pyserial pillow
Run:   python3 tools/webcam_stream.py [/dev/ttyUSB1] [cam_index]
"""

import os
import sys
import time
import struct
import signal

import cv2
import numpy as np
from PIL import Image
import serial

DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
CAM = int(sys.argv[2]) if len(sys.argv) > 2 else 0
BAUD = 115200
W, H = 160, 120

# Fixed EGA-16 palette — chosen for decent flesh tones + dark/light spread.
# Indexing is 0..15. Each entry is (R, G, B).
EGA16 = [
    (  0,   0,   0),  # 0  black
    (  0,   0, 170),  # 1  blue
    (  0, 170,   0),  # 2  green
    (  0, 170, 170),  # 3  cyan
    (170,   0,   0),  # 4  red
    (170,   0, 170),  # 5  magenta
    (170,  85,   0),  # 6  brown
    (170, 170, 170),  # 7  light gray
    ( 85,  85,  85),  # 8  dark gray
    ( 85,  85, 255),  # 9  light blue
    ( 85, 255,  85),  # 10 light green
    ( 85, 255, 255),  # 11 light cyan
    (255,  85,  85),  # 12 light red
    (255,  85, 255),  # 13 light magenta
    (255, 255,  85),  # 14 yellow
    (255, 255, 255),  # 15 white
]

# Pillow palette image used for Image.quantize(palette=...). Must be a 'P'
# image with up to 256 RGB triples in its palette. We fill 16 real colors
# then pad the rest with black.
_pal_bytes = bytearray()
for r, g, b in EGA16:
    _pal_bytes += bytes([r, g, b])
_pal_bytes += b"\x00\x00\x00" * (256 - 16)
_PAL_IMG = Image.new("P", (1, 1))
_PAL_IMG.putpalette(bytes(_pal_bytes))


def quantize_frame(bgr) -> bytes:
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    im = Image.fromarray(rgb).resize((W, H), Image.BILINEAR)
    # Dither against the fixed EGA palette. Much faster than median-cut.
    q = im.quantize(palette=_PAL_IMG, dither=Image.Dither.FLOYDSTEINBERG)

    pal_flat = []
    for r, g, b in EGA16:
        pal_flat += [r, g, b]
    indices = list(q.getdata())

    out = bytearray()
    out += b"img1"
    for i in range(16):
        r, g, b = pal_flat[i*3], pal_flat[i*3+1], pal_flat[i*3+2]
        out += struct.pack("<I", (r << 16) | (g << 8) | b)
    # 8 pixels per 32-bit word, nibble 0 = leftmost
    for row in range(H):
        for chunk in range(W // 8):
            base = row * W + chunk * 8
            w = 0
            for k in range(8):
                w |= (indices[base + k] & 0xF) << (k * 4)
            out += struct.pack("<I", w)
    return bytes(out)


def main():
    print(f"[webcam_stream] cam={CAM}  dev={DEV}  baud={BAUD}")
    cap = cv2.VideoCapture(CAM, cv2.CAP_V4L2)
    if not cap.isOpened():
        print(f"[webcam_stream] cannot open camera {CAM}")
        sys.exit(1)
    # ask for a small frame to save capture time
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 320)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)

    ser = serial.Serial(DEV, BAUD, timeout=2.0)
    ser.dtr = False
    ser.rts = False

    stop_flag = {"v": False}
    def on_sigint(sig, frame):
        stop_flag["v"] = True
    signal.signal(signal.SIGINT, on_sigint)

    print("[webcam_stream] streaming — Ctrl-C to stop")
    n = 0
    t0 = time.monotonic()
    try:
        while not stop_flag["v"]:
            ok, bgr = cap.read()
            if not ok:
                time.sleep(0.05)
                continue
            payload = quantize_frame(bgr)
            ser.write(payload)
            ser.flush()
            n += 1
            if n % 5 == 0:
                fps = n / (time.monotonic() - t0)
                print(f"\r[webcam_stream] frame {n}  ~{fps:.2f} fps", end="", flush=True)
    finally:
        # tell FPGA to exit cam mode
        try:
            ser.write(b"\x1b")
            ser.flush()
        except Exception:
            pass
        cap.release()
        ser.close()
        print("\n[webcam_stream] bye")


if __name__ == "__main__":
    main()
