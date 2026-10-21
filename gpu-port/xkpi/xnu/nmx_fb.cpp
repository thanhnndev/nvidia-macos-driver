/*
 * The display bridge, IOKit side: an IOFramebuffer whose VRAM is the scan-out buffer the DRM side lit
 * (override/src/nmx_fb_bridge.c). WindowServer draws into it and the GPU's display engine scans it out; there is no
 * copy. One mode (the largest connected output's), XRGB8888.
 *
 * The IOFramebuffer behaviour (one fixed mode, timing, power acknowledged at once, the colour answers WindowServer
 * asks, a 60 Hz timer as the VBL until the display interrupt is wired) is the same as NMIntelFB's, which met the
 * same WindowServer.
 */
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread_call.h>
#include "nmx_port.h"
#include "../../../kexts/NVRM/nvrm_vram_abi.h"

#define FBLOG(fmt, ...) IOLog("NullMoth display: " fmt "\n", ##__VA_ARGS__)

static IOPMPowerState kFbPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

class NMXGPUFB : public IOFramebuffer {
    OSDeclareDefaultStructors(NMXGPUFB)
    void *fDev = nullptr;
    nmx_fb_info fInfo = {};
    nmx_fb_display_info fDisplay = {};
    uint32_t fRefresh16 = 60 << 16;
    uint64_t fFrameNs = 16666667ull;
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl, fConnect;
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0;
    volatile bool fStopping = false;
    bool fPowerRegistered = false;
    bool fForeign = false;      // head 4 of NVAccel's display machine (the NV controller group)
    bool fBeside = false;       // another framebuffer on this GPU's PCI device already drives the console
    unsigned fTraceN = 0;

    /* ga403wm b15: WindowServer only said "Failed to submit transaction: IOPRESENTMENT_METHOD_FAILURE:
     * IOReturn 0x2d8" (kIOReturnNotReady) for the OLED, which names neither the call nor the stage. Say which of
     * the framebuffer calls its display pipe makes, once each, in boot order. */
    void trace(const char *what, unsigned long long a = 0, unsigned long long b = 0) {
        if (fTraceN >= 40) return;
        fTraceN++;
        FBLOG("pipe: %s (foreign %d head %llu) 0x%llx 0x%llx", what, fForeign ? 1 : 0,
              (unsigned long long)(fForeign ? 4 : 0), a, b);
    }

    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        NMXGPUFB *me = (NMXGPUFB *)p0;
        if (me->fStopping) return;
        if (me->fVbl.proc) me->fVbl.proc(me->fVbl.target, me->fVbl.ref);
        uint64_t period;
        nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
    }

