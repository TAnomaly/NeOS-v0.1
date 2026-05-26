#!/usr/bin/env python3
"""
NeOS audio listener — replaces picocom for the `synth` command.

Bridges /dev/ttyUSB1 <-> terminal stdin/stdout AND intercepts
"[TONE <hz> <ms>]" lines emitted by the FPGA. Each match plays a
sine wave through host PulseAudio via `paplay`.

Deps:  pip install pyserial numpy ; paplay (PulseAudio) installed
Run:   python3 tools/audio_listener.py [/dev/ttyUSB1] [115200]
Quit:  Ctrl-C
"""

import re
import sys
import threading
import termios
import tty
import select
import subprocess

import serial
import numpy as np

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
SR   = 22050
TONE_RE = re.compile(rb"\[TONE\s+(\d+)\s+(\d+)\]")

def play_tone(hz: int, ms: int):
    if hz == 0 or ms == 0:
        return
    n = int(SR * ms / 1000)
    if n <= 0:
        return
    t = np.arange(n, dtype=np.float32) / SR
    wave = 0.30 * np.sin(2 * np.pi * hz * t)
    env = min(int(SR * 0.005), n // 4)
    if env > 0:
        wave[:env]  *= np.linspace(0, 1, env, dtype=np.float32)
        wave[-env:] *= np.linspace(1, 0, env, dtype=np.float32)
    pcm = (wave * 32767).astype("<i2").tobytes()
    try:
        p = subprocess.Popen(
            ["paplay", "--raw",
             f"--rate={SR}", "--channels=1", "--format=s16le"],
            stdin=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        p.stdin.write(pcm)
        p.stdin.close()
    except Exception as ex:
        sys.stderr.write(f"\n[paplay error: {ex}]\n")


def reader(ser: serial.Serial):
    buf = b""
    while True:
        try:
            chunk = ser.read(256)
        except Exception:
            return
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            m = TONE_RE.search(line)
            if m:
                hz = int(m.group(1))
                ms = int(m.group(2))
                # uncomment for debug:
                # sys.stderr.write(f"\n[tone {hz} Hz {ms} ms]\n")
                threading.Thread(target=play_tone, args=(hz, ms), daemon=True).start()
                continue
            sys.stdout.buffer.write(line + b"\n")
            sys.stdout.buffer.flush()
        if len(buf) > 4096:
            sys.stdout.buffer.write(buf)
            sys.stdout.buffer.flush()
            buf = b""


def main():
    print(f"[audio_listener] {PORT} @ {BAUD} (paplay backend) — Ctrl-C to quit")
    ser = serial.Serial(PORT, BAUD, timeout=0.05)
    ser.dtr = False
    ser.rts = False

    t = threading.Thread(target=reader, args=(ser,), daemon=True)
    t.start()

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        while True:
            r, _, _ = select.select([sys.stdin], [], [], 0.1)
            if r:
                ch = sys.stdin.buffer.read(1)
                if not ch:
                    break
                ser.write(ch)
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        ser.close()
        print("\n[audio_listener] bye")


if __name__ == "__main__":
    main()
