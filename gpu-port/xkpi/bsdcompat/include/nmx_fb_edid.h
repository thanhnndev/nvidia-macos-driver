/* Bounded, complete EDID only: reject bad blocks instead of inventing identity. */
#ifndef NMX_FB_EDID_H
#define NMX_FB_EDID_H
#include <stdint.h>
#include <stddef.h>
static inline int nmx_fb_edid_valid(const void *data, size_t size, size_t capacity)
{
    static const uint8_t header[8] = {0,255,255,255,255,255,255,0};
    const uint8_t *bytes = data;
    if (!bytes || size < 128 || size > capacity || size % 128) return 0;
    for (unsigned i = 0; i < 8; i++) if (bytes[i] != header[i]) return 0;
    if ((size_t)(bytes[126] + 1) * 128 != size) return 0;
    for (size_t b = 0; b < size; b += 128) {
        uint8_t sum = 0;
        for (unsigned i = 0; i < 128; i++) sum += bytes[b + i];
        if (sum) return 0;
    }
    return 1;
}
#endif
