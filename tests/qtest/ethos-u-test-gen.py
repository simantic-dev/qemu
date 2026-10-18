#!/usr/bin/env python3
# Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Generate the committed data for tests/qtest/ethos-u-test.c: a set of int8
# convolutions laid out in guest DRAM exactly as the NPU consumes them - a
# Vela-style command stream, mlw-encoded weight stream(s), per-channel scale/bias
# stream(s) and an IFM - plus the expected OFM. Each case exercises one slice of
# the executor; the cases escalate from the single-brick path the small demo used
# to the multi-brick / depth-split path real (wider) models need:
#
#   single_8_nhwc      ofm 8ch  NHWC,    1 op            (control, was passing)
#   single_32_nhwc     ofm 32ch NHWC,    1 op            (>16ch single-op reorder)
#   single_32_nhcwb16  ofm 32ch NHCWB16, 1 op            (multi-brick FM r/w)
#   split_32_nhcwb16   ofm 32ch NHCWB16, 2 ops [16,16]   (depth-split / multi-op)
#   multiifm_64        ifm 64ch, ofm 32ch, 2 ops         (multi IFM-block reorder)
#
# Weights are reordered+encoded with the real Arm mlw_codec and scales packed
# with Vela's encode_bias; the golden OFM is the reference pipeline output. Each
# case is emitted as a list of (addr,bytes) "pokes" + the command stream + the
# region bases + the OFM check, so the qtest just replays it.
#
# Re-run after changing cases:  python3 tests/qtest/ethos-u-test-gen.py

import math
import numpy as np
import ethosu.mlw_codec as mlw
from ethosu.vela.weight_compressor import encode_bias

OUT = "tests/qtest/ethos-u-test-data.h"

# guest DRAM layout (DDR @ 0x80000000)
WS_BASE = 0x80100000       # region 0: weights + scales
ARENA_BASE = 0x80200000    # region 1: ifm + ofm
CMS_BASE = 0x80300000      # command stream
IFM_OFF = 0x0
OFM_OFF = 0x40000

# ---- reference integer pipeline (mirrors the C kernels) ----
INT32_MIN, INT32_MAX = -(1 << 31), (1 << 31) - 1


def i32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x >= (1 << 31) else x


def _ctrunc(n, d):
    q = abs(n) // d
    return q if n >= 0 else -q


def srdhm(a, b):
    a, b = i32(a), i32(b)
    ov = a == b == INT32_MIN
    ab = a * b
    nudge = (1 << 30) if ab >= 0 else (1 - (1 << 30))
    return INT32_MAX if ov else i32(_ctrunc(ab + nudge, 1 << 31))


def rdpot(x, e):
    x = i32(x)
    if e <= 0:
        return x
    mask = (1 << e) - 1
    rem = x & mask
    thr = (mask >> 1) + (1 if x < 0 else 0)
    return (x >> e) + (1 if rem > thr else 0)


def mqm(x, m, s):
    left = s if s > 0 else 0
    right = 0 if s > 0 else -s
    return rdpot(srdhm(i32(x * (1 << left)), m), right)


def quantise_scale(sc):
    sig, exp = math.frexp(sc)
    s = sig * (1 << 31)
    q = int(math.floor(s + 0.5)) if s >= 0 else int(math.ceil(s - 0.5))
    return q, 31 - exp


