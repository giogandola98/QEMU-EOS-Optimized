/*
 * CPU-bound bare-metal ARMv5 loop for TCG speed measurements (bench/microbench.sh), no ROM needed.
 * Each iteration: a load/add/store on RAM, a call + return (two more TBs, one indirect jump, like
 * DryOS code), a conditional branch. Exits through semihosting, so wall time = emulation time.
 * Instructions per iteration: loop 7 + func 3 = 10; total = ITERS * 10 + a few setup instructions.
 */
        .equ ITERS, 60000000
        .equ SYS_EXIT, 0x18
        .equ ADP_Stopped_ApplicationExit, 0x20026

        .text
        .arm
        .global _start
_start:
        ldr     r0, =ITERS
        ldr     r2, =buf
        mov     r1, #0
loop:
        ldr     r3, [r2]
        add     r3, r3, r0
        str     r3, [r2]
        bl      func
        add     r1, r1, #1
        subs    r0, r0, #1
        bne     loop

        mov     r0, #SYS_EXIT
        ldr     r1, =ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .

func:
        eor     r3, r3, r1
        add     r3, r3, #1
        bx      lr

        .data
        .align  2
buf:    .word   0
