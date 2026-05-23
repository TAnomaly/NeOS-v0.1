#!/usr/bin/env python3
"""Convert raw binary to 32-bit-word $readmemh hex file."""
import sys

if len(sys.argv) != 4:
    print("usage: bin2hex.py <in.bin> <out.hex> <num_words>", file=sys.stderr)
    sys.exit(1)

in_path, out_path, n_words = sys.argv[1], sys.argv[2], int(sys.argv[3])

with open(in_path, "rb") as f:
    data = f.read()

if len(data) % 4 != 0:
    data += b"\x00" * (4 - len(data) % 4)

words = [int.from_bytes(data[i:i+4], "little") for i in range(0, len(data), 4)]
while len(words) < n_words:
    words.append(0)
if len(words) > n_words:
    raise SystemExit(f"firmware too big: {len(words)} > {n_words} words")

with open(out_path, "w") as f:
    for w in words:
        f.write(f"{w:08x}\n")
