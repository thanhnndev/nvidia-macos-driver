/*
 * The kext: one IOService per AMD or Intel GPU it is allowed to drive (amdgpu and i915 share one DRM core and one
 * port layer here). Compiled with Apple's headers only; everything else is reached through nmx_port.h.
 *
 * Opt-in only, per vendor. The service matches every AMD and Intel display-class device but declines in probe()
 * unless the user asked for that vendor: boot argument nmxamd=1 / nmxintel=1, or the device property
 * "nullmoth-gpu" (set through the bootloader's DeviceProperties). Without that, the device keeps whatever driver macOS
 * gives it. Taking a GPU that is lighting the boot display resets its display engine, so the opt-in is per user,
 * never a default.
 *
 * The port (the FreeBSD/Linux layer and the AMD driver) starts with the first device and stops when the kext
 * unloads; unloading is refused while any process still has a GPU device node open.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include "nmx_port.h"
#include "nmx_xnu.h"

#define AMD_VENDOR 0x1002
#define INTEL_VENDOR 0x8086

/* the display bridge (nmx_fb.cpp) */
IOService *nmx_fb_publish(IOService *gpu, void *dev);
void nmx_fb_unpublish(IOService *fb, IOService *gpu, void *dev);

/* ---- M2 job trace ------------------------------------------------------------------------------------------------
 * No kernel message from any kext is dependable in the system log on this machine, and the answer is needed inside
 * the probing process's lifetime, so the driver publishes what it decided on sysctl instead of printf:
 *
 *   debug.nmx_trace  the last 16 submissions of every ring: ring, VM hub, ring type, vmid, pasid, page directory,
 *                    the raw job->vm_needs_flush and the flush the call actually emitted.
 *   debug.nmx_grab   the last 16 VMID assignments: hub, vmid, the flush decision, the VM's fence context, the VMID's
 *                    owner before the update and its cached page directory before and after. "stray" marks a job that
 *                    skips the flush while the VMID still belongs to another VM - the state that leaves the hardware
 *                    reading the other VM's page table.
 *   debug.nmx_timeouts  scheduler timeouts, also recorded as a row whose ring is TIME.
 */
#include <sys/sysctl.h>

struct nmx_tr_rec {
    unsigned long long seq, pd;
    unsigned hub, type, vmid, pasid, needs_flush, flush, pasid_map, pipe_sync, gpu_reset, gds;
    char ring[12];
};
static nmx_tr_rec gTr[16];
static unsigned gTrN;
static unsigned long long gTrSeq;

struct nmx_gr_rec {
    unsigned long long seq, vm_ctx, owner, pd_new, pd_old;
    unsigned hub, vmid, needs_flush, stray;
};
static nmx_gr_rec gGr[16];
static unsigned gGrN;
static unsigned long long gGrSeq;

static volatile SInt64 gNmxJobs, gNmxJobsFlushed, gNmxJobsNoFlush, gNmxTimeouts;
static volatile SInt64 gNmxGrabFlushed, gNmxGrabSkipped, gNmxGrabStray;

static void nmx_tr_ring(nmx_tr_rec *r, const char *name)
{
    unsigned i = 0;
    if (name) while (name[i] && i + 1 < sizeof r->ring) { r->ring[i] = name[i]; i++; }
    r->ring[i] = 0;
}

extern "C" void nmx_vm_trace(const char *ring, unsigned hub, unsigned type, unsigned vmid, unsigned pasid,
                             unsigned long long pd, unsigned needs_flush, unsigned flags)
{
    nmx_tr_rec *r = &gTr[gTrN % 16];
    r->seq = ++gTrSeq;
    r->pd = pd;
    r->hub = hub;
    r->type = type;
    r->vmid = vmid;
    r->pasid = pasid;
    r->needs_flush = needs_flush ? 1u : 0u;
    r->flush = (flags & 1) ? 1u : 0u;
    r->pasid_map = (flags & 2) ? 1u : 0u;
    r->pipe_sync = (flags & 4) ? 1u : 0u;
    r->gpu_reset = (flags & 8) ? 1u : 0u;
    r->gds = (flags & 16) ? 1u : 0u;
    nmx_tr_ring(r, ring);
    gTrN++;
    gNmxJobs = gNmxJobs + 1;
    if (flags & 1) gNmxJobsFlushed = gNmxJobsFlushed + 1;
    else gNmxJobsNoFlush = gNmxJobsNoFlush + 1;
}