def ref_conv(ifm, w, bias, mult, vshift, izp, ozp, k, pad):
    """ifm [H,W,Cin] int, w [Co,k,k,Cin], -> ofm [Ho,Wo,Co] int8 (VALID/SAME)."""
    H, W, Ci = ifm.shape
    Co = w.shape[0]
    Ho, Wo = H - k + 1 + 2 * pad, W - k + 1 + 2 * pad
    ofm = np.zeros((Ho, Wo, Co), np.int8)
    for oy in range(Ho):
        for ox in range(Wo):
            for oc in range(Co):
                acc = int(bias[oc])
                for ky in range(k):
                    iy = oy - pad + ky
                    if iy < 0 or iy >= H:
                        continue
                    for kx in range(k):
                        ix = ox - pad + kx
                        if ix < 0 or ix >= W:
                            continue
                        for ic in range(Ci):
                            acc += (int(ifm[iy, ix, ic]) - izp) \
                                * int(w[oc, ky, kx, ic])
                v = mqm(acc, i32(mult[oc]), 31 - vshift[oc]) + ozp
                ofm[oy, ox, oc] = max(-128, min(127, v))
    return ofm


# ---- feature-map (de)serialization ----
def to_nhwc_bytes(a):
    return a.reshape(-1).astype(np.uint8).tobytes()


def to_nhcwb16_bytes(a):
    """a [H,W,C], C a multiple of 16 -> NHCWB16 byte layout."""
    H, W, C = a.shape
    Cb = C // 16
    out = np.zeros(H * Cb * W * 16, np.int8)
    for h in range(H):
        for w in range(W):
            for c in range(C):
                cb, c16 = c // 16, c % 16
                out[h * Cb * W * 16 + cb * W * 16 + w * 16 + c16] = a[h, w, c]
    return out.astype(np.uint8).tobytes()


def fm_strides(layout, W, C):
    if layout == "nhcwb16":
        Cb = (C + 15) // 16
        return 16, Cb * W * 16, W * 16        # sx, sy, sc(brick)
    return C, C * W, 1                          # NHWC: sx, sy, sc


# ---- command-stream emitter (matches ethos_u_cmdstream.c framing) ----
PAY = 0x4000
C0 = dict(IFM_PAD_TOP=0x100, IFM_PAD_LEFT=0x101, IFM_PAD_RIGHT=0x102,
          IFM_PAD_BOTTOM=0x103, IFM_DEPTH_M1=0x104, IFM_PRECISION=0x105,
          IFM_ZERO_POINT=0x109, IFM_WIDTH0_M1=0x10a, IFM_HEIGHT0_M1=0x10b,
          IFM_REGION=0x10f, OFM_WIDTH_M1=0x111, OFM_HEIGHT_M1=0x112,
          OFM_DEPTH_M1=0x113, OFM_PRECISION=0x114, OFM_BLK_DEPTH_M1=0x117,
          OFM_ZERO_POINT=0x118, OFM_REGION=0x11f, KERNEL_WIDTH_M1=0x120,
          KERNEL_HEIGHT_M1=0x121, KERNEL_STRIDE=0x122, ACT_MIN=0x126,
          ACT_MAX=0x127, WEIGHT_REGION=0x128, SCALE_REGION=0x129)
C1 = dict(IFM_BASE0=0x000, IFM_STRIDE_X=0x004, IFM_STRIDE_Y=0x005,
          IFM_STRIDE_C=0x006, OFM_BASE0=0x010, OFM_STRIDE_X=0x014,
          OFM_STRIDE_Y=0x015, OFM_STRIDE_C=0x016, WEIGHT_BASE=0x020,
          WEIGHT_LENGTH=0x021, SCALE_BASE=0x022, DMA0_SRC=0x030,
          DMA0_DST=0x031, DMA0_LEN=0x032)
C0.update(DMA0_SRC_REGION=0x130, DMA0_DST_REGION=0x131)
OP_CONV, OP_STOP, OP_DMA_START = 0x002, 0x000, 0x010


class CMS:
    def __init__(self):
        self.b = bytearray()

    def c0(self, name, imm=0):
        self.b += int(C0[name]).to_bytes(2, "little")
        self.b += int(imm & 0xffff).to_bytes(2, "little")

    def c1(self, name, data):
        self.b += int(C1[name] | PAY).to_bytes(2, "little")
        self.b += (0).to_bytes(2, "little")
        self.b += int(data & 0xffffffff).to_bytes(4, "little")

    def op(self, opc, imm=0):
        self.b += int(opc).to_bytes(2, "little")
        self.b += int(imm & 0xffff).to_bytes(2, "little")


