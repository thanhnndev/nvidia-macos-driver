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
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include "nmx_port.h"

extern "C" long long nmx_fb_flip_call(const char *name, void *req);
/* review of fdffd32: a user thread running driver code outside the device node is counted like a device-node entry, so
 * nmx_proc_reap keeps its Linux task while it is inside (nmx_kernel.c) */
struct thread;
/* 0 = inside; ENXIO = the port is not started or is stopping: run no driver code (review of 0266402) */
extern "C" int nmx_entry_try(struct thread **td);
extern "C" void nmx_entry_end(struct thread *td);
struct NmxEntry { struct thread *td = nullptr; int err; NmxEntry() : err(nmx_entry_try(&td)) {}
                  ~NmxEntry() { if (!err) nmx_entry_end(td); } NmxEntry(const NmxEntry &) = delete; };
/* frame-path trace (nmx_fb_bridge.c): the timer's phase against the real vblank */
extern "C" void nmx_ft_vbl_timer(uint64_t frame_ns);
/* out-31 row 2 (nmx_fb_bridge.c): CRTC vblank sink and the DRM vblank reference; debug.nmx_vbl_irq / nmxvblirq=0 */
extern "C" void nmx_fb_vbl_set_sink(void (*fn)(void *, uint64_t), void *arg);
extern "C" int nmx_fb_vbl_ref(int on);
extern "C" volatile SInt64 gNmxVblDelivered;     // out-33 (B): VBLs delivered to IOFramebuffer from the CRTC vblank
extern "C" int nmx_vbl_wanted;                   // out-33 (B): an enabled 'vbl ' registration exists (trace)
extern "C" int nmx_vbl_irq;
#include "../../../kexts/NVRM/nvrm_vram_abi.h"

#define FBLOG(fmt, ...) IOLog("NullMoth display: " fmt "\n", ##__VA_ARGS__)

/*
 * Hardware cursor (2026-11-25, v2): boot-arg nmxhwcur=1 makes the internal panel's framebuffer advertise
 * kIOHardwareCursorAttribute at open; any other value, or no boot-arg, changes nothing (the attribute is left to
 * IOFramebuffer, WindowServer composites its cursor window as today). There is no run-time switch: once the attribute
 * is advertised IOFramebuffer has no software cursor, and WindowServer's own cursor window is only used when
 * hardwareCursorActive is 0 at its next reload (see the cursor section in NMXAMDFB). debug.nmx_hwcursor reports.
 */
extern "C" {
int nmx_hwcursor;                   // 1 = boot-arg nmxhwcur=1 (read-only)
int nmx_hwcursor_avail;             // 1 = the plane came up and the framebuffer advertises it
volatile SInt64 gNmxCurStates, gNmxCurAccepted, gNmxCurSwFallback, gNmxCurCopyRetry, gNmxCurKeptLast;
}

/* override/src/linux_pci.c: the panel brightness in percent through amdgpu's backlight ops (set 0 = read) */
extern "C" int nmx_backlight_percent(int set, unsigned *pct);

/*
 * ga403wm Step130: the brightness slider and keys. IOGraphics builds an AppleBacklightDisplay (slider, F-keys, dim on
 * sleep) only if, at the framebuffer's first open, a child service named "backlight" exists and getAppleSense() says
 * LCD. This handler is that child; it applies the level through amdgpu (DC, AUX nits on the OLED) from a thread call,
 * because IODisplay calls doIntegerSet with the framebuffer lock held. It was the NMXBacklight kext, which called
 * NMXGPU by address and went inert on the first source build. Boot-arg -nmxbloff leaves it out.
 */
class NMXFBBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(NMXFBBacklight)
    IOService *fFb = nullptr;
    thread_call_t fApply = nullptr;
    volatile SInt32 fPending = -1;
    SInt32 fLevel = 80;
    UInt32 fWrites = 0, fFailures = 0, fRaw = 0;

    static void applyFire(thread_call_param_t p, thread_call_param_t) {
        NMXFBBacklight *me = (NMXFBBacklight *)p;
        SInt32 v;
        do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
        if (v < 0) return;
        unsigned pct = (unsigned)v;
        int rc = nmx_backlight_percent(1, &pct);
        bool ok = rc == 0;
        if (ok) { me->fLevel = v; me->fWrites++; } else me->fFailures++;
        if (IOService *fb = me->fFb) {
            fb->setProperty("Backlight,Writes", me->fWrites, 32);
            fb->setProperty("Backlight,Failures", me->fFailures, 32);
            fb->setProperty("Backlight,LastRc", (UInt32)rc, 32);
            if (ok) fb->setProperty("Backlight,Percent", (UInt32)v, 32);
        }
        if ((ok && me->fWrites <= 20) || (!ok && me->fFailures <= 8))
            FBLOG("backlight %d%% %s (rc %d)", (int)v, ok ? "applied" : "FAILED", rc);
    }
public:
    static NMXFBBacklight *withFramebuffer(IOService *fb) {
        NMXFBBacklight *h = OSTypeAlloc(NMXFBBacklight);
        if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
        h->fFb = fb;
        unsigned pct = 0;
        if (nmx_backlight_percent(0, &pct) == 0 && pct <= 100) h->fLevel = (SInt32)pct;
        h->fApply = thread_call_allocate(applyFire, h);
        if (!h->fApply) { h->release(); return nullptr; }
        return h;
    }
    SInt32 level() const { return fLevel; }
    void quiesce() {
        fFb = nullptr;
        if (fApply) {
            thread_call_cancel(fApply);
            while (!thread_call_free(fApply)) { thread_call_cancel(fApply); IOSleep(1); }
            fApply = nullptr;
        }
    }
    void free() override { quiesce(); IODisplayParameterHandler::free(); }
    /* the framebuffer reports no display parameters, so IODisplayParameters may not exist yet, and
     * IODisplay::setProperties() only accepts parameters listed there */
    bool setDisplay(IODisplay *display) override {
        if (!display) return false;
        OSObject *old = display->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, old);
        OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
        if (old) old->release();
        if (!params) return false;
        IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, fLevel);
        IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, fLevel);
        display->setProperty(gIODisplayParametersKey, params);
        params->release();
        FBLOG("backlight parameters on %s (%s) at %d%%", display->getName(), display->getMetaClass()->getClassName(),
              (int)fLevel);
        return true;
    }
    bool doIntegerSet(OSDictionary *, const OSSymbol *name, UInt32 value) override {
        if (fRaw < 20) { fRaw++; FBLOG("backlight set %s = %u", name ? name->getCStringNoCopy() : "?", (unsigned)value); }
        if (name != gIODisplayBrightnessKey && name != gIODisplayLinearBrightnessKey) return false;
        fPending = value > 100 ? 100 : (SInt32)value;
        if (fApply) thread_call_enter(fApply);
        return true;
    }
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    bool doUpdate() override { return true; }
};
OSDefineMetaClassAndStructors(NMXFBBacklight, IODisplayParameterHandler)

