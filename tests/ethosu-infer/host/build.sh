#!/usr/bin/env bash
#
# Build the fork-only Ethos-U65 inference demo's host assets:
#   model_int8.tflite        - the int8 reference model (host golden runs this)
#   model_int8_vela.tflite   - Vela-compiled for ethos-u65-256 (the M33 runs this)
#   sample_top.bin / sample_bottom.bin - two 16x16 int8 input images
#
# Requires (host): tensorflow-cpu, ethos-u-vela==4.3.0, tflite-runtime, numpy<2.
# See README.md. Not an upstream QEMU deliverable.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

VELA=${VELA:-$HOME/.local/bin/vela}
ACC=${ACC:-ethos-u65-256}

echo "[1/2] training + quantizing the model..."
TF_CPP_MIN_LOG_LEVEL=3 python3 make_model.py

echo "[2/2] compiling with Vela ($ACC, Shared_Sram)..."
"$VELA" --accelerator-config "$ACC" --memory-mode Shared_Sram \
    --system-config Ethos_U65_High_End \
    --output-dir "$HERE" model_int8.tflite >/dev/null

echo "done:"
ls -la "$HERE"/model_int8.tflite "$HERE"/model_int8_vela.tflite \
       "$HERE"/sample_top.bin "$HERE"/sample_bottom.bin
