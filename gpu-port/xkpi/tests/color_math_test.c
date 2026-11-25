/*
 * Hardware colour stages (2026-11-25): user-space test of nmx_fb_color.h against what amdgpu_dm does with the result.
 *
 *   cc -O2 -Wall -o /tmp/color_math_test tests/color_math_test.c -lm && /tmp/color_math_test
 *
 * 1. Identity tables of any input size become a LUT amdgpu_dm's __is_lut_linear accepts (copied as
 *    nmx_color_lut_is_linear): an identity stage is bypassed in hardware, so moving a stage never costs precision.
 * 2. A gamma-2.2 / sRGB-shaped curve of 256, 1024 and 4096 points resamples to 4096 entries within 1/65535 of the
 *    exact curve's linear interpolation; ends are exact; monotonic input stays monotonic.
 * 3. Clamping: >= 1.0 is 0xffff, 0 is 0; frac 8..24.
 * 4. CTM: the S31.32 sign-magnitude amdgpu_dm_fixpt_from_s3132 decodes (copied below) equals the input coefficient,
 *    negatives included, for frac 16 / 24 / 32 / 40.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bsdcompat/include/nmx_fb_color.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* amdgpu_dm_fixpt_from_s3132 (amdgpu_dm_color.c): sign-magnitude S31.32 -> two's complement */
static int64_t dm_from_s3132(uint64_t x)
{
    uint64_t mag = x & ~(1ull << 63);
    return (x & (1ull << 63)) ? -(int64_t)mag : (int64_t)mag;
}

static uint32_t fx(double v, unsigned frac) { return (uint32_t)llround(v * (double)(1ull << frac)); }

int main(void)
{
    static struct nmx_color_lut lut[NMX_COLOR_LUT_ENTRIES];
    static uint32_t r[4096], g[4096], b[4096];
    const uint32_t sizes[] = { 2, 17, 256, 1024, 1025, 4096 };
    const unsigned fracs[] = { 8, 12, 16, 20, 24 };

    /* 1: identity */
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++)
        for (unsigned f = 0; f < sizeof fracs / sizeof *fracs; f++) {
            const uint32_t n = sizes[s];
            const unsigned fr = fracs[f];
            for (uint32_t i = 0; i < n; i++)
                r[i] = g[i] = b[i] = fx((double)i / (n - 1), fr);
            CHECK(nmx_color_lut_build(r, g, b, n, fr, lut) == 0, "build n %u", n);
            CHECK(nmx_color_lut_is_linear(lut, NMX_COLOR_LUT_ENTRIES), "identity n %u frac %u not linear for DM", n, fr);
            CHECK(lut[0].red == 0 && lut[NMX_COLOR_LUT_ENTRIES - 1].red == 0xffff, "identity ends n %u frac %u", n, fr);
        }

    /* 2: curves */
    for (unsigned s = 2; s < sizeof sizes / sizeof *sizes; s++) {
        const uint32_t n = sizes[s];
        const unsigned fr = 16;
        for (uint32_t i = 0; i < n; i++) {
            const double x = (double)i / (n - 1);
            r[i] = fx(pow(x, 2.2), fr);
            g[i] = fx(pow(x, 1 / 2.2), fr);
            b[i] = fx(x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4), fr);
        }
        CHECK(nmx_color_lut_build(r, g, b, n, fr, lut) == 0, "build curve n %u", n);
        int worst = 0;
        for (uint32_t j = 0; j < NMX_COLOR_LUT_ENTRIES; j++) {
            const double p = (double)j * (n - 1) / (NMX_COLOR_LUT_ENTRIES - 1);
            uint32_t i = (uint32_t)p;
            if (i >= n - 1) i = n - 2;
            const double t = p - i;
            const uint32_t *ch[3] = { r, g, b };
            const uint16_t got[3] = { lut[j].red, lut[j].green, lut[j].blue };
            for (int c = 0; c < 3; c++) {
                const double want = ((1 - t) * ch[c][i] + t * ch[c][i + 1]) / 65536.0 * 65535.0;
                const int d = (int)lround(fabs(got[c] - want));
                if (d > worst) worst = d;
                if (j && got[c] < (c == 0 ? lut[j - 1].red : c == 1 ? lut[j - 1].green : lut[j - 1].blue))
                    CHECK(0, "n %u channel %d not monotonic at %u", n, c, j);
            }
        }
        CHECK(worst <= 1, "curve n %u: worst error %d / 65535", n, worst);
        CHECK(lut[0].red == 0 && lut[NMX_COLOR_LUT_ENTRIES - 1].red == 0xffff && lut[NMX_COLOR_LUT_ENTRIES - 1].blue == 0xffff,
              "curve ends n %u", n);
        CHECK(!nmx_color_lut_is_linear(lut, NMX_COLOR_LUT_ENTRIES), "a gamma curve must not look linear");
    }

    /* 3: clamping */
    CHECK(nmx_color_to_u16(0, 16) == 0 && nmx_color_to_u16(1u << 16, 16) == 0xffff && nmx_color_to_u16(5u << 16, 16) == 0xffff,
          "clamp");
    CHECK(nmx_color_to_u16(1u << 15, 16) == 0x8000, "half = %#x", nmx_color_to_u16(1u << 15, 16));
    CHECK(nmx_color_resample(r, 1, 16, &lut[0].red, NMX_COLOR_LUT_ENTRIES, 4) == -1, "n_in 1 refused");

    /* 4: CTM */
    {
        const double coefs[] = { 1.0, 0.0, -0.0, 0.5, -0.5, 1.25, -1.9921875, 0.0000152587890625, 3.75, -7.5 };
        const unsigned cfr[] = { 16, 24, 32, 40 };
        for (unsigned f = 0; f < sizeof cfr / sizeof *cfr; f++)
            for (unsigned k = 0; k < sizeof coefs / sizeof *coefs; k++) {
                const int64_t in = (int64_t)llround(coefs[k] * (double)(1ull << cfr[f]));
                const uint64_t s = nmx_color_ctm_coef(in, cfr[f]);
                const double back = (double)dm_from_s3132(s) / 4294967296.0;
                CHECK(fabs(back - (double)in / (double)(1ull << cfr[f])) < 1e-9, "ctm frac %u coef %g -> %g", cfr[f], coefs[k], back);
                CHECK(in != 0 || s == 0, "zero must encode as +0");
            }
        int64_t m[9] = { 1 << 16, 0, 0, 0, 1 << 16, 0, 0, 0, 1 << 16 };
        uint64_t o[9];
        nmx_color_ctm_build(m, 16, o);
        CHECK(o[0] == 1ull << 32 && o[4] == 1ull << 32 && o[8] == 1ull << 32 && o[1] == 0, "identity CTM");
    }

    printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
