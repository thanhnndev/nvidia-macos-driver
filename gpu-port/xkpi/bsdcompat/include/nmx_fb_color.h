/*
 * Hardware colour stages (2026-11-25): the pure conversions from the tables CoreDisplay hands over to what amdgpu_dm's
 * CRTC colour properties take. Shared by the bridge (override/src/nmx_fb_bridge.c) and its unit test
 * (tests/color_math_test.c). Integer only (kernel: no FPU). No kernel headers.
 *
 * CoreDisplay's GPU colour pass (CoreDisplay default.metallib "GPUPass", WS-CPU.md 9.8; AIR disassembled 25G332) runs,
 * per pixel, gated by enabledFeatures bits:
 *   bit0 preMatrixLinearization: parametric, sign-preserving: |x| >= d ? (a|x| + b)^g + e : c|x| + f (7 float3 coeffs)
 *   bit1 cscMatrix: 4x4
 *   bit2 combinedLUT: x * scale + offset -> per-channel 1D LUT (EOTF/encoding, transfer tables and EDR folded in)
 *   bit3 PQ encode (ST 2084 constants, no parameters)
 *   bit4 postGammaCSC: 4x4
 * DisplayPipe::RunFullDisplayPipe passes bits 0-3 only (bit3 only when the pipe is forced: mirroring flag / output
 * mode 4), so postGammaCSC is never asked for there. amdgpu_dm maps CRTC DEGAMMA_LUT -> DGM, CTM -> gamut remap,
 * GAMMA_LUT -> RGM in the same order (amdgpu_dm_update_crtc_color_mgmt): bits 0-2 have a hardware home. The parametric
 * linearization is sampled into the 4096-entry DEGAMMA_LUT by the caller.
 *
 * Inputs are unsigned fixed point with `frac` fractional bits (1.0 = 1 << frac); the exact format IOPresentment's
 * transactions carry is given by the capability's FixedFormat, which the caller turns into `frac`.
 */
#ifndef NMX_FB_COLOR_H
#define NMX_FB_COLOR_H

#if !defined(_KERNEL) && !defined(KERNEL)
#include <stdint.h>
#endif

#define NMX_COLOR_LUT_ENTRIES   4096u           /* amdgpu_dm MAX_COLOR_LUT_ENTRIES: the only DEGAMMA_LUT size, and the
                                                 * non-legacy GAMMA_LUT size (256 = legacy, forces an sRGB base) */
#define NMX_COLOR_LUT_MAX       0xffffu         /* MAX_DRM_LUT_VALUE */

struct nmx_color_lut { uint16_t red, green, blue, reserved; };      /* = struct drm_color_lut */

/* one fixed-point value (frac fractional bits) -> 0..0xffff, clamped to [0, 1], rounded to nearest */
static inline uint16_t nmx_color_to_u16(uint64_t v, unsigned frac)
{
    const uint64_t one = 1ull << frac;
    if (v >= one)
        return NMX_COLOR_LUT_MAX;
    return (uint16_t)((v * NMX_COLOR_LUT_MAX + (one >> 1)) >> frac);
}

/*
 * Resample one channel of n_in points (evenly spaced over [0, 1], n_in >= 2) to n_out points by linear interpolation,
 * writing 0..0xffff values with stride (in uint16_t units) into out. Point j of the output sits at x = j / (n_out - 1),
 * i.e. input position j * (n_in - 1) / (n_out - 1): computed in 32.32 fixed point, exact at both ends.
 * Returns 0, or -1 for a bad size.
 */
static inline int nmx_color_resample(const uint32_t *in, uint32_t n_in, unsigned frac, uint16_t *out,
                                     uint32_t n_out, uint32_t stride)
{
    if (n_in < 2 || n_out < 2 || frac > 31)
        return -1;
    for (uint32_t j = 0; j < n_out; j++) {
        const uint64_t pos = ((uint64_t)j * (n_in - 1) << 32) / (n_out - 1);    /* 32.32 input index */
        const uint32_t i = (uint32_t)(pos >> 32);
        const uint64_t t = pos & 0xffffffffull;
        uint64_t v;
        if (i >= n_in - 1) {
            v = in[n_in - 1];
        } else {
            const uint64_t a = in[i], b = in[i + 1];
            v = b >= a ? a + (((b - a) * t + (1ull << 31)) >> 32) : a - (((a - b) * t + (1ull << 31)) >> 32);
        }
        out[(uint64_t)j * stride] = nmx_color_to_u16(v, frac);
    }
    return 0;
}

