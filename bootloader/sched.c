#include "sched.h"

tcb_t   tasks[MAX_TASKS];
uint8_t cur_task;
uint8_t task_stacks[MAX_TASKS][STACK_SZ] __attribute__((aligned(16)));

extern char irq_trampoline_tail[];

int task_create(void (*entry)(void)) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE) {
            /* Combined frame layout (136 bytes = 34 words):
             * - sp+0..52   : ctx_switch frame (14 words)
             *   sp+0: ra = irq_trampoline_tail
             *   sp+4..52: s0..s11 + pad = 0
             * - sp+56..135 : irq_trampoline frame (20 words)
             *   sp+56+0: ra=0, sp+56+4..60: t0..t6,a0..a7 = 0
             *   sp+56+64 (= sp+120): entry PC (loaded into q0 by tail)
             *   sp+56+68..76: pad
             *
             * Word indices into the combined frame:
             *   word 0  (sp+0)   = ctx_switch ra slot
             *   word 14 (sp+56)  = irq_trampoline frame base
             *   word 30 (sp+120) = entry PC (sp+56 + 64)
             */
            uint8_t *top = task_stacks[i] + STACK_SZ;
            uint32_t *frame = (uint32_t *)(top - 136);
            for (int j = 0; j < 34; j++) frame[j] = 0;
            frame[0]  = (uint32_t)irq_trampoline_tail;  /* ctx_switch ra */
            frame[30] = (uint32_t)entry;                /* q0 = entry PC */

            tasks[i].sp    = (uint32_t)frame;
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

/* Defined in ctx_switch.S (Task 5). Args: addr of old sp, addr of new sp. */
extern void ctx_switch(uint32_t *old_sp, uint32_t *new_sp);

void irq_handler(void) {
    /* DIAGNOSTIC: toggle LED bit 4 every 50 IRQs (~1 Hz) to prove IRQ firing
     * independently of context switching. */
    static uint32_t irq_count = 0;
    irq_count++;
    if ((irq_count % 50) == 0) {
        volatile uint32_t *led = (volatile uint32_t *)0x10000010;
        *led ^= 0x10;
    }

    uint8_t prev = cur_task;
    uint8_t next = pick_next();
    if (next == prev) return;
    tasks[prev].state = TASK_READY;
    tasks[next].state = TASK_RUNNING;
    cur_task = next;
    ctx_switch(&tasks[prev].sp, &tasks[next].sp);
}

/* Defined in ctx_switch.S (Task 5) — temporary stub in start.S until then. */
extern void sched_dispatch_first(uint32_t new_sp);

void sched_start(void) {
    cur_task = 0;
    tasks[0].state = TASK_RUNNING;
    /* Unmask timer IRQ (bit 0). */
    uint32_t mask = ~1u;
    asm volatile (".insn r 0x0b, 0, 3, x0, %0, x0" :: "r"(mask));
    /* Arm timer: 270000 cycles = 100 Hz at 27 MHz. */
    uint32_t cnt = 270000;
    asm volatile (".insn r 0x0b, 0, 5, x0, %0, x0" :: "r"(cnt));
    sched_dispatch_first(tasks[0].sp);
    for (;;) {}
}