static IOPMPowerState kFbPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* ga403wm Step74: CoreDisplay tells the GPU vendor from the framebuffer's class name ("Intel", "AMD",
 * "AppleParavirt"); any other name gives vendor 1, IOPresentment off and a software compositor that asserts at every
 * WindowServer start. NMXAccel and NMXAGDC match this name. */
class NMXAMDFB : public IOFramebuffer {
    OSDeclareDefaultStructors(NMXAMDFB)
    void *fDev = nullptr;
    nmx_fb_info fInfo = {};
    nmx_fb_display_info fDisplay = {};
    uint32_t fRefresh16 = 60 << 16;
    uint64_t fFrameNs = 16666667ull;
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; volatile bool on = true; };
    /*
     * out-31 row 2. IOFramebuffer's VBL contract (FRAME-PATH.md section 4): each client (the display pipe, the cursor)
     * registers its own 'vbl ' interrupt and turns it on and off with setInterruptState, refcounted, and IOFramebuffer
     * turns it off itself after idle frames (deferredVBLDisable). Up to kVblSlots registrations are kept; while any is
     * on, NMXGPU holds a DRM vblank reference (drm_crtc_vblank_get, as a Linux vblank client does) and every CRTC
     * vblank (dm_crtc_high_irq) is delivered from fVblHw. The timer only stands in when the CRTC gives no vblank (off,
     * reference refused, IRQ stale > 2 frames) and otherwise wakes every kVblWatchMs to check. debug.nmx_vbl_irq=0:
     * the old path, the last registration always on, delivered by the frame timer.
     */
    static const int kVblSlots = 4;
    static const uint64_t kVblWatchMs = 100;
    Irq fVblSlot[kVblSlots], fConnect;
    Irq *fVblLast = nullptr;
    thread_call_t fVblTimer = nullptr, fVblHw = nullptr, fVblRef = nullptr;
    uint64_t fVblNext = 0;
    volatile uint64_t fHwVblAt = 0;     // mach time of the last CRTC vblank handed over
    volatile UInt32 fTimerIdle = 0;     // the timer did not re-arm itself (nothing to deliver)
    volatile int fRefHeld = 0;          // NMXGPU holds the DRM vblank reference for us
    unsigned fRefRetry = 0;
    IOLock *fRefLock = nullptr;         // vblRefFire runs one at a time (a thread call can run on two threads at once)
    volatile SInt32 fDelivering = 0;    // vblDeliver in progress (unregisterInterrupt waits it out)
    volatile bool fStopping = false;
    bool fPowerRegistered = false;
    bool fForeign = false;      // head 4 of NVAccel's display machine (the NV controller group)
    bool fBeside = false;       // another framebuffer on this GPU's PCI device already drives the console
    unsigned fTraceN = 0;
    unsigned fNotReadyN = 0;    // 'scanout NOT READY' repeats every poll: counted apart so it cannot eat the trace cap
    NMXFBBacklight *fBacklight = nullptr;
    bool fInternalPanel = false;
    bool fLcd = false;          // getAppleSense says LCD for connection 0 (the backlight handler is published)

    /*
     * ga403wm Step130 (was NMXBacklight 0.4.0, Step109d): the system wake work IOGraphics never does here. A system
     * sleep clears pagingState (IOFramebuffer::systemWork, kIOFBNotifyWillSleep to every client); IOGraphics sets it
     * again only from its own power work (IOFramebuffer::setPowerState -> checkPowerWork). setPowerState below acks at
     * once and handleEvent() does nothing off the controller's power thread, so after a system sleep pagingState
     * stayed clear and WindowServer slept in _extEntry forever (userspace watchdog panic). On power-on after a system
     * sleep: DidWake(true), DidWake(false) for each WillSleep(false), DidPowerOn(false), pagingState on, WindowServer
     * woken, under a gate on the framebuffer's workloop. A thread call 3 s after the system power-on does the same if
     * the framebuffer's own power-on never comes. -nmxfbwakeoff disables it.
     */
    IONotifier *fSleepTap = nullptr;
    IONotifier *fSysNote = nullptr;
    IOCommandGate *fWakeGate = nullptr;
    thread_call_t fWakeLate = nullptr;
    volatile UInt32 fSysSlept = 0;
    volatile SInt32 fWillSleepOff = 0;
    UInt32 fWakeWorks = 0;

    static IOReturn sleepTap(OSObject *self, void *, IOFramebuffer *fb, IOIndex event, void *info) {
        NMXAMDFB *me = OSDynamicCast(NMXAMDFB, self);
        if (!me || fb != me || event != kIOFBNotifyWillSleep) return kIOReturnSuccess;
        me->fSysSlept = 1;
        if (!info) OSIncrementAtomic(&me->fWillSleepOff);
        return kIOReturnSuccess;
    }
    static IOReturn wakeGated(OSObject *self, void *why, void *, void *, void *) {
        NMXAMDFB *me = (NMXAMDFB *)self;
        if (me->pagingState || me->fStopping) return kIOReturnSuccess;
        const SInt32 owed = me->fWillSleepOff;
        IOReturn r1 = me->deliverFramebufferNotification(kIOFBNotifyDidWake, (void *)true);
        IOReturn r2 = me->deliverFramebufferNotification(kIOFBNotifyDidWake, (void *)false);
        for (SInt32 i = 1; i < owed; ++i) r2 = me->deliverFramebufferNotification(kIOFBNotifyDidWake, (void *)false);
        IOReturn r3 = me->deliverFramebufferNotification(kIOFBNotifyDidPowerOn, (void *)false);
        me->pagingState = true;
        me->fWakeGate->commandWakeup(&me->fServerConnect, false);
        me->fWillSleepOff = 0;
        ++me->fWakeWorks;
        FBLOG("paging wake (%s, #%u): DidWake true + %d x false (0x%x/0x%x), DidPowerOn 0x%x, paging on, WindowServer woken",
              (const char *)why, (unsigned)me->fWakeWorks, owed > 1 ? (int)owed : 1, r1, r2, r3);
        return kIOReturnSuccess;
    }
    void wakeWork(const char *why) {
        if (!fWakeGate || fStopping) return;
        if (!OSCompareAndSwap(1, 0, &fSysSlept)) return;
        if (pagingState) { FBLOG("%s: paging already on, nothing to do", why); return; }
        fWakeGate->runAction(&wakeGated, const_cast<char *>(why));
    }
    static void wakeLateFire(thread_call_param_t p, thread_call_param_t) {
        NMXAMDFB *me = (NMXAMDFB *)p;
        /* a dark wake leaves the framebuffer off: its own power-on does the work later */
        if (!me->fStopping && me->getPowerState() > 0) me->wakeWork("3 s after system power-on");
    }
    static IOReturn sysPower(void *target, void *, UInt32 type, IOService *, void *arg, vm_size_t) {
        NMXAMDFB *me = (NMXAMDFB *)target;
        IOPowerStateChangeNotification *note = (IOPowerStateChangeNotification *)arg;
        if ((type == kIOMessageCanSystemSleep || type == kIOMessageSystemWillSleep) && note) note->returnValue = 0;
        /* WillPowerOn as well: HasPoweredOn did not arrive in the 2026-11-04 test (NMXPowerKeep) */
        if ((type == kIOMessageSystemWillPowerOn || type == kIOMessageSystemHasPoweredOn) && me->fWakeLate && !me->fStopping) {
            uint64_t d;
            clock_interval_to_deadline(3, kSecondScale, &d);
            thread_call_enter_delayed(me->fWakeLate, d);
        }
        return kIOReturnSuccess;
    }
    void setupWakeWork() {
        if (PE_parse_boot_argn("-nmxfbwakeoff", nullptr, 0)) { FBLOG("-nmxfbwakeoff: no paging wake"); return; }
        /* IOGraphics' _extEntry sleeps on this+0x188 (fServerConnect) in its binary: the SDK header must agree */
        if ((uintptr_t)&fServerConnect - (uintptr_t)this != 0x188) {
            FBLOG("fServerConnect at +0x%lx, not 0x188: no paging wake", (unsigned long)((uintptr_t)&fServerConnect - (uintptr_t)this));
            return;
        }
        IOWorkLoop *wl = getWorkLoop();
        fWakeGate = IOCommandGate::commandGate(this);
        if (fWakeGate && (!wl || wl->addEventSource(fWakeGate) != kIOReturnSuccess)) OSSafeReleaseNULL(fWakeGate);
        fWakeLate = thread_call_allocate(wakeLateFire, this);
        fSleepTap = addFramebufferNotification(&sleepTap, this, nullptr);
        fSysNote = registerPrioritySleepWakeInterest(&sysPower, this, nullptr);
        if (!fWakeGate || !fWakeLate || !fSleepTap) {
            FBLOG("paging wake setup failed (gate %d call %d tap %d)", fWakeGate != nullptr, fWakeLate != nullptr,
                  fSleepTap != nullptr);
            teardownWakeWork();
            return;
        }
        setProperty("NMXPagingWake", kOSBooleanTrue);
    }
    void teardownWakeWork() {
        if (fSysNote) { fSysNote->remove(); fSysNote = nullptr; }
        if (fSleepTap) { fSleepTap->remove(); fSleepTap = nullptr; }
        if (fWakeLate) {
            thread_call_cancel(fWakeLate);
            while (!thread_call_free(fWakeLate)) { thread_call_cancel(fWakeLate); IOSleep(1); }
            fWakeLate = nullptr;
        }
        if (fWakeGate) {
            if (IOWorkLoop *wl = getWorkLoop()) wl->removeEventSource(fWakeGate);
            OSSafeReleaseNULL(fWakeGate);
        }
    }