WSTAGE_OFF = 0x20000       # region 1: DMA destination for streamed weights


def build_case(rng, name, ih, iw, ifm_c, ofm_c, k, pad,
               ifm_layout, ofm_layout, splits, blk, is_pk=0, dma=False):
    """Return a case dict: pokes, cms, basep0/1, ofm_addr, golden.

    dma=True mirrors Shared_Sram: per slice the combined scale+weight blob lives
    in region 0 (flash) and is DMA-streamed to region 1 (SRAM) before the conv,
    which then reads its weights/scales from region 1.
    """
    w = rng.integers(-90, 91, (ofm_c, k, k, ifm_c), dtype=np.int16)
    ifm = rng.integers(-128, 128, (ih, iw, ifm_c), dtype=np.int16)
    bias = rng.integers(-4000, 4000, ofm_c, dtype=np.int64)
    scales = [0.0008 + 0.00025 * j for j in range(ofm_c)]
    mult, vshift = zip(*[quantise_scale(s) for s in scales])
    izp, ozp = -7, 4

    ofm = ref_conv(ifm, w, bias, mult, vshift, izp, ozp, k, pad)
    oh, ow = ofm.shape[0], ofm.shape[1]

    # IFM bytes in its layout
    ifm_i8 = ifm.astype(np.int8)
    ifm_bytes = (to_nhcwb16_bytes(ifm_i8) if ifm_layout == "nhcwb16"
                 else to_nhwc_bytes(ifm_i8))
    ifm_sx, ifm_sy, ifm_sc = fm_strides(ifm_layout, iw, ifm_c)
    ofm_sx, ofm_sy, ofm_sc = fm_strides(ofm_layout, ow, ofm_c)

    pokes = [(ARENA_BASE + IFM_OFF, ifm_bytes)]

    # Weights + scales per output-channel slice, packed into region 0.
    c = CMS()
    c.c0("IFM_REGION", 1)
    c.c1("IFM_BASE0", IFM_OFF)
    c.c0("IFM_WIDTH0_M1", iw - 1)
    c.c0("IFM_HEIGHT0_M1", ih - 1)
    c.c0("IFM_DEPTH_M1", ifm_c - 1)
    c.c0("IFM_PRECISION", 1 | (0x40 if ifm_layout == "nhcwb16" else 0))
    c.c0("IFM_ZERO_POINT", izp & 0xffff)
    c.c1("IFM_STRIDE_X", ifm_sx)
    c.c1("IFM_STRIDE_Y", ifm_sy)
    c.c1("IFM_STRIDE_C", ifm_sc)
    c.c0("IFM_PAD_TOP", pad)
    c.c0("IFM_PAD_LEFT", pad)
    c.c0("IFM_PAD_RIGHT", pad)
    c.c0("IFM_PAD_BOTTOM", pad)
    c.c0("OFM_REGION", 1)
    c.c0("OFM_WIDTH_M1", ow - 1)
    c.c0("OFM_HEIGHT_M1", oh - 1)
    c.c0("OFM_PRECISION", 1 | (0x40 if ofm_layout == "nhcwb16" else 0))
    c.c0("OFM_ZERO_POINT", ozp & 0xffff)
    c.c1("OFM_STRIDE_X", ofm_sx)
    c.c1("OFM_STRIDE_Y", ofm_sy)
    c.c1("OFM_STRIDE_C", ofm_sc)
    c.c0("KERNEL_WIDTH_M1", k - 1)
    c.c0("KERNEL_HEIGHT_M1", k - 1)
    c.c0("KERNEL_STRIDE", is_pk << 2)        # stride 1, dilation 1; bit2=part-kernel
    weight_region = 1 if dma else 0
    c.c0("WEIGHT_REGION", weight_region)
    c.c0("SCALE_REGION", weight_region)
    c.c0("ACT_MIN", (-128) & 0xffff)
    c.c0("ACT_MAX", 127)

    flash = 0       # region 0 offset (source blobs)
    cbase = 0
    for s_len in splits:
        sl = slice(cbase, cbase + s_len)
        enc, _ = mlw.reorder_encode(8, 8, np.ascontiguousarray(w[sl]),
                                    blk, 0, is_pk, 8, 8, 8)
        enc = bytes(enc)
        scale_stream = bytearray()
        for oc in range(cbase, cbase + s_len):
            scale_stream += encode_bias(np.int64(int(bias[oc])),
                                        int(mult[oc]), int(vshift[oc]))
        scale_padded = (len(scale_stream) + 15) & ~15
        blob = bytes(scale_stream) + b"\x00" * (scale_padded - len(scale_stream)) \
            + enc

        ofm_brick = cbase // 16
        ofm_slice_off = OFM_OFF + ofm_brick * ofm_sc
        c.c1("OFM_BASE0", ofm_slice_off)
        c.c0("OFM_DEPTH_M1", s_len - 1)
        c.c0("OFM_BLK_DEPTH_M1", blk - 1)

        if dma:
            # combined blob in flash (region 0), DMA -> region 1 staging
            pokes.append((WS_BASE + flash, blob))
            stage = WSTAGE_OFF + flash       # distinct per slice, no overlap
            c.c0("DMA0_SRC_REGION", 0)
            c.c1("DMA0_SRC", flash)
            c.c0("DMA0_DST_REGION", 1)
            c.c1("DMA0_DST", stage)
            c.c1("DMA0_LEN", len(blob))
            c.op(OP_DMA_START, 0)
            c.c1("SCALE_BASE", stage)
            c.c1("WEIGHT_BASE", stage + scale_padded)
            c.c1("WEIGHT_LENGTH", len(enc))
        else:
            pokes.append((WS_BASE + flash, blob))
            c.c1("SCALE_BASE", flash)
            c.c1("WEIGHT_BASE", flash + scale_padded)
            c.c1("WEIGHT_LENGTH", len(enc))
        flash += (len(blob) + 15) & ~15
        c.op(OP_CONV, 0)
        cbase += s_len
    c.op(OP_STOP, 0)

    golden = (to_nhcwb16_bytes(ofm) if ofm_layout == "nhcwb16"
              else to_nhwc_bytes(ofm))
    return dict(name=name, cms=bytes(c.b), pokes=pokes,
                basep0=WS_BASE, basep1=ARENA_BASE,
                ofm_addr=ARENA_BASE + OFM_OFF, golden=golden)