extern "C" void nmx_grab_trace(unsigned hub, unsigned vmid, unsigned needs_flush,
                               unsigned long long vm_ctx, unsigned long long id_owner,
                               unsigned long long pd_new, unsigned long long pd_old)
{
    nmx_gr_rec *r = &gGr[gGrN % 16];
    r->seq = ++gGrSeq;
    r->hub = hub;
    r->vmid = vmid;
    r->needs_flush = needs_flush ? 1u : 0u;
    r->vm_ctx = vm_ctx;
    r->owner = id_owner;
    r->pd_new = pd_new;
    r->pd_old = pd_old;
    r->stray = (!needs_flush && id_owner != vm_ctx) ? 1u : 0u;
    gGrN++;
    if (needs_flush) gNmxGrabFlushed = gNmxGrabFlushed + 1;
    else gNmxGrabSkipped = gNmxGrabSkipped + 1;
    if (r->stray) gNmxGrabStray = gNmxGrabStray + 1;
}

extern "C" void nmx_job_timeout(unsigned vmid)
{
    nmx_tr_rec *r = &gTr[gTrN % 16];
    r->seq = ++gTrSeq;
    r->pd = 0;
    r->hub = 0xff;
    r->type = 0xff;
    r->vmid = vmid;
    r->pasid = 0;
    r->needs_flush = r->flush = r->pasid_map = r->pipe_sync = r->gpu_reset = r->gds = 0;
    nmx_tr_ring(r, "TIME");
    gTrN++;
    gNmxTimeouts = gNmxTimeouts + 1;
}

static int nmx_trace_sysctl SYSCTL_HANDLER_ARGS
{
    char buf[2048];
    int n = snprintf(buf, sizeof buf,
                     "seq ring hub type vmid pasid pd needs_flush flush pasid_map pipe_sync gpu_reset gds\n");
    unsigned count = gTrN < 16 ? gTrN : 16;
    for (unsigned k = 0; k < count && n > 0 && (size_t)n < sizeof buf - 1; k++) {
        const nmx_tr_rec *r = &gTr[(gTrN - count + k) % 16];
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%llu %s %u %u %u %u %llx %u %u %u %u %u %u\n",
                      r->seq, r->ring, r->hub, r->type, r->vmid, r->pasid, r->pd,
                      r->needs_flush, r->flush, r->pasid_map, r->pipe_sync, r->gpu_reset, r->gds);
        if ((size_t)n >= sizeof buf) { n = (int)sizeof buf - 1; break; }
    }
    if (n < 0) n = 0;
    return sysctl_handle_string(oidp, buf, sizeof buf, req);
}

static int nmx_grab_sysctl SYSCTL_HANDLER_ARGS
{
    char buf[2048];
    int n = snprintf(buf, sizeof buf,
                     "seq hub vmid needs_flush vm_ctx owner pd_new pd_old stray\n");
    unsigned count = gGrN < 16 ? gGrN : 16;
    for (unsigned k = 0; k < count && n > 0 && (size_t)n < sizeof buf - 1; k++) {
        const nmx_gr_rec *r = &gGr[(gGrN - count + k) % 16];
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%llu %u %u %u %llx %llx %llx %llx %u\n",
                      r->seq, r->hub, r->vmid, r->needs_flush, r->vm_ctx, r->owner,
                      r->pd_new, r->pd_old, r->stray);
        if ((size_t)n >= sizeof buf) { n = (int)sizeof buf - 1; break; }
    }
    if (n < 0) n = 0;
    return sysctl_handle_string(oidp, buf, sizeof buf, req);
}