public:
    /*
     * ga403wm Step130c: called by nmx_fb_publish() AFTER this framebuffer's registerService(), not from start(). In
     * start() the child's synchronous registerService ran before the framebuffer was published (and before
     * NMXScanoutBridgeVersion was set): NMXAccel's fbPublished never armed the display pipe and both Step130 boots
     * had a black panel (-nmxbloff gave the picture back). The old NMXBacklight kext attached at this same point (it
     * matched the published NMXAMDFB) and its slider worked: IOGraphics looks for the child at the first open, which
     * is WindowServer's, seconds later.
     */
    void publishBacklight() {
        if (!fInternalPanel || fStopping || fBacklight) return;
        if (PE_parse_boot_argn("-nmxbloff", nullptr, 0)) { FBLOG("-nmxbloff: no backlight"); return; }
        fBacklight = NMXFBBacklight::withFramebuffer(this);
        if (!fBacklight) { FBLOG("backlight handler allocation failed"); return; }
        fBacklight->setName("backlight");
        if (!fBacklight->attach(this)) { FBLOG("backlight handler attach failed"); OSSafeReleaseNULL(fBacklight); return; }
        fLcd = true;    /* before the child is visible: the open that finds it must also see LCD */
        fBacklight->registerService();
        setProperty("Backlight,Initial", (UInt32)fBacklight->level(), 32);
        FBLOG("backlight handler published after the framebuffer (amdgpu DC, initial %d%%), panel sense LCD",
              (int)fBacklight->level());
    }
