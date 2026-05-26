#ifndef SCHED_H
#define SCHED_H
#include <stdint.h>

#define MAX_TASKS 2
#define STACK_SZ  2048

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