SYSCTL_PROC(_debug, OID_AUTO, nmx_trace, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
            0, 0, nmx_trace_sysctl, "A",
            "last 16 AMD submissions: ring, VM hub, vmid, page directory, job->vm_needs_flush and the flush emitted");
SYSCTL_PROC(_debug, OID_AUTO, nmx_grab, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
            0, 0, nmx_grab_sysctl, "A",
            "last 16 VMID assignments: hub, vmid, flush decision, VM context, VMID owner, page directory");

#define NMX_CNT(nm, var, desc) \
    SYSCTL_QUAD(_debug, OID_AUTO, nm, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, (SInt64 *)&var, desc)
NMX_CNT(nmx_jobs, gNmxJobs, "AMD submissions whose VM/flush decision was recorded");
NMX_CNT(nmx_jobs_flushed, gNmxJobsFlushed, "of those, submissions that emitted a VM flush");
NMX_CNT(nmx_jobs_no_flush, gNmxJobsNoFlush, "of those, submissions with no VM flush");
NMX_CNT(nmx_timeouts, gNmxTimeouts, "AMD submissions the scheduler reported as timed out");
NMX_CNT(nmx_grab_flushed, gNmxGrabFlushed, "VMID assignments that asked for a flush");
NMX_CNT(nmx_grab_skipped, gNmxGrabSkipped, "VMID assignments that skipped the flush");
NMX_CNT(nmx_grab_stray, gNmxGrabStray,
        "VMID assignments that skipped the flush while the VMID still belonged to another VM");

/* A kext's SYSCTL_* entries are not registered automatically: sys/sysctl.h says so plainly ("UNLESS they are defined in
 * I/O-Kit. In this case, you have to call sysctl_register_oid() manually - just like in a KEXT"). NMXAccel did that and
 * its debug.* entries work; NMXGPU did not, so every debug.nmx_* was an unknown oid even though the kext carried it. */
/* User (mmap) cache mode for GPU buffers, read by nmx_umap_insert: 1 = uncached (default, this GPU does not snoop
 * the CPU caches), 2 = write-combined, 0 = whatever the DRM driver asked for. */
/* Flush every CPU cache before a submission (amdgpu_cs.c). Blunt on purpose: it tests whether a dirty write-back line
 * from the kernel's view is being written over what userland put in a buffer through its uncached mapping. */
/* Flush every CPU cache when a user mapping of GPU buffers is created (nmx_mmap): dirty write-back lines from the
 * kernel's view of those pages would otherwise land on top of what userland writes through its uncached mapping. */
extern "C" int nmx_umap_flush_on_map = 0;
SYSCTL_INT(_debug, OID_AUTO, nmx_umap_flush, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &nmx_umap_flush_on_map, 0,
           "flush all CPU caches when a user BO mapping is created: 0 off (default), 1 on");

extern "C" int nmx_wbinvd_before_submit = 0;
SYSCTL_INT(_debug, OID_AUTO, nmx_wbinvd, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &nmx_wbinvd_before_submit, 0,
           "flush all CPU caches before each submission: 0 off (default), 1 on");

extern "C" int nmx_umap_cache = 1;
SYSCTL_INT(_debug, OID_AUTO, nmx_umap_cache, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &nmx_umap_cache, 0,
           "user BO mapping cache: 0 driver default, 1 uncached (default), 2 write-combined");

static void nmx_sysctl_install(void)
{
    static bool installed = false;
    if (installed) return;
    sysctl_register_oid(&sysctl__debug_nmx_trace);
    sysctl_register_oid(&sysctl__debug_nmx_grab);
    sysctl_register_oid(&sysctl__debug_nmx_jobs);
    sysctl_register_oid(&sysctl__debug_nmx_jobs_flushed);
    sysctl_register_oid(&sysctl__debug_nmx_jobs_no_flush);
    sysctl_register_oid(&sysctl__debug_nmx_timeouts);
    sysctl_register_oid(&sysctl__debug_nmx_grab_flushed);
    sysctl_register_oid(&sysctl__debug_nmx_grab_skipped);
    sysctl_register_oid(&sysctl__debug_nmx_grab_stray);
    sysctl_register_oid(&sysctl__debug_nmx_umap_cache);
    sysctl_register_oid(&sysctl__debug_nmx_wbinvd);
    sysctl_register_oid(&sysctl__debug_nmx_umap_flush);
    installed = true;
}

