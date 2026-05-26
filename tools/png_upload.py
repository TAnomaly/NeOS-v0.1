#!/usr/bin/env python3
"""
PNG → NeOS HDMI framebuffer uploader.

Loads a PNG (any size, any depth), resizes to 160x120, quantizes to a
16-color palette, packs to the FPGA's wire format, and sends over UART.

Wire format (after the user types `img` at the NeOS prompt):
    4-byte magic:        'i' 'm' 'g' '1'
    16 × 4-byte LE:      palette entries, low 24 bits = 0x00_RR_GG_BB
    2400 × 4-byte LE:    pixel words, 8 pixels/word, 4 bpp,
                         nibble 0 (LSB) = leftmost pixel of group

Deps:  pip install pillow pyserial
Run:   python3 tools/png_upload.py <image.png> [/dev/ttyUSB1 or /tmp/neos-shell]
"""

import os
import sys
import struct
import time

from PIL import Image
import serial

if len(sys.argv) < 2:
    print("usage: png_upload.py <image.png> [serial_dev]")
    sys.exit(1)

IMG  = sys.argv[1]
DEV  = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB1"
BAUD = 115200
W, H = 160, 120


def quantize_image(path: str):
    im = Image.open(path).convert("RGB")
    im = im.resize((W, H), Image.LANCZOS)
    # Quantize to 16 colors (median-cut). Dither = floyd-steinberg by default.
    q = im.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
    pal_flat = list(q.getpalette() or [])
    # Pad to at least 48 bytes (16 RGB triples) so single-color/low-color
    # images don't crash on indexing.
    while len(pal_flat) < 16 * 3:
        pal_flat.append(0)
    pal_flat = pal_flat[: 16 * 3]
    pal = [(pal_flat[i*3], pal_flat[i*3+1], pal_flat[i*3+2]) for i in range(16)]
    indices = list(q.getdata())     # length W*H, each 0..15
    return pal, indices


def pack_payload(pal, indices) -> bytes:
    out = bytearray()
    out += b"img1"
    # Palette: 16 entries, LE 32-bit, low 24 bits = 0x00_RR_GG_BB
    for r, g, b in pal:
        out += struct.pack("<I", (r << 16) | (g << 8) | b)
    # Pixels: pack 8 pixels per word, nibble 0 = leftmost
    words = []
    for row in range(H):
        for chunk in range(W // 8):
            base = row * W + chunk * 8
            w = 0
            for k in range(8):
                w |= (indices[base + k] & 0xF) << (k * 4)
            words.append(w)
    assert len(words) == 2400
    for w in words:
        out += struct.pack("<I", w)
    return bytes(out)


def main():
    pal, indices = quantize_image(IMG)
    payload = pack_payload(pal, indices)
    print(f"[png_upload] {IMG}: {W}x{H} 16-color → {len(payload)} bytes")
    print(f"[png_upload] opening {DEV} @ {BAUD}")
    print("[png_upload] make sure NeOS has 'img' command running (typed and waiting)")

    ser = serial.Serial(DEV, BAUD, timeout=2.0)
    ser.dtr = False
    ser.rts = False

    # Tiny startup delay so receiver is in tight read loop
    time.sleep(0.2)

    chunk = 256
    sent = 0
    t0 = time.monotonic()
    while sent < len(payload):
        n = ser.write(payload[sent:sent + chunk])
        sent += n
        if sent % 1024 == 0 or sent == len(payload):
            pct = sent * 100 // len(payload)
            print(f"\r[png_upload] {sent}/{len(payload)} ({pct}%)", end="", flush=True)
    # Block until OS + driver have actually pushed the bytes on the wire.
    ser.flush()
    # Extra safety: pyserial flush() doesn't always wait on the TTY transmit
    # FIFO; sleep enough for any straggling bytes (~10 KB / 11.5 KB/s + slack).
    time.sleep(0.4)
    elapsed = time.monotonic() - t0
    print(f"\n[png_upload] done in {elapsed:.2f}s ({sent/elapsed/1024:.1f} KB/s)")
    ser.close()


if __name__ == "__main__":
    main()
