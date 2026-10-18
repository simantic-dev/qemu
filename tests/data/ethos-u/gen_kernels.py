#!/usr/bin/env python3
# Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Generate committed golden vectors for the Ethos-U int8 compute kernels
# (hw/npu/ethos_u_kernels.c), emitting tests/unit/test-ethos-u-kernels-vectors.h.
#
# For each op a tiny single-layer model is built, fully int8-quantized with
# TensorFlow's converter and run with the tflite runtime; the quantized tensors
# and per-channel scales are captured. The per-channel (multiplier, shift) the
# kernel needs are derived from the captured scales with Vela's quantise_scale
# (which equals gemmlowp QuantizeMultiplier for positive significands - so it
# matches the values Vela writes to guest memory).
#
# The committed golden is the TFLite-Micro *reference* kernel output, computed
# here by a reference pipeline (the same integer math the C kernels and the
# Ethos-U datapath implement). That reference is cross-checked at generation
# time against the tflite runtime's own output and must agree to within 1 LSB -
# tflite's desktop runtime uses optimized (ruy/NEON) kernels that differ from
# the reference, and from Ethos-U, by at most 1 LSB, so the reference (not the
# optimized output) is the correct bit-exact oracle. The C unit test then
# requires an exact match against this committed reference golden.
#
# Requires: tensorflow (full, for the converter), numpy.
# Re-run after changing cases:  python3 tests/data/ethos-u/gen_kernels.py

import math
import os

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
import numpy as np
import tensorflow as tf

OUT = "tests/unit/test-ethos-u-kernels-vectors.h"
RNG = np.random.default_rng(93)


def quantise_scale(scale):
    """Vela scaling.quantise_scale -> (Q31 multiplier, shift=31-exponent)."""
    significand, exponent = math.frexp(scale)
    s = significand * (1 << 31)
    q = int(math.floor(s + 0.5)) if s >= 0 else int(math.ceil(s - 0.5))
    return q, 31 - exponent


def gemmlowp_shift(scale):
    """gemmlowp-style (multiplier, shift) with shift = 31 - vela_shift."""
    m, vs = quantise_scale(scale)
    return m, 31 - vs


# ---- reference integer pipeline (mirrors hw/npu/ethos_u_{requant,kernels}.c) --
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1


def i32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x >= (1 << 31) else x


def _ctrunc(n, d):
    q = abs(n) // d
    return q if n >= 0 else -q


def srdhm(a, b):
    a, b = i32(a), i32(b)
    overflow = a == b == INT32_MIN
    ab = a * b
    nudge = (1 << 30) if ab >= 0 else (1 - (1 << 30))
    return INT32_MAX if overflow else i32(_ctrunc(ab + nudge, 1 << 31))


def rdpot(x, exp):
    x = i32(x)
    if exp <= 0:
        return x
    mask = (1 << exp) - 1
    rem = x & mask
    thr = (mask >> 1) + (1 if x < 0 else 0)
    return (x >> exp) + (1 if rem > thr else 0)


def mqm(x, mult, shift):
    left = shift if shift > 0 else 0
    right = 0 if shift > 0 else -shift
    return rdpot(srdhm(i32(x * (1 << left)), mult), right)


def requant_vela(x, mult, vshift):
    return mqm(x, i32(mult), 31 - vshift)


def clamp8(v, lo, hi):
    return max(lo, min(hi, v))


