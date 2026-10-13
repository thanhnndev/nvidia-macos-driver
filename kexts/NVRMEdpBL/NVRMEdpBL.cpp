/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// Packages nvrm_edpbl.cpp (AUX-only eDP panel brightness) as a kext of its own, for an NVRM that was built without it.
// NVRMFB asks this service first for "NVRMBacklight"; it answers kIOReturnNotFound for panels that RM drives.
#include "nv-xnu.h"
#include <IOKit/IOService.h>
#include <mach/kmod.h>

extern "C" {
nv_state_t *nv_xnu_gpu_state(void);                         // NVRM
int nvrm_edpbl_get(NvU32 gpuId, NvU32 *percent);            // nvrm_edpbl.cpp
int nvrm_edpbl_set(NvU32 gpuId, NvU32 percent);
}

class NVRMEdpBL : public IOService {
    OSDeclareDefaultStructors(NVRMEdpBL)
public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        registerService();
        return true;
    }
    // p1 = NvU32 *percent, p2 != NULL sets it (the NVRM function of the same name)
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override
    {
        if (!fn || !fn->isEqualTo("NVRMBacklight")) return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
        NvU32 *percent = (NvU32 *)p1;
        nv_state_t *nv = nv_xnu_gpu_state();
        if (!percent || !nv) return kIOReturnNotFound;
        int r = p2 ? nvrm_edpbl_set(nv->gpu_id, *percent) : nvrm_edpbl_get(nv->gpu_id, percent);
        return r > 0 ? kIOReturnSuccess : (r < 0 ? kIOReturnIOError : kIOReturnNotFound);
    }
};
#define super IOService
OSDefineMetaClassAndStructors(NVRMEdpBL, IOService)
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVRMEdpBL, "0.1", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }
