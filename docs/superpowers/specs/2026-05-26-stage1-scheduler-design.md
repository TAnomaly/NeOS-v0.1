# NeOS Stage 1: Preemptive Scheduler — Design

**Date:** 2026-05-26
**Target:** Tang Nano 9K + picorv32 SoC
**Scope:** Stage 1 of OS roadmap (Scheduler → Process Model → PMP → MMU)

## Goal

Add preemptive multitasking to NeOS so a background task runs while the shell is active.
Concrete success: onboard LED blinks at ~1 Hz even while a CPU-bound shell command
(`mandel`) is executing.

This stage establishes the context-switch primitive that Stages 2-4 will build on.

## Non-Goals (Stage 1)

- Memory isolation between tasks
- Dynamic task creation from shell
- Inter-task communication (mutex / message queue)
- Stack overflow detection
- Priority scheduling (round-robin only)

## Architecture Decisions

### Scheduler model: real preemptive (rejected: cooperative, ISR-only)

- **Cooperative** rejected: `cmd_mandel` busy loop would freeze the LED — visible
  demo failure.
- **ISR-only fake-task** rejected: not a scheduler, dead-end for Stage 2.
- **Preemptive** chosen: timer IRQ saves callee-saved regs + sp, switches stacks.
  Each task gets its own stack. Cost: ~2 KB RAM, ~80 lines asm/C.

### Timer source: picorv32 built-in timer instruction (rejected: external MMIO timer)

picorv32 has `ENABLE_IRQ_TIMER` parameter enabling a built-in countdown timer
accessed via custom `timer` instruction. No external Verilog timer module needed.

Tradeoff: saves ~50 LUTs and avoids an MMIO register. Cost: requires inline asm with
custom instruction encoding (`.insn r 0x0b, 0, 5, x0, %0, x0`).

### Scheduling policy: round-robin

Simplest, fair, smallest LUT/code. Stage 1 demo has only 2 tasks so any policy works;
round-robin generalizes cleanly to 4+ tasks in Stage 2.

### Context switch scope: callee-saved + ra + interrupted PC

RV32IM callee-saved = s0..s11 (12 regs) + ra = 13 words = 52 bytes per switch.
Caller-saved (a0-a7, t0-t6) preserved by task itself — standard ABI.

Plus interrupted PC (read from q0) = 14 words = 56 bytes per context switch.

## Hardware Changes

### `src/soc.v` (one edit)

```verilog
// line 60-65 area
.ENABLE_IRQ          (1),        // was 0
.ENABLE_IRQ_QREGS    (1),        // default 1, made explicit
.ENABLE_IRQ_TIMER    (1),        // default 1, made explicit
```

Add port binding in picorv32 instance:
```verilog
.irq(32'h0),    // no external IRQ sources, only internal timer
```

### LUT cost estimate

picorv32 with IRQ enabled adds:
- 4 q-regs (q0..q3) banked register file: ~80 LUTs + 128 FFs
- IRQ FSM states: ~40 LUTs
- Timer countdown logic: ~30 LUTs

Estimate: +150 LUTs. Current util 91% → expected 93-94%. Fit margin tight but workable.

**Rollback option** if LUT overflow: set `ENABLE_DIV=0` (saves ~200 LUTs, breaks any
cc-compiled code using divide — would need software divide library, deferred).

## Software Changes

### File list (new)

- `bootloader/sched.h` — public API
- `bootloader/sched.c` — scheduler state + IRQ handler in C
- `bootloader/ctx_switch.S` — context switch asm primitive

### File list (modified)

- `bootloader/start.S` — rearrange so IRQ vector lands at 0x10
- `bootloader/main.c` — replace direct `repl()` call with `task_create + sched_start`
- `bootloader/Makefile` — add sched.c, ctx_switch.S to sources

### `start.S` rearrangement

```asm
.section .text.init
.globl _start
_start:
    j _real_start          ; 0x00 (4 bytes)
    .word 0, 0, 0          ; 0x04-0x0F padding
irq_vec:                   ; 0x10 — PROGADDR_IRQ
    j irq_handler          ; tail call to C handler
_real_start:               ; 0x14
    la sp, _stack_top
    ; bss zero loop unchanged
    call main
```

Rationale: picorv32 PROGADDR_IRQ default 0x10 cannot move without recompiling
RTL. Easier to pad `_start`.

### `sched.h`

```c
#ifndef SCHED_H
#define SCHED_H
#include <stdint.h>

#define MAX_TASKS 4
#define STACK_SZ  512

typedef struct {
    uint32_t sp;
    uint8_t  state;   // 0=free 1=ready 2=running
    uint8_t  id;
    uint8_t  pad[2];
} tcb_t;

extern tcb_t   tasks[MAX_TASKS];
extern uint8_t cur_task;
extern uint8_t task_stacks[MAX_TASKS][STACK_SZ];

int  task_create(void (*entry)(void));   // returns task id or -1
void sched_start(void);                  // jumps to task 0, never returns
void irq_handler(void);                  // called from start.S IRQ vector
#endif
```

### `sched.c` outline

