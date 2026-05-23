#!/usr/bin/env python3
"""Upload an app .bin to picorv32 bootloader over UART."""

import argparse
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed. Run: sudo apt install python3-serial")


def open_port(port, baud):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.bytesize = serial.EIGHTBITS
    ser.parity   = serial.PARITY_NONE
    ser.stopbits = serial.STOPBITS_ONE
    ser.timeout  = 0.1
    ser.xonxoff  = False
    ser.rtscts   = False
    ser.dsrdtr   = False
    # Don't toggle modem control lines — some adapters route DTR to a reset pin.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def read_for(ser, seconds):
    """Read everything available for `seconds` and return the bytes."""
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.005)
    return buf


def wait_for(ser, needle, timeout):
    """Read until `needle` (bytes) appears in the stream, or timeout. Return full buffer."""
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if needle in buf:
                return buf
        else:
            time.sleep(0.005)
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("port", nargs="?", default="/dev/ttyUSB1")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("-v", "--verbose", action="store_true", help="dump every byte from bootloader")
    args = ap.parse_args()

    bin_path = Path(args.binary)
    payload = bin_path.read_bytes()
    if not payload:
        sys.exit("empty binary")

    APP_MAX = 16 * 1024
    if len(payload) > APP_MAX:
        sys.exit(f"binary too big: {len(payload)} > {APP_MAX}")

    chksum = sum(payload) & 0xFFFFFFFF
    print(f"[*] {bin_path.name}: {len(payload)} bytes, sum=0x{chksum:08x}")
    print(f"[*] opening {args.port} @ {args.baud} 8N1 (no DTR/RTS)")

    ser = open_port(args.port, args.baud)

    # Drain any pending bytes (boot banner, leftover, etc.)
    initial = read_for(ser, 0.3)
    if args.verbose:
        print(f"[v] drained: {initial!r}")

    # Try a few times: send 'u', wait for "sz?".
    sz_resp = b""
    for attempt in range(1, 6):
        print(f"[*] attempt {attempt}: sending 'u'")
        ser.write(b"u")
        ser.flush()
        sz_resp = wait_for(ser, b"sz?", timeout=1.5)
        if b"sz?" in sz_resp:
            break
        # If we got "BOOT>" instead, bootloader is at prompt — retry directly.
        if args.verbose:
            print(f"[v] response after 'u': {sz_resp!r}")
        # Nudge with a newline (any unknown char re-prints BOOT> ) and retry
        ser.write(b"\n")
        time.sleep(0.1)
        ser.reset_input_buffer()
    else:
        sys.exit(
            "bootloader did not respond with 'sz?'.\n"
            "Hint: press the reset button (S1) — you should see 'BOOT>' in picocom — "
            "then close picocom and re-run upload."
        )

    if args.verbose:
        print(f"[v] sz response: {sz_resp!r}")

    print(f"[*] sending size={len(payload)}")
    ser.write(len(payload).to_bytes(4, "little"))
    ser.flush()

    ack = wait_for(ser, b"\n", timeout=2.0)
    print(f"[+] bootloader: {ack.decode(errors='replace').strip()}")
    if b"ok" not in ack:
        sys.exit(f"bootloader rejected size: {ack!r}")

    print(f"[*] sending {len(payload)} bytes payload + checksum")
    CHUNK = 64
    for i in range(0, len(payload), CHUNK):
        ser.write(payload[i:i + CHUNK])
        ser.flush()
    ser.write(chksum.to_bytes(4, "little"))
    ser.flush()

    final = wait_for(ser, b"\n", timeout=5.0)
    line = final.decode(errors="replace").strip()
    print(f"[+] bootloader: {line}")
    if "OK" in line:
        print(f"\n[*] app is running. Open picocom to talk to it:")
        print(f"      picocom -b {args.baud} {args.port}")
    else:
        sys.exit("upload failed")


if __name__ == "__main__":
    main()
