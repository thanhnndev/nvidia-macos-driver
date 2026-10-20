/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
/* Work profile: one Vulkan backend per process. A private probe can explicitly
 * choose AMD; WindowServer and ordinary applications ignore this probe setting. */
static inline uint16_t nvmtl_hybrid_vendor(const char *process, const char *probe_gpu,
                                          bool nvidia_present, uint16_t candidate)
{
    if (process && !strcmp(process, "WindowServer")) return 0x10de;
    bool probe = process && (!strcmp(process, "nvmtltest") || !strcmp(process, "mtlprobe") || !strcmp(process, "nvmtlrender"));
    if (probe && probe_gpu) {
        if (!strcmp(probe_gpu, "amd")) return 0x1002;
        if (!strcmp(probe_gpu, "nvidia")) return 0x10de;
    }
    return nvidia_present ? 0x10de : candidate;
}
