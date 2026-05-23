# Design: Bidirectional UART + SoC Feature Expansion

Date: 2026-05-23
Target: Tang Nano 9K (GW1NR-LV9 QN88), picorv32 SoC
Scope: Make CPU-driven UART TX work, add UART RX, expand RAM to 32 KB, enable MUL and Compressed ISA in picorv32, ship demo C firmware.

---

## 1. Motivation

Current state (verified by reading `src/soc.v`, `src/uart_tx.v`, `firmware/main.c`):

- picorv32 boots, runs C, LEDs writable from CPU.
- UART TX module is correct, but in `soc.v:82-98` it is wired to a debug counter (`dbg_cnt[18]`) and a hardcoded data byte `8'h41` ('A'). CPU writes to `0x10000000` do not reach the UART. The pin sends a fixed 'A' stream.
- No UART RX path exists.
- BRAM is 8 KB. Firmware is small.
- picorv32 has `ENABLE_MUL=0` and `COMPRESSED_ISA=0`, so `-march=rv32i` is required and `*` / `/` need software emulation.
- Firmware lives in BRAM, initialized at synthesis via `$readmemh` of `firmware.hex`. Any C change requires full bitstream rebuild. (Bootloader is out of scope for this spec.)

Goals:

1. Fix UART TX so the CPU actually drives the data.
2. Add UART RX so C code can read serial input.
3. Grow RAM 8 KB → 32 KB.
4. Enable `MUL` and `COMPRESSED_ISA` in picorv32.
5. Replace `main.c` with a demo that proves all of the above (echo + multiply).

Non-goals:

- No interrupts. Polling only. (`ENABLE_IRQ` stays 0.)
- No division enable. (`ENABLE_DIV` stays 0.)
- No RX FIFO. Single-byte latch with overrun-on-write semantics.
- No bootloader / live firmware reload. Bitstream rebuild per firmware change.
- No SPI flash, no XIP.

---

## 2. Architecture

```
top
 └── soc
      ├── picorv32   (RV32IMC, ENABLE_MUL=1, COMPRESSED_ISA=1)
      ├── ram        (32 KB BRAM, byte-write, $readmemh firmware.hex)
      ├── uart_tx    (existing module, now wired to CPU)
      ├── uart_rx    (new module, 2-FF sync, 16x oversample-style mid-bit sample)
      └── led        (6-bit register, write-only, active-low to pins)
```

External pins (Tang Nano 9K):

| Signal    | Pin | CST IOLOC |
|-----------|-----|-----------|
| clk 27 MHz | 52 | (existing) |
| rst_n      |  4 | (existing) IOT2A |
| uart_tx    | 17 | (existing) IOB2A |
| uart_rx    | 18 | NEW: IOB1A, PULL_MODE=UP |
| led[5:0]   | 10,11,13,14,15,16 | (existing) |

The existing CST entry for the clock string contains an escaped name (`"\u.clk_IBUF_O "`) which is suspect but currently builds. Out of scope to clean up.

---

## 3. Memory Map

Word-aligned. Address decode by `mem_addr[31:28]` then sub-decode.

| Address       | R/W | Name           | Notes |
|---------------|-----|----------------|-------|
| `0x00000000`-`0x00007FFF` | R/W | RAM | 32 KB BRAM, code + data + stack |
| `0x10000000`  | W   | UART_TX_DATA   | Byte in `wdata[7:0]`. Drop silently if TX busy. |
| `0x10000004`  | R   | UART_STATUS    | `{30'b0, rx_valid, tx_busy}` |
| `0x10000008`  | R   | UART_RX_DATA   | `{24'b0, byte}`. Read clears `rx_valid`. |
| `0x1000000C`  | -   | reserved       | reads 0 |
| `0x10000010`  | W   | LED            | `~wdata[5:0]` driven to LED pins (active-low on board) |

Decode:

```verilog
sel_ram  = (mem_addr[31:28] == 4'h0);
sel_uart = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h0);
sel_led  = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h1);
// inside UART block, sub-decode on mem_addr[3:2]:
//   2'b00 = TX_DATA, 2'b01 = STATUS, 2'b10 = RX_DATA
```

Stack pointer initialized to `0x00008000` (top of 32 KB RAM, grows down).

---

## 4. UART TX — Wiring Fix

`uart_tx.v` unchanged.

In `soc.v`, replace the debug block (current lines ~82-104):

```verilog
wire        uart_tx_we   = mem_valid && sel_uart && (mem_addr[3:2] == 2'b00)
                          && (mem_wstrb != 4'b0000) && !mem_ready_r;
wire [ 7:0] uart_tx_data = mem_wdata[7:0];
wire        uart_tx_busy;

uart_tx #(.CLK_HZ(27_000_000), .BAUD(115200)) u_tx (
    .clk  (clk),
    .reset(reset),
    .we   (uart_tx_we),
    .data (uart_tx_data),
    .tx   (uart_tx),
    .busy (uart_tx_busy)
);
```