class NMXGPU : public IOService {
    OSDeclareDefaultStructors(NMXGPU)
    void *dev_;
    IOService *fb_;
    IOPCIDevice *pci_;
    volatile int attach_;   /* 0 = attach pending on its thread, 1 = attached, -1 = not attached */
    bool pm_;
    static void attachThread(void *arg, wait_result_t);
    void attachNow();
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    void systemWillShutdown(IOOptionBits spec) override;
};
OSDefineMetaClassAndStructors(NMXGPU, IOService)

static IOLock *g_lock;
static bool g_started;
static int g_devices;

static bool opted_in(IOPCIDevice *pci, uint16_t vendor)
{
    int v = 0;
    /* the boot picker's "Start once without NVIDIA driver" entry (-nvoff) parks every NullMoth GPU driver for that start */
    if (PE_parse_boot_argn("-nvoff", &v, sizeof(v)))
        return false;
    if (PE_parse_boot_argn(vendor == AMD_VENDOR ? "nmxamd" : "nmxintel", &v, sizeof(v)) && v != 0)
        return true;
    OSObject *p = pci->getProperty("nullmoth-gpu");
    if (OSBoolean *b = OSDynamicCast(OSBoolean, p))
        return b->isTrue();
    if (OSData *d = OSDynamicCast(OSData, p))
        return d->getLength() > 0 && ((const uint8_t *)d->getBytesNoCopy())[0] != 0;
    return false;
}

IOService *NMXGPU::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    uint16_t vendor = pci ? pci->configRead16(kIOPCIConfigVendorID) : 0;
    if (vendor != AMD_VENDOR && vendor != INTEL_VENDOR)
        return nullptr;
    if (!opted_in(pci, vendor))
        return nullptr;     /* not asked for: leave the device to macOS */
    return IOService::probe(provider, score);
}

bool NMXGPU::start(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == nullptr || g_lock == nullptr || !IOService::start(provider))
        return false;
    nmx_sysctl_install();
    IOLockLock(g_lock);
    if (!g_started) {
        int err = nmx_port_start();
        if (err) {
            IOLockUnlock(g_lock);
            IOLog("NullMoth GPU: the driver layer did not start (%d)\n", err);
            IOService::stop(provider);
            return false;
        }
        g_started = true;
    }
    g_devices++;
    IOLockUnlock(g_lock);

    /*
     * amdgpu, i915 and xe load firmware while they attach. During boot matching that read would come before the root and
     * Data volumes are mounted, and mounting waits for matching to settle: the Mac would stop at the logo (seen with the
     * NPU kext on the same layer, 10-11). The attach runs on its own thread once the firmware folder exists; the firmware
     * framebuffer keeps the screen until then.
     */
    pci_ = pci;
    dev_ = nullptr;
    fb_ = nullptr;
    pm_ = false;
    attach_ = 0;
    retain();
    thread_t t = THREAD_NULL;
    if (kernel_thread_start(&NMXGPU::attachThread, this, &t) != KERN_SUCCESS) {
        IOLog("NullMoth GPU: could not start the attach thread\n");
        attach_ = -1;
        release();
        return true;
    }
    thread_deallocate(t);
    return true;
}

void NMXGPU::attachThread(void *arg, wait_result_t)
{
    NMXGPU *self = (NMXGPU *)arg;
    self->attachNow();
    self->release();
}

