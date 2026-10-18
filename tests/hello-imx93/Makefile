# SPDX-License-Identifier: GPL-2.0-or-later
# Bare-metal LPUART hello-world for the QEMU imx93-11x11-evk machine.

CROSS_COMPILE ?= aarch64-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy

all: hello.bin

hello.elf: hello.S link.ld
	$(CC) -nostdlib -static -Wl,--build-id=none -Wl,-T,link.ld -o $@ hello.S

hello.bin: hello.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f hello.elf hello.bin

.PHONY: all clean