def build_pool_case(rng, name, ih, iw, c, k, stride, layout, is_max=True):
    """Max/avg pool case (no weights/scales), NHCWB16 or NHWC, multi-brick."""
    ifm = rng.integers(-128, 128, (ih, iw, c), dtype=np.int16).astype(np.int8)
    oh, ow = (ih - k) // stride + 1, (iw - k) // stride + 1
    ofm = np.zeros((oh, ow, c), np.int8)
    for oy in range(oh):
        for ox in range(ow):
            for ch in range(c):
                vals = [int(ifm[oy * stride + ky, ox * stride + kx, ch])
                        for ky in range(k) for kx in range(k)]
                if is_max:
                    v = max(vals)
                else:
                    s = sum(vals)
                    n = len(vals)
                    v = (s + n // 2) // n if s > 0 else -((-s + n // 2) // n)
                ofm[oy, ox, ch] = max(-128, min(127, v))

    ifm_bytes = (to_nhcwb16_bytes(ifm) if layout == "nhcwb16"
                 else to_nhwc_bytes(ifm))
    ifm_sx, ifm_sy, ifm_sc = fm_strides(layout, iw, c)
    ofm_sx, ofm_sy, ofm_sc = fm_strides(layout, ow, c)
    pokes = [(ARENA_BASE + IFM_OFF, ifm_bytes)]

    ks = 0
    if stride == 2:
        ks = 0b11           # stride_x=stride_y=2
    cc = CMS()
    cc.c0("IFM_REGION", 1)
    cc.c1("IFM_BASE0", IFM_OFF)
    cc.c0("IFM_WIDTH0_M1", iw - 1)
    cc.c0("IFM_HEIGHT0_M1", ih - 1)
    cc.c0("IFM_DEPTH_M1", c - 1)
    cc.c0("IFM_PRECISION", 1 | (0x40 if layout == "nhcwb16" else 0))
    cc.c0("IFM_ZERO_POINT", 0)
    cc.c1("IFM_STRIDE_X", ifm_sx)
    cc.c1("IFM_STRIDE_Y", ifm_sy)
    cc.c1("IFM_STRIDE_C", ifm_sc)
    cc.c0("IFM_PAD_TOP", 0)
    cc.c0("IFM_PAD_LEFT", 0)
    cc.c0("OFM_REGION", 1)
    cc.c1("OFM_BASE0", OFM_OFF)
    cc.c0("OFM_WIDTH_M1", ow - 1)
    cc.c0("OFM_HEIGHT_M1", oh - 1)
    cc.c0("OFM_DEPTH_M1", c - 1)
    cc.c0("OFM_PRECISION", 1 | (0x40 if layout == "nhcwb16" else 0))
    cc.c0("OFM_ZERO_POINT", 0)
    cc.c1("OFM_STRIDE_X", ofm_sx)
    cc.c1("OFM_STRIDE_Y", ofm_sy)
    cc.c1("OFM_STRIDE_C", ofm_sc)
    cc.c0("KERNEL_WIDTH_M1", k - 1)
    cc.c0("KERNEL_HEIGHT_M1", k - 1)
    cc.c0("KERNEL_STRIDE", ks)
    cc.c0("ACT_MIN", (-128) & 0xffff)
    cc.c0("ACT_MAX", 127)
    cc.op(0x005, 0 if is_max else 1)        # NPU_OP_POOL, imm: 0=MAX 1=AVG
    cc.op(OP_STOP, 0)

    golden = (to_nhcwb16_bytes(ofm) if layout == "nhcwb16"
              else to_nhwc_bytes(ofm))
    return dict(name=name, cms=bytes(cc.b), pokes=pokes,
                basep0=WS_BASE, basep1=ARENA_BASE,
                ofm_addr=ARENA_BASE + OFM_OFF, golden=golden)


def c_bytes(name, data):
    out = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 12):
        out.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12])
                   + ",")
    out.append("};")
    return "\n".join(out)


