/*
 * Hardware colour stages (2026-11-25): NMXAGDC's answers to AGDC command 0x711 (AGDCFBGetExtendedCapability) for the
 * pipe CSC (type 0x1), pipe gamma (0x2) and linearization (0x4) capabilities. With them, CoreDisplay moves its colour
 * stages from the GPUPass shader (one full-screen pass per present) into display-pipe transactions, which NMXAccel
 * hands to NMXGPU (DCN degamma / CTM / regamma). Off unless boot-arg nmxhwcolor=1. Pure C, no kernel headers: shared
 * with tools/test_nmxagdc_color.c.
 *
 * Layout, from IOPresentment 25G332 (IOPresentmentCapabilitiesDisplayPipe::IOPresentmentCreate{CSC,Gamma,
 * Linearization}Dictionary): reply = { u32 fbIndex, u32 type, u32 count, entry[8] of 0x32c bytes }, each entry
 *   +0x00 u32 flags           (AGDCFlagsToIOPresentment: & 0x803fffff; 0x4 is what CoreDisplay's SupportsCSC,
 *                              SupportsLinearization(.., true) and IsEOTFSupportedInHW(.., atomic) require)
 *   +0x04 u64 mask            (bit i = pipe/plane i; an entry with neither its bit nor flags bit 0 is skipped)
 *   +0x0c f32 NonAtomicApplicationPeriod
 *   +0x10 u32 type            (a priority encoder keeps one bit: 1, 2, 4, 8, 0x400..0x2000, then 0x10..0x200)
 *   +0x14 u32 FixedFormat     (gamma: low byte; CSC: & 0xf)
 *   +0x18 s64 ConfigurationID
 *   gamma / linearization:  +0x20 f32 MinimumValue  +0x24 f32 MaximumValue  +0x28 s64 MaximumSize
 *   CSC:                    +0x20 u32 PixelFormats  +0x24 f32 MinimumValue  +0x28 f32 MaximumValue
 *
 * Types chosen (CoreDisplay IOPCapabilities::GammaChannelTableEntryCountAndType / IsEOTFSupportedInHW /
 * IOPresentmentDisplayPipe::IOPresentmentEnd):
 *   gamma 0x200: 16-bit per-channel table. The 0x3c08 group (0x8, 0x400..0x2000) is the table-only group: CoreDisplay
 *     then demands R = G = B, so Night Shift / calibration tables would fall back to the GPU. No transfer-function
 *     flags (0x1f000): an entry without them accepts any curve. Pipe gamma of a table type goes to the kernel as
 *     the pipe post-gamma table (DCN regamma); type 1 would go as the post-CSC gamma "VID".
 *   linearization 0x200: table-based (CoreDisplay IsLinearizationTableBased: type & 0x3fcc); IOPresentment sends a
 *     table type as the pipe pre-gamma table (DCN degamma); type 1 would go as the pre-CSC linearization "VID".
 *   CSC 0x2: IOPresentment sends types 2 / 4 with IOAccelDisplayPipeTransactionSetPipeColorMatrix (type 1 = the
 *     "Extended" call), to the kernel's pipe colour matrix (DCN CTM).
 * MaximumSize 4096 = amdgpu_dm MAX_COLOR_LUT_ENTRIES. MinimumValue 0 / MaximumValue 1 for tables; CSC -4 .. 4 (DCN
 * gamut remap S2.13 holds a bit under +-4).
 */
#ifndef NMX_AGDC_COLOR_H
#define NMX_AGDC_COLOR_H
#include <stdint.h>

