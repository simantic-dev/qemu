# hello-imx93 — bare-metal LPUART console test

A minimal AArch64 bare-metal program that prints a string on the
**i.MX 93 11x11 EVK** machine's console (LPUART1) by writing bytes to the
LPUART `DATA` register. It exercises the `imx.lpuart` device model and the
SoC's LPUART wiring without needing a BSP, kernel, or rootfs.

## Build

```sh
make            # produces hello.elf and hello.bin
```

Requires an AArch64 toolchain (`aarch64-linux-gnu-` by default; override with
`make CROSS_COMPILE=...`).

## Run

```sh
qemu-system-aarch64 -M imx93-11x11-evk -nographic -kernel hello.elf
```

Expected output:

```
Hello from i.MX 93!
```

(The program then parks in a `wfi` loop; stop QEMU with `Ctrl-A x`.)

Both `hello.elf` and the flat `hello.bin` work — the linker script keeps
`_start` at file offset 0 and discards the allocatable GCC note section so
the flat image boots from its first byte.

## What it proves

- LPUART1 (`0x44380000`) is mapped as a live `imx.lpuart` region, not an
  unimplemented stub.
- A `DATA`-register write reaches the chardev backend (`serial_hd(0)`).
- `serial_hd(1)`/`serial_hd(2)` (LPUART2/3) are routed independently — a
  byte written to LPUART1 does not appear on the others.
