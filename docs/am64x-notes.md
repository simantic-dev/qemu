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
Stock DT now boots to a `~ #` shell on ttyS2 (8250_omap UART needs nothing extra; the ns16550a model is enough).
Command (dtb at ~/ahnaf/am64x-linux/k3-am642-sk.dtb, kernel/initrd as above, accel tcg or hvf):
    qemu-system-aarch64 -M ti-am64x -accel tcg -dtb k3-am642-sk.dtb -kernel Image -initrd initrd.cpio.gz -nographic -serial mon:stdio -monitor none \
      -append "console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x2800000 rdinit=/bin/sh" [-d unimp -D unimp.log]
Time to shell (process start to prompt, pty harness): tcg 4.6 s, hvf 4.6 s (kernel timestamp of "Run /bin/sh" 4.0 s tcg / 2.8 s hvf; dominated by 500 ms probe timeouts in cadence-qspi, cpsw, sdhci).

Modelled in `hw/misc/ti-sci-stub.c`:
- `ti-sci-stub`: secure proxy (target_data 0x4d000000, rt 0x4a600000, scfg 0x4a400000, rx IRQ SPI 34) plus a fake DMSC. Tx thread 13 (one credit always),
  rx thread 12 (dir bit 31 in scfg ctrl, count in rt status); the write to the last data word (reg 14) runs the request, the read of the last word pops the response and drops the IRQ.
  Every request is ACKed with a zeroed payload; specials: VERSION (ABI 3.1), GET_DEVICE_STATE (ON), GET_CLOCK_STATE (ready), GET_CLOCK_FREQ (48 MHz), QUERY_CLOCK_FREQ (target), GET_RESOURCE_RANGE (0,64).
- `ti-am64x-i2c-id`: ID/SYSS-only OMAP I2C v2 stub on the 6 I2C controllers. Without it i2c-omap oopses (unaligned write through the scheme-0 register map when REVNB reads 0). No bus transfers.

Remaining failed probes (dmesg, same on tcg and hvf): optee (-22, no TEE), clk "failed to reparent" (assigned-clock-parents; stub reports 0 clock parents),
pca953x 1-0070 (-16, i2c not modelled), am65-cpsw-nuss (-22, "error initializing ale engine", mdio idle timeout), cadence-qspi (busy timeout, FIFO depth 1), sdhci-am654 fa10000.mmc (-110).
Top unimp hits over a boot: fss0_ospi_flash_cfg reads 761550 tcg / 461012 hvf (qspi poll loops), cpsw0_cpsw3 reads 33842 / 31786, padcfg_ctrl0_cfg0 88 / 77 each r+w, ctrl_mmr0_cfg0 15 reads, gpio0/gpio1 writes 18/13.

## Unimplemented blocks
`hw/arm/ti-am64x.c` has 505 TRM Table 2-1 MAIN blocks, named `am64x.<block>` and sorted largest first so small blocks win overlaps.
Count accesses with: `grep -o 'unimplemented device [a-z]* (size [0-9]*, offset' unimp.log` or `grep -c am64x.ctrl_mmr0 unimp.log`.
Blocks of interest: ctrl_mmr0_cfg0, psc0_psc, dmass0_sec_proxy_*, dmsc0_*, dmass0_intaggr_*, dmass0_bcdma_*, dmass0_pktdma_*, gpio0/1, i2c*, mcspi*, mcan*, timer*, mailbox0_regs*, cpsw0_cpsw3, pru_icssg*, mmcsd*_mmc_*, usb0_*, pcie0_*.

## Zephyr results
Board: sk_am64/am6442/a53 (simantic-dev/zephyr agent/am64-a53-board-c5e2, fix 536d7e590ed). Zephyr v4.4.0 checkout 413b789deb3 is not the board branch; the branch pins SDK 1.0.1, so use that SDK (aarch64 toolchain under gnu/).
Build: `west build -p always -b sk_am64/am6442/a53 samples/hello_world -d build-hello` (same for synchronization, build-sync).

- Blocker fixed in board DT: main_mmr_cfg0 reg was 32 KiB but the SoC unlocks partitions at 0x43009008. Data abort at FAR 0x43009008 in k3_unlock_all_ctrl_partitions, then fatal. Board now sets 128 KiB.
- tcg, hello_world: `Booting Zephyr OS build v4.4.0-3839-g68983e7b7f66`, `Secondary CPU core 1 (MPID:0x1) is up`, `Hello World! sk_am64/am6442/a53`. Output within ~0.1 s of start.
- tcg, synchronization: thread_a/thread_b alternate, one line every ~0.72-0.75 s on this host (nominal 0.6 s: 100 ms busy + 500 ms sleep per turn).
- hvf root cause (fixed in zephyr agent/am64-a53-board-c5e2): Zephyr's `z_arm64_el_highest_init()` writes CNTFRQ_EL0 at entry because ID_AA64PFR0 shows no EL2/EL3, so EL1 counts as the highest EL. That write is UNDEFINED under hvf (the guest is not allowed to set CNTFRQ). The guest took EC=0 at 0x82002448, vectored through VBAR=0 and the vCPU then sat at PC 0 taking stage-2 aborts (the `ifsc=6` / `ec=0x20` in QEMU's log; QEMU's env->pc was stale, so the logged entry PC was misleading). Nothing is wrong with the memory map or the ELF load.
  Fix: the A53 SoC selects TIMER_READS_ITS_FREQUENCY_AT_RUNTIME (cycles/s is read from CNTFRQ, 24 MHz on hvf, 200 MHz on tcg) and reset.c no longer writes CNTFRQ in that case. No QEMU change needed.
- After the fix, both samples work on both accels: `Booting Zephyr OS...`, `Secondary CPU core 1 (MPID:0x1) is up`, `Hello World! sk_am64/am6442/a53`, and synchronization alternates cpu 0/cpu 1.
  Run: `qemu-system-aarch64 -M ti-am64x -accel {tcg|hvf} -nographic -serial mon:stdio -monitor none -kernel build-hello/zephyr/zephyr.elf`.
- Timing (line period, tcg / hvf): 0.72-0.75 s / 0.73-0.75 s. The guest is not the cause: sleep(500 ms) reports 5000 requested ticks but ~6450 elapse, and the QEMU wfi timer (500.04 ms of virtual time) fires 630-650 ms later. QEMU's main loop waits in g_poll/select, and on this macOS host a plain `select()` with a 0.5 s timeout takes 0.62-0.65 s (python3 -c, outside QEMU, sandbox or not) because of timer coalescing. With a spinning thread (no host sleep) hvf sleep(500) is exactly 5001 ticks. So expect ~0.6 s/line on a host without that slack.
- `-smp 1` aborts on tcg (`tcg_register_thread: n < tcg_max_ctxs`; the machine always creates 2 CPUs) and is ignored on hvf. Not fixed.