def main():
    rng = np.random.default_rng(0x93e5)
    cases = [
        build_case(rng, "single_8_nhwc", 6, 6, 4, 8, 3, 0,
                   "nhwc", "nhwc", [8], 16),
        build_case(rng, "single_32_nhwc", 4, 4, 8, 32, 1, 0,
                   "nhwc", "nhwc", [32], 32),
        build_case(rng, "single_32_nhcwb16", 4, 4, 32, 32, 1, 0,
                   "nhcwb16", "nhcwb16", [32], 32),
        build_case(rng, "split_32_nhcwb16", 4, 4, 32, 32, 1, 0,
                   "nhcwb16", "nhcwb16", [16, 16], 16),
        build_case(rng, "multiifm_64", 4, 4, 64, 32, 1, 0,
                   "nhcwb16", "nhcwb16", [16, 16], 16),
        # mirror the big model's real layers (3x3, part-kernel, non-uniform split)
        build_case(rng, "layer1_pk_3x3", 8, 8, 1, 32, 3, 1,
                   "nhwc", "nhcwb16", [16, 16], 16, is_pk=1),
        build_case(rng, "layer2_df_3x3", 8, 8, 32, 64, 3, 1,
                   "nhcwb16", "nhcwb16", [16, 48], 64, is_pk=0),
        build_case(rng, "layer3_df_3x3", 4, 4, 64, 128, 3, 1,
                   "nhcwb16", "nhcwb16", [16, 112], 128, is_pk=0),
        # weights DMA-streamed flash(region0) -> SRAM(region1), as Shared_Sram
        build_case(rng, "dma_single_8", 6, 6, 4, 8, 3, 0,
                   "nhwc", "nhwc", [8], 16, dma=True),
        build_case(rng, "dma_layer2_3x3", 8, 8, 32, 64, 3, 1,
                   "nhcwb16", "nhcwb16", [16, 48], 64, is_pk=0, dma=True),
        # maxpool 2x2 s2 on multi-brick NHCWB16 (the model's pools)
        build_pool_case(rng, "maxpool_32_nhcwb16", 16, 16, 32, 2, 2,
                        "nhcwb16", is_max=True),
        build_pool_case(rng, "maxpool_64_nhcwb16", 8, 8, 64, 2, 2,
                        "nhcwb16", is_max=True),
    ]

    out = ["/*",
           " * Auto-generated by tests/qtest/ethos-u-test-gen.py - do not edit.",
           " * int8 conv cases (single-brick -> multi-brick) staged in DRAM,",
           " * with the expected OFM (reference pipeline).",
           " *",
           " * SPDX-License-Identifier: GPL-2.0-or-later",
           " */",
           "#ifndef ETHOS_U_TEST_DATA_H",
           "#define ETHOS_U_TEST_DATA_H",
           "",
           f"#define CMS_BASE_ADDR  0x{CMS_BASE:08x}ULL",
           "",
           "typedef struct { uint64_t addr; const uint8_t *data; "
           "uint32_t len; } EuPoke;",
           "typedef struct {",
           "    const char *name;",
           "    const uint8_t *cms; uint32_t cms_len;",
           "    const EuPoke *pokes; int n_pokes;",
           "    uint64_t basep0, basep1;",
           "    uint64_t ofm_addr; const uint8_t *golden; uint32_t ofm_len;",
           "} EuCase;",
           ""]

    for ci, c in enumerate(cases):
        out.append(c_bytes(f"c{ci}_cms", c["cms"]))
        for pi, (addr, data) in enumerate(c["pokes"]):
            out.append(c_bytes(f"c{ci}_poke{pi}", data))
        out.append(c_bytes(f"c{ci}_golden", c["golden"]))
        rows = []
        for pi, (addr, data) in enumerate(c["pokes"]):
            rows.append(f"    {{ 0x{addr:08x}ULL, c{ci}_poke{pi}, "
                        f"sizeof(c{ci}_poke{pi}) }},")
        out.append(f"static const EuPoke c{ci}_pokes[] = {{")
        out.extend(rows)
        out.append("};")
        out.append("")

    out.append("static const EuCase eu_cases[] = {")
    for ci, c in enumerate(cases):
        out.append(
            f'    {{ "{c["name"]}", c{ci}_cms, sizeof(c{ci}_cms), '
            f'c{ci}_pokes, ARRAY_SIZE(c{ci}_pokes), '
            f'0x{c["basep0"]:08x}ULL, 0x{c["basep1"]:08x}ULL, '
            f'0x{c["ofm_addr"]:08x}ULL, c{ci}_golden, sizeof(c{ci}_golden) }},')
    out.append("};")
    out.append("")
    out.append("#endif /* ETHOS_U_TEST_DATA_H */")

    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {OUT}: {len(cases)} cases "
          + ", ".join(f'{c["name"]}(ofm={c["ofm_len"] if False else len(c["golden"])}B)'
                      for c in cases))


if __name__ == "__main__":
    main()
