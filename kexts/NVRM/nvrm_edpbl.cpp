/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// Laptop panels whose brightness is only reachable over the eDP AUX channel (DPCD 0x720..0x723).
//
// Some eDP panels (OLED ones in particular) have no PWM input: eDP_GENERAL_CAP_2 (DPCD 0x702) lacks
// BACKLIGHT_BRIGHTNESS_PWM_PIN_CAP and has BACKLIGHT_BRIGHTNESS_AUX_SET_CAP, and the brightness is set by writing
// the 16-bit level to DPCD 0x722/0x723 after switching the panel to "DPCD brightness control" (0x721 bits 1:0 = 2).
// RM's NV0073_CTRL_CMD_SPECIFIC_SET_BACKLIGHT_BRIGHTNESS (nvKmsSetBacklight) returns success on such a panel
// without the panel changing (GA403WM, ATNA OLED, 2026-10-12): the registers stay 0x721 = 0, 0x722/3 = 0.
// This module drives the registers itself, through RM's own AUX control (NV0073_CTRL_CMD_DP_AUXCH_CTRL, the one
// NVKMS uses for DPCD access), on a private kernel RM client like nvrm_fbinfo.cpp.
//
// It only takes over when the panel can not be driven by RM's PWM path (no PWM pin capability). Boot-arg
// nvrmedpbl: 0 = never, 1 = AUX-only panels (default), 2 = every panel that has the AUX brightness capability.

#include "nv-xnu.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "nvtypes.h"
#include "nvos.h"
#include "nvmisc.h"
#include "nv-kernel-rmapi-ops.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
void NV_API_CALL rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd);
}

#define EBLOG(fmt, ...) kprintf("NVRM-edpbl: " fmt "\n", ##__VA_ARGS__)

// ctrl0073dp.h / ctrl0073system.h, restated like nvrm_fbinfo.cpp does (this file stays free of the ctrl headers)
enum {
    kClsRootClient      = 0x00000041u,
    kClsDevice0         = 0x00000080u,
    kClsSubdevice0      = 0x00002080u,
    kClsDisplayCommon   = 0x00000073u,
    kCmdGpuAttachIds    = 0x00000215u,   // NV0000_CTRL_CMD_GPU_ATTACH_IDS
    kCmdInternalDisplays= 0x00730116u,   // NV0073_CTRL_CMD_SYSTEM_GET_INTERNAL_DISPLAYS
    kCmdAuxCh           = 0x00731341u,   // NV0073_CTRL_CMD_DP_AUXCH_CTRL
    kAuxCmdAuxRead      = 0x00000009u,   // CMD_TYPE_AUX (bit 3) | REQ_TYPE_READ (1)
    kAuxCmdAuxWrite     = 0x00000008u,   // CMD_TYPE_AUX (bit 3) | REQ_TYPE_WRITE (0)
    kGpuInvalidId       = 0xffffffffu,
    kMaxAttachedGpus    = 32u
};
struct EbAttachIds { NvU32 gpuIds[kMaxAttachedGpus]; NvU32 failedId; };
struct EbInternalDisplays { NvU32 subDeviceInstance, internalDisplaysMask, availableInternalDisplaysMask; };
struct EbAuxParams {                     // NV0073_CTRL_DP_AUXCH_CTRL_PARAMS
    NvU32 subDeviceInstance;
    NvU32 displayId;
    NvU8  bAddrOnly;
    NvU32 cmd;
    NvU32 addr;
    NvU8  data[16];
    NvU32 size;                          // in: bytes - 1, out: bytes transferred
    NvU32 replyType;
    NvU32 retryTimeMs;
};

