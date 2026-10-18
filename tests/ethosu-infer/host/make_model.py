#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# make_model.py - build a tiny int8 CNN for the QEMU i.MX93 Ethos-U65 demo.
#
# The model is a 16x16 grayscale, 2-class classifier: class 0 = "top half
# brighter", class 1 = "bottom half brighter". It is trained on synthetic data
# (seconds), then fully int8-quantized so every op maps to the Ethos-U65 NPU.
#
# Outputs (in this directory):
#   model_int8.tflite   - the quantized reference model (host golden runs this)
#   sample_top.bin       - a 16x16 int8 IFM, top brighter   (expected class 0)
#   sample_bottom.bin    - a 16x16 int8 IFM, bottom brighter (expected class 1)
#
# This is fork-only demo tooling, not an upstream QEMU deliverable.
import os
import numpy as np
import tensorflow as tf

HERE = os.path.dirname(os.path.abspath(__file__))
H = W = 16
np.random.seed(1234)
tf.random.set_seed(1234)


def gen(n):
    x = np.zeros((n, H, W, 1), np.float32)
    y = np.zeros((n,), np.int32)
    for i in range(n):
        base = np.random.uniform(0.1, 0.4)
        img = np.random.uniform(0.0, 0.2, (H, W, 1)).astype(np.float32) + base
        if i % 2 == 0:
            img[: H // 2] += np.random.uniform(0.3, 0.5)   # top brighter
            y[i] = 0
        else:
            img[H // 2:] += np.random.uniform(0.3, 0.5)     # bottom brighter
            y[i] = 1
        x[i] = np.clip(img, 0.0, 1.0)
    return x, y


def build():
    m = tf.keras.Sequential([
        tf.keras.layers.Input((H, W, 1)),
        tf.keras.layers.Conv2D(8, 3, padding="same", activation="relu"),
        tf.keras.layers.MaxPooling2D(2),
        tf.keras.layers.Conv2D(16, 3, padding="same", activation="relu"),
        tf.keras.layers.MaxPooling2D(2),
        # 1x4x4x16 -> 1x1x1x2. A valid-padded conv is mathematically the same
        # as flatten+dense, but (unlike Keras Flatten) maps 100% to the NPU -
        # so the NPU's OFM is the whole model output, nothing falls to the CPU.
        tf.keras.layers.Conv2D(2, 4, padding="valid"),
    ])
    return m


def main():
    xtr, ytr = gen(2000)
    xte, yte = gen(400)
    m = build()   # base: ends at Conv2D, output (1,1,1,2), 100% NPU-mappable
    # Train through a wrapper that squeezes to (2,) so the loss is happy; the
    # wrapper shares the base's layers, so training updates the base in place.
    trainer = tf.keras.Sequential([m, tf.keras.layers.Reshape((2,))])
    trainer.compile(optimizer="adam",
                    loss=tf.keras.losses.SparseCategoricalCrossentropy(from_logits=True),
                    metrics=["accuracy"])
    trainer.fit(xtr, ytr, epochs=6, batch_size=64, verbose=2,
                validation_data=(xte, yte))

    def rep():
        for i in range(200):
            yield [xtr[i:i + 1]]

    conv = tf.lite.TFLiteConverter.from_keras_model(m)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()
    out = os.path.join(HERE, "model_int8.tflite")
    open(out, "wb").write(tfl)
    print("wrote", out, len(tfl), "bytes")

    # Emit two int8 sample inputs at the model's input quantization.
    interp = tf.lite.Interpreter(model_content=tfl)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    outp = interp.get_output_details()[0]
    s, zp = inp["quantization"]

    def to_int8(imgf):
        q = np.round(imgf / s + zp)
        return np.clip(q, -128, 127).astype(np.int8)

    def run(q):
        interp.set_tensor(inp["index"], q.reshape(inp["shape"]))
        interp.invoke()
        return interp.get_tensor(outp["index"]).flatten()

    top = np.full((H, W, 1), 0.15, np.float32); top[: H // 2] += 0.45
    bot = np.full((H, W, 1), 0.15, np.float32); bot[H // 2:] += 0.45
    for name, imgf, exp in [("sample_top", top, 0), ("sample_bottom", bot, 1)]:
        q = to_int8(np.clip(imgf, 0, 1))
        q.tofile(os.path.join(HERE, name + ".bin"))
        logits = run(q)
        print(f"{name}: int8 logits={logits.tolist()} argmax={int(np.argmax(logits))} (expect {exp})")
    print("input quant: scale=%.6f zero=%d shape=%s" % (s, zp, inp["shape"].tolist()))
    print("output quant:", outp["quantization"], "shape", outp["shape"].tolist())


if __name__ == "__main__":
    main()
