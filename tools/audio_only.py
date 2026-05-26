#!/usr/bin/env python3
"""
NeOS audio-only listener.

Reads from a pty (typically /tmp/neos-tap set up by serial_mux.py),
parses "[TONE <hz> <ms>]" lines, plays each tone via paplay.
Does NOT mirror text or forward stdin — pure audio side-channel.

Deps:  pip install numpy ; paplay (PulseAudio)
Run:   python3 tools/audio_only.py [/tmp/neos-tap]
Quit:  Ctrl-C
"""

import os
import re
import sys
import subprocess
import threading

import numpy as np

DEV  = sys.argv[1] if len(sys.argv) > 1 else "/tmp/neos-tap"
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
        sys.stderr.write(f"[paplay error: {ex}]\n")


def main():
    print(f"[audio_only] reading {DEV} — Ctrl-C to quit")
    fd = os.open(DEV, os.O_RDONLY | os.O_NOCTTY)
    buf = b""
    try:
        while True:
            chunk = os.read(fd, 256)
            if not chunk:
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
    finally:
        os.close(fd)
        print("\n[audio_only] bye")


if __name__ == "__main__":
    main()
