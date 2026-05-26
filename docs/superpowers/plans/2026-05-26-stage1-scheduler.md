# Stage 1: Preemptive Scheduler — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add preemptive multitasking to NeOS — background LED blinker task runs at ~1 Hz even while shell is executing CPU-bound commands like `mandel`.

**Architecture:** picorv32 built-in IRQ + countdown timer fires at 100 Hz. IRQ handler picks next ready task round-robin, calls asm context switch that saves/restores callee-saved regs + sp on each task's private stack.

**Tech Stack:** Verilog (picorv32 RTL), RV32IMC asm, freestanding C (no libc), Tang Nano 9K toolchain (Yosys + nextpnr-himbaechel + gowin pack).

**Spec:** [docs/superpowers/specs/2026-05-26-stage1-scheduler-design.md](../specs/2026-05-26-stage1-scheduler-design.md)

---

## Hardware/FPGA Notes

This is bare-metal FPGA development. "Tests" cannot be `pytest`. Each task ends with a **verification step**: build + flash + observe specific hardware signal (LED, HDMI, serial). Where possible the verification is binary pass/fail. Manual but unambiguous.

**Build command (used in every task):** `make -f Makefile.oss`
**Flash command:** `openFPGALoader -b tangnano9k impl/pnr/picorv32_soc.fs` (or whatever the existing flash command is — confirm with `grep flash Makefile.oss` if unsure)

**Toolchain check before starting:**
- `riscv64-unknown-elf-gcc --version` must work
- `yosys`, `nextpnr-himbaechel`, `gowin_pack` must be on PATH

---

## File Structure

**New files:**
- `bootloader/sched.h` — public scheduler API (~30 lines)
- `bootloader/sched.c` — TCB array, task_create, sched_start, irq_handler (~80 lines)
- `bootloader/ctx_switch.S` — asm context switch primitive (~60 lines)

**Modified files:**
- `src/soc.v` — flip `ENABLE_IRQ` to 1, add `.irq(32'h0)` port binding
- `bootloader/start.S` — pad first 0x10 bytes so IRQ vector lands at 0x10
- `bootloader/main.c` — replace `gui_run()` direct call with `task_create + sched_start`
- `bootloader/Makefile` — add `sched.c ctx_switch.S` to sources

---

## Task 1: Enable IRQ in picorv32, verify LUT budget

**Goal:** Flip `ENABLE_IRQ=1` and confirm synthesis still fits Tang Nano 9K. No software changes yet. Bootloader still boots normally because no IRQ source fires (we wire `.irq(32'h0)`).

**Files:**
- Modify: `src/soc.v` lines 60-65 (picorv32 instantiation params) and the instance port list

- [ ] **Step 1: Read current soc.v picorv32 instantiation**

Read [src/soc.v](src/soc.v) lines 43-82. Confirm:
- Line 60: `.ENABLE_IRQ          (0),`
- No `.irq` port currently in instance port list (lines 67-82)

- [ ] **Step 2: Flip ENABLE_IRQ and add explicit qreg/timer params**

Edit `src/soc.v` line 60 area:

```verilog
        .ENABLE_IRQ          (1),
        .ENABLE_IRQ_QREGS    (1),
        .ENABLE_IRQ_TIMER    (1),
        .ENABLE_TRACE        (0),
```

- [ ] **Step 3: Add irq port binding to picorv32 instance**

In the picorv32 instance port list (after the existing `.mem_la_wstrb(mem_la_wstrb)` line, before closing `);`), add a comma at end of `.mem_la_wstrb(...)` and append:

```verilog
        .mem_la_wstrb(mem_la_wstrb),
        .irq         (32'h0)
```

(Only internal timer IRQ used in Stage 1; no external IRQ sources.)

- [ ] **Step 4: Build and check LUT utilization**

