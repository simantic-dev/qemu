#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# ethosu_host_infer.py - host TFLite reference for the QEMU i.MX93 Ethos-U65
# fork-only inference demo.
#
# The QEMU NPU model (hw/misc/imx93_ethosu.c) execs this when the M33 firmware
# kicks the NPU: it reads the guest's IFM out of guest memory, hands it here,
# and we run the *reference* int8 TFLite model on the host to produce the exact
# OFM the real silicon would. QEMU writes that OFM back into guest memory and
# raises the NPU completion IRQ. This is why the result is correct for any input
# - it is a real inference, just executed on the host instead of in a modelled
# NPU compute engine.
#
#   usage: ethosu_host_infer.py <model_int8.tflite> <ifm.bin> <ofm.bin>
#
# Reads raw int8 IFM bytes from ifm.bin, writes raw int8 OFM bytes to ofm.bin.
# Not an upstream QEMU deliverable.
import sys
import numpy as np

try:
    from tflite_runtime.interpreter import Interpreter
except ImportError:
    import tensorflow as tf
    Interpreter = tf.lite.Interpreter


def main():
    if len(sys.argv) != 4:
        sys.stderr.write("usage: %s model.tflite ifm.bin ofm.bin\n" % sys.argv[0])
        return 2
    model, ifm_path, ofm_path = sys.argv[1:4]

    interp = Interpreter(model_path=model)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    outp = interp.get_output_details()[0]

    ifm = np.fromfile(ifm_path, dtype=np.int8)
    n = int(np.prod(inp["shape"]))
    if ifm.size < n:
        sys.stderr.write("ifm too small: %d < %d\n" % (ifm.size, n))
        return 1
    interp.set_tensor(inp["index"], ifm[:n].reshape(inp["shape"]))
    interp.invoke()
    ofm = interp.get_tensor(outp["index"]).astype(np.int8).flatten()
    ofm.tofile(ofm_path)
    sys.stderr.write("host-infer: ifm=%dB ofm=%dB values=%s\n"
                     % (n, ofm.size, ofm.tolist()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
