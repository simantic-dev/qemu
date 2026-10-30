# ti-boundary trace format (v1)

The A53 side (QEMU, `-M ti-am64x`) and the R5F side (pyrite) share one block:
`mailbox@29020000` (`am64x.mailbox0_regs2`, 0x200 bytes, DT SPI 80 and 81).
QEMU's `ti-boundary` device taps that block. The trace file is the only
interface between the two sides. No code is shared.

## Machine options

    qemu-system-aarch64 -M ti-am64x,boundary-record=boundary.trace ...   # record
    qemu-system-aarch64 -M ti-am64x,boundary-replay=boundary.trace ...   # replay

Recording and replay are exclusive. Without either option the tap is
passive: reads return 0 and writes are dropped.

## File layout

Text, one record per line, LF endings.

    ti-boundary-trace 1 base=0x29020000 size=0x200
    a <n> <r|w> <offset-hex> <size> <value-hex>
    i <n> <line> <level>

- Header: the version, then the block base and size. A replay with a
  different header is refused.
- `a`: one A-side access to the block. `n` is the boundary access count,
  1-based, sequential, with no gaps. `offset-hex` is relative to `base`.
  `size` is 1, 2 or 4. `value-hex` is the data written (for `w`) or the
  value returned to the guest (for `r`).
- `i`: a change of interrupt line `line` (0 = SPI 80, 1 = SPI 81) to
  `level` (0 or 1). `n` is the number of accesses completed when the change
  happened, so `i 0 ...` precedes the first access. `n` is nondecreasing.

## Keys

- A-side events (`a`, and `i` lines recorded in record mode) are keyed by
  the access count `n`. QEMU has no clock for the A side.
- R-side events are to be keyed by virtual time on the pyrite side. The
  pyrite side converts them into this format, ordered by access, before
  QEMU replays them. The conversion is pyrite's job and is not defined here.

## Replay rules (QEMU)

- Access k must match record k exactly: direction, offset and size. For a
  write the value must match too. A mismatch is fatal, so a divergence is
  never silently absorbed.
- A read returns the value of its record. This is the R-side response, matched
  by access order.
- `i` lines fire after access `n` completes. Lines with `n` = 0 fire at reset.

## Open

- The R-side trace (virtual-time keyed) is not written yet. It is pyrite's
  output, and the conversion to `a`/`i` records is not defined on the pyrite
  side.
- Only the mailbox is tapped. Other shared blocks need the same device with a
  different base, which is not implemented.

## Verified runs (2026-10-30, macOS HVF)

Kernel: 6.12.49-gdf24f9428e38 (`~/ahnaf/npu/Image`, same tree as lima `tilawa`),
DT: `k3-am642-sk.dtb` (stock, mailbox@29020000 `status = "okay"`).
The stock kernel has `CONFIG_OMAP2PLUS_MBOX=m` and no module, so the mailbox
driver is built out of tree in `tilawa` (`make M=drivers/mailbox
KBUILD_MODPOST_WARN=1 modules`; modversions are off) and loaded from an
initramfs with `busybox insmod /lib/omap-mailbox.ko`.

    qemu-system-aarch64 -M ti-am64x,boundary-record=rec.trace -accel hvf \
      -dtb k3-am642-sk.dtb -kernel Image -initrd initrd-mbox.cpio.gz -nographic \
      -serial mon:stdio -monitor none \
      -append "console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x2800000 rdinit=/bin/sh"
    # shell: busybox insmod /lib/omap-mailbox.ko; dmesg | grep mailbox

Recorded `rec.trace` (guest log: `omap-mailbox 29020000.mailbox: omap mailbox rev 0x0`):

    ti-boundary-trace 1 base=0x29020000 size=0x200
    a 1 r 0 4 0

The same file replayed with `boundary-replay=rec.trace` produces the same
probe with no divergence. No `i` lines were recorded: no R-side model is
attached, so nothing drives the interrupt inputs yet.

Parser test: `sh tests/ti-boundary/test-parse.sh <qemu> <scratch>` (good file
runs; gap, bad header, bad record and bad line are refused).
