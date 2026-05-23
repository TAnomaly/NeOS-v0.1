# Bidirectional UART + SoC Feature Expansion — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the picorv32 SoC's UART CPU-driven in both directions, grow RAM to 32 KB, enable MUL + Compressed ISA, and ship a demo C firmware that echoes serial input.

**Architecture:** Single-spec change touching the open-source build (`src/` + `Makefile.oss`) and the Gowin IDE project (`picorv32/` + `picorv32.gprj`). All Verilog edits are mirrored across both copies. New module `uart_rx.v` is unit-tested with iverilog before integration. Integration is verified end-to-end on hardware.

**Tech Stack:** Verilog (yosys + nextpnr-himbaechel + gowin_pack), picorv32 RV32IMC, `riscv64-unknown-elf-gcc`, openFPGALoader, optional iverilog for simulation.

**Spec:** [`docs/superpowers/specs/2026-05-23-uart-bidir-and-soc-features-design.md`](../specs/2026-05-23-uart-bidir-and-soc-features-design.md)

**Working dir:** `/home/tugmirk/tangnano9k_picorv32`

---

## Pre-flight Notes

- The repo is **not** a git repository at plan-writing time. Task 0 initializes one. If the user has already initialized git, skip Task 0.
- Verilog sources exist in two synced copies: `src/*.v` and `picorv32/*.v`. Every Verilog edit must be applied to both.
- `iverilog` may not be installed. If absent, Task 1's simulation steps are skipped; the new module is then verified only by Task 8 (hardware echo). The plan flags this explicitly.
- All shell commands assume the working dir `/home/tugmirk/tangnano9k_picorv32` unless stated otherwise.

---

## Task 0: Initialize Git (Baseline)

**Files:**
- Create: `.gitignore`
- New repo at project root

- [ ] **Step 1: Check if git repo already exists**

Run: `git rev-parse --is-inside-work-tree 2>/dev/null || echo "NO_REPO"`
- If output is `true`: skip the rest of Task 0.
- If output is `NO_REPO`: continue.

- [ ] **Step 2: Create `.gitignore`**

Write file `.gitignore` with these contents:
```
build_oss/
build_raw/
build_test/
firmware/firmware.elf
firmware/firmware.bin
firmware/firmware.hex
picorv32/firmware.hex
picorv32/impl/
*.bak
__pycache__/
.vscode/
```

- [ ] **Step 3: Init + initial commit**

Run:
```bash
git init
git add .
git commit -m "chore: baseline before bidir UART + SoC feature expansion"
```

Expected: `Initialized empty Git repository ...` then a commit hash printed.

---

## Task 1: New `uart_rx.v` Module (TDD)

**Files:**
- Create: `src/uart_rx.v`
- Create: `picorv32/uart_rx.v` (copy of `src/`)
- Create: `sim/tb_uart_rx.v` (testbench; optional, only if iverilog present)

- [ ] **Step 1: Detect iverilog**

Run: `command -v iverilog && command -v vvp || echo "NO_SIM"`
- If `NO_SIM`: skip simulation steps (3, 4, 5) and proceed to Step 6 (implementation).
- Otherwise: do all steps.

- [ ] **Step 2: Make sim dir**

Run: `mkdir -p sim`

- [ ] **Step 3: Write failing testbench `sim/tb_uart_rx.v`**