// eDP DPCD (DP 1.4 / eDP 1.4b), VESA names
enum {
    kDpcdEdpRev        = 0x700,
    kDpcdEdpCap1       = 0x701,          // bit 0 TCON_BACKLIGHT_ADJUSTMENT_CAP, bit 2 BACKLIGHT_AUX_ENABLE_CAP
    kDpcdEdpCap2       = 0x702,          // bit 0 BRIGHTNESS_PWM_PIN_CAP, bit 1 BRIGHTNESS_AUX_SET_CAP, bit 2 BYTE_COUNT
    kDpcdDisplayCtl    = 0x720,          // bit 0 BACKLIGHT_ENABLE
    kDpcdBlMode        = 0x721,          // bits 1:0 control mode, 2 = DPCD brightness
    kDpcdBlLevelMsb    = 0x722,
    kDpcdPwmgenBits    = 0x724,
    kDpcdPwmgenBitsMax = 0x726
};

static inline void ebZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static NvV32 ebAlloc(NvU32 hRoot, NvU32 hParent, NvU32 hClass, void *pParams, NvU32 size, NvU32 *hOut)
{
    nvidia_kernel_rmapi_ops_t ops;
    ebZero(&ops, sizeof ops);
    ops.op = NV04_ALLOC;
    ops.params.alloc.hRoot            = hRoot;
    ops.params.alloc.hObjectParent    = hParent;
    ops.params.alloc.hObjectNew       = 0;
    ops.params.alloc.hClass           = (NvV32)hClass;
    ops.params.alloc.pAllocParms      = (NvP64)(NvUPtr)pParams;
    ops.params.alloc.pRightsRequested = (NvP64)0;
    ops.params.alloc.paramsSize       = size;
    ops.params.alloc.flags            = NVOS64_FLAGS_NONE;
    rm_kernel_rmapi_op(NULL, &ops);
    if (ops.params.alloc.status == 0 && hOut) *hOut = ops.params.alloc.hObjectNew;
    return ops.params.alloc.status;
}

static NvV32 ebControl(NvU32 hClient, NvU32 hObject, NvU32 cmd, void *pParams, NvU32 size)
{
    nvidia_kernel_rmapi_ops_t ops;
    ebZero(&ops, sizeof ops);
    ops.op = NV04_CONTROL;
    ops.params.control.hClient    = hClient;
    ops.params.control.hObject    = hObject;
    ops.params.control.cmd        = cmd;
    ops.params.control.params     = (NvP64)(NvUPtr)pParams;
    ops.params.control.paramsSize = size;
    rm_kernel_rmapi_op(NULL, &ops);
    return ops.params.control.status;
}

static void ebFree(NvU32 hRoot, NvU32 hParent, NvU32 hObject)
{
    nvidia_kernel_rmapi_ops_t ops;
    ebZero(&ops, sizeof ops);
    ops.op = NV01_FREE;
    ops.params.free.hRoot         = hRoot;
    ops.params.free.hObjectParent = hParent;
    ops.params.free.hObjectOld    = hObject;
    rm_kernel_rmapi_op(NULL, &ops);
}

// One private RM client with a DISPLAY_COMMON object, kept between calls and rebuilt after a failure.
static struct {
    IOLock  *lock;
    NvU32    hClient, hDevice, hSub, hDisp;
    NvU32    displayId;
    int      state;                      // 0 not probed, 1 AUX brightness active, 2 not applicable (final)
    int      probeTries;
    NvU32    nbytes;                     // brightness register width: 1 or 2
    NvU32    max;                        // highest level
    bool     auxEnable;                  // panel wants BACKLIGHT_ENABLE over AUX
    NvU32    percent;                    // last value set, 100 until the first set
    bool     logged;
} gEb = { NULL, 0, 0, 0, 0, 0, 0, 0, 1, 0, false, 100, false };

static void ebDropClient()
{
    if (gEb.hClient) ebFree(gEb.hClient, gEb.hClient, gEb.hClient);   // frees the whole tree under the root
    gEb.hClient = gEb.hDevice = gEb.hSub = gEb.hDisp = 0;
}