private:

    /* ga403wm b15: WindowServer only said "Failed to submit transaction: IOPRESENTMENT_METHOD_FAILURE:
     * IOReturn 0x2d8" (kIOReturnNotReady) for the OLED, which names neither the call nor the stage. Say which of
     * the framebuffer calls its display pipe makes, once each, in boot order. */
    void trace(const char *what, unsigned long long a = 0, unsigned long long b = 0) {
        if (fTraceN >= 40) return;
        fTraceN++;
        FBLOG("pipe: %s (foreign %d head %llu) 0x%llx 0x%llx", what, fForeign ? 1 : 0,
              (unsigned long long)(fForeign ? 4 : 0), a, b);
    }

    static bool vblIrqMode() { return __atomic_load_n(&nmx_vbl_irq, __ATOMIC_RELAXED) != 0; }
    bool vblWanted() const {
        for (int i = 0; i < kVblSlots; i++)
            if (fVblSlot[i].proc && fVblSlot[i].on) return true;
        return false;
    }
    void vblDeliver() {
        OSIncrementAtomic(&fDelivering);
        if (!vblIrqMode()) {            // old path: the last registration, whatever its state
            Irq *l = fVblLast;
            IOFBInterruptProc pr = l ? __atomic_load_n(&l->proc, __ATOMIC_ACQUIRE) : nullptr;
            if (pr) pr(l->target, l->ref);
        } else {
            for (int i = 0; i < kVblSlots; i++) {
                Irq &q = fVblSlot[i];
                IOFBInterruptProc pr = __atomic_load_n(&q.proc, __ATOMIC_ACQUIRE);   // read once
                if (pr && q.on) pr(q.target, q.ref);
            }
        }
        OSDecrementAtomic(&fDelivering);
    }
    /* IH thread (dm_crtc_high_irq): no locks, no sleeping, only the hand-over */
    static void vblSink(void *p0, uint64_t now) {
        NMXAMDFB *me = (NMXAMDFB *)p0;
        __atomic_store_n(&me->fHwVblAt, now, __ATOMIC_RELEASE);
        if (!me->fStopping && me->fVblHw && vblIrqMode() && me->fRefHeld) thread_call_enter(me->fVblHw);
    }
    static void vblHwFire(thread_call_param_t p0, thread_call_param_t) {
        NMXAMDFB *me = (NMXAMDFB *)p0;
        if (!me->fStopping && vblIrqMode() && me->fRefHeld) {
            me->vblDeliver();
            OSAddAtomic64(1, &gNmxVblDelivered);
        }
    }
    void vblArmTimer() {
        if (fVblTimer && OSCompareAndSwap(1, 0, &fTimerIdle)) {
            uint64_t period;
            nanoseconds_to_absolutetime(fFrameNs, &period);
            fVblNext = mach_absolute_time() + period;
            thread_call_enter_delayed(fVblTimer, fVblNext);
        }
    }
    /* thread call: take or drop the DRM vblank reference to match the registrations (drm_crtc_vblank_get may sleep) */
    static void vblRefFire(thread_call_param_t p0, thread_call_param_t) {
        NMXAMDFB *me = (NMXAMDFB *)p0;
        if (me->fStopping || !me->fRefLock) return;
        IOLockLock(me->fRefLock);
        bool want;
        do {                            // until the registrations did not change under us
            want = vblIrqMode() && me->vblWanted();
            __atomic_store_n(&nmx_vbl_wanted, me->vblWanted() ? 1 : 0, __ATOMIC_RELAXED);
            const int held = nmx_fb_vbl_ref(want ? 1 : 0);
            if (held != me->fRefHeld) me->trace(held ? "vblank ref held" : "vblank ref dropped", (unsigned long long)want);
            __atomic_store_n(&me->fRefHeld, held, __ATOMIC_RELEASE);
        } while (!me->fStopping && want != (vblIrqMode() && me->vblWanted()));
        IOLockUnlock(me->fRefLock);
        me->vblArmTimer();
    }
    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        NMXAMDFB *me = (NMXAMDFB *)p0;
        if (me->fStopping) return;
        uint64_t period;
        nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t now = mach_absolute_time();
        if (vblIrqMode()) {
            if (!me->vblWanted()) {
                if (me->fRefHeld && me->fVblRef) thread_call_enter(me->fVblRef);
                __atomic_store_n(&me->fTimerIdle, 1, __ATOMIC_RELEASE);   // vblRefFire re-arms when one turns on
                if (me->vblWanted()) me->vblArmTimer();   // ... unless that happened between the check and the store
                return;
            }
            const uint64_t hw = __atomic_load_n(&me->fHwVblAt, __ATOMIC_ACQUIRE);
            // stale = no CRTC vblank for 2 frames, but at least 50 ms: VRR may stretch a frame to the panel's minimum
            uint64_t stale, floor50;
            nanoseconds_to_absolutetime(50000000ull, &floor50);
            stale = 2 * period > floor50 ? 2 * period : floor50;
            if (me->fRefHeld && hw && now - hw <= stale) {
                uint64_t watch;
                nanoseconds_to_absolutetime(kVblWatchMs * 1000000ull, &watch);
                me->fVblNext = now + watch;     // the CRTC delivers: only watch
                thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
                return;
            }
            /* fallback frame: no reference (CRTC off when asked) or the IRQ went quiet; ask again about once a second */
            if (!me->fRefHeld && me->fVblRef && (++me->fRefRetry % 64) == 0) thread_call_enter(me->fVblRef);
        } else if (me->fRefHeld && me->fVblRef) {
            thread_call_enter(me->fVblRef);     // switched to the old path: drop the reference
        }
        nmx_ft_vbl_timer(me->fFrameNs);
        me->vblDeliver();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
    }

