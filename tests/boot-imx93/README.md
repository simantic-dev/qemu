# boot-imx93 — boot real i.MX 93 Linux

Boots an NXP i.MX 93 BSP kernel on the QEMU `imx93-11x11-evk` machine.

Unlike the i.MX 95, the i.MX 93 has **no System Manager** — there is no M33
SM firmware to load and no SCMI server. Linux programs the CCM/ANATOP
directly, so this is a plain `kernel + DTB (+ optional rootfs)` boot.

## Usage

```sh
# Defaults point at the imx93evk Yocto deploy dir this project builds.
tests/boot-imx93/run.sh

# Or point at artifacts explicitly:
QEMU=./build-imx93/qemu-system-aarch64 \
KERNEL=/path/to/Image \
DTB=/path/to/imx93-11x11-evk.dtb \
INITRD=/path/to/rootfs.cpio.gz \
    tests/boot-imx93/run.sh
```

`INITRD` is **optional**. With no rootfs the kernel boots all the way through
driver probe and only panics at *"Unable to mount root fs"* — which is exactly
the bring-up triage data we want (does the console come up? do CCM/ANATOP/GIC/
timer probe? where does it die?).

## Knobs

- `ICOUNT=1` — enable `-icount shift=auto` (debug determinism for timer/IRQ
  races; off by default).
- `CMDLINE=...` — override the kernel command line. Default enables
  `earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1`.
- Extra QEMU args (e.g. `-d unimp,guest_errors`) pass through after the
  script name.

Stop QEMU with `Ctrl-A x`.