static bool ebEnsureClient(NvU32 gpuId)
{
    if (gEb.hClient && gEb.hDisp) return true;
    ebDropClient();
    NvV32 st = ebAlloc(0, 0, kClsRootClient, NULL, 0, &gEb.hClient);
    if (st != 0 || !gEb.hClient) { EBLOG("root client -> 0x%x", (unsigned)st); gEb.hClient = 0; return false; }
    do {
        EbAttachIds at; ebZero(&at, sizeof at);
        at.gpuIds[0] = gpuId; at.gpuIds[1] = kGpuInvalidId;
        st = ebControl(gEb.hClient, gEb.hClient, kCmdGpuAttachIds, &at, sizeof at);
        if (st != 0) { EBLOG("attach ids -> 0x%x", (unsigned)st); break; }
        NV0080_ALLOC_PARAMETERS dp; ebZero(&dp, sizeof dp);
        dp.deviceId = 0; dp.hClientShare = gEb.hClient;
        st = ebAlloc(gEb.hClient, gEb.hClient, kClsDevice0, &dp, sizeof dp, &gEb.hDevice);
        if (st != 0 || !gEb.hDevice) { EBLOG("device -> 0x%x", (unsigned)st); break; }
        NV2080_ALLOC_PARAMETERS sp; ebZero(&sp, sizeof sp);
        sp.subDeviceId = 0;
        st = ebAlloc(gEb.hClient, gEb.hDevice, kClsSubdevice0, &sp, sizeof sp, &gEb.hSub);
        if (st != 0 || !gEb.hSub) { EBLOG("subdevice -> 0x%x", (unsigned)st); break; }
        st = ebAlloc(gEb.hClient, gEb.hDevice, kClsDisplayCommon, NULL, 0, &gEb.hDisp);
        if (st != 0 || !gEb.hDisp) { EBLOG("display common -> 0x%x", (unsigned)st); break; }
        return true;
    } while (0);
    ebDropClient();
    return false;
}

static bool ebAux(bool write, NvU32 addr, NvU8 *buf, NvU32 n)
{
    EbAuxParams p; ebZero(&p, sizeof p);
    p.displayId = gEb.displayId;
    p.cmd  = write ? kAuxCmdAuxWrite : kAuxCmdAuxRead;
    p.addr = addr;
    p.size = n - 1;
    if (write) for (NvU32 i = 0; i < n; i++) p.data[i] = buf[i];
    NvV32 st = ebControl(gEb.hClient, gEb.hDisp, kCmdAuxCh, &p, sizeof p);
    if (st != 0 || p.size != n) return false;
    if (!write) for (NvU32 i = 0; i < n; i++) buf[i] = p.data[i];
    return true;
}

// Reads the panel's capabilities and decides whether this module drives it. Sets gEb.state when it is final.
static bool ebProbe(NvU32 gpuId)
{
    int policy = 1;
    int v = 0;
    if (PE_parse_boot_argn("nvrmedpbl", &v, sizeof v)) policy = v;
    if (policy <= 0) { gEb.state = 2; return false; }

    if (!ebEnsureClient(gpuId)) return false;                       // RM not ready yet: try again later
    EbInternalDisplays id; ebZero(&id, sizeof id);
    NvV32 st = ebControl(gEb.hClient, gEb.hDisp, kCmdInternalDisplays, &id, sizeof id);
    if (st != 0) return false;
    if (!id.availableInternalDisplaysMask) { gEb.state = 2; return false; }   // no laptop panel (desktop card)
    gEb.displayId = id.availableInternalDisplaysMask & (0u - id.availableInternalDisplaysMask);

    NvU8 caps[4] = { 0 };
    if (!ebAux(false, kDpcdEdpRev, caps, 4)) return false;           // panel not powered yet, or LVDS: no AUX
    bool adjust  = (caps[1] & 0x01) != 0;
    bool pwmPin  = (caps[2] & 0x01) != 0;
    bool auxSet  = (caps[2] & 0x02) != 0;
    bool twoByte = (caps[2] & 0x04) != 0;
    gEb.auxEnable = (caps[1] & 0x04) != 0;
    gEb.nbytes = twoByte ? 2 : 1;

    bool use = adjust && auxSet && (policy >= 2 || !pwmPin);
    if (!use) {
        EBLOG("display 0x%x: eDP rev 0x%02x caps 0x%02x 0x%02x (AUX brightness %s, PWM pin %s): left to RM",
              (unsigned)gEb.displayId, caps[0], caps[1], caps[2], auxSet ? "yes" : "no", pwmPin ? "yes" : "no");
        gEb.state = 2;
        return false;
    }

    NvU8 bits[1] = { 0 };
    NvU32 pn = 0;
    if (ebAux(false, kDpcdPwmgenBits, bits, 1)) pn = bits[0] & 0x1f;
    if (!pn && ebAux(false, kDpcdPwmgenBitsMax, bits, 1)) pn = bits[0] & 0x1f;
    NvU32 width = gEb.nbytes * 8;
    if (!pn || pn > width) pn = width;
    gEb.max = (pn >= 32) ? 0xffffffffu : ((1u << pn) - 1u);
    gEb.state = 1;
    EBLOG("display 0x%x: eDP rev 0x%02x caps 0x%02x 0x%02x: brightness over AUX, %u byte(s), %u bits, max %u",
          (unsigned)gEb.displayId, caps[0], caps[1], caps[2], (unsigned)gEb.nbytes, (unsigned)pn, (unsigned)gEb.max);
    return true;
}

