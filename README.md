# NeOS — A tiny OS on Tang Nano 9K

A from-scratch RISC-V mini operating system running on a $27 FPGA board,
with HDMI output, a serial shell, an interpreter, **and a real on-chip
C compiler that emits native RV32IM machine code**.

Built end-to-end:

- **picorv32** soft RISC-V CPU (RV32IMC) instantiated in Verilog
- **Bidirectional UART** + **6 LEDs** + **640×480 HDMI text terminal**
- **UART bootloader** → 3-second iteration loop (no bitstream rebuild)
- **NeOS shell** with REPL, line editor, mini-C interpreter, mini-C compiler
- A handful of fun demos (Mandelbrot, Pong AI, Matrix rain, Hangman, ...)

100% open-source toolchain (Yosys + nextpnr-himbaechel + gowin_pack +
openFPGALoader). No vendor IDE required.

> See [`JOURNEY.md`](JOURNEY.md) for the full step-by-step story of how
> this was built from nothing.

---

## Hardware

- [Sipeed Tang Nano 9K](https://wiki.sipeed.com/hardware/en/tang/tang-nano-9k/nano-9k.html)
  (Gowin GW1NR-LV9 QN88, ~8.8k LUT, 468 Kbit BRAM, on-die HyperRAM, HDMI, USB-C, 6 LEDs, S1/S2 buttons)
- HDMI cable + monitor (for visual output)
- USB-C cable (powers the board + provides UART/JTAG)

## Software dependencies

- [oss-cad-suite](https://github.com/YosysHQ/oss-cad-suite-build) (Yosys,
  nextpnr-himbaechel, gowin_pack, openFPGALoader bundled)
- `riscv64-unknown-elf-gcc` (`sudo apt install gcc-riscv64-unknown-elf` on Ubuntu)
- `picocom` (or any serial terminal at 115200 8N1)
- Python 3 + `pyserial` (for the UART uploader)

---

## Quick start

```bash
# 1. Build firmware (gcc + bin2hex)
make -C firmware

# 2. Copy hex into the path the BRAM init reads
cp firmware/firmware.hex picorv32/firmware.hex

# 3. Build bitstream (synthesize + place&route + pack)
source ~/oss-cad-suite/environment
make -f Makefile.oss

# 4. Flash to the board (persistent — survives power cycle)
make -f Makefile.oss flash

# 5. Open serial terminal
picocom -b 115200 /dev/ttyUSB1

# 6. Press S1 (reset). You should see the NeOS splash on both the
#    terminal and the HDMI monitor.
```

After the first flash, *all* further C development uses the UART
bootloader — no bitstream rebuild needed:

```bash
# Edit firmware/main.c, then:
./upload_app.sh        # ~3 seconds: gcc → UART upload → jump
```

---

## NeOS shell

After reset you get the splash screen and a `NeOS> ` prompt
(mirrored to both UART and HDMI):

```
  _   _        ___    ____
 | \ | |  ___ / _ \  / ___|
 |  \| | / _ \ | | | \___ \
 | |\  ||  __/ |_| |  ___) |
 |_| \_| \___|\___/  |____/

       v0.3  picorv32 / Tang Nano 9K
       32K BRAM  /  RV32IMC  /  HDMI
       type 'help' for commands

NeOS>
```

### Built-in commands

| Command | Description |
|---------|-------------|
| `help` | List every command |
| `clear` / `cls` | Clear screens |
| `info` | System info (RAM, peripherals, clock) |
| `ascii` | Print ASCII table |
| `led <hex>` | Set LED pattern (low 6 bits) |
| `peek <addr>` | Read a 32-bit word |
| `poke <addr> <val>` | Write a 32-bit word |
| `dump <addr> <len>` | Hex + ASCII dump |
| `hex <dec>` / `mul <a> <b>` | Number helpers |
| `u` | UART bootloader upload mode |
| `g` / `run` | Jump to uploaded app slot |
| `mandel` | ASCII Mandelbrot fractal |
| `matrix` | Matrix-rain animation (any key to stop) |
| `pong` | AI-vs-AI text Pong |
| `guess` | 1-100 number guess game |
| `hangman` | Word-guess game |
| `cc <C source>` | **Compile + run native RV32IM** (see below) |

### Mini-C interpreter

C-syntax expressions / statements interpreted in real-time:

```
NeOS> let x = 7*13
x = 91
NeOS> print x*x
8281
NeOS> printh x
0x0000005B
NeOS> led(0x2A)
led=2a
NeOS> print "Wake up, Neo"
Wake up, Neo
```

Operators: `+ - * / % & | ^ << >> < <= > >= == != && || ! ~`
Builtins: `led(v)`, `peek(a)`, `poke(a,v)`, `delay(ms)`, `read()`,
`write(b)`, `tone(hz)`.

### Mini-C compiler (`cc`)

This is the most fun part. You type C source, NeOS **tokenizes it,
parses it, and emits RV32IM machine code straight into RAM**, then
jumps to it. The code runs at native CPU speed.

```
NeOS> cc print(7*13);
[cc] 14 instr
91
[cc] done

NeOS> cc int x=0; while (x<6) { led(1<<x); delay(150); x=x+1; }
[cc] 38 instr
(LEDs sweep across, 1-2 seconds)
[cc] done

NeOS> cc int n=5; if (n>3) puts("big"); else puts("small");
[cc] 30 instr
big
[cc] done
```

Supported subset: `int` variables, `if/else/while`, arithmetic +
comparison + bitwise + shifts, builtin calls (`led`, `delay`, `peek`,
`poke`, `print`, `puts`, `getc`), string literals (in `puts` only),
hex/decimal numeric literals, single-letter variable names (`a-z, A-Z`).

The compiler is ~800 lines of C in [`bootloader/cc.c`](bootloader/cc.c).
It emits RV32IM directly — no intermediate bytecode, no VM.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  top.v   (Tang Nano 9K pinout)                           │
│                                                          │
│  ┌──────────┐   ┌──────────┐   ┌─────────────────────┐  │
│  │ picorv32 │──▶│  soc.v   │──▶│ svo_hdmi_top        │  │
│  │ (RV32IMC)│   │ addr dec │   │  text terminal      │  │
│  └──────────┘   │ mem mux  │   │  640×480 @ 60 Hz    │  │
│        │        └──────────┘   │  black bg / green fg│  │
│        ▼              │        └─────────┬───────────┘  │
│   ┌─────────┐   ┌────▼─────┐  ┌───────┐ │              │
│   │ ram.v   │   │ uart_tx  │  │  led  │ │              │
│   │ 32 KB   │   │ uart_rx  │  │ 6 bit │ │              │
│   │ BRAM    │   │ 115200   │  └───────┘ │              │
│   └─────────┘   └──────────┘             ▼              │
│                                  TMDS encoder + OSER10  │
│                                  + ELVDS_OBUF → HDMI    │
└──────────────────────────────────────────────────────────┘
```

### Memory map

| Range | Use |
|-------|-----|
| `0x00000000-0x00005FFF` | Bootloader / NeOS / interpreter / compiler (24 KB) |
| `0x00006000-0x0000607F` | `cc` compiled-code variables (32 ints) |
| `0x00006080-0x000060FF` | `cc` syscall function-pointer table |
| `0x00006100-0x00006FFF` | `cc` compiled-code buffer (~960 instructions) |
| `0x00007000-0x00007FFF` | Legacy `u` upload slot |
| `0x10000000` | UART_TX_DATA |
| `0x10000004` | UART_STATUS |
| `0x10000008` | UART_RX_DATA |
| `0x10000010` | LED |
| `0x10000020` | TERM_DATA (HDMI text byte stream) |
| `0x10000024` | TERM_STATUS |

---

## Repository layout

```
.
├── src/                    # Open-source flow (Yosys + nextpnr) sources
│   ├── top.v               # Top-level (HDMI + SoC + PLL + CLKDIV)
│   ├── soc.v               # CPU + RAM + UART + LED + term mem map
│   ├── ram.v               # 32 KB BRAM, $readmemh firmware.hex
│   ├── picorv32.v          # picorv32 (Clifford Wolf)
│   ├── uart_tx.v           # UART transmitter
│   ├── uart_rx.v           # UART receiver (2-FF sync, 8N1)
│   ├── hdmi/               # SVO HDMI text-terminal framework
│   │   ├── svo_term.v
│   │   ├── svo_hdmi_top.v
│   │   └── ...
│   ├── ip/                 # Gowin rPLL + CLKDIV IP wrappers
│   └── hyperram.v          # Unused — open PSRAM controller skeleton
│
├── picorv32/               # Gowin IDE flow mirror (not the active build)
│
├── bootloader/             # NeOS bootloader + shell + interpreter + compiler
│   ├── main.c              # Splash, REPL, commands, demos
│   ├── interp.c            # Mini-C expression interpreter
│   ├── cc.c                # Mini-C → RV32IM compiler
│   ├── start.S             # _start, sp init, BSS clear, call main
│   ├── linker.ld           # 0x0000-0x5FFF, stack top 0x6000
│   └── Makefile
│
├── firmware/               # Optional uploadable app (lives at 0x7000)
│   ├── main.c
│   ├── start.S
│   ├── linker.ld           # 0x7000-0x7FFF
│   └── Makefile
│
├── tools/
│   ├── bin2hex.py          # raw .bin → 32-bit-per-word $readmemh hex
│   └── upload.py           # UART bootloader uploader (host-side)
│
├── Makefile.oss            # Open-source build: yosys / nextpnr / gowin_pack
├── tangnano9k.cst          # Pin constraints (clk, rst, UART, LED, HDMI)
├── build_and_flash.sh      # Convenience: bake bootloader into bitstream + flash
├── upload_app.sh           # Convenience: gcc + UART upload (~3s loop)
├── JOURNEY.md              # Full from-scratch build story (read this!)
└── README.md
```

---

## Development loop

After the bootloader is baked into the bitstream (once per Verilog change):

```bash
# 1. Edit firmware/main.c
nano firmware/main.c

# 2. Build + upload over UART (gcc + bin2hex + UART payload)
./upload_app.sh

# 3. Watch output in your serial terminal (and on the HDMI monitor)
picocom -b 115200 /dev/ttyUSB1
```

Or work entirely inside NeOS using the `cc` compiler — no PC compilation,
no upload:

```
NeOS> cc int i=0; while (i<5) { print(i*i); delay(300); i=i+1; }
```

---

## What's *not* in here

- **HDMI audio** — attempted; data-island packet generation was too
  complex to debug without an HDMI analyzer. Audio path stubbed but
  inactive.
- **On-die HyperRAM (8 MB PSRAM)** — attempted; hardware bank conflict
  on Tang Nano 9K (HDMI pins force 3.3 V on Bank 1, HyperRAM forces
  1.8 V on the same bank, electrically incompatible). Would need
  Tang Nano 20K or an external SPI PSRAM module.
- **Full gcc/TinyCC** — TinyCC needs ~10 MB RAM; we have 32 KB BRAM.
  Hence the custom `cc` compiler instead.

---

## License

picorv32 and SVO are © Clifford Wolf, ISC license. The rest of the
code here is original and provided under the same ISC license unless
noted otherwise.