public:
    bool initWith(void *dev, const nmx_fb_info &info) {
        if (!init()) return false;
        fDev = dev;
        fInfo = info;
        nmx_fb_get_display_info(dev, &fDisplay);
        if (info.pclk_khz && info.htotal && info.vtotal) {
            uint64_t mhz = (uint64_t)info.pclk_khz * 1000000ull / ((uint64_t)info.htotal * info.vtotal);   /* milli-Hz */
            if (mhz >= 20000 && mhz <= 500000) {
                fRefresh16 = (uint32_t)((mhz << 16) / 1000);
                fFrameNs = 1000000000000ull / mhz;
            }
        }
        return true;
    }

    bool start(IOService *provider) override {
        /* ga403wm b15: IOFramebuffer::start() makes the controller this framebuffer's display pipe lives on from a
         * dependent ID that is ALREADY set (IOFBController::copyController(kDepIDCreate)). Written after start, the
         * framebuffer still shows the group in the registry, but WindowServer's pipe for it never activates: every
         * transaction came back kIOReturnNotReady ("Failed to submit transaction: IOPRESENTMENT_METHOD_FAILURE:
         * IOReturn 0x2d8") and the panel kept the boot console. Set the group before start. */
        /* Who serves this panel: the accelerator of the GPU that owns it. The hybrid bridge (NMXNvidiaDisplayBridge on
         * the provider) hands it to the NVIDIA card as its foreign head 4; NMXOwnsDisplay keeps it on its own GPU as
         * head 0, so the display pipe and the scan-out belong to the same accelerator. The bridge is the temporary
         * hybrid: CoreDisplay refuses to present a GPU's panel through another GPU's pipe ("Fallback GPU Display Pipe
         * transaction submission failed"), so the owned path is the one with a future. */
        const bool internalPanel = fDisplay.internal && fInfo.width == 2880 && fInfo.height == 1800;
        const bool foreign = internalPanel && provider->getProperty("NMXNvidiaDisplayBridge") == kOSBooleanTrue;
        const bool owned = internalPanel && !foreign && provider->getProperty("NMXOwnsDisplay") == kOSBooleanTrue;
        fForeign = foreign;
        setProperty("IOFBDependentIndex", foreign ? 4ULL : 0ULL, 32);
        if (foreign) {
            // Same controller group as upstream NVRMFB/NMIntelFB, separate head4.
            setProperty("IOFBDependentID", 0x4E56524D00000001ULL, 64);
            setProperty("NMXNvidiaDisplayBridge", kOSBooleanTrue);
        } else if (owned) {
            // Own controller group: start() builds the framebuffer's controller from a dependent ID that already
            // exists, and a lone framebuffer with no group leaves that controller NULL (power-change panic).
            setProperty("IOFBDependentID", 0x4E4D584700000001ULL, 64);
            setProperty("NMXOwnsDisplay", kOSBooleanTrue);
        }
        if (foreign || owned) {
            setProperty("built-in", kOSBooleanTrue);
            // Frozen AGDC K5 timing wire format, actual selected DRM timing (Hz/milli-Hz).
            // No synthetic timing or HDR capability. Consumers validate this tagged 13-word record.
            if (fInfo.pclk_khz && fInfo.pclk_khz <= 0xffffffffU / 1000) {
                UInt32 timing[13] = {0x4b350001U, fInfo.pclk_khz * 1000, fInfo.width,
                    fInfo.hsync_start, fInfo.hsync_end, fInfo.htotal, fInfo.height,
                    fInfo.vsync_start, fInfo.vsync_end, fInfo.vtotal,
                    (UInt32)(((uint64_t)fRefresh16 * 1000) >> 16),
                    fDisplay.hsync_positive, fDisplay.vsync_positive};
                if (OSData *data = OSData::withBytes(timing, sizeof(timing))) {
                    setProperty("NVRMTiming", data); data->release();
                }
            }
        }
        /* Another framebuffer on the same GPU (the firmware's) already drives the console: this one must not claim
         * it, or the console never hands the panel to WindowServer (PhxFB/7c5c09a: "not the console beside IONDRV"). */
        if (OSIterator *it = provider->getChildIterator(gIOServicePlane)) {
            while (OSObject *o = it->getNextObject())
                if (o != this && OSDynamicCast(IOFramebuffer, o)) fBeside = true;
            it->release();
        }
        if (!IOFramebuffer::start(provider)) return false;
        setProperty("NMXScanoutBridgeVersion", 1ULL, 32);
        setProperty("NMXEDIDBytes", (unsigned long long)fDisplay.edid_size, 32);
        if (fDisplay.edid_size) setProperty("NMXEDIDSource", "drm-connector");
        fVblTimer = thread_call_allocate(vblFire, this);
        if (fVblTimer) {
            uint64_t d;
            clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d);
            fVblNext = d;
            thread_call_enter_delayed(fVblTimer, d);
        }
        /* the power driver is registered in open(): see there */
        FBLOG("framebuffer %ux%u at %u.%02u Hz", fInfo.width, fInfo.height, fRefresh16 >> 16,
              ((fRefresh16 & 0xFFFF) * 100) >> 16);
        return true;
    }

    void stop(IOService *provider) override {
        fStopping = true;
        /* thread_call_cancel_wait is not exported to kexts: cancel and retry the free until the callback is out */
        if (fVblTimer) {
            thread_call_cancel(fVblTimer);
            while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); }
            fVblTimer = nullptr;
        }
        IOFramebuffer::stop(provider);
    }

    /*
     * ga403wm: on macOS 26 IOFramebuffer::powerStateWillChangeTo() (and getAttributeExt/handleEvent) dereference
     * fPrivate->controller, which only IOFramebuffer::open() assigns. Registering the power driver in start() let the
     * first power change reach it before any open and panicked (CR2 0x120, powerStateWillChangeTo+0x25). Register once
     * the framebuffer is open.
     */
    IOReturn open() override {
        IOReturn r = IOFramebuffer::open();
        trace("open", (unsigned long long)r);
        if (r == kIOReturnSuccess && !fPowerRegistered) {
            fPowerRegistered = true;
            registerPowerDriver(this, kFbPowerStates, 2);
        }
        return r;
    }

    /* Display-pipe adapter only. CPU physical addresses are not NVIDIA
     * VRAM handles or RADV GPU virtual addresses: never export them as such. */
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait,
                                  void *p1, void *p2, void *p3, void *p4) override {
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
            NVRMVramRequest *r = static_cast<NVRMVramRequest *>(p1);
            if (!r || r->version != NVRM_VRAM_ABI_VERSION) { trace("scanout bad ABI"); return kIOReturnBadArgument; }
            if (fStopping || !fPowerRegistered || !fInfo.ok || !fInfo.kva || !fInfo.width || !fInfo.height ||
                (uint64_t)fInfo.pitch < 4ULL * fInfo.width ||
                (uint64_t)fInfo.pitch * fInfo.height > fInfo.size) {
                trace("scanout NOT READY", (unsigned long long)!fPowerRegistered, (unsigned long long)fInfo.ok);
                return kIOReturnNotReady;
            }
            r->kva = reinterpret_cast<void *>(fInfo.kva);
            r->phys = 0;
            r->handle = nullptr;
            r->actualSize = fInfo.size;
            r->width = fInfo.width; r->height = fInfo.height; r->pitch = fInfo.pitch;
            trace("scanout ok", fInfo.width, fInfo.pitch);
            return kIOReturnSuccess;
        }
        return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }

    IOReturn enableController() override { trace("enableController"); return kIOReturnSuccess; }
    bool isConsoleDevice() override { trace("isConsoleDevice", fBeside ? 0 : 1); return !fBeside; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
        if (aperture != kIOFBSystemAperture) return nullptr;
        return IODeviceMemory::withRange(fInfo.phys, (IOPhysicalLength)fInfo.pitch * fInfo.height);
    }
    IODeviceMemory *getVRAMRange() override { return IODeviceMemory::withRange(fInfo.phys, fInfo.size); }
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        if (mode != 1 || !info) return kIOReturnBadArgument;
        bzero(info, sizeof(*info));
        info->nominalWidth = fInfo.width;
        info->nominalHeight = fInfo.height;
        info->refreshRate = fRefresh16;
        info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag |
                      kDisplayModeValidForHiResFlag;
        info->imageWidth = fDisplay.width_mm <= 65535 ? (UInt16)fDisplay.width_mm : 0;
        info->imageHeight = fDisplay.height_mm <= 65535 ? (UInt16)fDisplay.height_mm : 0;
        return kIOReturnSuccess;
    }
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
                                 IOPixelInformation *pi) override {
        if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = fInfo.pitch;
        pi->bitsPerPixel = 32;
        pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3;
        pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000;
        pi->componentMasks[1] = 0x0000FF00;
        pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = fInfo.width;
        pi->activeHeight = fInfo.height;
        return kIOReturnSuccess;
    }
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override {
        *mode = 1; *depth = 0; return kIOReturnSuccess;
    }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    /* without a timing, WindowServer shows the display at 0.00 Hz (seen with NMIntelFB) */
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (mode != 1 || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;
        const nmx_fb_info &t = fInfo;
        if (!t.pclk_khz || t.htotal <= t.width || t.vtotal <= t.height) return kIOReturnSuccess;
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = (uint64_t)t.pclk_khz * 1000;
        d.horizontalActive = t.width;
        d.horizontalBlanking = t.htotal - t.width;
        d.horizontalSyncOffset = t.hsync_start > t.width ? t.hsync_start - t.width : 0;
        d.horizontalSyncPulseWidth = t.hsync_end > t.hsync_start ? t.hsync_end - t.hsync_start : 0;
        d.verticalActive = t.height;
        d.verticalBlanking = t.vtotal - t.height;
        d.verticalSyncOffset = t.vsync_start > t.height ? t.vsync_start - t.height : 0;
        d.verticalSyncPulseWidth = t.vsync_end > t.vsync_start ? t.vsync_end - t.vsync_start : 0;
        info->flags = kIODetailedTimingValid;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override {
        trace("setDisplayMode", mode, depth);
        return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    IOItemCount getConnectionCount() override { return 1; }
    bool hasDDCConnect(IOIndex idx) override { return idx == 0 && fDisplay.edid_size >= 128; }
    IOReturn getDDCBlock(IOIndex idx, UInt32 block, IOSelect type, IOOptionBits,
                        UInt8 *data, IOByteCount *length) override {
        if (idx || type != kIODDCBlockTypeEDID || !fDisplay.edid_size) return kIOReturnUnsupported;
        if (!data || !length || !block || block > fDisplay.edid_size / 128) return kIOReturnBadArgument;
        if (*length < 128) return kIOReturnNoSpace;
        bcopy(fDisplay.edid + (block - 1) * 128, data, 128);
        *length = 128;
        return kIOReturnSuccess;
    }
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref,
                                      void **iref) override {
        if (type == kIOFBVBLInterruptType) { trace("register VBL", (unsigned long long)(proc != nullptr)); fVbl = { proc, target, ref }; if (iref) *iref = &fVbl; return kIOReturnSuccess; }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *) override { return kIOReturnSuccess; }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    /* power acknowledged at once; left to IOFramebuffer it waits on a shared thread (seen with NVRMFB) */
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) { trace("setAttribute wsrv", value); return kIOReturnSuccess; }
        if (attribute == kIOPowerAttribute) {
            /* display sleep turns the outputs off; wake lights them again with the same buffer */
            if (fDev) nmx_fb_power(fDev, value != 0);
            if (value) {
                IOReturn r = IOFramebuffer::setAttribute(attribute, value);
                handleEvent(kIOFBNotifyDidPowerOn);
                return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
            }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default:
            return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag | kDisplayModeValidForHiResFlag;
        return kIOReturnSuccess;
    }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { trace("getAttribute wsrv"); if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
        if (idx != 0) return kIOReturnBadArgument;
        // IODisplay probes this capability with a NULL output pointer.
        if (attr == kConnectionSupportsHLDDCSense)
            return fDisplay.edid_size ? kIOReturnSuccess : kIOReturnUnsupported;
        if (!value) return kIOReturnBadArgument;
        switch (attr) {
        case kConnectionFlags: *value = fDisplay.internal ? kIOConnectionBuiltIn : 0; return kIOReturnSuccess;
        case kConnectionSupportsAppleSense: case kConnectionSupportsLLDDCSense: return kIOReturnUnsupported;
        case kConnectionEnable: case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
        case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
        case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
            *value = 0x00000002; return kIOReturnSuccess;
        default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
        }
    }
};
OSDefineMetaClassAndStructors(NMXGPUFB, IOFramebuffer)

/* lights the device's outputs and publishes them under 'gpu'; nullptr when there is nothing to show (reason logged) */
IOService *nmx_fb_publish(IOService *gpu, void *dev)
{
    nmx_fb_info info;
    int err = nmx_fb_open(dev, &info);
    if (err || !info.ok) {
        FBLOG("no framebuffer (%d)", err);
        return nullptr;
    }
    NMXGPUFB *fb = OSTypeAlloc(NMXGPUFB);
    if (fb == nullptr || !fb->initWith(dev, info) || !fb->attach(gpu)) {
        OSSafeReleaseNULL(fb);
        nmx_fb_close(dev);
        return nullptr;
    }
    if (!fb->start(gpu)) {
        fb->detach(gpu);
        fb->release();
        nmx_fb_close(dev);
        return nullptr;
    }
    fb->registerService();
    return fb;
}

void nmx_fb_unpublish(IOService *fbsvc, IOService *gpu, void *dev)
{
    if (fbsvc == nullptr)
        return;
    fbsvc->terminate(kIOServiceSynchronous);
    fbsvc->release();
    nmx_fb_close(dev);
}