Remove orphans created by this change: `dbg_cnt`, `dbg_we`, `uart_done`. (Karpathy: clean up only your own mess.)

TX overrun behavior: if C code writes `UART_TX_DATA` while `tx_busy=1`, the byte is dropped silently. C side MUST poll `UART_STATUS & 1` first. This is documented in firmware comments and the spec, not enforced in hardware.

---

## 5. UART RX — New Module

New file: `src/uart_rx.v`.

```verilog
module uart_rx #(
    parameter CLK_HZ = 27_000_000,
    parameter BAUD   = 115200
)(
    input  wire       clk,
    input  wire       reset,
    input  wire       rx,            // async pin
    output reg  [7:0] data,          // last received byte
    output reg        valid,         // sticky: 1 once byte ready, cleared by read_ack
    input  wire       read_ack       // 1-cycle pulse to clear valid
);
    localparam DIV  = CLK_HZ / BAUD;
    localparam HALF = DIV / 2;

    // 2-FF synchronizer for async rx pin
    reg rx_s0, rx_s1;
    always @(posedge clk) begin
        rx_s0 <= rx;
        rx_s1 <= rx_s0;
    end
    wire rx_sync = rx_s1;

    reg [15:0] cnt;
    reg [ 3:0] bit_idx;
    reg [ 7:0] shift;
    reg [ 1:0] state;
    localparam S_IDLE=2'd0, S_START=2'd1, S_DATA=2'd2, S_STOP=2'd3;

    always @(posedge clk) begin
        if (reset) begin
            state <= S_IDLE; cnt <= 0; bit_idx <= 0;
            valid <= 1'b0;   data <= 8'h00;
        end else begin
            if (read_ack) valid <= 1'b0;
            case (state)
            S_IDLE:  if (!rx_sync) begin cnt <= 0; state <= S_START; end
            S_START: if (cnt == HALF - 1) begin
                         cnt <= 0;
                         if (!rx_sync) begin state <= S_DATA; bit_idx <= 0; end
                         else            state <= S_IDLE;       // glitch
                     end else cnt <= cnt + 1;
            S_DATA:  if (cnt == DIV - 1) begin
                         cnt <= 0;
                         shift <= {rx_sync, shift[7:1]};
                         if (bit_idx == 7) state <= S_STOP;
                         else              bit_idx <= bit_idx + 1;
                     end else cnt <= cnt + 1;
            S_STOP:  if (cnt == DIV - 1) begin
                         cnt <= 0; state <= S_IDLE;
                         if (rx_sync) begin           // valid stop bit
                             data  <= shift;
                             valid <= 1'b1;
                         end
                     end else cnt <= cnt + 1;
            endcase
        end
    end
endmodule
```

Design notes (why):

- **2-FF sync** is mandatory for an asynchronous input pin to avoid metastability propagating into the FSM.
- **Mid-bit sampling**: wait HALF after start edge, then full DIV per data bit. Standard 8N1 receiver.
- **Sticky `valid`**: cleared only by `read_ack`, so polling code does not have to be cycle-precise.
- **No FIFO**: if a second byte arrives before the CPU reads, `data` is overwritten (and `valid` stays 1). User accepted this in brainstorming (option A over B).
- **Frame error**: if stop bit reads 0, the byte is dropped. No status flag for it.

---

## 6. SoC Integration (`soc.v`)

New port on `soc` and `top`: `input wire uart_rx`.

Add inside `soc`:

```verilog
wire [7:0] uart_rx_data;
wire       uart_rx_valid;
wire       uart_rx_ack = mem_valid && sel_uart && (mem_addr[3:2] == 2'b10)
                        && (mem_wstrb == 4'b0000) && !mem_ready_r;

uart_rx #(.CLK_HZ(27_000_000), .BAUD(115200)) u_rx (
    .clk(clk), .reset(reset), .rx(uart_rx),
    .data(uart_rx_data), .valid(uart_rx_valid),
    .read_ack(uart_rx_ack)
);
```

Update the read mux:

```verilog
assign mem_rdata =
    sel_ram                              ? ram_rdata :
    (sel_uart && mem_addr[3:2] == 2'b00) ? 32'h0 :
    (sel_uart && mem_addr[3:2] == 2'b01) ? {30'b0, uart_rx_valid, uart_tx_busy} :
    (sel_uart && mem_addr[3:2] == 2'b10) ? {24'b0, uart_rx_data} :
    32'h0;
```

