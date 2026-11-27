/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef NMX_AGDC_CLIENT_H
#define NMX_AGDC_CLIENT_H

#include <stdint.h>

/*
 * IOPresentment command 0x2001 carries a passive 32-bit client declaration.
 * It has no output payload. Decode it without assuming input alignment.
 */
static inline int nmx_agdc_decode_client_set(const void *in, unsigned long size,
                                            unsigned long output_size, uint32_t *value)
{
    if (!in || !value || size != sizeof(*value) || output_size != 0) return 0;
    __builtin_memcpy(value, in, sizeof(*value));
    return 1;
}

#endif