private:
    /*
     * ---- hardware cursor (2026-11-25, v2) ----
     * IOFramebuffer calls setCursorImage / setCursorState with its cursor spinlock held (OSSpinLock, SETSEMA), from
     * WindowServer's calls and the HID path: nothing there may sleep. They only stage: the image is converted into one
     * of two staging slots (convertCursorImage, under a generation seqlock), the position and visibility are stored,
     * and fCurWork (a ONCE thread call: never two runs at a time) is entered. fCurWork copies the image, then commits it
     * to the CRTC's cursor plane the way Linux's cursor ioctl does (nmx_fb_cursor_apply in nmx_fb_bridge.c). It runs at
     * most once per frame period (deadline = last commit + fFrameNs).
     *
     * Contract with WindowServer (static reading of SkyLight/CoreDisplay 25G332, docs in the commit): once the
     * framebuffer advertises kIOHardwareCursorAttribute, IOFramebuffer has no software cursor of its own; WindowServer
     * draws a cursor window instead whenever shmem->hardwareCursorActive is 0. _CGXReloadCursor reads that flag BEFORE
     * it loads the new image (CoreDisplayManager::set_cursor -> IOFBSetNewCursor): if the previous image was on the
     * hardware cursor and the new one is refused, WindowServer has already chosen the hardware path and removes its
     * cursor window -> no cursor until the next image. So nothing here refuses an image that IOFramebuffer hands over:
     * oversized images never arrive (WindowServer checks MPMaxCursorSize first, and IOFramebuffer checks maxCursorSize
     * in extSetNewCursor), inverting pixels are accepted (NMX_CUR_INVERT_PIXEL), and there is no run-time off switch.
     * The only refusal left is IOFramebuffer's own descriptor probe at open (convertCursorImage fails by design there).
     *
     * Refusal after accept (self-check, IOGraphics IOFramebuffer.cpp): _setCursorImage turns our return value into
     * shmem->hardwareCursorActive, and resetCursor / showCursor / extSetNewCursor reload through it. A refusal there,
     * after an image was on the plane, makes IOFramebuffer stop calling setCursorState while WindowServer (which read
     * the flag before the load) keeps no cursor window: no cursor until the next image. So once an image was accepted
     * in this WindowServer session, a failed conversion keeps the last accepted image on the plane and answers success
     * (debug.nmx_cur_kept_last): wrong shape at worst, never a lost cursor. The probe cannot be told from a real
     * failure (testingCursor is IOFramebuffer-private, both return false untouched), but it runs once per framebuffer
     * lifetime: IOFramebuffer::open returns early once `opened` is set and close() never clears it, so postOpen (the
     * probe) never runs again, and shmem->hardwareCursorActive survives a WindowServer restart (the new WindowServer
     * takes the hardware path at once). So fCurSession stays set from the first accepted image on, for the lifetime.
     * At IOFramebuffer::close (logout, WindowServer exit or crash, shutdown: kIOWindowServerActiveAttribute =
     * kIOWSAA_Unaccelerated, setAttribute below) the plane is hidden (visibility only, the image stays accepted), so
     * no frozen arrow is left; the next setCursorState(visible) shows it again.
     */
    uint32_t fCurSide = 0;              // hardware cursor buffer side; 0 = no hardware cursor (or tearing down)
    uint32_t *fCurStage[2] = {};        // converted images (straight ARGB, rows of fCurW), written under IOFB's lock
    uint32_t fCurW[2] = {}, fCurH[2] = {};
    uint32_t *fCurCopy = nullptr;       // fCurWork's copy of the published image (only fCurWork touches it)
    volatile UInt32 fCurGen = 0;        // odd while setCursorImage converts (seqlock over the slots)
    volatile UInt32 fCurSeq = 0;        // published image; slot fCurSeq & 1; 0 = none yet
    volatile uint64_t fCurPos = 0;      // (uint32)x << 32 | (uint32)y: the image's top-left (setCursorState)
    volatile UInt32 fCurVis = 0;        // last setCursorState visibility
    volatile UInt32 fCurRefused = 1;    // the last setCursorImage was refused (the probe at open): plane hidden
    volatile UInt32 fCurSession = 0;    // an image was accepted (framebuffer lifetime): never refuse again
    volatile UInt32 fCurArmed = 0;      // fCurWork entered and not yet run
    volatile UInt32 fCurNeedImg = 1;    // upload the image on the next commit (first one, re-arm after a bridge commit)
    volatile SInt32 fCurBusy = 0;       // callers inside the cursor code (teardown waits them out)
    volatile uint64_t fCurLastAt = 0;   // mach time of the last commit
    uint32_t fCurDone = 0;              // fCurWork: the image seq last committed
    uint32_t fCurFails = 0;             // fCurWork: commits failed in a row (retry bounded)
    thread_call_t fCurWork = nullptr;
    unsigned fCurLogs = 0;
    static NMXAMDFB *volatile sCurFb;   // the framebuffer that owns the cursor plane (one: the internal panel)