def ref_conv(ifm, w, sb, p, depthwise):
    ih, iw, ic = p["ifm_h"], p["ifm_w"], p["ifm_c"]
    oh, ow, oc = p["ofm_h"], p["ofm_w"], p["ofm_c"]
    kh, kw = p["kh"], p["kw"]
    sy, sx = p["stride_y"], p["stride_x"]
    pt, pl = p["pad_top"], p["pad_left"]
    izp, ozp = p["ifm_zp"], p["ofm_zp"]
    out = []
    for oy in range(oh):
        for ox in range(ow):
            for o in range(oc):
                acc = sb[o][0]
                for ky in range(kh):
                    iy = oy * sy - pt + ky
                    if iy < 0 or iy >= ih:
                        continue
                    for kx in range(kw):
                        ix = ox * sx - pl + kx
                        if ix < 0 or ix >= iw:
                            continue
                        if depthwise:
                            acc += (int(ifm[(iy * iw + ix) * ic + o]) - izp) \
                                * int(w[(ky * kw + kx) * oc + o])
                        else:
                            for c in range(ic):
                                acc += (int(ifm[(iy * iw + ix) * ic + c]) - izp) \
                                    * int(w[((o * kh + ky) * kw + kx) * ic + c])
                v = requant_vela(int(acc), sb[o][1], sb[o][2]) + ozp
                out.append(clamp8(v, p["act_min"], p["act_max"]))
    return out


