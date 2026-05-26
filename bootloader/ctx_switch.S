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