#define CURLOG(fmt, ...) do { if (fCurLogs < 32) { fCurLogs++; FBLOG("cursor: " fmt, ##__VA_ARGS__); } } while (0)

    /* in-flight guard: true = inside and the cursor is up (pair with curLeave); teardown zeroes fCurSide first */
    bool curEnter() {
        OSIncrementAtomic(&fCurBusy);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&fCurSide, __ATOMIC_SEQ_CST) && !fStopping) return true;
        OSDecrementAtomic(&fCurBusy);
        return false;
    }
    void curLeave() { OSDecrementAtomic(&fCurBusy); }

    /* enter fCurWork once; deadline = last commit + one frame (or later by afterNs). Any context, never sleeps.
     * Callers are inside curEnter. */
    void curArm(uint64_t afterNs) {
        if (!OSCompareAndSwap(0, 1, &fCurArmed)) return;
        uint64_t now = mach_absolute_time(), frame, d;
        nanoseconds_to_absolutetime(fFrameNs, &frame);
        d = __atomic_load_n(&fCurLastAt, __ATOMIC_RELAXED) + frame;
        if (d < now) d = now;
        if (afterNs) { uint64_t a; nanoseconds_to_absolutetime(afterNs, &a); if (d < now + a) d = now + a; }
        thread_call_enter_delayed(fCurWork, d);
    }
    /* the bridge's re-arm (its own commits disabled the plane, the display came back): re-upload and re-commit.
     * The bridge holds its own busy count around this call (nmx_fb_cursor_set_kick(NULL) waits it out). */
    static void curKick(void *arg) {
        NMXAMDFB *me = (NMXAMDFB *)arg;
        if (!me->curEnter()) return;
        __atomic_store_n(&me->fCurNeedImg, 1, __ATOMIC_RELEASE);
        me->curArm(0);
        me->curLeave();
    }
    /* copy the published image out of its slot; false = none, or setCursorImage kept writing (asked again later) */
    bool curCopy(uint32_t &seq, uint32_t &w, uint32_t &h) {
        for (int n = 0; n < 50; n++) {
            const UInt32 g1 = __atomic_load_n(&fCurGen, __ATOMIC_ACQUIRE);
            if (g1 & 1) { IODelay(20); continue; }
            seq = __atomic_load_n(&fCurSeq, __ATOMIC_ACQUIRE);
            if (!seq) return false;
            const int s = seq & 1;
            w = fCurW[s];
            h = fCurH[s];
            if (w > fCurSide || h > fCurSide) return false;
            bcopy(fCurStage[s], fCurCopy, (size_t)w * h * 4);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&fCurGen, __ATOMIC_ACQUIRE) == g1) return true;
            OSIncrementAtomic64(&gNmxCurCopyRetry);
        }
        return false;
    }
    static void curWorkFire(thread_call_param_t p, thread_call_param_t) {
        NMXAMDFB *me = (NMXAMDFB *)p;
        if (!me->curEnter()) return;
        me->curWork();
        me->curLeave();
    }
    void curWork() {
        __atomic_store_n(&fCurArmed, 0, __ATOMIC_SEQ_CST);     // changes from here on enter the call again (ONCE:
                                                               // the re-entry waits until this run returns)
        NmxEntry entry;
        if (entry.err) return;
        const uint64_t pos = __atomic_load_n(&fCurPos, __ATOMIC_ACQUIRE);
        const int vis = __atomic_load_n(&fCurVis, __ATOMIC_ACQUIRE) && !__atomic_load_n(&fCurRefused, __ATOMIC_ACQUIRE);
        const uint32_t *img = nullptr;
        uint32_t seq = __atomic_load_n(&fCurSeq, __ATOMIC_ACQUIRE), w = 0, h = 0;
        const bool need = __atomic_exchange_n(&fCurNeedImg, 0, __ATOMIC_ACQ_REL) != 0;
        if (seq && (need || seq != fCurDone)) {
            if (curCopy(seq, w, h)) {
                img = fCurCopy;
            } else {
                __atomic_store_n(&fCurNeedImg, 1, __ATOMIC_RELEASE);
                curArm(1000000);
                return;
            }
        }
        const int e = nmx_fb_cursor_apply(img, w, h, seq, (int32_t)(pos >> 32), (int32_t)(uint32_t)pos, vis && seq);
        __atomic_store_n(&fCurLastAt, mach_absolute_time(), __ATOMIC_RELAXED);
        if (e == 0) {
            if (img) fCurDone = seq;
            fCurFails = 0;
            return;
        }
        /* the bridge marked the next commit full; retry a few times, then wait for the next change or re-arm */
        if (img || need) __atomic_store_n(&fCurNeedImg, 1, __ATOMIC_RELEASE);
        CURLOG("commit failed (%d), %u in a row", e, fCurFails + 1);
        if (++fCurFails <= 8) curArm(4000000);
    }
    /* first kIOHardwareCursorAttribute query (IOFramebuffer::open): bring the plane up only with boot-arg nmxhwcur=1 */
    bool curSetup() {
        if (fCurSide) return true;
        static bool parsed, wanted;
        if (!parsed) {
            int v = 0;
            parsed = true;
            wanted = PE_parse_boot_argn("nmxhwcur", &v, sizeof v) && v == 1;
            nmx_hwcursor = wanted;
        }
        if (!wanted || fStopping || fForeign || !fInternalPanel || __atomic_load_n(&sCurFb, __ATOMIC_SEQ_CST)) return false;
        uint32_t side = 0;
        int e;
        { NmxEntry entry; e = entry.err ? entry.err : nmx_fb_cursor_init(&side); }
        if (e || !side) {
            /* logged once, plainly: the framebuffer then has no hardware cursor for its whole lifetime (IOFramebuffer
             * asks at open only); EAGAIN = the display bridge was not up yet at open */
            FBLOG("cursor: NO HARDWARE CURSOR for this framebuffer's lifetime: nmx_fb_cursor_init errno %d%s, side %u",
                  e, e == EAGAIN ? " (EAGAIN: display bridge not ready at open)" : "", side);
            return false;
        }
        const size_t bytes = (size_t)side * side * 4;
        fCurStage[0] = (uint32_t *)IOMallocZero(bytes);
        fCurStage[1] = (uint32_t *)IOMallocZero(bytes);
        fCurCopy = (uint32_t *)IOMallocZero(bytes);
        fCurWork = thread_call_allocate_with_options(curWorkFire, this, THREAD_CALL_PRIORITY_HIGH,
                                                     THREAD_CALL_OPTIONS_ONCE);
        if (!fCurStage[0] || !fCurStage[1] || !fCurCopy || !fCurWork) {
            CURLOG("no hardware cursor (allocation): software cursor");
            curFree(side);
            return false;
        }
        fCurRefused = 1;                                    // nothing accepted yet: the probe at open comes first
        fCurSession = 0;
        __atomic_store_n(&fCurSide, side, __ATOMIC_SEQ_CST);
        nmx_fb_cursor_set_kick(curKick, this);
        __atomic_store_n(&sCurFb, this, __ATOMIC_SEQ_CST);
        nmx_hwcursor_avail = 1;
        setProperty("NMXHardwareCursor", (unsigned long long)side, 32);
        CURLOG("hardware cursor %ux%u ARGB (premultiplied on upload, inverting pixels grey)", side, side);
        return true;
    }
    void curFree(uint32_t side) {
        const size_t bytes = (size_t)side * side * 4;
        if (fCurWork) {
            thread_call_cancel_wait(fCurWork);              // cancels a pending run, or waits for the running one
            while (!thread_call_free(fCurWork)) { thread_call_cancel_wait(fCurWork); IOSleep(1); }
            fCurWork = nullptr;
        }
        uint32_t **bufs[3] = { &fCurStage[0], &fCurStage[1], &fCurCopy };
        for (uint32_t **b : bufs)
            if (*b) { IOFree(*b, bytes); *b = nullptr; }
    }
    /* stop(): fStopping is set. Order: no new entries (fCurSide 0), no bridge kicks, no callers inside, then free. */
    void curTeardown() {
        const uint32_t side = __atomic_exchange_n(&fCurSide, 0u, __ATOMIC_SEQ_CST);
        if (!side) return;
        __atomic_store_n(&sCurFb, (NMXAMDFB *)nullptr, __ATOMIC_SEQ_CST);
        nmx_fb_cursor_set_kick(nullptr, nullptr);           // returns when no re-arm is inside curKick
        if (fCurWork) thread_call_cancel_wait(fCurWork);    // a run asleep in a commit finishes first
        for (int n = 0; __atomic_load_n(&fCurBusy, __ATOMIC_SEQ_CST); n++) {
            if (n == 100000) FBLOG("cursor: teardown still waiting for %d caller(s)", (int)fCurBusy);
            IODelay(10);
        }
        curFree(side);
        { NmxEntry entry; if (!entry.err) nmx_fb_cursor_off(); }
        nmx_hwcursor_avail = 0;
    }