def ref_pool(ifm, p, is_max):
    ih, iw, c = p["ifm_h"], p["ifm_w"], p["c"]
    oh, ow = p["ofm_h"], p["ofm_w"]
    kh, kw = p["kh"], p["kw"]
    sy, sx = p["stride_y"], p["stride_x"]
    pt, pl = p["pad_top"], p["pad_left"]
    out = []
    for oy in range(oh):
        for ox in range(ow):
            for ch in range(c):
                acc = INT32_MIN if is_max else 0
                cnt = 0
                for ky in range(kh):
                    iy = oy * sy - pt + ky
                    if iy < 0 or iy >= ih:
                        continue
                    for kx in range(kw):
                        ix = ox * sx - pl + kx
                        if ix < 0 or ix >= iw:
                            continue
                        val = int(ifm[(iy * iw + ix) * c + ch])
                        acc = max(acc, val) if is_max else acc + val
                        cnt += 1
                if is_max:
                    v = acc
                else:
                    # truncate toward zero (TFLM AveragePool), not floor
                    v = _ctrunc(acc + cnt // 2, cnt) if acc > 0 \
                        else _ctrunc(acc - cnt // 2, cnt)
                out.append(clamp8(v, p["act_min"], p["act_max"]))
    return out


def ref_add(a, b, p):
    out = []
    for i in range(p["n"]):
        av = (int(a[i]) - p["in1_zp"]) * (1 << p["left_shift"])
        bv = (int(b[i]) - p["in2_zp"]) * (1 << p["left_shift"])
        sa = mqm(av, p["in1_mult"], p["in1_shift"])
        sb_ = mqm(bv, p["in2_mult"], p["in2_shift"])
        v = mqm(sa + sb_, p["out_mult"], p["out_shift"]) + p["out_zp"]
        out.append(clamp8(v, p["act_min"], p["act_max"]))
    return out


def ref_mul(a, b, p):
    out = []
    for i in range(p["n"]):
        prod = (int(a[i]) - p["in1_zp"]) * (int(b[i]) - p["in2_zp"])
        v = mqm(prod, p["out_mult"], p["out_shift"]) + p["out_zp"]
        out.append(clamp8(v, p["act_min"], p["act_max"]))
    return out


def cross_check(name, ref, tfl):
    md = max(abs(int(r) - int(t)) for r, t in zip(ref, tfl)) if ref else 0
    assert md <= 1, f"{name}: reference vs tflite diff {md} > 1 LSB"
    return md


def same_pad(in_sz, k, stride, dilation=1):
    out = (in_sz + stride - 1) // stride
    eff_k = (k - 1) * dilation + 1
    total = max((out - 1) * stride + eff_k - in_sz, 0)
    return out, total // 2


def quantize_model(build, inputs_shapes):
    """build() -> keras model; return interpreter after int8 conversion."""
    model = build()

    def rep():
        for _ in range(64):
            yield [RNG.uniform(-1, 1, (1, *s)).astype(np.float32)
                   for s in inputs_shapes]

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()
    interp = tf.lite.Interpreter(model_content=tfl)
    interp.allocate_tensors()
    return interp


def tensors_by(interp):
    return {d["index"]: d for d in interp.get_tensor_details()}


def find(details, dtype, ndim, exclude):
    out = []
    for idx, d in details.items():
        if idx in exclude:
            continue
        q = d["quantization_parameters"]
        if d["dtype"] == dtype and len(d["shape"]) == ndim \
                and d["shape"].size and len(q["scales"]):
            out.append(d)
    return out


def c_arr(t, name, vals, fmt="%d"):
    body = ", ".join(fmt % v for v in vals)
    return f"static const {t} {name}[] = {{ {body} }};"


def sb_struct(name, biases, mults, vshifts):
    rows = []
    for b, m, s in zip(biases, mults, vshifts):
        rows.append(f"    {{ {int(b)}LL, {m & 0xffffffff}u, {s} }},")
    return (f"static const EthosUScaleBias {name}[] = {{\n"
            + "\n".join(rows) + "\n};")


def gen_conv(out, name, ih, iw, ic, oc, k, stride, padding, depthwise=False):
    shape = (ih, iw, ic)

    def build():
        x = inp = tf.keras.Input(shape=shape)
        if depthwise:
            x = tf.keras.layers.DepthwiseConv2D(
                k, strides=stride, padding=padding, use_bias=True)(x)
        else:
            x = tf.keras.layers.Conv2D(
                oc, k, strides=stride, padding=padding, use_bias=True)(x)
        return tf.keras.Model(inp, x)

    interp = quantize_model(build, [shape])
    det = tensors_by(interp)
    inp_d = interp.get_input_details()[0]
    out_d = interp.get_output_details()[0]

    x_i8 = RNG.integers(-128, 128, (1, ih, iw, ic), dtype=np.int8)
    interp.set_tensor(inp_d["index"], x_i8)
    interp.invoke()
    y = interp.get_tensor(out_d["index"])[0]   # NHWC int8

    exclude = {inp_d["index"], out_d["index"]}
    w_d = find(det, np.int8, 4, exclude)[0]
    b_d = find(det, np.int32, 1, exclude)[0]
    w = interp.get_tensor(w_d["index"])
    b = interp.get_tensor(b_d["index"])
    in_scale = inp_d["quantization_parameters"]["scales"][0]
    in_zp = int(inp_d["quantization_parameters"]["zero_points"][0])
    out_scale = out_d["quantization_parameters"]["scales"][0]
    out_zp = int(out_d["quantization_parameters"]["zero_points"][0])
    w_scales = w_d["quantization_parameters"]["scales"]

    n_oc = w.shape[3] if depthwise else w.shape[0]
    mults, vshifts = [], []
    for c in range(n_oc):
        m, vs = quantise_scale(in_scale * float(w_scales[c]) / out_scale)
        mults.append(m)
        vshifts.append(vs)

    oh, ow = y.shape[0], y.shape[1]
    if padding == "same":
        _, pt = same_pad(ih, k, stride)
        _, pl = same_pad(iw, k, stride)
    else:
        pt = pl = 0

    pdict = {"ifm_h": ih, "ifm_w": iw, "ifm_c": ic, "ofm_h": oh, "ofm_w": ow,
             "ofm_c": n_oc, "kh": k, "kw": k, "stride_y": stride,
             "stride_x": stride, "pad_top": pt, "pad_left": pl,
             "ifm_zp": in_zp, "ofm_zp": out_zp, "act_min": -128, "act_max": 127}
    sb = [(int(b[c]), m & 0xffffffff, vshifts[c])
          for c, m in enumerate(mults)]
    ref = ref_conv(x_i8.reshape(-1), w.reshape(-1), sb, pdict, depthwise)
    md = cross_check(name, ref, y.reshape(-1))

    out.append(f"/* {name}: {'depthwise' if depthwise else 'conv'} "
               f"{ih}x{iw}x{ic} k{k} s{stride} {padding} -> {oh}x{ow}x{n_oc} "
               f"(tflite diff <= {md}) */")
    out.append(c_arr("int8_t", f"{name}_ifm", x_i8.reshape(-1)))
    out.append(c_arr("int8_t", f"{name}_w", w.reshape(-1)))
    out.append(sb_struct(f"{name}_sb", b, mults, vshifts))
    out.append(c_arr("int8_t", f"{name}_golden", ref))
    out.append(f"static const EthosUConvParams {name}_p = {{ "
               f".ifm_h={ih}, .ifm_w={iw}, .ifm_c={ic}, "
               f".ofm_h={oh}, .ofm_w={ow}, .ofm_c={n_oc}, "
               f".kh={k}, .kw={k}, .stride_y={stride}, .stride_x={stride}, "
               f".dilation_y=1, .dilation_x=1, .pad_top={pt}, .pad_left={pl}, "
               f".ifm_zp={in_zp}, .ofm_zp={out_zp}, "
               f".act_min=-128, .act_max=127 }};")
    out.append("")
    return name, depthwise


def gen_pool(out, name, ih, iw, c, k, stride, kind):
    shape = (ih, iw, c)

    def build():
        inp = tf.keras.Input(shape=shape)
        if kind == "max":
            x = tf.keras.layers.MaxPool2D(k, strides=stride,
                                          padding="valid")(inp)
        else:
            x = tf.keras.layers.AveragePooling2D(k, strides=stride,
                                                 padding="valid")(inp)
        return tf.keras.Model(inp, x)

    interp = quantize_model(build, [shape])
    inp_d = interp.get_input_details()[0]
    out_d = interp.get_output_details()[0]
    x_i8 = RNG.integers(-128, 128, (1, ih, iw, c), dtype=np.int8)
    interp.set_tensor(inp_d["index"], x_i8)
    interp.invoke()
    y = interp.get_tensor(out_d["index"])[0]
    oh, ow = y.shape[0], y.shape[1]

    pdict = {"ifm_h": ih, "ifm_w": iw, "c": c, "ofm_h": oh, "ofm_w": ow,
             "kh": k, "kw": k, "stride_y": stride, "stride_x": stride,
             "pad_top": 0, "pad_left": 0, "act_min": -128, "act_max": 127}
    ref = ref_pool(x_i8.reshape(-1), pdict, kind == "max")
    md = cross_check(name, ref, y.reshape(-1))

    out.append(f"/* {name}: {kind}pool {ih}x{iw}x{c} k{k} s{stride} "
               f"-> {oh}x{ow}x{c} (tflite diff <= {md}) */")
    out.append(c_arr("int8_t", f"{name}_ifm", x_i8.reshape(-1)))
    out.append(c_arr("int8_t", f"{name}_golden", ref))
    out.append(f"static const EthosUPoolParams {name}_p = {{ "
               f".ifm_h={ih}, .ifm_w={iw}, .c={c}, .ofm_h={oh}, .ofm_w={ow}, "
               f".kh={k}, .kw={k}, .stride_y={stride}, .stride_x={stride}, "
               f".pad_top=0, .pad_left=0, .act_min=-128, .act_max=127 }};")
    out.append("")
    return name, kind


def gen_add(out, name, n):
    shape = (n,)

    def build():
        a = tf.keras.Input(shape=shape)
        b = tf.keras.Input(shape=shape)
        return tf.keras.Model([a, b], tf.keras.layers.Add()([a, b]))

    interp = quantize_model(build, [shape, shape])
    ins = interp.get_input_details()
    out_d = interp.get_output_details()[0]
    a_i8 = RNG.integers(-128, 128, (1, n), dtype=np.int8)
    b_i8 = RNG.integers(-128, 128, (1, n), dtype=np.int8)
    interp.set_tensor(ins[0]["index"], a_i8)
    interp.set_tensor(ins[1]["index"], b_i8)
    interp.invoke()
    y = interp.get_tensor(out_d["index"])[0]

    s1 = ins[0]["quantization_parameters"]["scales"][0]
    z1 = int(ins[0]["quantization_parameters"]["zero_points"][0])
    s2 = ins[1]["quantization_parameters"]["scales"][0]
    z2 = int(ins[1]["quantization_parameters"]["zero_points"][0])
    so = out_d["quantization_parameters"]["scales"][0]
    zo = int(out_d["quantization_parameters"]["zero_points"][0])

    left = 20
    twice_max = 2 * max(s1, s2)
    m1, sh1 = gemmlowp_shift(s1 / twice_max)
    m2, sh2 = gemmlowp_shift(s2 / twice_max)
    mo, sho = gemmlowp_shift(twice_max / ((1 << left) * so))

    pdict = {"n": n, "in1_zp": z1, "in2_zp": z2, "out_zp": zo,
             "in1_mult": m1 & 0xffffffff, "in2_mult": m2 & 0xffffffff,
             "out_mult": mo & 0xffffffff, "in1_shift": sh1, "in2_shift": sh2,
             "out_shift": sho, "left_shift": left, "act_min": -128,
             "act_max": 127}
    ref = ref_add(a_i8.reshape(-1), b_i8.reshape(-1), pdict)
    md = cross_check(name, ref, y.reshape(-1))

    out.append(f"/* {name}: elementwise add n={n} (tflite diff <= {md}) */")
    out.append(c_arr("int8_t", f"{name}_a", a_i8.reshape(-1)))
    out.append(c_arr("int8_t", f"{name}_b", b_i8.reshape(-1)))
    out.append(c_arr("int8_t", f"{name}_golden", ref))
    out.append(f"static const EthosUAddParams {name}_p = {{ "
               f".n={n}, .in1_zp={z1}, .in2_zp={z2}, .out_zp={zo}, "
               f".in1_mult={m1 & 0xffffffff}, .in2_mult={m2 & 0xffffffff}, "
               f".out_mult={mo & 0xffffffff}, "
               f".in1_shift={sh1}, .in2_shift={sh2}, .out_shift={sho}, "
               f".left_shift={left}, .act_min=-128, .act_max=127 }};")
    out.append("")
    return name


def gen_mul(out, name, n):
    shape = (n,)

    def build():
        a = tf.keras.Input(shape=shape)
        b = tf.keras.Input(shape=shape)
        return tf.keras.Model([a, b], tf.keras.layers.Multiply()([a, b]))

    interp = quantize_model(build, [shape, shape])
    ins = interp.get_input_details()
    out_d = interp.get_output_details()[0]
    a_i8 = RNG.integers(-128, 128, (1, n), dtype=np.int8)
    b_i8 = RNG.integers(-128, 128, (1, n), dtype=np.int8)
    interp.set_tensor(ins[0]["index"], a_i8)
    interp.set_tensor(ins[1]["index"], b_i8)
    interp.invoke()
    y = interp.get_tensor(out_d["index"])[0]

    s1 = ins[0]["quantization_parameters"]["scales"][0]
    z1 = int(ins[0]["quantization_parameters"]["zero_points"][0])
    s2 = ins[1]["quantization_parameters"]["scales"][0]
    z2 = int(ins[1]["quantization_parameters"]["zero_points"][0])
    so = out_d["quantization_parameters"]["scales"][0]
    zo = int(out_d["quantization_parameters"]["zero_points"][0])
    mo, sho = gemmlowp_shift(s1 * s2 / so)

    pdict = {"n": n, "in1_zp": z1, "in2_zp": z2, "out_zp": zo,
             "out_mult": mo & 0xffffffff, "out_shift": sho, "act_min": -128,
             "act_max": 127}
    ref = ref_mul(a_i8.reshape(-1), b_i8.reshape(-1), pdict)
    md = cross_check(name, ref, y.reshape(-1))

    out.append(f"/* {name}: elementwise mul n={n} (tflite diff <= {md}) */")
    out.append(c_arr("int8_t", f"{name}_a", a_i8.reshape(-1)))
    out.append(c_arr("int8_t", f"{name}_b", b_i8.reshape(-1)))
    out.append(c_arr("int8_t", f"{name}_golden", ref))
    out.append(f"static const EthosUMulParams {name}_p = {{ "
               f".n={n}, .in1_zp={z1}, .in2_zp={z2}, .out_zp={zo}, "
               f".out_mult={mo & 0xffffffff}, .out_shift={sho}, "
               f".act_min=-128, .act_max=127 }};")
    out.append("")
    return name


def main():
    out = ["/*",
           " * Auto-generated by tests/data/ethos-u/gen_kernels.py - do not edit.",
           " * int8 kernel goldens from TensorFlow Lite int8 reference ops.",
           " *",
           " * SPDX-License-Identifier: GPL-2.0-or-later",
           " */",
           "#ifndef TEST_ETHOS_U_KERNELS_VECTORS_H",
           "#define TEST_ETHOS_U_KERNELS_VECTORS_H",
           ""]

    convs = []
    convs.append(gen_conv(out, "conv_valid", 8, 8, 3, 4, 3, 1, "valid"))
    convs.append(gen_conv(out, "conv_same_s2", 9, 9, 2, 6, 3, 2, "same"))
    convs.append(gen_conv(out, "conv_1x1", 5, 5, 8, 5, 1, 1, "valid"))
    convs.append(gen_conv(out, "dw_valid", 8, 8, 4, 4, 3, 1, "valid",
                          depthwise=True))
    convs.append(gen_conv(out, "dw_same", 7, 7, 3, 3, 3, 1, "same",
                          depthwise=True))

    pools = []
    pools.append(gen_pool(out, "maxpool", 8, 8, 3, 2, 2, "max"))
    pools.append(gen_pool(out, "avgpool", 8, 8, 3, 2, 2, "avg"))

    adds = [gen_add(out, "add0", 17)]
    muls = [gen_mul(out, "mul0", 17)]

    # Indexes for the test driver.
    out.append("typedef struct { const char *name; const int8_t *ifm;")
    out.append("    const int8_t *w; const EthosUScaleBias *sb;")
    out.append("    const int8_t *golden; const EthosUConvParams *p;")
    out.append("    int depthwise; } ConvVec;")
    out.append("static const ConvVec conv_vecs[] = {")
    for nm, dw in convs:
        out.append(f"    {{ \"{nm}\", {nm}_ifm, {nm}_w, {nm}_sb, "
                   f"{nm}_golden, &{nm}_p, {1 if dw else 0} }},")
    out.append("};")
    out.append("")
    out.append("typedef struct { const char *name; const int8_t *ifm;")
    out.append("    const int8_t *golden; const EthosUPoolParams *p;")
    out.append("    int is_max; } PoolVec;")
    out.append("static const PoolVec pool_vecs[] = {")
    for nm, kind in pools:
        out.append(f"    {{ \"{nm}\", {nm}_ifm, {nm}_golden, &{nm}_p, "
                   f"{1 if kind == 'max' else 0} }},")
    out.append("};")
    out.append("")
    out.append("typedef struct { const char *name; const int8_t *a;")
    out.append("    const int8_t *b; const int8_t *golden;")
    out.append("    const EthosUAddParams *p; } AddVec;")
    out.append("static const AddVec add_vecs[] = {")
    for nm in adds:
        out.append(f"    {{ \"{nm}\", {nm}_a, {nm}_b, {nm}_golden, &{nm}_p }},")
    out.append("};")
    out.append("")
    out.append("typedef struct { const char *name; const int8_t *a;")
    out.append("    const int8_t *b; const int8_t *golden;")
    out.append("    const EthosUMulParams *p; } MulVec;")
    out.append("static const MulVec mul_vecs[] = {")
    for nm in muls:
        out.append(f"    {{ \"{nm}\", {nm}_a, {nm}_b, {nm}_golden, &{nm}_p }},")
    out.append("};")
    out.append("")
    out.append("#endif /* TEST_ETHOS_U_KERNELS_VECTORS_H */")

    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {OUT}: {len(convs)} conv/dw + {len(pools)} pool + "
          f"{len(adds)} add + {len(muls)} mul")


if __name__ == "__main__":
    main()
