/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Place the bare-metal image at the start of i.MX 93 DDR (0x8000_0000),
 * which is arm_load_kernel()'s loader_start for the imx93-11x11-evk machine.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    /* Keep _start at file offset 0 so the flat .bin boots correctly. */
    .text : { *(.text._start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }

    /* GCC emits an allocatable .note.gnu.property (BTI/PAC); dropping it
     * keeps the raw binary from starting with a note instead of code. */
    /DISCARD/ : { *(.note*) *(.comment) *(.ARM.attributes) }
}