#define NMX_AGDC_CAP_CSC            0x1u
#define NMX_AGDC_CAP_GAMMA          0x2u
#define NMX_AGDC_CAP_LINEARIZATION  0x4u
#define NMX_AGDC_COLOR_FLAGS        0x4u
#define NMX_AGDC_COLOR_MASK         0x1ull          /* pipe / plane 0 */
#define NMX_AGDC_GAMMA_TYPE         0x200u
#define NMX_AGDC_LIN_TYPE           0x200u
#define NMX_AGDC_CSC_TYPE           0x2u
#define NMX_AGDC_LUT_SIZE           4096u
#define NMX_AGDC_F32_0              0x00000000u
#define NMX_AGDC_F32_1              0x3f800000u
#define NMX_AGDC_F32_M4             0xc0800000u
#define NMX_AGDC_F32_4              0x40800000u

static inline void nmx_agdc_put32(uint8_t *e, unsigned off, uint32_t v)
{
    e[off] = (uint8_t)v; e[off + 1] = (uint8_t)(v >> 8); e[off + 2] = (uint8_t)(v >> 16); e[off + 3] = (uint8_t)(v >> 24);
}
static inline void nmx_agdc_put64(uint8_t *e, unsigned off, uint64_t v)
{
    nmx_agdc_put32(e, off, (uint32_t)v); nmx_agdc_put32(e, off + 4, (uint32_t)(v >> 32));
}

/* is this a colour capability NMXAGDC answers (when enabled)? */
static inline int nmx_agdc_color_type(uint32_t type)
{
    return type == NMX_AGDC_CAP_CSC || type == NMX_AGDC_CAP_GAMMA || type == NMX_AGDC_CAP_LINEARIZATION;
}

/*
 * Fill a 0x711 reply (blob = the whole 0x196c reply; fbIndex and type already in it) for a colour type.
 * Returns 1 when answered, 0 when the type is not a colour one.
 */
static inline int nmx_agdc_color_fill(uint8_t *blob, unsigned long size)
{
    if (size < 0xc + 0x32c)
        return 0;
    const uint32_t idx = (uint32_t)blob[0] | (uint32_t)blob[1] << 8 | (uint32_t)blob[2] << 16 | (uint32_t)blob[3] << 24;
    const uint32_t type = (uint32_t)blob[4] | (uint32_t)blob[5] << 8 | (uint32_t)blob[6] << 16 | (uint32_t)blob[7] << 24;
    if (!nmx_agdc_color_type(type))
        return 0;
    for (unsigned long i = 0; i < size; i++)
        blob[i] = 0;
    nmx_agdc_put32(blob, 0, idx);
    nmx_agdc_put32(blob, 4, type);
    nmx_agdc_put32(blob, 8, 1);
    uint8_t *e = blob + 0xc;
    nmx_agdc_put32(e, 0x00, NMX_AGDC_COLOR_FLAGS);
    nmx_agdc_put64(e, 0x04, NMX_AGDC_COLOR_MASK);
    nmx_agdc_put32(e, 0x0c, NMX_AGDC_F32_0);
    if (type == NMX_AGDC_CAP_CSC) {
        nmx_agdc_put32(e, 0x10, NMX_AGDC_CSC_TYPE);
        nmx_agdc_put32(e, 0x14, 0);
        nmx_agdc_put64(e, 0x18, 0);
        nmx_agdc_put32(e, 0x20, 0);
        nmx_agdc_put32(e, 0x24, NMX_AGDC_F32_M4);
        nmx_agdc_put32(e, 0x28, NMX_AGDC_F32_4);
    } else {
        nmx_agdc_put32(e, 0x10, type == NMX_AGDC_CAP_GAMMA ? NMX_AGDC_GAMMA_TYPE : NMX_AGDC_LIN_TYPE);
        nmx_agdc_put32(e, 0x14, 0);
        nmx_agdc_put64(e, 0x18, 0);
        nmx_agdc_put32(e, 0x20, NMX_AGDC_F32_0);
        nmx_agdc_put32(e, 0x24, NMX_AGDC_F32_1);
        nmx_agdc_put64(e, 0x28, NMX_AGDC_LUT_SIZE);
    }
    return 1;
}
#endif
