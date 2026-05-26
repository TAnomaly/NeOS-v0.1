#!/usr/bin/env python3
"""
Webcam → FPGA + HTTP MJPEG stream.

Captures from a host webcam, quantizes to 16-color 160x120 (the same
representation the FPGA HDMI sees), sends it to the FPGA over UART
(as the regular `cam` protocol), and serves the post-quantize frames
as a live MJPEG stream on http://<host>:8000/. Open that URL on a
phone or any browser on the same LAN to see what the HDMI shows.

Deps:  pip install opencv-python numpy pyserial pillow
Run:
  picocom -b 115200 /dev/ttyUSB1   # type `cam`, then Ctrl-A Ctrl-Q
  python3 tools/webcam_web.py [/dev/ttyUSB1] [cam_index] [http_port]

Quit:  Ctrl-C
"""

import io
import os
import sys
import time
import struct
import signal
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2
import numpy as np
from PIL import Image
import serial

DEV  = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
CAM  = int(sys.argv[2]) if len(sys.argv) > 2 else 0
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8000
BAUD = 115200
W, H = 160, 120
SCALE = 4              # JPEG output upscaled so it's not tiny on phones

EGA16 = [
    (  0,   0,   0), (  0,   0, 170), (  0, 170,   0), (  0, 170, 170),
    (170,   0,   0), (170,   0, 170), (170,  85,   0), (170, 170, 170),
    ( 85,  85,  85), ( 85,  85, 255), ( 85, 255,  85), ( 85, 255, 255),
    (255,  85,  85), (255,  85, 255), (255, 255,  85), (255, 255, 255),
]

_pal_bytes = bytearray()
for r, g, b in EGA16:
    _pal_bytes += bytes([r, g, b])
_pal_bytes += b"\x00\x00\x00" * (256 - 16)
_PAL_IMG = Image.new("P", (1, 1))
_PAL_IMG.putpalette(bytes(_pal_bytes))


# ---- shared state ----
current_jpeg = b""
current_lock = threading.Lock()
stop_flag    = threading.Event()


def quantize_to_indexed(bgr):
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    im = Image.fromarray(rgb).resize((W, H), Image.BILINEAR)
    return im.quantize(palette=_PAL_IMG, dither=Image.Dither.FLOYDSTEINBERG)


def to_fpga_payload(quant) -> bytes:
    indices = list(quant.getdata())
    out = bytearray()
    out += b"img1"
    for r, g, b in EGA16:
        out += struct.pack("<I", (r << 16) | (g << 8) | b)
    for row in range(H):
        for chunk in range(W // 8):
            base = row * W + chunk * 8
            w = 0
            for k in range(8):
                w |= (indices[base + k] & 0xF) << (k * 4)
            out += struct.pack("<I", w)
    return bytes(out)


def to_jpeg(quant) -> bytes:
    big = quant.convert("RGB").resize((W * SCALE, H * SCALE), Image.NEAREST)
    buf = io.BytesIO()
    big.save(buf, format="JPEG", quality=70)
    return buf.getvalue()


def capture_loop(ser):
    cap = cv2.VideoCapture(CAM, cv2.CAP_V4L2)
    if not cap.isOpened():
        print(f"[webcam_web] cannot open camera {CAM}", file=sys.stderr)
        stop_flag.set()
        return
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 320)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)

    print("[webcam_web] capturing — Ctrl-C to stop")
    n, t0 = 0, time.monotonic()
    while not stop_flag.is_set():
        ok, bgr = cap.read()
        if not ok:
            time.sleep(0.05)
            continue
        quant = quantize_to_indexed(bgr)

        jpeg = to_jpeg(quant)
        with current_lock:
            global current_jpeg
            current_jpeg = jpeg

        try:
            ser.write(to_fpga_payload(quant))
            ser.flush()
        except Exception as ex:
            print(f"[webcam_web] serial write error: {ex}", file=sys.stderr)

        n += 1
        if n % 5 == 0:
            fps = n / (time.monotonic() - t0)
            print(f"\r[webcam_web] frame {n}  ~{fps:.2f} fps", end="", flush=True)

    try:
        ser.write(b"\x1b")
        ser.flush()
    except Exception:
        pass
    cap.release()
    print()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass   # silence access log

    def do_GET(self):
        if self.path == "/":
            html = b"""<!doctype html><html><head>
<title>NeOS HDMI live</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>html,body{margin:0;background:#000;color:#0f0;font:14px monospace;}
img{display:block;width:100vw;max-width:100vw;height:auto;image-rendering:pixelated;}
.hdr{padding:8px;}</style>
</head><body>
<div class="hdr">NeOS HDMI live - 160x120 16-color</div>
<img id="f" src="/stream.mjpg"/>
</body></html>"""
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return

        if self.path == "/stream.mjpg":
            self.send_response(200)
            self.send_header("Age", "0")
            self.send_header("Cache-Control", "no-cache, private")
            self.send_header("Pragma", "no-cache")
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=FRAME")
            self.end_headers()
            try:
                while not stop_flag.is_set():
                    with current_lock:
                        jpeg = current_jpeg
                    if jpeg:
                        self.wfile.write(b"--FRAME\r\n")
                        self.send_header("Content-Type", "image/jpeg")
                        self.send_header("Content-Length", str(len(jpeg)))
                        self.end_headers()
                        self.wfile.write(jpeg)
                        self.wfile.write(b"\r\n")
                    time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return

        if self.path == "/frame.jpg":
            with current_lock:
                jpeg = current_jpeg
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(jpeg)))
            self.end_headers()
            self.wfile.write(jpeg)
            return

        self.send_error(404)


def get_lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def main():
    ser = serial.Serial(DEV, BAUD, timeout=2.0)
    ser.dtr = False
    ser.rts = False

    def on_sigint(sig, frame):
        stop_flag.set()
    signal.signal(signal.SIGINT, on_sigint)

    cap_thread = threading.Thread(target=capture_loop, args=(ser,), daemon=True)
    cap_thread.start()

    ip = get_lan_ip()
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[webcam_web] HTTP on http://{ip}:{PORT}/  (any LAN device)")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_flag.set()
        httpd.shutdown()
        ser.close()
        print("\n[webcam_web] bye")


if __name__ == "__main__":
    main()
