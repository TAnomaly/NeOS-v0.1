#!/usr/bin/env python3
"""
NeOS serial multiplexer.

Owns /dev/ttyUSB1 (single Linux serial open) and exposes TWO virtual ptys:
  /tmp/neos-shell   bidirectional shell  — point picocom here
  /tmp/neos-tap     read-only fan-out    — point audio_only.py here

Everything the FPGA sends is broadcast to BOTH ptys.
Bytes written to shell pty are forwarded to the FPGA.
Bytes written to tap pty are discarded.

Deps:  pip install pyserial
Run:   python3 tools/serial_mux.py [/dev/ttyUSB1] [115200]
Quit:  Ctrl-C
"""

import os
import sys
import pty
import threading
import select

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

SHELL_LINK = "/tmp/neos-shell"
TAP_LINK   = "/tmp/neos-tap"


def make_pty(link_path):
    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)
    # remove old link, create fresh
    try:
        os.unlink(link_path)
    except FileNotFoundError:
        pass
    os.symlink(slave_name, link_path)
    # we keep slave_fd open so the pty stays alive even before the client connects
    return master_fd, slave_fd, slave_name


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0)
    ser.dtr = False
    ser.rts = False

    shell_m, shell_s, shell_name = make_pty(SHELL_LINK)
    tap_m,   tap_s,   tap_name   = make_pty(TAP_LINK)

    print(f"[serial_mux] {PORT} @ {BAUD}")
    print(f"  shell pty -> {SHELL_LINK} -> {shell_name}")
    print(f"  tap   pty -> {TAP_LINK}   -> {tap_name}")
    print("  picocom -b 115200 /tmp/neos-shell")
    print("  python3 tools/audio_only.py /tmp/neos-tap")
    print("Ctrl-C to quit.")

    stop = threading.Event()

    def loop():
        # poll all sources, fan out
        while not stop.is_set():
            try:
                r, _, _ = select.select([ser.fileno(), shell_m, tap_m], [], [], 0.1)
            except (OSError, ValueError):
                return
            for fd in r:
                if fd == ser.fileno():
                    data = ser.read(256)
                    if not data:
                        continue
                    # fan out to BOTH ptys
                    try: os.write(shell_m, data)
                    except OSError: pass
                    try: os.write(tap_m, data)
                    except OSError: pass
                elif fd == shell_m:
                    try:
                        data = os.read(shell_m, 256)
                    except OSError:
                        continue
                    if data:
                        ser.write(data)
                elif fd == tap_m:
                    # discard anything the tap side might write
                    try:
                        os.read(tap_m, 256)
                    except OSError:
                        pass

    t = threading.Thread(target=loop, daemon=True)
    t.start()

    try:
        t.join()
    except KeyboardInterrupt:
        stop.set()
    finally:
        ser.close()
        for fd in (shell_m, shell_s, tap_m, tap_s):
            try: os.close(fd)
            except OSError: pass
        for link in (SHELL_LINK, TAP_LINK):
            try: os.unlink(link)
            except FileNotFoundError: pass
        print("\n[serial_mux] bye")


if __name__ == "__main__":
    main()
