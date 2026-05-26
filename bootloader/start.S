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

.globl irq_trampoline
irq_trampoline:
    /* On entry (picorv32 IRQ): q0=return PC, q1=irq mask.
     * picorv32 only banks q-regs; normal regs are NOT auto-saved.
     * Save ra + all caller-saved regs (a0-a7, t0-t6) on current task's stack.
     *
     * 16 caller-saved regs + ra = 17 words = 68 bytes. Round to 16-byte
     * alignment: 80 bytes total (sp+64..76 used for q0 save + pad).
     */
    addi sp, sp, -80
    sw   ra,  0(sp)
    sw   t0,  4(sp)
    sw   t1,  8(sp)
    /* DIAGNOSTIC: prove IRQ entry. OR LED bit 0 (preserves other bits). */
    li   t0, 0x10000010
    lw   t1, 0(t0)
    ori  t1, t1, 0x01
    sw   t1, 0(t0)
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
    /* Save q0 (interrupted PC) into the per-task IRQ frame. */
    .insn r 0x0b, 0, 0, t0, x0, x0   /* getq t0, q0 */
    sw   t0, 64(sp)
    /* sp+68..76 reserved/pad */

    call irq_handler

.globl irq_trampoline_tail
irq_trampoline_tail:
    /* Reload timer countdown: 270000 cycles = 100 Hz at 27 MHz. */
    li   t0, 270000
    .insn r 0x0b, 0, 5, x0, t0, x0   /* timer x0, t0 -> arms countdown */

    /* Restore q0 from the (possibly new) task's IRQ frame. */
    lw   t0, 64(sp)
    .insn r 0x0b, 0, 1, x0, t0, x0   /* setq q0, t0 */

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