`mem_ready_r` logic unchanged (single-cycle ready).

---

## 7. RAM Growth (`src/ram.v`)

```verilog
module ram (
    input  wire        clk,
    input  wire [ 3:0] we,
    input  wire [12:0] addr,    // word address, 8192 words = 32 KB
    input  wire [31:0] wdata,
    output reg  [31:0] rdata
);
    reg [31:0] mem [0:8191];
    initial $readmemh("/home/tugmirk/tangnano9k_picorv32/picorv32/firmware.hex", mem);

    always @(posedge clk) begin
        if (we[0]) mem[addr][ 7: 0] <= wdata[ 7: 0];
        if (we[1]) mem[addr][15: 8] <= wdata[15: 8];
        if (we[2]) mem[addr][23:16] <= wdata[23:16];
        if (we[3]) mem[addr][31:24] <= wdata[31:24];
        rdata <= mem[addr];
    end
endmodule
```

In `soc.v` instance: `.addr(mem_addr[14:2])`.

BRAM budget: GW1N-9C family has 468 Kbit (~58 KB) of B-SRAM split across many 16 Kbit blocks. 32 KB fits comfortably; exact block count is decided by the synthesizer.

---

## 8. picorv32 Parameter Changes

In `soc.v` picorv32 instantiation:

| Param            | Old  | New             |
|------------------|------|-----------------|
| `ENABLE_MUL`     | 0    | **1**           |
| `COMPRESSED_ISA` | 0    | **1**           |
| `STACKADDR`      | `0x00002000` | **`0x00008000`** |

All other params unchanged (IRQ off, DIV off, counters off, etc.).

---

## 9. Toolchain Changes

`firmware/Makefile`:

```
CFLAGS = -march=rv32imc -mabi=ilp32 -Os -ffreestanding -nostdlib -Wall
...
$(OUT).hex: $(OUT).bin
	python3 ../tools/bin2hex.py $< $@ 8192
```

`firmware/linker.ld`:

```
MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
}
...
_stack_top = ORIGIN(RAM) + LENGTH(RAM);
```

`firmware/start.S`: add BSS clear. `tools/bin2hex.py` already zero-pads the hex up to the requested word count, so BRAM starts zeroed today; the BSS clear is defensive (independent of toolchain behavior, matches standard C startup, costs ~10 instructions):

```asm
.section .text.init
.globl _start
_start:
    la   sp, _stack_top
    la   t0, _bss_start
    la   t1, _bss_end
1:  bge  t0, t1, 2f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    1b
2:  call main
3:  j    3b
```

---

## 10. Demo Firmware (`firmware/main.c`)

```c
#include <stdint.h>

#define UART_TX_DATA  (*(volatile uint32_t *)0x10000000)
#define UART_STATUS   (*(volatile uint32_t *)0x10000004)
#define UART_RX_DATA  (*(volatile uint32_t *)0x10000008)
#define LED_REG       (*(volatile uint32_t *)0x10000010)

#define ST_TX_BUSY    (1u << 0)
#define ST_RX_VALID   (1u << 1)

static void uart_putc(char c) {
    while (UART_STATUS & ST_TX_BUSY) ;
    UART_TX_DATA = (uint32_t)(uint8_t)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

static int uart_getc_nb(void) {
    if (!(UART_STATUS & ST_RX_VALID)) return -1;
    return (int)(UART_RX_DATA & 0xFF);
}

int main(void) {
    uint32_t led = 0;
    LED_REG = led;

    uart_puts("\r\npicorv32 ready. echo + MUL demo.\r\n");

    uint32_t a = 7, b = 13;
    uint32_t product = a * b;          // proves ENABLE_MUL
    uart_puts("7*13=");
    uart_putc('0' + (product / 100) % 10);
    uart_putc('0' + (product / 10)  % 10);
    uart_putc('0' +  product        % 10);
    uart_puts("\r\n> ");

    for (;;) {
        int c = uart_getc_nb();
        if (c >= 0) {
            uart_putc((char)c);
            if (c == '\r') uart_putc('\n');
            led = (led + 1) & 0x3F;
            LED_REG = led;
        }
    }
}
```

Note: `/` and `%` by constants in the multiply demo: `gcc -Os` will turn divides by literal `10` and `100` into multiplication tricks, so `ENABLE_DIV=0` stays fine.

---

## 11. File Change Summary

**Dual-copy note.** The repo carries two identical sets of Verilog sources:

- `src/` is consumed by `Makefile.oss` (open-source flow: yosys + nextpnr + gowin_pack).
- `picorv32/` is the Gowin IDE project (`picorv32.gprj`).

