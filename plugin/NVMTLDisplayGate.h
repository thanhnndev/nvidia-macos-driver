/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* The allow list is checked separately. This is ownership/readiness only. */
static inline bool nvmtl_ws_display_gate(uint16_t vendor, bool nmx_owned_pipe, bool nv_headless)
{
    if (vendor == 0x1002) return nmx_owned_pipe;
    if (vendor == 0x10de) return !nv_headless;
    return false;
}
