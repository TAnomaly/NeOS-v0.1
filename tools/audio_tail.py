#!/usr/bin/env python3
"""
NeOS audio tail-listener.

Reads picocom's --logfile in real time (tail -f style) and plays
[TONE hz ms] tones via paplay.

Usage:
  Terminal 1:  picocom -b 115200 --logfile /tmp/neos.log /dev/ttyUSB1
  Terminal 2:  python3 tools/audio_tail.py /tmp/neos.log

Deps:  pip install numpy ; paplay (PulseAudio)
Quit:  Ctrl-C

NOTE: picocom buffers its log writes by default. Tones may arrive
in bursts rather than one-by-one. Latency depends on libc stdio
buffering at picocom's end (~4 KiB). For tight real-time, use
audio_listener.py instead.
"""

import os
import re
import sys
import time
import subprocess
import threading

import numpy as np

LOG = sys.argv[1] if len(sys.argv) > 1 else "/tmp/neos.log"
SR  = 22050
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
        sys.stderr.write(f"[paplay error: {ex}]\n")


def main():
    # wait for file to exist
    print(f"[audio_tail] waiting for {LOG} ... (start picocom first)")
    while not os.path.exists(LOG):
        time.sleep(0.2)
    print(f"[audio_tail] tailing {LOG} — Ctrl-C to quit")

    with open(LOG, "rb") as f:
        f.seek(0, os.SEEK_END)
        buf = b""
        try:
            while True:
                chunk = f.read(4096)
                if not chunk:
                    time.sleep(0.05)
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    m = TONE_RE.search(line)
                    if m:
                        hz = int(m.group(1))
                        ms = int(m.group(2))
                        sys.stderr.write(f"[♪ {hz} Hz {ms} ms]\n")
                        threading.Thread(
                            target=play_tone, args=(hz, ms), daemon=True
                        ).start()
        except KeyboardInterrupt:
            pass

    print("\n[audio_tail] bye")


if __name__ == "__main__":
    main()