static void ebLockInit()
{
    if (gEb.lock) return;
    IOLock *l = IOLockAlloc();
    if (l && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&gEb.lock)) IOLockFree(l);
}

// true when this module drives the panel. A probe that fails for now (RM or the panel not ready) leaves the panel to
// RM's own path, as before this module existed; it is repeated a few times.
static bool ebActive(NvU32 gpuId)
{
    if (gEb.state == 1) return true;
    if (gEb.state == 2 || gEb.probeTries >= 4) return false;
    gEb.probeTries++;
    return ebProbe(gpuId);
}

// 1: handled, 0: not this module's panel (use RM), -1: this module's panel but the access failed
extern "C" int nvrm_edpbl_get(NvU32 gpuId, NvU32 *percent)
{
    ebLockInit();
    if (!gEb.lock || !percent) return 0;
    IOLockLock(gEb.lock);
    bool active = ebActive(gpuId);
    if (active) *percent = gEb.percent;
    IOLockUnlock(gEb.lock);
    return active ? 1 : 0;
}

extern "C" int nvrm_edpbl_set(NvU32 gpuId, NvU32 percent)
{
    ebLockInit();
    if (!gEb.lock) return 0;
    IOLockLock(gEb.lock);
    int r = 0;
    if (ebActive(gpuId)) {
        if (percent > 100) percent = 100;
        NvU32 level = (NvU32)(((NvU64)percent * gEb.max + 50) / 100);
        NvU8 b[2] = { (NvU8)(level >> 8), (NvU8)level };
        if (gEb.nbytes == 1) b[0] = (NvU8)level;
        bool ok = false;
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            if (!ebEnsureClient(gpuId)) break;
            // level first, then the mode: the panel never sits in DPCD mode with a stale level
            ok = ebAux(true, kDpcdBlLevelMsb, b, gEb.nbytes);
            if (ok) {
                NvU8 mode = 0;
                ok = ebAux(false, kDpcdBlMode, &mode, 1);
                if (ok && (mode & 0x03) != 0x02) { mode = (NvU8)((mode & ~0x03) | 0x02); ok = ebAux(true, kDpcdBlMode, &mode, 1); }
            }
            if (ok && gEb.auxEnable) {
                NvU8 ctl = 0;
                if (ebAux(false, kDpcdDisplayCtl, &ctl, 1) && !(ctl & 0x01)) { ctl |= 0x01; ok = ebAux(true, kDpcdDisplayCtl, &ctl, 1); }
            }
            if (!ok) ebDropClient();       // stale handles (GPU reset) or a sleeping panel: rebuild once, then give up
        }
        if (ok) {
            gEb.percent = percent;
            if (!gEb.logged) { gEb.logged = true; EBLOG("first brightness write: %u%% = level %u of %u", (unsigned)percent, (unsigned)level, (unsigned)gEb.max); }
            r = 1;
        } else r = -1;
    }
    IOLockUnlock(gEb.lock);
    return r;
}