```verilog
`timescale 1ns/1ps
module tb_uart_rx;
    // 27 MHz clock, 115200 baud
    localparam CLK_HZ = 27_000_000;
    localparam BAUD   = 115200;
    localparam BIT_NS = 1_000_000_000 / BAUD;   // ~8681 ns per bit

    reg        clk = 0;
    reg        reset = 1;
    reg        rx_pin = 1;
    wire [7:0] data;
    wire       valid;
    reg        read_ack = 0;

    always #18 clk = ~clk;       // ~27.78 MHz, close enough

    uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) dut (
        .clk(clk), .reset(reset), .rx(rx_pin),
        .data(data), .valid(valid), .read_ack(read_ack)
    );

    // Send one byte over rx_pin at 115200 baud, 8N1
    task send_byte(input [7:0] b);
        integer i;
        begin
            rx_pin = 0;                  #(BIT_NS);   // start
            for (i = 0; i < 8; i = i + 1) begin
                rx_pin = b[i];           #(BIT_NS);   // data lsb first
            end
            rx_pin = 1;                  #(BIT_NS);   // stop
        end
    endtask

    integer errors = 0;
    initial begin
        $dumpfile("sim/tb_uart_rx.vcd");
        $dumpvars(0, tb_uart_rx);

        #200 reset = 0;
        #1000;

        send_byte(8'h41);    // 'A'
        #(BIT_NS);

        if (!valid)        begin $display("FAIL: valid not asserted after byte"); errors = errors + 1; end
        if (data !== 8'h41) begin $display("FAIL: data=%h expected 41", data);    errors = errors + 1; end

        // Clear valid via read_ack
        @(posedge clk) read_ack = 1;
        @(posedge clk) read_ack = 0;
        #100;
        if (valid)         begin $display("FAIL: valid did not clear");           errors = errors + 1; end

        // Second byte
        send_byte(8'h5A);    // 'Z'
        #(BIT_NS);
        if (!valid)        begin $display("FAIL: valid not reasserted");          errors = errors + 1; end
        if (data !== 8'h5A) begin $display("FAIL: data=%h expected 5A", data);    errors = errors + 1; end

        if (errors == 0) $display("PASS");
        else             $display("FAILED with %0d errors", errors);
        $finish;
    end
endmodule
```

- [ ] **Step 4: Run the testbench — expect compile/link failure (no `uart_rx.v` yet)**

Run:
```bash
iverilog -o sim/tb_uart_rx.vvp sim/tb_uart_rx.v src/uart_rx.v 2>&1
```
Expected: error mentioning that `src/uart_rx.v` does not exist, or that `uart_rx` module is undefined. This proves the test is wired correctly.

- [ ] **Step 5: (After Step 6) Re-run testbench — expect PASS**

(Returns here once `src/uart_rx.v` exists. Run:)
```bash
iverilog -o sim/tb_uart_rx.vvp sim/tb_uart_rx.v src/uart_rx.v
vvp sim/tb_uart_rx.vvp
```
Expected last line: `PASS`.

- [ ] **Step 6: Write `src/uart_rx.v`**

```verilog
module uart_rx #(
    parameter CLK_HZ = 27_000_000,
    parameter BAUD   = 115200
)(
    input  wire       clk,
    input  wire       reset,
    input  wire       rx,
    output reg  [7:0] data,
    output reg        valid,
    input  wire       read_ack
);
    localparam DIV  = CLK_HZ / BAUD;
    localparam HALF = DIV / 2;

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
                         else            state <= S_IDLE;
                     end else cnt <= cnt + 1;
            S_DATA:  if (cnt == DIV - 1) begin
                         cnt <= 0;
                         shift <= {rx_sync, shift[7:1]};
                         if (bit_idx == 7) state <= S_STOP;
                         else              bit_idx <= bit_idx + 1;
                     end else cnt <= cnt + 1;
            S_STOP:  if (cnt == DIV - 1) begin
                         cnt <= 0; state <= S_IDLE;
                         if (rx_sync) begin
                             data  <= shift;
                             valid <= 1'b1;
                         end
                     end else cnt <= cnt + 1;
            endcase
        end
    end
endmodule
```

- [ ] **Step 7: Re-run testbench (if iverilog present)**

Go back to Step 5. Expected: `PASS`.

- [ ] **Step 8: Mirror to `picorv32/uart_rx.v`**

Run:
```bash
cp src/uart_rx.v picorv32/uart_rx.v
```

- [ ] **Step 9: Sanity-diff the two copies**

Run: `diff src/uart_rx.v picorv32/uart_rx.v`
Expected: no output (files identical).

- [ ] **Step 10: Commit**

```bash
git add src/uart_rx.v picorv32/uart_rx.v sim/ 2>/dev/null
git commit -m "feat(soc): add uart_rx module (8N1, 2FF sync, sticky valid)"
```

---

## Task 2: Grow RAM to 32 KB

**Files:**
- Modify: `src/ram.v`
- Modify: `picorv32/ram.v`
- Modify: `firmware/linker.ld`
- Modify: `firmware/Makefile` (bin2hex word count only)

- [ ] **Step 1: Edit `src/ram.v` — change memory size and address width**

Replace the file contents with:
```verilog
module ram (
    input  wire        clk,
    input  wire [ 3:0] we,
    input  wire [12:0] addr,   // word address, 8192 words = 32 KB
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

- [ ] **Step 2: Mirror to `picorv32/ram.v`**

Run: `cp src/ram.v picorv32/ram.v`

- [ ] **Step 3: Verify both copies match**

Run: `diff src/ram.v picorv32/ram.v`
Expected: no output.

- [ ] **Step 4: Edit `firmware/linker.ld`**

Change the `MEMORY` block — replace:
```
RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 8K
```
with:
```
RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
```
Leave the rest of the file unchanged. `_stack_top = ORIGIN(RAM) + LENGTH(RAM)` automatically becomes `0x00008000`.

- [ ] **Step 5: Edit `firmware/Makefile` — bin2hex word count**

Change the line:
```
	python3 ../tools/bin2hex.py $< $@ 2048
```
to:
```
	python3 ../tools/bin2hex.py $< $@ 8192
```

(CFLAGS edits happen in Task 4 — don't touch them here.)

- [ ] **Step 6: Commit**

```bash
git add src/ram.v picorv32/ram.v firmware/linker.ld firmware/Makefile
git commit -m "feat(ram): grow BRAM 8K to 32K, update linker and hex gen"
```

---

## Task 3: SoC Wiring — TX Fix, RX Add, picorv32 Params, RAM Addr Width

**Files:**
- Modify: `src/soc.v`
- Modify: `picorv32/soc.v`

This is the largest single edit. Take it in one shot; both copies get the same final content.

- [ ] **Step 1: Replace `src/soc.v` with the new full file**

Replace the entire contents of `src/soc.v` with:

```verilog
module soc (
    input  wire       clk,
    input  wire       reset,
    input  wire       uart_rx,
    output wire       uart_tx,
    output reg  [5:0] led
);

    // PicoRV32 mem interface
    wire        mem_valid;
    wire        mem_instr;
    wire        mem_ready;
    wire [31:0] mem_addr;
    wire [31:0] mem_wdata;
    wire [ 3:0] mem_wstrb;
    wire [31:0] mem_rdata;
    wire        mem_la_read;
    wire        mem_la_write;
    wire [31:0] mem_la_addr;
    wire [31:0] mem_la_wdata;
    wire [ 3:0] mem_la_wstrb;

    reg         mem_ready_r;

    picorv32 #(
        .ENABLE_COUNTERS     (0),
        .ENABLE_COUNTERS64   (0),
        .ENABLE_REGS_16_31   (1),
        .ENABLE_REGS_DUALPORT(1),
        .LATCHED_MEM_RDATA   (0),
        .TWO_STAGE_SHIFT     (1),
        .BARREL_SHIFTER      (0),
        .TWO_CYCLE_COMPARE   (0),
        .TWO_CYCLE_ALU       (0),
        .COMPRESSED_ISA      (1),
        .CATCH_MISALIGN      (0),
        .CATCH_ILLINSN       (0),
        .ENABLE_PCPI         (0),
        .ENABLE_MUL          (1),
        .ENABLE_FAST_MUL     (0),
        .ENABLE_DIV          (0),
        .ENABLE_IRQ          (0),
        .ENABLE_TRACE        (0),
        .REGS_INIT_ZERO      (0),
        .STACKADDR           (32'h0000_8000),
        .PROGADDR_RESET      (32'h0000_0000),
        .PROGADDR_IRQ        (32'h0000_0010)
    ) cpu (
        .clk         (clk),
        .resetn      (!reset),
        .trap        (),
        .mem_valid   (mem_valid),
        .mem_instr   (mem_instr),
        .mem_ready   (mem_ready),
        .mem_addr    (mem_addr),
        .mem_wdata   (mem_wdata),
        .mem_wstrb   (mem_wstrb),
        .mem_rdata   (mem_rdata),
        .mem_la_read (mem_la_read),
        .mem_la_write(mem_la_write),
        .mem_la_addr (mem_la_addr),
        .mem_la_wdata(mem_la_wdata),
        .mem_la_wstrb(mem_la_wstrb)
    );

    // Address decode
    wire sel_ram  = (mem_addr[31:28] == 4'h0);
    wire sel_uart = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h0);
    wire sel_led  = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h1);

    // BRAM: 32 KB, 8192 words, byte-write enables
    wire [31:0] ram_rdata;
    wire [ 3:0] ram_we = (mem_valid && sel_ram) ? mem_wstrb : 4'b0000;
    ram ram_i (
        .clk  (clk),
        .we   (ram_we),
        .addr (mem_addr[14:2]),
        .wdata(mem_wdata),
        .rdata(ram_rdata)
    );

    // UART TX — CPU-driven
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

    // UART RX — CPU polls; read clears valid
    wire [7:0] uart_rx_data;
    wire       uart_rx_valid;
    wire       uart_rx_ack = mem_valid && sel_uart && (mem_addr[3:2] == 2'b10)
                            && (mem_wstrb == 4'b0000) && !mem_ready_r;

    uart_rx #(.CLK_HZ(27_000_000), .BAUD(115200)) u_rx (
        .clk     (clk),
        .reset   (reset),
        .rx      (uart_rx),
        .data    (uart_rx_data),
        .valid   (uart_rx_valid),
        .read_ack(uart_rx_ack)
    );

    // LED write (single-pulse via mem_ready_r gating)
    always @(posedge clk) begin
        if (reset)
            led <= 6'b111111;
        else if (mem_valid && sel_led && (mem_wstrb != 4'b0000) && !mem_ready_r)
            led <= ~mem_wdata[5:0];
    end

    // Single-cycle mem_ready
    always @(posedge clk) begin
        if (reset)                  mem_ready_r <= 1'b0;
        else if (mem_ready_r)       mem_ready_r <= 1'b0;
        else if (mem_valid)         mem_ready_r <= 1'b1;
    end
    assign mem_ready = mem_ready_r;

    // Read mux
    assign mem_rdata =
        sel_ram                              ? ram_rdata :
        (sel_uart && mem_addr[3:2] == 2'b00) ? 32'h0000_0000 :
        (sel_uart && mem_addr[3:2] == 2'b01) ? {30'b0, uart_rx_valid, uart_tx_busy} :
        (sel_uart && mem_addr[3:2] == 2'b10) ? {24'b0, uart_rx_data} :
        32'h0000_0000;

endmodule
```

Removed from the old file (orphans): `uart_done`, `dbg_cnt`, `dbg_we`, the hardcoded `data(8'h41)` UART debug feed.

- [ ] **Step 2: Mirror to `picorv32/soc.v`**

Run: `cp src/soc.v picorv32/soc.v`

- [ ] **Step 3: Verify both copies match**

Run: `diff src/soc.v picorv32/soc.v`
Expected: no output.

- [ ] **Step 4: Commit**

```bash
git add src/soc.v picorv32/soc.v
git commit -m "feat(soc): CPU-driven UART TX, add UART RX, picorv32 RV32IMC, 32K RAM addr"
```

---

## Task 4: Update `top.v` to Expose `uart_rx` Port

**Files:**
- Modify: `src/top.v`
- Modify: `picorv32/top.v`

- [ ] **Step 1: Replace `src/top.v` contents**

```verilog
module top (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       uart_rx,
    output wire       uart_tx,
    output wire [5:0] led
);

    wire reset;
    reg [15:0] rst_cnt = 16'h0000;
    always @(posedge clk) begin
        if (!rst_n)
            rst_cnt <= 16'h0000;
        else if (rst_cnt != 16'hFFFF)
            rst_cnt <= rst_cnt + 16'h0001;
    end
    assign reset = (rst_cnt != 16'hFFFF);

    soc soc_i (
        .clk     (clk),
        .reset   (reset),
        .uart_rx (uart_rx),
        .uart_tx (uart_tx),
        .led     (led)
    );

endmodule
```

- [ ] **Step 2: Mirror to `picorv32/top.v`**

Run: `cp src/top.v picorv32/top.v`

- [ ] **Step 3: Verify**

Run: `diff src/top.v picorv32/top.v`
Expected: no output.

- [ ] **Step 4: Commit**

```bash
git add src/top.v picorv32/top.v
git commit -m "feat(top): expose uart_rx port"
```

---

## Task 5: CST + Build Manifests

**Files:**
- Modify: `tangnano9k.cst`
- Modify: `picorv32/tangnano9k.cst`
- Modify: `Makefile.oss`
- Modify: `picorv32/picorv32.gprj`

- [ ] **Step 1: Edit `tangnano9k.cst` — append RX pin**

Add these two lines at the end of the file (preserve existing lines):
```
IO_LOC "uart_rx" IOB1A;
IO_PORT "uart_rx" PULL_MODE=UP;
```

- [ ] **Step 2: Mirror to `picorv32/tangnano9k.cst`**

Run: `cp tangnano9k.cst picorv32/tangnano9k.cst`

- [ ] **Step 3: Edit `Makefile.oss` — add `uart_rx.v` to `SOURCES`**

Change:
```
SOURCES := $(SRC_DIR)/top.v $(SRC_DIR)/soc.v $(SRC_DIR)/picorv32.v \
           $(SRC_DIR)/ram.v $(SRC_DIR)/uart_tx.v
```
to:
```
SOURCES := $(SRC_DIR)/top.v $(SRC_DIR)/soc.v $(SRC_DIR)/picorv32.v \
           $(SRC_DIR)/ram.v $(SRC_DIR)/uart_tx.v $(SRC_DIR)/uart_rx.v
```

- [ ] **Step 4: Register `uart_rx.v` in `picorv32/picorv32.gprj`**

Open `picorv32/picorv32.gprj`. Find the `<Source>` entries listing the other `.v` files (`uart_tx.v`, `ram.v`, etc.). Add an entry alongside them for `uart_rx.v`. Example pattern (mimic existing entries exactly — they may have absolute or relative path):
```xml
<File path="uart_rx.v" type="file.verilog" enable="1"/>
```
Match the **exact attribute names and casing** used for the existing `uart_tx.v` entry. If unsure, open `picorv32/picorv32.gprj` and copy the `uart_tx.v` line, paste, change filename.

- [ ] **Step 5: Commit**

```bash
git add tangnano9k.cst picorv32/tangnano9k.cst Makefile.oss picorv32/picorv32.gprj
git commit -m "build: register uart_rx.v in both flows, add IOB1A RX pin"
```

---

## Task 6: Firmware Toolchain — RV32IMC + BSS Clear

**Files:**
- Modify: `firmware/Makefile` (CFLAGS only — bin2hex already updated in Task 2)
- Modify: `firmware/start.S`

- [ ] **Step 1: Edit `firmware/Makefile` — CFLAGS**

Change:
```
CFLAGS  = -march=rv32i -mabi=ilp32 -Os -ffreestanding -nostdlib -Wall
```
to:
```
CFLAGS  = -march=rv32imc -mabi=ilp32 -Os -ffreestanding -nostdlib -Wall
```

- [ ] **Step 2: Rewrite `firmware/start.S`**

Replace the whole file:
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

- [ ] **Step 3: Quick build smoke test**

Run:
```bash
cd firmware && make clean && make
cd ..
```
Expected: `firmware/firmware.elf`, `firmware/firmware.bin`, `firmware/firmware.hex` all generated, no warnings about unknown ISA or relocation overflow. If `firmware too big` error appears, the binary is fine but tools/bin2hex word count needs to be ≥ binary words (it's 8192 from Task 2).

- [ ] **Step 4: Commit**

```bash
git add firmware/Makefile firmware/start.S
git commit -m "feat(fw): build RV32IMC, clear BSS in start.S"
```

---

## Task 7: Demo C Firmware — Echo + MUL

**Files:**
- Modify: `firmware/main.c`

- [ ] **Step 1: Replace `firmware/main.c` contents**

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
    uint32_t product = a * b;
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

- [ ] **Step 2: Build firmware again**

Run:
```bash
cd firmware && make clean && make
cd ..
```
Expected: clean build, `firmware/firmware.hex` produced.

- [ ] **Step 3: Sanity check — symbol size**

Run: `riscv64-unknown-elf-size firmware/firmware.elf`
Expected: text + data + bss well under 32768 bytes total.

- [ ] **Step 4: Commit**

```bash
git add firmware/main.c
git commit -m "feat(fw): echo + MUL demo using new UART API"
```

---

## Task 8: Build Bitstream and Flash

**Files:** (none modified — build artifacts only)

- [ ] **Step 1: Copy firmware hex to the path `ram.v` reads from**

Run:
```bash
cp firmware/firmware.hex picorv32/firmware.hex
```
(Without this, both `Makefile.oss` and the Gowin IDE flow will fail at `$readmemh` or load a stale hex.)

- [ ] **Step 2: Build bitstream with the open-source flow**

Run:
```bash
make -f Makefile.oss clean
make -f Makefile.oss
```
Expected: produces `build_oss/picorv32_soc.fs`. Check the nextpnr-himbaechel output for the resource utilization line — confirm BRAM and LUT usage are within GW1NR-LV9 limits.

If `yosys` / `nextpnr-himbaechel` are not on `PATH`, source the oss-cad-suite environment first (the path varies per machine).

- [ ] **Step 3: Flash the board**

Connect the Tang Nano 9K via USB-C, then run:
```bash
make -f Makefile.oss flash
```
Expected: openFPGALoader writes to embedded flash, prints `Done` or similar.

- [ ] **Step 4: Identify the serial port**

Run: `dmesg | tail -20 | grep -i tty` (or `ls /dev/ttyUSB*`).
Tang Nano 9K typically exposes two `/dev/ttyUSB*` nodes — the JTAG one and the UART bridge. The UART bridge is usually `ttyUSB1`.

- [ ] **Step 5: Open serial terminal**

Pick one (whichever the user has):
- `picocom -b 115200 /dev/ttyUSB1`
- `minicom -D /dev/ttyUSB1 -b 115200` (8N1, no flow control)
- `screen /dev/ttyUSB1 115200`

- [ ] **Step 6: Press the reset button on the Tang Nano 9K**

Expected output in the terminal:
```
picorv32 ready. echo + MUL demo.
7*13=091
>
```

If `7*13=091` appears: TX wiring works, MUL works, COMPRESSED build works.

- [ ] **Step 7: Type characters in the terminal**

Expected:
- Each typed character echoes back.
- Pressing Enter (CR) prints CR + LF.
- The LED bar on the board advances by one position per character.

If echo works: RX module + CST IOB1A pin + read mux are all correct end-to-end.

- [ ] **Step 8: If echo fails — debug per spec section 12**

Refer to the spec's diagnostic decision tree:
- No output at all → TX wiring or clock/baud issue.
- Output but `7*13` wrong → toolchain ISA mismatch.
- Output but no echo → RX pin (try IOB1B), or RX module bug, or terminal not transmitting.
- Garbage output → baud / clock mismatch (verify the on-board 27 MHz).

- [ ] **Step 9: Final commit (artifacts excluded by .gitignore)**

```bash
git status
git commit --allow-empty -m "test: hardware verified — UART echo + MUL working on Tang Nano 9K"
```

---

## Self-Review Notes

- Spec section 1 (motivation) → implicit; no task required.
- Spec section 2 (architecture) → Tasks 1, 3, 4.
- Spec section 3 (memory map) → Task 3.
- Spec section 4 (UART TX wiring fix) → Task 3.
- Spec section 5 (UART RX module) → Task 1.
- Spec section 6 (SoC integration) → Tasks 3, 4.
- Spec section 7 (RAM growth) → Task 2.
- Spec section 8 (picorv32 params) → Task 3.
- Spec section 9 (toolchain) → Tasks 2 (bin2hex + linker), 6 (CFLAGS + start.S).
- Spec section 10 (demo firmware) → Task 7.
- Spec section 11 (file change summary, dual-copy) → enforced in every Verilog task via `cp` + `diff`.
- Spec section 12 (test plan) → Task 8.
- Spec section 13 (risks) → surfaced in Task 8 Step 8 (debug tree).

All spec sections covered. No placeholders, every code step shows full code, every shell step shows the exact command.

---

## Execution Notes

- Tasks 0–7 are pure-software edits and safe to run without the board attached.
- Task 8 requires the Tang Nano 9K connected and powered.
- If a step changes a Verilog file in `src/` without mirroring to `picorv32/`, **stop immediately** and run the `cp` + `diff` recovery before continuing. Letting the two copies drift is the most likely way to lose hours.
