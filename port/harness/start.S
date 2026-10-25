@ Entry point for the test-harness build (runs under tools/harness.py).
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    ldr sp, =__stack_top
    @ Enable VFP: full access to cp10/cp11, then set FPEXC.EN.
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #(0xF << 20)
    mcr p15, 0, r0, c1, c0, 2
    mov r0, #0x40000000
    vmsr fpexc, r0
    @ Zero .bss (including the game's EWRAM/IWRAM sections).
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    bl HarnessMain
    @ HarnessMain never returns; if it does, report exit code 0.
    ldr r1, =0xF0000010
    mov r0, #0
    str r0, [r1]
2:  b 2b
