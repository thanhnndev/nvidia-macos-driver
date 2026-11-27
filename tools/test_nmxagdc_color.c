/*
 * nmx-agdc-color.h against what IOPresentment and CoreDisplay 25G332 do with the reply (re-implemented from their
 * disassembly, see the header):  cc -O2 -Wall -o /tmp/t tools/test_nmxagdc_color.c && /tmp/t
 */
#include "../kexts/NMXAGDC/nmx-agdc-color.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

/* IOPresentmentCapabilities::AGDCGammaTypeToIOPresentment / AGDCLinearizationTypeToIOPresentment (same order) */
static uint32_t iop_gamma_type(uint32_t t)
{
    static const uint32_t order[] = { 1, 2, 4, 8, 0x400, 0x800, 0x1000, 0x2000, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200 };
    for (unsigned i = 0; i < sizeof order / sizeof *order; i++)
        if (t & order[i]) return order[i];
    return 0;
}
static uint32_t iop_csc_type(uint32_t t) { return t & 1 ? 1 : t & 2 ? 2 : t & 4 ? 4 : t & 8; }

/* CoreDisplay GammaChannelTableEntryCountAndType: entry precision in bits */
static int cd_gamma_bits(uint32_t t)
{
    if (t & (0x200 | 0x2000)) return 16;
    if (t & (0x100 | 0x1000)) return 12;
    if (t & (0x80 | 0x800)) return 10;
    if (t & (0x40 | 0x10 | 0x400 | 0x8)) return 8;
    return 0;
}

int main(void)
{
    static uint8_t r[0x196c];
    const uint32_t types[] = { 1, 2, 4 };

    for (unsigned k = 0; k < 3; k++) {
        memset(r, 0xa5, sizeof r);
        uint32_t idx = 0, t = types[k];
        memcpy(r, &idx, 4); memcpy(r + 4, &t, 4);
        CHECK(nmx_agdc_color_fill(r, sizeof r) == 1, "type %u answered", t);
        CHECK(rd32(r) == 0 && rd32(r + 4) == t && rd32(r + 8) == 1, "header type %u", t);
        const uint8_t *e = r + 0xc;
        const uint32_t flags = rd32(e) & 0x803fffffu;                       /* AGDCFlagsToIOPresentment */
        CHECK(flags & 0x4, "flags 0x4 (SupportsCSC / SupportsLinearization(true) / atomic EOTF)");
        CHECK((flags & 0x1f000) == 0, "no transfer-function flags: the entry accepts any curve");
        CHECK((rd64(e + 4) & 1) || (flags & 1), "entry visible for pipe/plane 0");
        if (t == NMX_AGDC_CAP_GAMMA) {
            const uint32_t it = iop_gamma_type(rd32(e + 0x10));
            CHECK(it == 0x200, "gamma type seen by CoreDisplay %#x", it);
            CHECK((it & 0x3d0) && !(it & 0x3c08), "per-channel group, not the table-only (R=G=B) group");
            CHECK(cd_gamma_bits(it) == 16, "16-bit entries");
            CHECK(it != 1, "table type: IOPresentmentEnd sends SetPipePostGammaTable, not PostCSCGamma");
            CHECK(rd64(e + 0x28) == 4096, "MaximumSize 4096");
            CHECK(rdf(e + 0x20) == 0.0f && rdf(e + 0x24) == 1.0f, "range 0..1");
        } else if (t == NMX_AGDC_CAP_LINEARIZATION) {
            const uint32_t it = iop_gamma_type(rd32(e + 0x10));
            CHECK(it & 0x3fcc, "table-based (IsLinearizationTableBased)");
            CHECK(it == 0x8 || it == 0x40 || it == 0x80 || it == 0x100 || it == 0x200,
                  "IOPresentmentEnd sends this type as SetPlaneLinearizationTable -> pipe pre-gamma table (%#x)", it);
            CHECK(rd64(e + 0x28) == 4096, "MaximumSize 4096");
        } else {
            const uint32_t it = iop_csc_type(rd32(e + 0x10));
            CHECK(it == 2, "CSC type %u (plain SetPipeColorMatrix)", it);
            CHECK(rdf(e + 0x24) == -4.0f && rdf(e + 0x28) == 4.0f, "CSC range -4..4");
        }
        for (unsigned i = 0xc + 0x32c; i < sizeof r; i++)
            if (r[i]) { CHECK(0, "type %u: byte %#x past entry 0 not cleared", t, i); break; }
    }
    memset(r, 0, sizeof r);
    r[4] = 0x10;
    CHECK(nmx_agdc_color_fill(r, sizeof r) == 0, "plane scaler (0x10) left to K5");
    r[4] = 0x80; r[5] = 0x00; r[4] = 0; r[5] = 0x80;
    CHECK(nmx_agdc_color_fill(r, sizeof r) == 0, "cursor (0x8000) not answered");
    r[5] = 0; r[4] = 2;
    CHECK(nmx_agdc_color_fill(r, 0x100) == 0, "short buffer refused");

    printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