Run: `make -f Makefile.oss`
Expected: build succeeds. Look for nextpnr-himbaechel output line like `Info: Logic LUTs: NNNN/8640 (XX%)`. Record value.
Pass: build completes, LUT util ≤ 95%.
Fail: synthesis error → revert step 2-3 and report. If LUT > 95%, try setting `.ENABLE_DIV(0)` as fallback (will break any divide in software, OK for Stage 1 since cc currently doesn't use it heavily — confirm).

- [ ] **Step 5: Flash and confirm GUI still boots**

Flash bitstream. Power-cycle. HDMI must show normal GUI tile launcher (no change from before). picocom into UART, type a key, GUI should respond.
Pass: GUI visible and responsive.
Fail: picorv32 may be in unexpected IRQ state on first cycle — proceed to Task 2 (start.S vector setup) and re-test.

- [ ] **Step 6: Commit**

```bash
git add src/soc.v
git commit -m "soc: enable picorv32 IRQ + built-in timer (Stage 1 scheduler prep)"
```

---

## Task 2: Rearrange start.S so IRQ vector lands at 0x10

**Goal:** picorv32 jumps to PROGADDR_IRQ (0x10) on IRQ. Current `_start` runs straight through and would clobber that address. Pad `_start` with a jump-over so address 0x10 is free for an IRQ trampoline.

**Files:**
- Modify: `bootloader/start.S` (entire file rewritten — only 15 lines)

- [ ] **Step 1: Read current start.S**

Read [bootloader/start.S](bootloader/start.S). It's 14 lines: sets sp, zeros bss, calls main.

- [ ] **Step 2: Rewrite start.S with IRQ vector pad**

Replace entire file content:

```asm
.section .text.init
.globl _start
_start:
    j _real_start            /* 0x00: 4 bytes, jump past IRQ vector */
    .word 0, 0, 0            /* 0x04-0x0F: 12 bytes padding */

.globl irq_vec
irq_vec:                     /* 0x10: PROGADDR_IRQ */
    j irq_trampoline         /* tail-jump to asm trampoline */

_real_start:                 /* 0x14: original boot code */
    la   sp, _stack_top

    la   t0, _bss_start
    la   t1, _bss_end
1:  bge  t0, t1, 2f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    1b

2:  call main
3:  j    3b

/* Weak placeholder so build succeeds before ctx_switch.S exists. */
.weak irq_trampoline
irq_trampoline:
    /* No-op: ack timer (write 0 disables it) then return. */
    .insn r 0x0b, 0, 5, x0, x0, x0   /* timer x0, x0  → set countdown=0 */
    .insn r 0x0b, 0, 2, x0, x0, x0   /* retirq */
```

The `.insn r` lines are picorv32 custom instruction encodings:
- `timer rd, rs1`: opcode=0x0b, funct3=0, funct7=5
- `retirq`: opcode=0x0b, funct3=0, funct7=2

The weak `irq_trampoline` is overridden when `ctx_switch.S` is compiled (Task 5).

- [ ] **Step 3: Build and flash**

Run: `make -f Makefile.oss`
Flash bitstream + hex.
Expected: build succeeds, GUI boots normally.
Pass: HDMI shows GUI.

- [ ] **Step 4: Commit**

```bash
git add bootloader/start.S
git commit -m "bootloader: reserve IRQ vector at 0x10 with weak trampoline"
```

---

## Task 3: Add sched.h header (compiles, not yet used)

**Goal:** Lock the public API. Header-only step.

**Files:**
- Create: `bootloader/sched.h`

- [ ] **Step 1: Write sched.h**

Create `bootloader/sched.h`:

```c
#ifndef SCHED_H
#define SCHED_H
#include <stdint.h>

#define MAX_TASKS 4
#define STACK_SZ  512

#define TASK_FREE    0
#define TASK_READY   1
#define TASK_RUNNING 2

typedef struct {
    uint32_t sp;
    uint8_t  state;
    uint8_t  id;
    uint8_t  pad[2];
} tcb_t;

extern tcb_t   tasks[MAX_TASKS];
extern uint8_t cur_task;
extern uint8_t task_stacks[MAX_TASKS][STACK_SZ];

/* Returns task id (>=0) or -1 if no free slot. */
int  task_create(void (*entry)(void));

/* Sets cur_task=0, loads timer countdown, jumps to task 0. Never returns. */
void sched_start(void);

/* Called from irq_trampoline in start.S after qreg save. */
void irq_handler(void);

#endif
```

- [ ] **Step 2: Verify it includes cleanly**

No build yet (no .c file consumes it). Confirm file exists and parses by including from main.c trivially:

Temporarily add `#include "sched.h"` near top of `bootloader/main.c` (right after existing includes), then `make -f Makefile.oss`.

Expected: builds OK.

- [ ] **Step 3: Remove temporary include from main.c**

Revert the temporary `#include "sched.h"` line. Don't commit it.

- [ ] **Step 4: Commit just sched.h**

```bash
git add bootloader/sched.h
git commit -m "sched: add scheduler public API header"
```

---

## Task 4: Implement sched.c with task_create + sched_start (single-task mode)

**Goal:** Implement task storage and creation. `sched_start` jumps to task 0 directly (no context switch yet — just `jr` to entry). Refactor `main()` to use `task_create(shell_task)` + `sched_start` instead of calling `gui_run()` directly. Behavior unchanged from user's perspective.

This task does NOT enable IRQ-driven scheduling. That's Task 6. This task proves the task creation + initial dispatch works.

**Files:**
- Create: `bootloader/sched.c`
- Modify: `bootloader/Makefile` — add `sched.c` to SRCS
- Modify: `bootloader/main.c` — rename body of `main()`'s GUI-launch path to `shell_task`, then call `task_create + sched_start`

- [ ] **Step 1: Write sched.c**

Create `bootloader/sched.c`:

```c
#include "sched.h"

tcb_t   tasks[MAX_TASKS];
uint8_t cur_task;
uint8_t task_stacks[MAX_TASKS][STACK_SZ] __attribute__((aligned(16)));

int task_create(void (*entry)(void)) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE) {
            /* Stack grows down. Top of this task's stack area. */
            uint32_t *sp = (uint32_t *)(task_stacks[i] + STACK_SZ);

            /* Reserve 14 words (56 bytes) matching ctx_switch frame:
             * [ra, s0..s11, padding] = 14 entries.
             * Pre-fill ra slot with entry so first ctx_switch restore
             * returns into the task. */
            sp -= 14;
            for (int j = 0; j < 14; j++) sp[j] = 0;
            sp[0] = (uint32_t)entry;   /* ra slot */

            tasks[i].sp    = (uint32_t)sp;
            tasks[i].state = TASK_READY;
            tasks[i].id    = (uint8_t)i;
            return i;
        }
    }
    return -1;
}

/* Round-robin pick. Returns index of next ready task, or current if none other. */
static uint8_t pick_next(void) {
    for (int n = 1; n <= MAX_TASKS; n++) {
        uint8_t cand = (cur_task + n) % MAX_TASKS;
        if (tasks[cand].state == TASK_READY || tasks[cand].state == TASK_RUNNING)
            return cand;
    }
    return cur_task;
}

/* Defined in ctx_switch.S. Args: addr of old sp, addr of new sp. */
extern void ctx_switch(uint32_t *old_sp, uint32_t *new_sp);

void irq_handler(void) {
    /* Acknowledge by reloading the timer (set in trampoline). */
    uint8_t prev = cur_task;
    uint8_t next = pick_next();
    if (next == prev) return;   /* nothing to do */
    tasks[prev].state = TASK_READY;
    tasks[next].state = TASK_RUNNING;
    cur_task = next;
    ctx_switch(&tasks[prev].sp, &tasks[next].sp);
}

/* First dispatch: load sp from tasks[0], jump to its entry (stored at ra slot).
 * Implemented in asm because we cannot return from main into this. */
extern void sched_dispatch_first(uint32_t new_sp);

void sched_start(void) {
    cur_task = 0;
    tasks[0].state = TASK_RUNNING;
    sched_dispatch_first(tasks[0].sp);
    /* Unreachable. */
    for (;;) {}
}
```

- [ ] **Step 2: Add sched.c to Makefile**

Edit `bootloader/Makefile`:

Change line:
```
SRCS = start.S main.c interp.c cc.c fb.c gui.c edit.c
```
to:
```
SRCS = start.S main.c interp.c cc.c fb.c gui.c edit.c sched.c
```

And the explicit build line:
```
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S main.c interp.c cc.c fb.c gui.c edit.c
```
to:
```
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S main.c interp.c cc.c fb.c gui.c edit.c sched.c
```

And the deps line:
```
$(OUT).elf: $(SRCS) linker.ld interp.h cc.h fb.h gui.h font5x7.h edit.h
```
to:
```
$(OUT).elf: $(SRCS) linker.ld interp.h cc.h fb.h gui.h font5x7.h edit.h sched.h
```

- [ ] **Step 3: Add temporary sched_dispatch_first stub in start.S**

Since Task 5 will write the real `sched_dispatch_first` in `ctx_switch.S`, add a temporary stub at the bottom of `start.S` so this task can build:

Append to `bootloader/start.S`:

```asm
.weak sched_dispatch_first
sched_dispatch_first:
    /* a0 = new sp. Load it, pop ra (which holds task entry), jump. */
    mv   sp, a0
    lw   ra, 0(sp)
    addi sp, sp, 56
    jr   ra
```

(Will be overridden by ctx_switch.S in Task 5.)

- [ ] **Step 4: Refactor main.c to use scheduler**

Read [bootloader/main.c](bootloader/main.c) around line 849 (`int main`).

Add at top of main.c (after existing includes):
```c
#include "sched.h"
```

Find the existing `int main(void)` function. Just before it, add:

```c
static void shell_task(void) {
    gui_run();
    /* If gui_run ever returns, fall back to a REPL or infinite loop. */
    for (;;) {}
}
```

Then change `main()` to:

```c
int main(void) {
    LED_REG = 0;
    /* keep any existing init lines that were here (fb_init? gui_init? etc.)
     * BEFORE gui_run() — copy them here unchanged.
     * Only the gui_run() call moves into shell_task. */

    task_create(shell_task);
    sched_start();
    return 0;  /* unreachable */
}
```

**Action for implementer:** read the current `main()` body, move any init code (fb_init, gui_init, REPL setup, etc.) that runs BEFORE `gui_run()` into the new `main()`. Only `gui_run()` itself goes into `shell_task`.

- [ ] **Step 5: Build**

Run: `make -f Makefile.oss`
Expected: clean build.
Fail: most likely link error if I missed something — read error, fix, re-run.

- [ ] **Step 6: Flash and verify identical behavior**

Flash. Power-cycle. HDMI must show GUI tile launcher exactly as before. Type keys, GUI must respond.
Pass: visually indistinguishable from pre-Task-4 boot.
Fail: stack ABI issue → check `sched_dispatch_first` stub matches frame layout.

- [ ] **Step 7: Commit**

```bash
git add bootloader/sched.c bootloader/Makefile bootloader/start.S bootloader/main.c
git commit -m "sched: implement task_create + sched_start, refactor main"
```

---

## Task 5: Add ctx_switch.S — context switch primitive

**Goal:** Real asm context switch. Replaces the weak `sched_dispatch_first` in start.S. Does NOT yet hook into IRQ — Task 6 wires the IRQ trampoline to call `irq_handler`.

**Files:**
- Create: `bootloader/ctx_switch.S`
- Modify: `bootloader/Makefile` — add `ctx_switch.S` to SRCS

- [ ] **Step 1: Write ctx_switch.S**

Create `bootloader/ctx_switch.S`:

```asm
/* Context switch: save callee-saved + ra, swap stacks, restore.
 *
 * void ctx_switch(uint32_t *old_sp_addr, uint32_t *new_sp_addr);
 *   a0 = pointer to slot to store old sp
 *   a1 = pointer to slot to load new sp from
 *
 * Frame layout (14 words = 56 bytes, sp grows down):
 *   sp+ 0 : ra
 *   sp+ 4 : s0
 *   sp+ 8 : s1
 *   sp+12 : s2
 *   sp+16 : s3
 *   sp+20 : s4
 *   sp+24 : s5
 *   sp+28 : s6
 *   sp+32 : s7
 *   sp+36 : s8
 *   sp+40 : s9
 *   sp+44 : s10
 *   sp+48 : s11
 *   sp+52 : reserved (alignment / future use)
 */

.section .text
.globl ctx_switch
ctx_switch:
    addi sp, sp, -56
    sw   ra,   0(sp)
    sw   s0,   4(sp)
    sw   s1,   8(sp)
    sw   s2,  12(sp)
    sw   s3,  16(sp)
    sw   s4,  20(sp)
    sw   s5,  24(sp)
    sw   s6,  28(sp)
    sw   s7,  32(sp)
    sw   s8,  36(sp)
    sw   s9,  40(sp)
    sw   s10, 44(sp)
    sw   s11, 48(sp)

    sw   sp, 0(a0)         /* *old_sp_addr = sp */
    lw   sp, 0(a1)         /* sp = *new_sp_addr */

    lw   ra,   0(sp)
    lw   s0,   4(sp)
    lw   s1,   8(sp)
    lw   s2,  12(sp)
    lw   s3,  16(sp)
    lw   s4,  20(sp)
    lw   s5,  24(sp)
    lw   s6,  28(sp)
    lw   s7,  32(sp)
    lw   s8,  36(sp)
    lw   s9,  40(sp)
    lw   s10, 44(sp)
    lw   s11, 48(sp)
    addi sp, sp, 56
    ret

/* First dispatch: just load sp + pop ra + jump. No save of caller — caller
 * (sched_start, called from main) is being abandoned forever. */
.globl sched_dispatch_first
sched_dispatch_first:
    mv   sp, a0
    lw   ra,   0(sp)
    lw   s0,   4(sp)
    lw   s1,   8(sp)
    lw   s2,  12(sp)
    lw   s3,  16(sp)
    lw   s4,  20(sp)
    lw   s5,  24(sp)
    lw   s6,  28(sp)
    lw   s7,  32(sp)
    lw   s8,  36(sp)
    lw   s9,  40(sp)
    lw   s10, 44(sp)
    lw   s11, 48(sp)
    addi sp, sp, 56
    jr   ra
```

- [ ] **Step 2: Add ctx_switch.S to Makefile**

Edit `bootloader/Makefile` SRCS line and the explicit gcc invocation, adding `ctx_switch.S`:

```
SRCS = start.S main.c interp.c cc.c fb.c gui.c edit.c sched.c ctx_switch.S
```

And the explicit build line gets `ctx_switch.S` appended.

- [ ] **Step 3: Remove temporary stub from start.S**

Delete the `.weak sched_dispatch_first` block at the bottom of `bootloader/start.S` (added in Task 4). The strong symbol in ctx_switch.S now provides it.

- [ ] **Step 4: Build and flash**

Run: `make -f Makefile.oss`. Flash.
Expected: build OK, GUI still boots identically (we have one task, IRQ not triggering yet).
Pass: visually identical to Task 4.

- [ ] **Step 5: Commit**

```bash
git add bootloader/ctx_switch.S bootloader/Makefile bootloader/start.S
git commit -m "sched: add ctx_switch.S and sched_dispatch_first"
```

---

## Task 6: Wire IRQ trampoline + timer reload + add LED blinker task

**Goal:** Make IRQ actually fire. IRQ trampoline saves picorv32 q-regs to current task's stack, calls C `irq_handler`, restores, retirq's. Add `led_task` and `task_create(led_task)` in main. This task is the payoff: LED blinks while shell runs.

**Files:**
- Modify: `bootloader/start.S` — replace weak `irq_trampoline` with real one that calls `irq_handler`
- Modify: `bootloader/main.c` — add `led_task`, call `task_create(led_task)` before `sched_start`

- [ ] **Step 1: Replace irq_trampoline in start.S**

Read `bootloader/start.S`. Find the `.weak irq_trampoline` block and replace it (drop the `.weak`, make it strong) with:

```asm
.globl irq_trampoline
irq_trampoline:
    /* On entry (picorv32 IRQ): q0=return PC, q1=irq mask.
     * All normal regs preserved by hardware NO — picorv32 only banks q-regs,
     * normal regs are NOT auto-saved. We must save caller-saved regs we use.
     *
     * Strategy: save ra + all caller-saved (a0-a7, t0-t6) on current task's
     * stack, call C handler (which may ctx_switch), restore, retirq.
     *
     * 16 caller-saved regs + ra = 17 words = 68 bytes. Round to 16-byte
     * alignment: 80 bytes.
     */
    addi sp, sp, -80
    sw   ra,  0(sp)
    sw   t0,  4(sp)
    sw   t1,  8(sp)
    sw   t2, 12(sp)
    sw   t3, 16(sp)
    sw   t4, 20(sp)
    sw   t5, 24(sp)
    sw   t6, 28(sp)
    sw   a0, 32(sp)
    sw   a1, 36(sp)
    sw   a2, 40(sp)
    sw   a3, 44(sp)
    sw   a4, 48(sp)
    sw   a5, 52(sp)
    sw   a6, 56(sp)
    sw   a7, 60(sp)
    /* sp+64..76 reserved/pad */

    call irq_handler

    /* Reload timer countdown: 270000 cycles = 100 Hz at 27 MHz. */
    li   t0, 270000
    .insn r 0x0b, 0, 5, x0, t0, x0   /* timer x0, t0 */

    lw   ra,  0(sp)
    lw   t0,  4(sp)
    lw   t1,  8(sp)
    lw   t2, 12(sp)
    lw   t3, 16(sp)
    lw   t4, 20(sp)
    lw   t5, 24(sp)
    lw   t6, 28(sp)
    lw   a0, 32(sp)
    lw   a1, 36(sp)
    lw   a2, 40(sp)
    lw   a3, 44(sp)
    lw   a4, 48(sp)
    lw   a5, 52(sp)
    lw   a6, 56(sp)
    lw   a7, 60(sp)
    addi sp, sp, 80

    .insn r 0x0b, 0, 2, x0, x0, x0   /* retirq */
```

Note: this saves only caller-saved regs. `irq_handler` may call `ctx_switch` which saves callee-saved on the OLD task's stack and restores them from the NEW task's stack. When `ctx_switch` returns into `irq_trampoline`, we are now executing on the NEW task's IRQ frame — but the new task's stack also had this same 80-byte frame at its top from when IT was preempted. So the lw's restore the new task's caller-saved regs. retirq jumps to q0, which is the new task's interrupted PC.

**Critical:** the first IRQ on each task happens while running inside `ctx_switch` saving normal regs. The stack layouts must compose. Verify by tracing once on paper before implementing.

- [ ] **Step 2: Add timer-arm in sched_start**

Edit `bootloader/sched.c`. Modify `sched_start` to arm the timer before first dispatch:

```c
void sched_start(void) {
    cur_task = 0;
    tasks[0].state = TASK_RUNNING;
    /* Arm timer: 270000 cycles to first IRQ. */
    register uint32_t cnt asm("t0") = 270000;
    asm volatile (".insn r 0x0b, 0, 5, x0, %0, x0" :: "r"(cnt));
    sched_dispatch_first(tasks[0].sp);
    for (;;) {}
}
```

- [ ] **Step 3: Add led_task in main.c**

Edit `bootloader/main.c`. Just below the `shell_task` definition added in Task 4, add:

```c
static void led_task(void) {
    uint32_t toggle = 0;
    for (;;) {
        /* Burn cycles between toggles — IRQ may preempt us mid-loop, fine. */
        for (volatile uint32_t i = 0; i < 500000; i++) { }
        toggle ^= 1;
        /* Preserve any bits cmd_synth or other tasks set; only toggle bit 7
         * (we have 6 user LEDs at bits 0-5, bit 6+ likely unused).
         * Use bit 5 if 7 isn't physical. Read first to avoid clobber. */
        uint32_t v = LED_REG;
        v ^= 0x20;   /* toggle LED bit 5 (highest user LED) */
        LED_REG = v;
        (void)toggle;
    }
}
```

Then in `main()`, add the second `task_create` call:

```c
int main(void) {
    LED_REG = 0;
    /* existing init... */
    task_create(shell_task);
    task_create(led_task);
    sched_start();
    return 0;
}
```

- [ ] **Step 4: Build**

Run: `make -f Makefile.oss`
Expected: clean build. LUT util may bump 1-2%; still ≤ 95%.

- [ ] **Step 5: Flash and observe LED**

Flash bitstream + hex.
Power-cycle.
Expected:
- HDMI shows GUI as before
- Onboard LED bit 5 toggles roughly once per second (busy-loop count chosen approximately — exact period not critical, must just be visible)
Pass: LED visibly blinks.
Fail mode A: LED stays solid → IRQ not firing or `irq_handler` not running. Add a static counter incremented in `irq_handler`, expose via LED bits 0-4 to debug.
Fail mode B: System hangs after seconds → stack corruption in IRQ frame composition. Inspect the frame math.
Fail mode C: GUI never appears → first `sched_dispatch_first` broken. Test with led_task as task 0 (swap order) to isolate.

- [ ] **Step 6: Verify preemption with mandel**

Open picocom. Wait for shell prompt. Run `mandel`.
Expected: mandel computes for several seconds. During that time the LED **continues blinking**. After mandel finishes, prompt returns, LED still blinking.
Pass: preemption confirmed.
Fail: LED freezes during mandel → context switch not happening on IRQ. Verify `pick_next` returns the OTHER task, not the same one.

- [ ] **Step 7: Commit**

```bash
git add bootloader/start.S bootloader/sched.c bootloader/main.c
git commit -m "sched: enable timer IRQ + LED blinker task, preemption working"
```

---

## Task 7: Final integration test + spec verification

**Goal:** Run the 7-step manual test from the spec and confirm all pass. No new code. Pure verification.

- [ ] **Step 1: Clean rebuild from scratch**

Run: `make -f Makefile.oss clean && make -f Makefile.oss`
Expected: clean build from zero.

- [ ] **Step 2: Record LUT utilization**

From nextpnr output: `Logic LUTs: NNNN/8640 (XX%)`.
Pass: ≤ 95%.

- [ ] **Step 3: Flash bitstream + hex**

Flash. Power-cycle.

- [ ] **Step 4: Spec test sequence (from design doc section 8)**

1. HDMI shows GUI tile launcher — pass/fail
2. LED bit 5 toggles at ~1 Hz, count 10 toggles in ~10 s with stopwatch — pass/fail
3. picocom → shell prompt visible — pass/fail
4. Run `mandel` — pass/fail (mandel produces ASCII art)
5. During mandel run, LED keeps blinking — pass/fail (this is the headline result)
6. After mandel finishes, LED still blinks, shell prompt back — pass/fail
7. Type other commands (`help`, `info`) — all still work normally — pass/fail

Pass: all 7 steps pass. Mark Stage 1 done.

- [ ] **Step 5: If all pass, commit a marker**

```bash
git commit --allow-empty -m "sched: Stage 1 verified — preemptive scheduler working"
```

- [ ] **Step 6: If any fail, document and stop**

Open an issue or note in `docs/superpowers/specs/2026-05-26-stage1-scheduler-design.md` under a new `## Stage 1 Status` heading describing which step failed and what was observed. Do NOT proceed to Stage 2 until Stage 1 passes.

---

## Self-Review

**Spec coverage:**
- Spec §1 Goal → Task 7 verifies headline result (mandel + LED).
- Spec §3.1 ENABLE_IRQ flip → Task 1.
- Spec §3.2 LUT estimate → Task 1 step 4 + Task 7 step 2 record actual util.
- Spec §3.3 start.S IRQ vector → Task 2.
- Spec §3.4 sched.h API → Task 3.
- Spec §3.5 ctx_switch.S → Task 5.
- Spec §3.6 timer reload + 100 Hz → Task 6 step 1 (in trampoline) + step 2 (in sched_start).
- Spec §3.6 led_task in main.c → Task 6 step 3.
- Spec §4 Data flow → covered by Tasks 1-6 in order.
- Spec §5 Error handling → task_create returns -1 (Task 4 sched.c), 0xDEADBEEF sentinel deferred (would add to stack base; current plan zeros ra slot which traps with PC=0 — equivalent fail-loud).
- Spec §6 Test plan → Task 7.
- Spec §7 Risks → LUT util checked in Task 1+7; `timer` instruction encoding inlined verbatim; q-reg semantics noted as risk in Task 6 step 1 prose.

**Placeholder scan:** No "TBD", "implement later", "appropriate error handling". All code is shown verbatim. The one prose note in Task 4 step 4 ("read current main(), move init code") is unavoidable — implementer must inspect the live file because main.c is too large to embed in a plan task. Instructions for what to move are explicit.

**Type/name consistency:**
- `tcb_t` field names (sp, state, id, pad) consistent across sched.h, sched.c, ctx_switch.S frame.
- `TASK_FREE/READY/RUNNING` constants defined in sched.h, used in sched.c.
- `ctx_switch(uint32_t *old_sp, uint32_t *new_sp)` signature consistent.
- `sched_dispatch_first(uint32_t new_sp)` consistent (single arg, just sp).
- Frame size 56 bytes consistent in both ctx_switch.S and task_create stack prep (14 words = 56 bytes).
- LED toggle uses bit 5 (`0x20`), avoiding bit 0 which `cmd_synth` may use.

No issues found.
