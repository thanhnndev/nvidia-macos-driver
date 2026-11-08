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
 * generation changes whenever the bridge itself puts its home buffer back on the outputs (restore, hotplug, display
 * power): the caller's idea of what is on screen is then wrong, and it must start over from home.
 */
#pragma once

#define NMX_FLIP_ABI_VERSION 1u

#define NMX_FLIP_FN_CAPS     "nmxFlipCaps"
#define NMX_FLIP_FN_IMPORT   "nmxFlipImport"
#define NMX_FLIP_FN_BUFFERS  "nmxFlipBuffers"
#define NMX_FLIP_FN_RELEASE  "nmxFlipRelease"
#define NMX_FLIP_FN_SUBMIT   "nmxFlipSubmit"
#define NMX_FLIP_FN_STATUS   "nmxFlipStatus"

#define NMX_FLIP_HOME        0u
#define NMX_FLIP_NONE        0xffffffffu

enum {
    NMX_FLIP_CAP_SUBMIT       = 1u << 0,   // nmxFlipSubmit / nmxFlipStatus work (required for anything else)
    NMX_FLIP_CAP_IMPORT       = 1u << 1,   // nmxFlipImport: scan out caller memory (zero copy)
    NMX_FLIP_CAP_VRAM_CHAIN   = 1u << 2,   // nmxFlipBuffers: extra VRAM scan-out buffers
    NMX_FLIP_CAP_XRGB8888     = 1u << 8,   // formats the display accepts for an import
    NMX_FLIP_CAP_XRGB2101010  = 1u << 9,
};
enum {
    NMX_FLIP_FMT_XRGB8888    = 1u,   // IOSurface 'BGRA' (0x42475241)
    NMX_FLIP_FMT_XRGB2101010 = 2u,   // IOSurface 'l10r' (0x6C313072)
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
