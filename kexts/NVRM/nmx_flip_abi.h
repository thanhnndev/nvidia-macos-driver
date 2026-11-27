/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Page flips on the NMXGPU display bridge (NMXAMDFB, nmx_fb.cpp / nmx_fb_bridge.c), asked for by NMXAccel's display
 * pipe through IOService::callPlatformFunction on the framebuffer, the same channel as nvScanoutInfo. Every call takes
 * one struct NMXFlipRequest (param1). A framebuffer that does not know a name answers kIOReturnUnsupported
 * (IOService's default), which is how NMXAccel tells the API is missing: it then never flips and keeps the K6 copy.
 *
 * Handle 0 (NMX_FLIP_HOME) is the bridge's own scan-out buffer, the one nvScanoutInfo describes and the K6 copy writes.
 *
 *   nmxFlipCaps     out: caps, maxImports, width/height/pitch of the lit mode, generation.
 *   nmxFlipImport   in: md (an IOMemoryDescriptor * the caller has prepared and keeps prepared until release), offset,
 *                   width, height, pitch, format. out: handle (> 0), gpuAddr (the address DCN will fetch). The bridge
 *                   turns the descriptor's pages into a scan-out buffer (GTT, linear) and a DRM framebuffer.
 *   nmxFlipBuffers  out: nbuffers and buf[] = the VRAM swap chain (buf[0] is home). Created on first call.
 *   nmxFlipRelease  in: handle. kIOReturnBusy while that handle is on screen or pending.
 *   nmxFlipSubmit   in: handle. Queues a flip to it, latched by the display at the next vertical blank (never
 *                   immediate, never torn). Returns at once; out: seq. kIOReturnBusy while an earlier flip is pending.
 *   nmxFlipStatus   out: seqSubmitted, seqLatched, current, pending, generation, vblankCount, vblankNs. Never sleeps.
 *
 * Version 2 (out-31, FRAME-PATH rows 1 and 3) appends struct NMXFlipRequest2's fields; an NMXGPU that only knows
 * version 1 answers kIOReturnBadArgument to a version 2 call, and the caller drops back to version 1.
 *   nmxFlipCaps     v2 adds NMX_FLIP_CAP_FENCE / NMX_FLIP_CAP_NOTIFY when those paths are on in NMXGPU.
 *   nmxFlipImport   v2 in: surfaceId (the IOSurface ID): NMXGPU records render fences only for imported surfaces.
 *   nmxFlipSubmit   v2 in: surfaceId and flags NMX_FLIP_SUBMIT_FENCE(_ONLY). The commit then waits for that IOSurface's
 *                   last GPU job in the kernel (drm_atomic_helper_wait_for_fences), out: fenced = 1. With FENCE_ONLY and
 *                   no job known for the surface, or the last one recorded before fenceAfterNs (an older frame's):
 *                   kIOReturnNotFound, nothing queued (the caller waits its own way).
 *   nmxFlipStatus   v2 out: latchNs, mach_absolute_time of the pageflip interrupt that latched seqLatched.
 *   nmxFlipNotify   v2 in: notify, refcon (notify NULL = off). notify(refcon, seq, pflipNs) runs on NMXGPU's interrupt
 *                   thread right after each flip latched: it must only wake something (no locks, never sleep).
 *
 * generation changes whenever the bridge itself puts its home buffer back on the outputs (restore, hotplug, display
 * power): the caller's idea of what is on screen is then wrong, and it must start over from home.
 */
#pragma once

#define NMX_FLIP_ABI_VERSION 1u
#define NMX_FLIP_ABI_VERSION2 2u
#define NMX_FLIP_FN_NOTIFY   "nmxFlipNotify"

#define NMX_FLIP_FN_CAPS     "nmxFlipCaps"
#define NMX_FLIP_FN_IMPORT   "nmxFlipImport"
#define NMX_FLIP_FN_BUFFERS  "nmxFlipBuffers"
#define NMX_FLIP_FN_RELEASE  "nmxFlipRelease"
#define NMX_FLIP_FN_SUBMIT   "nmxFlipSubmit"
#define NMX_FLIP_FN_STATUS   "nmxFlipStatus"
/*
 * nmxFlipColor (2026-11-25, hardware colour stages, boot-arg nmxhwcolor=1): struct NMXColorRequest, not an
 * NMXFlipRequest. Stages DCN degamma / CTM / regamma for the next flip (absent = bypass); NMX_COLOR_COMMIT commits them
 * at once (a transaction without a plane). The tables are copied: the caller's buffers are not kept. An NMXGPU without
 * it answers kIOReturnUnsupported.
 */
#define NMX_FLIP_FN_COLOR    "nmxFlipColor"

#define NMX_FLIP_HOME        0u
#define NMX_FLIP_NONE        0xffffffffu

enum {
    NMX_FLIP_CAP_SUBMIT       = 1u << 0,   // nmxFlipSubmit / nmxFlipStatus work (required for anything else)
    NMX_FLIP_CAP_IMPORT       = 1u << 1,   // nmxFlipImport: scan out caller memory (zero copy)
    NMX_FLIP_CAP_VRAM_CHAIN   = 1u << 2,   // nmxFlipBuffers: extra VRAM scan-out buffers
    NMX_FLIP_CAP_FENCE        = 1u << 3,   // v2: nmxFlipSubmit waits for the surface's render fence in the kernel
    NMX_FLIP_CAP_NOTIFY       = 1u << 4,   // v2: nmxFlipNotify, flip completion from the pageflip interrupt
    NMX_FLIP_CAP_XRGB8888     = 1u << 8,   // formats the display accepts for an import
    NMX_FLIP_CAP_XRGB2101010  = 1u << 9,
};
enum {
    NMX_FLIP_FMT_XRGB8888    = 1u,   // IOSurface 'BGRA' (0x42475241)
    NMX_FLIP_FMT_XRGB2101010 = 2u,   // IOSurface 'l10r' (0x6C313072)
};
enum {
    NMX_FLIP_SUBMIT_FENCE      = 1u,   // v2 submit flags: attach the surface's last kernel GPU fence if one is known
    NMX_FLIP_SUBMIT_FENCE_ONLY = 2u,   // ... and refuse (kIOReturnNotFound) when none is
};
#define NMX_FLIP_MAX_BUFFERS 3u

struct NMXFlipBuffer {
    unsigned long long phys;      // CPU physical address (the GPU BAR)
    void              *kva;       // kernel mapping (write-combined BAR view)
    unsigned long long size;
    unsigned long long gpuAddr;
    unsigned int       handle;
    unsigned int       pitch;
};

struct NMXFlipRequest {
    unsigned int       version;         // in: NMX_FLIP_ABI_VERSION
    unsigned int       flags;           // in: 0
    unsigned int       handle;          // in: release/submit; out: import
    unsigned int       format;          // in: import (NMX_FLIP_FMT_*)
    unsigned int       width, height;   // in: import; out: caps (the lit mode)
    unsigned int       pitch;           // in: import (bytes per row); out: caps (home buffer pitch)
    unsigned int       caps;            // out: caps
    unsigned int       maxImports;      // out: caps
    unsigned int       nbuffers;        // out: buffers
    void              *md;              // in: import (IOMemoryDescriptor *, prepared by the caller)
    unsigned long long offset;          // in: import, byte offset of pixel (0, 0) in md
    unsigned long long gpuAddr;         // out: import
    unsigned long long seq;             // out: submit
    unsigned long long seqSubmitted;    // out: status
    unsigned long long seqLatched;      // out: status, the last seq the display has latched (on screen)
    unsigned int       current;         // out: status, handle on screen
    unsigned int       pending;         // out: status, handle queued (NMX_FLIP_NONE = none)
    unsigned int       generation;      // out: caps/status
    int                err;             // out: Linux errno of the step that failed, 0 on success
    unsigned long long vblankCount;     // out: status
    unsigned long long vblankNs;        // out: status, uptime of the last vertical blank
    struct NMXFlipBuffer buf[NMX_FLIP_MAX_BUFFERS];   // out: buffers
};
enum {
    NMX_COLOR_DEGAMMA = 1u,   // degamma: NMX_COLOR_LUT_ENTRIES x {u16 r, g, b, pad} (drm_color_lut)
    NMX_COLOR_CTM     = 2u,   // ctm: 9 x u64, S31.32 sign-magnitude, row-major (drm_color_ctm)
    NMX_COLOR_GAMMA   = 4u,   // gamma: as degamma
    NMX_COLOR_COMMIT  = 8u,   // commit now (blocking) instead of with the next flip
    NMX_COLOR_SYNC    = 16u,  // alone: stage nothing; commit what is staged unless a flip carried it (after a present).
                              // err EINVAL = DM refused that state (the screen keeps the last accepted one)
};
/* NMXGPU's struct nmx_colorreq (nmx_fb_bridge.c) must match */
struct NMXColorRequest {
    unsigned int              version;    // in: 1
    unsigned int              flags;      // in: NMX_COLOR_*
    const void               *degamma;
    const unsigned long long *ctm;
    const void               *gamma;
    int                       err;        // out: errno of the failing step
    unsigned int              pad;
};
/* version 2: NMXGPU's struct nmx_flipreq2 (nmx_fb_bridge.c) must match */
struct NMXFlipRequest2 {
    struct NMXFlipRequest v1;           // v1.version = NMX_FLIP_ABI_VERSION2
    unsigned int       surfaceId;       // in: submit
    unsigned int       fenced;          // out: submit
    unsigned long long latchNs;         // out: status
    void (*notify)(void *refcon, unsigned long long seq, unsigned long long pflipNs);   // in: notify
    void              *refcon;          // in: notify
    unsigned long long fenceAfterNs;    // in: submit, refuse a fence recorded before this mach time (0 = any)
    unsigned long long fenceSetNs;      // out: submit, when the attached fence was recorded
};
#ifdef __cplusplus
static_assert(sizeof(struct NMXFlipRequest) == 240, "NMXFlipRequest v1 layout (NMXGPU nmx_flipreq)");
static_assert(sizeof(struct NMXFlipRequest2) == 288, "NMXFlipRequest v2 layout (NMXGPU nmx_flipreq2)");
static_assert(sizeof(struct NMXColorRequest) == 40, "NMXColorRequest layout (NMXGPU nmx_colorreq)");
#else
_Static_assert(sizeof(struct NMXFlipRequest) == 240, "NMXFlipRequest v1 layout (NMXGPU nmx_flipreq)");
_Static_assert(sizeof(struct NMXFlipRequest2) == 288, "NMXFlipRequest v2 layout (NMXGPU nmx_flipreq2)");
_Static_assert(sizeof(struct NMXColorRequest) == 40, "NMXColorRequest layout (NMXGPU nmx_colorreq)");
#endif