public:
    IOReturn setCursorImage(void *cursorImage) override {
        if (!curEnter()) return kIOReturnUnsupported;
        const UInt32 next = fCurSeq + 1;                    // (4e9 cursor images before it wraps to the "none" 0)
        const int s = next & 1;
        IOHardwareCursorDescriptor d;
        IOHardwareCursorInfo info;
        bzero(&d, sizeof d);
        bzero(&info, sizeof info);
        d.majorVersion = kHardwareCursorDescriptorMajorVersion;
        d.minorVersion = kHardwareCursorDescriptorMinorVersion;
        d.width = d.height = fCurSide;                      // IOFramebuffer halves it down to the image (64/128/256)
        d.bitDepth = 32;                                    // ARGB8888
        d.supportedSpecialEncodings = kInvertingEncodedPixel;   // never refuse an inverting image (see above)
        d.specialEncodings[kInvertingEncoding] = NMX_CUR_INVERT_PIXEL;
        info.majorVersion = kHardwareCursorInfoMajorVersion;
        info.minorVersion = kHardwareCursorInfoMinorVersion;
        info.hardwareCursorData = (UInt8 *)fCurStage[s];
        OSIncrementAtomic(&fCurGen);                        // odd: fCurWork does not trust a copy taken now
        const bool ok = convertCursorImage(cursorImage, &d, &info);
        if (ok) { fCurW[s] = info.cursorWidth; fCurH[s] = info.cursorHeight; }
        OSIncrementAtomic(&fCurGen);
        IOReturn r = kIOReturnSuccess;
        if (ok && info.cursorWidth <= fCurSide && info.cursorHeight <= fCurSide) {
            __atomic_store_n(&fCurSeq, next, __ATOMIC_RELEASE);
            __atomic_store_n(&fCurRefused, 0u, __ATOMIC_RELEASE);  // S2: visibility comes back with the image
            __atomic_store_n(&fCurSession, 1u, __ATOMIC_RELEASE);
            OSIncrementAtomic64(&gNmxCurAccepted);
        } else if (__atomic_load_n(&fCurSession, __ATOMIC_ACQUIRE)) {
            /* refusal after accept: keep the last accepted image (published seq and visibility untouched) and say
             * yes, so IOFramebuffer keeps the hardware path and keeps sending positions (see the contract above) */
            OSIncrementAtomic64(&gNmxCurKeptLast);
            CURLOG("image not converted after an accepted one: last image kept on the plane");
        } else {
            /* IOFramebuffer's descriptor probe at open (by design), or a conversion failure: hide the plane; with
             * hardwareCursorActive 0 WindowServer draws its cursor window from its next reload on */
            __atomic_store_n(&fCurRefused, 1u, __ATOMIC_RELEASE);
            OSIncrementAtomic64(&gNmxCurSwFallback);
            r = kIOReturnUnsupported;
        }
        curArm(0);
        curLeave();
        return r;
    }
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override {
        if (!curEnter()) return kIOReturnUnsupported;
        OSIncrementAtomic64(&gNmxCurStates);
        __atomic_store_n(&fCurPos, ((uint64_t)(uint32_t)x << 32) | (uint32_t)y, __ATOMIC_RELEASE);
        __atomic_store_n(&fCurVis, visible ? 1u : 0u, __ATOMIC_RELEASE);
        curArm(0);
        curLeave();
        return kIOReturnSuccess;
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
        /* ga403wm Step128: the owned path is the default for the internal panel. A framebuffer that is neither foreign
         * nor owned has no IOFBDependentID, so IOFramebuffer::start() makes no controller (fPrivate->controller NULL):
         * NMXAccel's first mode change then faults in getAttributeExt (CR2 0x120, 10 s into the boot), and without it
         * WindowServer's pipe never goes active (black panel). The source build lost the personality key that the
         * Step80 installer had written into Info.plist, which is exactly that. Opt out only with an explicit false. */
        const bool owned = internalPanel && !foreign && provider->getProperty("NMXOwnsDisplay") != kOSBooleanFalse;
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
        if (internalPanel && !foreign && !owned)
            FBLOG("internal panel is neither owned nor foreign (NMXOwnsDisplay false): no controller group, no display pipe");
        if (!IOFramebuffer::start(provider)) return false;
        setProperty("NMXScanoutBridgeVersion", 1ULL, 32);
        fInternalPanel = internalPanel;
        setupWakeWork();
        setProperty("NMXEDIDBytes", (unsigned long long)fDisplay.edid_size, 32);
        if (fDisplay.edid_size) setProperty("NMXEDIDSource", "drm-connector");
        fRefLock = IOLockAlloc();
        fVblHw = thread_call_allocate(vblHwFire, this);
        fVblRef = thread_call_allocate(vblRefFire, this);
        if (fVblHw && fVblRef) nmx_fb_vbl_set_sink(vblSink, this);
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
        fLcd = false;
        curTeardown();
        teardownWakeWork();
        if (fBacklight) {
            fBacklight->quiesce();
            fBacklight->detach(this);
            OSSafeReleaseNULL(fBacklight);
        }
        /* thread_call_cancel_wait is not exported to kexts: cancel and retry the free until the callback is out */
        nmx_fb_vbl_set_sink(nullptr, nullptr);     // returns once no CRTC vblank is inside vblSink
        thread_call_t *tcs[3] = { &fVblTimer, &fVblHw, &fVblRef };
        for (thread_call_t *tc : tcs) {
            if (!*tc) continue;
            thread_call_cancel(*tc);
            while (!thread_call_free(*tc)) { thread_call_cancel(*tc); IOSleep(5); }
            *tc = nullptr;
        }
        if (fRefHeld) { nmx_fb_vbl_ref(0); fRefHeld = 0; }
        if (fRefLock) { IOLockFree(fRefLock); fRefLock = nullptr; }
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
            if (fStopping || !fInfo.ok || !fInfo.kva || !fInfo.width || !fInfo.height ||
                (uint64_t)fInfo.pitch < 4ULL * fInfo.width ||
                (uint64_t)fInfo.pitch * fInfo.height > fInfo.size) {
                if (fNotReadyN++ < 3 || (fNotReadyN % 100) == 0)
                    FBLOG("pipe: scanout NOT READY #%u (power %d ok %d)", fNotReadyN, fPowerRegistered ? 1 : 0, fInfo.ok ? 1 : 0);
                return kIOReturnNotReady;
            }
            r->kva = reinterpret_cast<void *>(fInfo.kva);
            /* ga403wm Step75: NMXAccel's initFramebufferResource needs the scan-out's physical range, or it makes no
             * framebuffer resource and IOAcceleratorFamily never marks the display pipe active */
            r->phys = fInfo.phys;
            r->handle = nullptr;
            r->actualSize = fInfo.size;
            r->width = fInfo.width; r->height = fInfo.height; r->pitch = fInfo.pitch;
            trace("scanout ok", fInfo.width, fInfo.pitch);
            return kIOReturnSuccess;
        }
        /* ga403wm Step98/99: the NMXAccel page-flip API (nmx_fb_bridge.c) */
        if (fn && !fStopping) {
            long long fr;
            { NmxEntry entry; if (entry.err) return kIOReturnNotReady; fr = nmx_fb_flip_call(fn->getCStringNoCopy(), p1); }
            if (fr >= 0)
                return (IOReturn)fr;
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
        if (type == kIOFBVBLInterruptType) {
            trace("register VBL", (unsigned long long)(proc != nullptr));
            Irq *slot = nullptr;
            for (int i = 0; i < kVblSlots && !slot; i++)
                if (!fVblSlot[i].proc) slot = &fVblSlot[i];
            if (!slot) return kIOReturnNoResources;
            slot->target = target;
            slot->ref = ref;
            slot->on = true;            // registered on; the caller turns it off with setInterruptState
            __atomic_store_n(&slot->proc, proc, __ATOMIC_RELEASE);
            fVblLast = slot;
            if (iref) *iref = slot;
            if (fVblRef) thread_call_enter(fVblRef);
            return kIOReturnSuccess;
        }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    Irq *vblSlotOf(void *iref) {
        for (int i = 0; i < kVblSlots; i++)
            if (iref == &fVblSlot[i]) return &fVblSlot[i];
        return nullptr;
    }
    IOReturn unregisterInterrupt(void *iref) override {
        if (Irq *q = vblSlotOf(iref)) {
            q->on = false;
            // out-31 N6: SEQ_CST (xchg on x86) so the store is visible before fDelivering is read below; with RELEASE
            // the store could sit in the store buffer while a delivery (OSIncrementAtomic: locked, full barrier) reads
            // the old proc and this side still reads fDelivering 0 (store->load reordering)
            __atomic_store_n(&q->proc, (IOFBInterruptProc)nullptr, __ATOMIC_SEQ_CST);
            if (fVblLast == q) __atomic_store_n(&fVblLast, (Irq *)nullptr, __ATOMIC_SEQ_CST);
            // the caller may free target/refcon on return: wait out a delivery that read the old proc (IOFramebuffer's
            // VBL handler is interrupt-safe and takes no gate, so this cannot wait on the caller)
            for (int n = 0; __atomic_load_n(&fDelivering, __ATOMIC_SEQ_CST) && n < 1000; n++) IODelay(10);
            if (fVblRef) thread_call_enter(fVblRef);
        } else if (iref == &fConnect) {
            fConnect.proc = nullptr;
        }
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *iref, UInt32 state) override {
        Irq *q = vblSlotOf(iref);
        if (!q) return kIOReturnSuccess;
        const bool on = state == kEnabledInterruptState;
        if (q->on != on) {
            q->on = on;
            if (fVblRef) thread_call_enter(fVblRef);
        }
        return kIOReturnSuccess;
    }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    /* power acknowledged at once; left to IOFramebuffer it waits on a shared thread (seen with NVRMFB) */
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        if (ordinal) wakeWork("framebuffer power-on");
        return kIOPMAckImplied;
    }
    IOReturn getAppleSense(IOIndex idx, UInt32 *senseType, UInt32 *primary, UInt32 *extended,
                           UInt32 *displayType) override {
        if (idx == 0 && fLcd) {
            if (senseType) *senseType = 0;
            if (primary) *primary = 0;
            if (extended) *extended = 0;
            if (displayType) *displayType = kGenericLCD;
            return kIOReturnSuccess;
        }
        return IOFramebuffer::getAppleSense(idx, senseType, primary, extended, displayType);
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) {
            trace("setAttribute wsrv", value);
            /* IOFramebuffer::close (WindowServer gone) or WindowServer itself going unaccelerated: hide the plane (no
             * frozen arrow); the image stays accepted and the next setCursorState(visible) brings it back */
            if ((value & kIOWSAA_StateMask) == kIOWSAA_Unaccelerated && curEnter()) {
                __atomic_store_n(&fCurVis, 0u, __ATOMIC_RELEASE);
                curArm(0);
                curLeave();
            }
            return kIOReturnSuccess;
        }
        if (attribute == kIOPowerAttribute) {
            /* display sleep turns the outputs off; wake lights them again with the same buffer */
            if (fDev) { NmxEntry entry; if (!entry.err) nmx_fb_power(fDev, value != 0); }     /* stopping: outputs go with the driver */
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
        /* IOFramebuffer::open reads it once (haveHWCursor); kIOFBHWCursorSupported = 1 */
        if (attribute == kIOHardwareCursorAttribute && value && curSetup()) { *value = 1; return kIOReturnSuccess; }
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
OSDefineMetaClassAndStructors(NMXAMDFB, IOFramebuffer)
NMXAMDFB *volatile NMXAMDFB::sCurFb;

/* lights the device's outputs and publishes them under 'gpu'; nullptr when there is nothing to show (reason logged) */
IOService *nmx_fb_publish(IOService *gpu, void *dev)
{
    nmx_fb_info info;
    int err = nmx_fb_open(dev, &info);
    if (err || !info.ok) {
        FBLOG("no framebuffer (%d)", err);
        return nullptr;
    }
    NMXAMDFB *fb = OSTypeAlloc(NMXAMDFB);
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
    fb->publishBacklight();
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
