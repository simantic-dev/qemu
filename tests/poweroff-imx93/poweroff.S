/*
 * Bare-metal PSCI SYSTEM_OFF test for the QEMU imx93-11x11-evk machine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Prints a marker on LPUART1 then issues PSCI SYSTEM_OFF via SMC. If the
 * machine wires PSCI poweroff to a QEMU shutdown, QEMU exits 0.
 */
#define LPUART1_DATA   0x4438001C
#define PSCI_SYSTEM_OFF 0x84000008

    .section .text._start, "ax"
    .global _start
_start:
    ldr     x0, =LPUART1_DATA
    adr     x1, msg
1:  ldrb    w2, [x1], #1
    cbz     w2, 2f
    str     w2, [x0]
    b       1b
2:  ldr     w0, =PSCI_SYSTEM_OFF
    smc     #0
3:  wfi
    b       3b
    .section .rodata
msg: .asciz "PSCI SYSTEM_OFF...\n"