- `tasks[]`, `cur_task`, `task_stacks[][]` definitions
- `task_create`: find free slot, prep stack with entry PC at top so first switch
  restores entry as ra, set state=ready
- `sched_start`: cur_task = 0, reload timer, jump to task 0 (load sp + jr ra)
- `irq_handler`: acknowledge timer (auto on `timer` write), pick next ready task,
  call `ctx_switch(old_sp_addr, new_sp_addr)`, reload timer for 270000 cycles

### `ctx_switch.S` outline

```asm
.globl ctx_switch
; a0 = address of tasks[old].sp
; a1 = address of tasks[new].sp
ctx_switch:
    addi sp, sp, -56
    sw   ra,  0(sp)
    sw   s0,  4(sp)
    ; ... s1..s11 ...
    sw   s11, 48(sp)
    ; interrupted PC handled separately via q0
    sw   sp,  0(a0)         ; save old sp
    lw   sp,  0(a1)         ; load new sp
    lw   ra,  0(sp)
    lw   s0,  4(sp)
    ; ... s1..s11 ...
    lw   s11, 48(sp)
    addi sp, sp, 56
    ret
```

Note: ISR entry/exit (q-reg save, retirq) is in a separate wrapper that calls
`irq_handler`, which then calls `ctx_switch`. Details locked during implementation.

### `main.c` changes

```c
extern void shell_task(void);   // was 'static void repl(void)'
static void led_task(void);

static void led_task(void) {
    extern volatile uint32_t LED;
    for (;;) {
        for (volatile uint32_t i = 0; i < 1000000; i++);  // ~rough delay
        LED ^= 1;
    }
}

int main(void) {
    /* existing init: fb_init, gui_init, etc. */
    task_create(shell_task);
    task_create(led_task);
    sched_start();
    return 0;  // unreachable
}
```

The `for` delay is rough — preemption guarantees LED toggles even if math is off,
since IRQ fires every 10 ms regardless.

## Data Flow

```
power-on
  → _start (0x00)
  → _real_start (0x14): zero bss, call main
  → main: init, task_create x2, sched_start
  → sched_start: timer_reload(270000), jump to tasks[0] entry
  → shell_task runs
    └─ 10ms later: timer underflow → IRQ
        → PROGADDR_IRQ (0x10): j irq_handler
        → irq_handler: pick next ready (led_task)
        → ctx_switch(&tasks[0].sp, &tasks[1].sp)
        → resume led_task at saved PC
        → ... another 10ms ...
        → IRQ → ctx_switch back to shell_task
```

## Error Handling

- `task_create` over MAX_TASKS → return -1. Caller (main) ignores; demo has 2 tasks
  hardcoded.
- Task returning from entry → falls into `0xDEADBEEF` sentinel at stack base → CPU
  fetches invalid PC, traps → debug-visible failure (no silent corruption).
- LUT overflow at synthesis → caught at `make`, rollback plan: disable `ENABLE_DIV`.

No stack overflow detection — out of scope for Stage 1 (per optimize-code preference,
not adding guards that cost LUT/cycles without demonstrated need).

## Test Plan

Manual verification (no automated test infrastructure on FPGA):

1. `make` succeeds, LUT util ≤95% reported.
2. Flash bitstream to Tang Nano 9K.
3. HDMI shows normal GUI tile launcher.
4. Onboard LED toggles at ~1 Hz (stopwatch: 10 toggles in ~10 seconds, ±2 s).
5. Open `picocom`, enter shell, run `mandel`.
6. While `mandel` is running (multi-second computation), LED **continues blinking**.
7. After `mandel` finishes, shell prompt returns, LED still blinks.

Pass = all 7 steps pass. Any step failing = scheduler broken or LUT-fit broken.

## Risks + Unknowns

| Risk | Mitigation |
|------|------------|
| LUT util exceeds 100% | Disable ENABLE_DIV; need soft-divide for cc later |
| picorv32 `timer` inline asm encoding wrong | Fallback: external timer.v module (~50 LUT) |
| Q-reg semantics misunderstood (q0 not PC?) | Cross-check picorv32 README before implementation |
| Stack size 512 too small for shell_task (which calls everything) | Bump to 1024, costs 2 KB total |
| First IRQ fires before sched_start fully initialized | Disable IRQ until last instruction of sched_start; set mask via `maskirq` |

## Out of Scope (Deferred to Later Stages)

- Stage 2: TCB extends to hold memory region (base, limit). Soft isolation via
  C-level checks.
- Stage 3: PMP RTL — picorv32 patches required. High effort, deferred.
- Stage 4: MMU — likely doesn't fit Tang Nano 9K LUT budget. Re-evaluate after Stage 2.

## Files Summary

**New:**
- `bootloader/sched.h` (~30 lines)
- `bootloader/sched.c` (~80 lines)
- `bootloader/ctx_switch.S` (~50 lines)

**Modified:**
- `src/soc.v` (3 lines)
- `bootloader/start.S` (4 lines added)
- `bootloader/main.c` (~10 lines)
- `bootloader/Makefile` (2 lines)

**Estimated total diff:** ~180 lines.
