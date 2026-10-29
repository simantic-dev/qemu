# TI AM64x (ti-am64x) machine notes

Status: WIP first pass. The machine builds. Zephyr, Linux and timing runs were not completed.

## Build
    mkdir build && cd build
    ../configure --target-list=aarch64-softmmu --enable-hvf --disable-docs --disable-rust
    ninja qemu-system-aarch64

## Build note
macOS 27 SDK breaks hw/display/apple-gfx.m (PGTask_t); add `--disable-pvg` to the configure line.

## Run commands (tested)
    # Linux 6.12 arm64 Image + busybox initramfs, minimal generated DT, drops to a busybox shell (prompt "~ #")
    qemu-system-aarch64 -M ti-am64x -accel {tcg|hvf|hvf,kernel-irqchip=on} -kernel Image -initrd initrd.cpio.gz \
      -nographic -serial mon:stdio -monitor none -append "console=ttyS0 earlycon rdinit=/bin/sh" [-d unimp -D unimp.log]
    # Zephyr: -kernel zephyr.elf (untested)
Without -dtb the machine generates a minimal DT (cpus, timer, GICv3, 7 ns16550a UARTs, fixed clock).

## Results
- `-M ti-am64x` uses 2x A53, GICv3 (GICD 0x01800000, GICR 0x01840000), DDR 0x80000000, MSRAM 0x70000000, UART0-6 at 0x02800000 + n*0x10000 on SPI 178+n.
- The "DEVICE: Object ... is not an instance of type device" abort was `memory_region_init_ram(&s->msram, OBJECT(machine), ...)`:
  memory_region_register_ram() requires the owner to be NULL or a DeviceState. Fixed by passing NULL. It hit every run, with or without -S.
- Wall time to shell (python pty harness, process start to " # "): tcg 0.7s warm (3.5s cold first run), hvf 0.2-0.3s, hvf kernel-irqchip=on 0.2-0.5s. SMP 2 CPUs come up in all three.
- HVF default uses the userspace GICv3 and works. kernel-irqchip=on needed hv_gic_create() at the ti-am64x addresses (it was hardwired to virt's); fixed in target/arm/hvf/hvf.c via include/hw/arm/ti-am64x.h.
- HVF counter is 24 MHz (arch_timer reports it, Linux reads CNTFRQ); TCG sets 200 MHz. No DT clock-frequency needed.
- HVF itself needs the NO_RAW cpreg skip commit (7327c92a07) at this QEMU commit.
- Minimal-DT Linux boots to shell with zero unimp accesses (-d unimp log empty).
- Zephyr not tried.

## Real k3-am642-sk.dtb (-dtb)
dtb and Image come from ~/linux-imx in lima VM `tilawa` (arch/arm64/boot/dts/ti/k3-am642-sk.dtb; the Image has ARCH_K3, TI_SCI, 8250_OMAP). No build needed.
    -accel tcg -dtb k3-am642-sk.dtb -append "console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x2800000 rdinit=/bin/sh"
Kernel reaches init (3 s) but has no console: ti-sci probe fails (-22, "Unable to startup the chan") so UART clocks/power domains never come up;
optee probe also fails (-22). init then dies, panic. unimp hits: padcfg_ctrl0_cfg0 24, ctrl_mmr0_cfg0 12, dmass0_sec_proxy_scfg 1, dmass0_sec_proxy_rt 1.
Main blocker: TISCI over the DMASS secure proxy (message manager mailbox, DMSC firmware) needs a model.

## Unimplemented blocks
`hw/arm/ti-am64x.c` has 505 TRM Table 2-1 MAIN blocks, named `am64x.<block>` and sorted largest first so small blocks win overlaps.
Count accesses with: `grep -o 'unimplemented device [a-z]* (size [0-9]*, offset' unimp.log` or `grep -c am64x.ctrl_mmr0 unimp.log`.
Blocks of interest: ctrl_mmr0_cfg0, psc0_psc, dmass0_sec_proxy_*, dmsc0_*, dmass0_intaggr_*, dmass0_bcdma_*, dmass0_pktdma_*, gpio0/1, i2c*, mcspi*, mcan*, timer*, mailbox0_regs*, cpsw0_cpsw3, pru_icssg*, mmcsd*_mmc_*, usb0_*, pcie0_*.
