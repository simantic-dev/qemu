/*
 * Bare-metal "hello world" for the QEMU imx93-11x11-evk machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Writes a string to LPUART1 (the EVK console) by storing bytes to the
 * LPUART DATA register. No clock/baud setup is needed: the QEMU imx.lpuart
 * model pushes each DATA write straight to the chardev backend.
 *
 * Run with:
 *   qemu-system-aarch64 -M imx93-11x11-evk -nographic -kernel hello.bin
 */

#define LPUART1_BASE    0x44380000
#define LPUART_DATA     0x1C

    .section .text._start, "ax"
    .global _start
_start:
    ldr     x0, =(LPUART1_BASE + LPUART_DATA)
    adr     x1, msg
1:
    ldrb    w2, [x1], #1
    cbz     w2, 2f
    str     w2, [x0]
    b       1b
2:
    wfi
    b       2b

    .section .rodata
msg:
    .asciz "Hello from i.MX 93!\n"
