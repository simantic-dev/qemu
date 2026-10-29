# TI AM64x (ti-am64x) machine notes

Status: WIP first pass. The machine builds. Zephyr, Linux and timing runs were not completed.

## Build
    mkdir build && cd build
    ../configure --target-list=aarch64-softmmu --enable-hvf --disable-docs --disable-rust
    ninja qemu-system-aarch64

## Run commands (untested past machine creation)
    qemu-system-aarch64 -M ti-am64x -accel tcg -nographic -kernel zephyr.elf -d unimp -D unimp.log
    qemu-system-aarch64 -M ti-am64x -accel hvf -nographic -kernel zephyr.elf
    # Linux: -kernel Image -initrd rootfs.cpio.gz -append "console=ttyS0" [-dtb k3-am642-sk.dtb]
Without -dtb the machine generates a minimal DT (cpus, timer, GICv3, 7 ns16550a UARTs, fixed clock).

## Results so far
- `-M ti-am64x` is listed and creates. It uses 2x A53, GICv3 (GICD 0x01800000, GICR 0x01840000), DDR 0x80000000, MSRAM 0x70000000, UART0-6 at 0x02800000 + n*0x10000 on SPI 178+n.
- The first TCG run with `-kernel zephyr.elf -serial mon:stdio -monitor none` aborted with
  "DEVICE: Object ... is not an instance of type device". With `-nographic -S` it starts fine.
  Not yet diagnosed; suspect the -serial/-monitor combination or the cpu/gic wiring after realize.
- HVF is broken at this QEMU commit for every machine, `virt` included. `hvf_arch_init_vcpu` asserts
  `!(ri->type & ARM_CP_NO_RAW)` on ID_AA64ISAR0_EL1 (FEAT_RNG_TRAP made it NO_RAW).
  Fixed in a separate commit by skipping NO_RAW regs. After the fix `virt -accel hvf -cpu host -S`
  starts. ti-am64x under HVF was not retested.
- HVF GICv3: the userspace GICv3 is the default under HVF (as in virt); `-accel hvf,kernel-irqchip=on` selects the macOS vGIC. Neither was tested for ti-am64x.
- TCG sets cntfrq to 200 MHz. HVF uses the host counter (24 MHz on Apple silicon), so Zephyr's 200 MHz assumption will run slow there.

## Not done
Zephyr hello/sync run, Linux (minimal DT and k3-am642-sk.dtb), timings, unimp access counts, UART output.

## Unimplemented blocks
`hw/arm/ti-am64x.c` has 505 TRM Table 2-1 MAIN blocks, named `am64x.<block>` and sorted largest first so small blocks win overlaps.
Count accesses with: `grep -o 'unimplemented device [a-z]* (size [0-9]*, offset' unimp.log` or `grep -c am64x.ctrl_mmr0 unimp.log`.
Blocks of interest: ctrl_mmr0_cfg0, psc0_psc, dmass0_sec_proxy_*, dmsc0_*, dmass0_intaggr_*, dmass0_bcdma_*, dmass0_pktdma_*, gpio0/1, i2c*, mcspi*, mcan*, timer*, mailbox0_regs*, cpsw0_cpsw3, pru_icssg*, mmcsd*_mmc_*, usb0_*, pcie0_*.