/* the input channel is the identity to within its own quantisation (one step of 1 / 2^frac, rounding) */
static inline int nmx_color_in_is_identity(const uint32_t *in, uint32_t n_in, unsigned frac)
{
    const uint64_t one = 1ull << frac;
    for (uint32_t i = 0; i < n_in; i++) {
        const int64_t want = (int64_t)(((uint64_t)i * one + (n_in - 1) / 2) / (n_in - 1));
        const int64_t d = (int64_t)in[i] - want;
        if (d < -1 || d > 1)
            return 0;
    }
    return 1;
}

/*
 * three channels (r, g, b: n_in points each) -> a 4096-entry DRM LUT. When all three are the identity the exact linear
 * ramp is written, which amdgpu_dm recognises and bypasses (no precision lost by a stage that does nothing); a
 * low-precision identity table resampled would miss __is_lut_linear's +/-1 and be applied as a curve.
 */
static inline int nmx_color_lut_build(const uint32_t *r, const uint32_t *g, const uint32_t *b, uint32_t n_in,
                                      unsigned frac, struct nmx_color_lut *out)
{
    if (n_in < 2 || frac > 31)
        return -1;
    if (nmx_color_in_is_identity(r, n_in, frac) && nmx_color_in_is_identity(g, n_in, frac) &&
        nmx_color_in_is_identity(b, n_in, frac)) {
        for (uint32_t j = 0; j < NMX_COLOR_LUT_ENTRIES; j++) {
            const uint16_t v = (uint16_t)((uint64_t)j * NMX_COLOR_LUT_MAX / (NMX_COLOR_LUT_ENTRIES - 1));
            out[j].red = out[j].green = out[j].blue = v;
            out[j].reserved = 0;
        }
        return 0;
    }
    if (nmx_color_resample(r, n_in, frac, &out[0].red, NMX_COLOR_LUT_ENTRIES, 4) ||
        nmx_color_resample(g, n_in, frac, &out[0].green, NMX_COLOR_LUT_ENTRIES, 4) ||
        nmx_color_resample(b, n_in, frac, &out[0].blue, NMX_COLOR_LUT_ENTRIES, 4))
        return -1;
    for (uint32_t j = 0; j < NMX_COLOR_LUT_ENTRIES; j++)
        out[j].reserved = 0;
    return 0;
}

/* amdgpu_dm's __is_lut_linear: such a LUT is bypassed (identity), so an identity stage costs nothing */
static inline int nmx_color_lut_is_linear(const struct nmx_color_lut *lut, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        if (lut[i].red != lut[i].green || lut[i].green != lut[i].blue)
            return 0;
        const int d = (int)lut[i].red - (int)((uint64_t)i * NMX_COLOR_LUT_MAX / (size - 1));
        if (d < -1 || d > 1)
            return 0;
    }
    return 1;
}

/*
 * One signed fixed-point matrix coefficient (two's complement, frac fractional bits) -> DRM CTM's S31.32
 * sign-magnitude (drm_color_ctm: bit 63 = sign, the rest = magnitude in 31.32).
 */
static inline uint64_t nmx_color_ctm_coef(int64_t v, unsigned frac)
{
    const int neg = v < 0;
    uint64_t m = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    m = frac <= 32 ? m << (32 - frac) : (m + (1ull << (frac - 33))) >> (frac - 32);
    m &= 0x7fffffffffffffffull;
    return neg && m ? (m | 0x8000000000000000ull) : m;
}

/* 3x3 row-major (out = M * in, like drm_color_ctm.matrix[row * 3 + col]) */
static inline void nmx_color_ctm_build(const int64_t m[9], unsigned frac, uint64_t out[9])
{
    for (int i = 0; i < 9; i++)
        out[i] = nmx_color_ctm_coef(m[i], frac);
}

#endif