The two sets are kept in sync manually and currently match byte-for-byte. Every Verilog and CST change in this spec MUST be applied to both copies, otherwise the two flows will diverge. `uart_rx.v` (new file) and Verilog edits go in both `src/` and `picorv32/`. The Gowin IDE project file (`picorv32/picorv32.gprj`) must also be updated to include the new `uart_rx.v` source.

| File | Change |
|------|--------|
| `src/top.v` **and** `picorv32/top.v` | Add `input wire uart_rx` port, plumb to soc instance |
| `src/soc.v` **and** `picorv32/soc.v` | Add `uart_rx` port, replace debug TX block with CPU-driven TX, instantiate `uart_rx`, update read mux, update picorv32 params, fix ram addr width |
| `src/ram.v` **and** `picorv32/ram.v` | 2048→8192 words, addr `[12:2]`→`[14:2]` (width 13) |
| `src/uart_rx.v` **and** `picorv32/uart_rx.v` | NEW file (module above) |
| `src/uart_tx.v` / `picorv32/uart_tx.v` | Unchanged |
| `tangnano9k.cst` **and** `picorv32/tangnano9k.cst` | Add `IO_LOC "uart_rx" IOB1A;` and `IO_PORT "uart_rx" PULL_MODE=UP;` |
| `picorv32/picorv32.gprj` | Register `uart_rx.v` as a project source (Gowin IDE file list) |
| `Makefile.oss` | Add `$(SRC_DIR)/uart_rx.v` to `SOURCES` |
| `firmware/Makefile` | `-march=rv32imc`, bin2hex word count `2048`→`8192` |
| `firmware/linker.ld` | `LENGTH = 32K` |
| `firmware/start.S` | Add BSS clear |
| `firmware/main.c` | Replace with echo + MUL demo |

**Out of scope cleanup (flagged for later):** the `ram.v` `$readmemh` path is hardcoded to `/home/tugmirk/tangnano9k_picorv32/picorv32/firmware.hex`. Both flows currently read the hex from the `picorv32/` directory regardless of which flow built it. `firmware/Makefile` writes to `firmware/firmware.hex`, so the hex must be copied (or symlinked) to `picorv32/firmware.hex` before synthesis. The plan should include this copy step in the build instructions but not refactor the path.

---

## 12. Test Plan

1. `cd firmware && make` — must produce `firmware/firmware.hex` with no errors.
   - Copy hex into the path the synth flows read: `cp firmware/firmware.hex picorv32/firmware.hex`.
2. `make -f Makefile.oss` from project root — synthesis + PnR + pack must succeed.
   - LUT/BRAM usage should stay well under GW1NR-LV9 limits.
3. `make -f Makefile.oss flash` — write bitstream to board flash.
4. Open a serial terminal at `/dev/ttyUSB1` (or wherever the BL702 bridge enumerates) at `115200 8N1, no flow control`.
5. Press the reset button.
   - Expected: `picorv32 ready. echo + MUL demo.\r\n7*13=091\r\n> `
   - If the line appears: TX wiring fix works, MUL works, COMPRESSED build works.
6. Type characters in the terminal.
   - Expected: each char echoes back; CR also emits LF; LED bar advances.
   - If echo works: RX module + CST RX pin + read-mux all correct.

Diagnostic decision tree:

- No output at all → TX wiring (section 4) wrong, or CLK_HZ mismatch in `uart_tx` instance, or board not flashed.
- Output appears but `7*13=` shows wrong number → MUL or COMPRESSED toolchain mismatch; check `ENABLE_MUL` param matches `-march=rv32imc`.
- Output appears, no echo → RX pin (IOB1A wrong for this board revision), or `uart_rx` module bug, or terminal not sending.
- Garbage instead of clean text → baud mismatch; verify both ends at 115200 and FPGA clock is truly 27 MHz.

---

## 13. Risks and Assumptions

- **Tang Nano 9K UART RX pin is IOB1A (pin 18).** Verified against common pinout references but not against this specific board revision. If it does not work, the next candidates are IOB1B and IOR16A — check the board schematic.
- **27 MHz clock.** Assumed from existing code. If the on-board oscillator is different (some Tang Nano 9K variants use 25 MHz), `CLK_HZ` parameters must match or baud rate will be off and all UART will be garbage.
- **`$readmemh` absolute path** in `ram.v` is preserved as-is. Cleanup is out of scope.
- **No bootloader.** Every firmware change requires `make -f Makefile.oss flash`. If iteration time becomes painful, a separate spec for a UART bootloader is the natural next step.
- **No interrupts.** Polling is acceptable at 115200 baud with this CPU. If a future workload needs concurrent UART + computation, `ENABLE_IRQ=1` and a vector table will be required (separate spec).
