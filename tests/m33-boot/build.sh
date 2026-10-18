#!/usr/bin/env bash
# Build the minimal M33 bring-up blob (needs arm-none-eabi-gcc).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles \
    -ffreestanding -Os -Wall -T "$HERE/m33.ld" "$HERE/m33_fw.c" \
    -o "$HERE/m33_fw.elf"
arm-none-eabi-objcopy -O binary "$HERE/m33_fw.elf" "$HERE/m33_fw.bin"
echo "built m33_fw.bin ($(stat -c%s "$HERE/m33_fw.bin") bytes)"
