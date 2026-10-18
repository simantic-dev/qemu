MEMORY { ITCM (rx) : ORIGIN = 0x1FFE0000, LENGTH = 256K }
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > ITCM
    .text    : { *(.text*) *(.rodata*) } > ITCM
}
