.section .text.init
.globl _start
_start:
    .option push
    .option norvc
    j _real_start            /* 0x00: forced 4-byte JAL, jumps past IRQ vector */
    .option pop
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
    .insn r 0x0b, 0, 5, x0, x0, x0   /* timer x0, x0  -> set countdown=0 */
    .insn r 0x0b, 0, 2, x0, x0, x0   /* retirq */
