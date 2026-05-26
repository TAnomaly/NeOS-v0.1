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

/* Defined in ctx_switch.S (Task 5). Args: addr of old sp, addr of new sp. */
extern void ctx_switch(uint32_t *old_sp, uint32_t *new_sp);

void irq_handler(void) {
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
    /* Arm timer: 270000 cycles to first IRQ (~100 Hz at 27 MHz). */
    uint32_t cnt = 270000;
    asm volatile (".insn r 0x0b, 0, 5, x0, %0, x0" :: "r"(cnt));
    sched_dispatch_first(tasks[0].sp);
    for (;;) {}
}
