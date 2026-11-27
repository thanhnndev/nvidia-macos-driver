/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Fence table (WS-CPU cut 1, item 1): the NVMTL plugin's per-IOSurface fence marks (NMXFencePending, ...Submitted,
 * ...Done and their times) handed to NMXAccel with one call on the NMXDrmClient connection (selector 3,
 * kNmxDrmSelFence) instead of IOSurfaceSetValues on every surface (two to three IOSurface property writes per surface
 * per frame in WindowServer). nmxSurfValue looks the six keys up here first and falls back to the surface's properties.
 *
 * Gate: debug.nmxaccel_fence_table (default 0). While it is 0 the selector answers kIOReturnUnsupported and
 * nmxSurfValue never looks here, so behaviour is exactly the property path. The plugin side is NVMTL_NMX_FENCE_CALL
 * (default off); it falls back to properties for the rest of the process on Unsupported and on a refused call
 * (kIOReturnNoSpace: every row in flight). A refused call writes nothing and drops every row of that client, so no
 * row can shadow the property values the process writes from then on. Any write of the gate clears the whole table
 * (a process that fell back while it was 0 must not be shadowed by its old rows when it is turned on again).
 *
 * ABI (struct input of selector 3, no scalars, no output), version 1:
 *   nmx_fence_call_v1 { u32 version = 1, u32 n (1..16), nmx_fence_entry_v1 e[n] }, size exactly 8 + 32 * n.
 *   entry { u32 surfaceID, u32 flags, u64 seq, u64 tNs, u64 gpuEndNs }; flags is exactly one of:
 *     PENDING            NMXFencePending = seq, NMXFencePendingT = tNs
 *     PENDING|SUBMITTED  the same, and NMXFenceSubmitted = seq
 *     SUBMITTED          NMXFenceSubmitted = seq
 *     DONE               NMXFenceDone = seq, NMXFenceDoneT = tNs, NMXFenceGpuEndT = gpuEndNs
 *   Anything else (unknown bits, DONE with PENDING/SUBMITTED, 0, surfaceID 0) refuses the whole call
 *   (kIOReturnBadArgument). The surface ID is only a table key: it is never looked up as an IOSurface here.
 * Values are stored as written (last writer wins), as the IOSurface properties were.
 */
#ifndef NMX_FENCE_TABLE_H
#define NMX_FENCE_TABLE_H
#include <stdint.h>

#define NMX_FENCE_CALL_VERSION  1u
#define NMX_FENCE_CALL_MAX      16u
#define NMX_FENCE_F_PENDING     1u
#define NMX_FENCE_F_SUBMITTED   2u
#define NMX_FENCE_F_DONE        4u

typedef struct nmx_fence_entry_v1 {
    uint32_t surfaceID;
    uint32_t flags;
    uint64_t seq;
    uint64_t tNs;        /* PENDING: commit time (NMXFencePendingT); DONE: completion-thread time (NMXFenceDoneT) */
    uint64_t gpuEndNs;   /* DONE: GPU end time (NMXFenceGpuEndT), 0 = none */
} nmx_fence_entry_v1;
typedef struct nmx_fence_call_v1 {
    uint32_t version;
    uint32_t n;
    nmx_fence_entry_v1 e[NMX_FENCE_CALL_MAX];
} nmx_fence_call_v1;
#define NMX_FENCE_CALL_SIZE(n) (8u + 32u * (uint32_t)(n))
_Static_assert(sizeof(nmx_fence_entry_v1) == 32, "fence entry ABI");

/* the six keys, in this order */
enum { kNmxFkPending = 0, kNmxFkPendingT, kNmxFkSubmitted, kNmxFkDone, kNmxFkDoneT, kNmxFkGpuEndT, kNmxFkCount };

#ifdef __cplusplus
extern "C" {
#endif
extern int nmx_fence_table_on;   /* debug.nmxaccel_fence_table (registered by nvrm-accel.cpp) */
/* counters (debug.nmxaccel_fence_table_*) */
extern int64_t nmx_fence_table_calls, nmx_fence_table_entries, nmx_fence_table_refused, nmx_fence_table_bad,
               nmx_fence_table_hits, nmx_fence_table_misses, nmx_fence_table_evicted, nmx_fence_table_cleared;
/* debug.nmxaccel_fence_table write: set the gate, clear every row */
void nmx_fence_table_set(int on);
/* -1 when key is not one of the six; else kNmxFk* */
int nmx_fence_key_index(const char *key);
/* true and *v when the table holds that key for the surface (gate on); false: read the property */
bool nmx_fence_table_get(uint32_t surfaceID, int keyIndex, unsigned long long *v);
#ifdef __cplusplus
}
#endif
#endif