void NMXGPU::attachNow()
{
    int err = nmx_xnu_wait_firmware_root(300);
    if (err) {
        IOLog("NullMoth GPU: the firmware folder never appeared (%d); this GPU stays with macOS\n", err);
        attach_ = -1;
        return;
    }
    dev_ = nmx_port_attach(pci_);
    if (dev_ == nullptr) {
        IOLog("NullMoth GPU: no driver took %04x:%04x\n", pci_->configRead16(kIOPCIConfigVendorID),
              pci_->configRead16(kIOPCIConfigDeviceID));
        attach_ = -1;
        return;
    }

    static IOPMPowerState states[2] = {
        { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    PMinit();
    pci_->joinPMtree(this);
    registerPowerDriver(this, states, 2);
    pm_ = true;
    registerService();
    /* NMXAccel (the Metal accelerator for this GPU) starts only on a device this driver has claimed; it declined while
       the claim was pending, so matching on the device runs again now that it is set */
    pci_->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci_->registerService();
    /* light the outputs and hand them to macOS; without it the device still renders, with no screen of its own */
    fb_ = nmx_fb_publish(this, dev_);
    IOLog("NullMoth GPU: driving %04x:%04x\n", pci_->configRead16(kIOPCIConfigVendorID),
          pci_->configRead16(kIOPCIConfigDeviceID));
    OSSynchronizeIO();
    attach_ = 1;
}

void NMXGPU::stop(IOService *provider)
{
    /* an attach still running on its thread finishes first (it holds a reference on this object meanwhile) */
    for (int i = 0; attach_ == 0 && i < 6000; i++)
        IOSleep(100);
    provider->removeProperty("nmxgpu-claimed");
    if (fb_ != nullptr)
        nmx_fb_unpublish(fb_, this, dev_);
    fb_ = nullptr;
    if (pm_)
        PMstop();
    pm_ = false;
    nmx_port_detach(dev_);
    dev_ = nullptr;
    IOLockLock(g_lock);
    g_devices--;
    IOLockUnlock(g_lock);
    IOService::stop(provider);
}

/* state 0 = sleep, 1 = on */
IOReturn NMXGPU::setPowerState(unsigned long state, IOService *device)
{
    if (dev_ != nullptr) {
        uint64_t t0 = mach_absolute_time(), t1, ns;
        IOLog("NullMoth GPU: setPowerState(%lu) begin\n", state);
        int err = nmx_port_power(dev_, state != 0);
        t1 = mach_absolute_time();
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        IOLog("NullMoth GPU: %s returned %d in %llu ms\n", state ? "resume" : "suspend", err, ns / 1000000);
    }
    return kIOPMAckImplied;
}

void NMXGPU::systemWillShutdown(IOOptionBits spec)
{
    if (dev_ != nullptr)
        nmx_port_shutdown(dev_);
    IOService::systemWillShutdown(spec);
}

/* ---- module start/stop: the stop refuses while device nodes are open ------------------------------------------------- */

extern "C" {
kern_return_t nmx_kmod_start(kmod_info_t *ki, void *data);
kern_return_t nmx_kmod_stop(kmod_info_t *ki, void *data);
}

kern_return_t nmx_kmod_start(kmod_info_t *ki, void *data)
{
    g_lock = IOLockAlloc();
    return g_lock ? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t nmx_kmod_stop(kmod_info_t *ki, void *data)
{
    IOLockLock(g_lock);
    if (g_started) {
        if (g_devices > 0 || nmx_port_busy() > 0) {
            IOLockUnlock(g_lock);
            IOLog("NullMoth GPU: still in use; not unloading\n");
            return KERN_FAILURE;
        }
        nmx_port_stop();
        g_started = false;
    }
    IOLockUnlock(g_lock);
    IOLockFree(g_lock);
    g_lock = nullptr;
    return KERN_SUCCESS;
}

/* libkmod's entry points; they call _realmain/_antimain below */
extern "C" {
kern_return_t _start(kmod_info_t *ki, void *data);
kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(com.nullmoth.NMXGPU, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = nmx_kmod_start;
__private_extern__ kmod_stop_func_t *_antimain = nmx_kmod_stop;
}
