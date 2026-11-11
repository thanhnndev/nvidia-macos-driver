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
#include <kern/thread_call.h>
#include "nmx_port.h"
#include "nmx_xnu.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/RootDomain.h>

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

extern "C" int nmx_pin_by_proc_format(char *buf, size_t size);
static int nmx_pin_by_proc_sysctl SYSCTL_HANDLER_ARGS
{
    char buf[512];
    int n = nmx_pin_by_proc_format(buf, sizeof buf);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    buf[n] = 0;
    return sysctl_handle_string(oidp, buf, sizeof buf, req);
}
extern "C" int nmx_userwire_format(char *buf, size_t size);
static int nmx_userwire_by_kind_sysctl SYSCTL_HANDLER_ARGS
{
    char buf[1024];
    int n = nmx_userwire_format(buf, sizeof buf);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    buf[n] = 0;
    return sysctl_handle_string(oidp, buf, sizeof buf, req);
}
SYSCTL_PROC(_debug, OID_AUTO, nmx_userwire_by_kind, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
            0, 0, nmx_userwire_by_kind_sysctl, "A",
            "Step142: user wirings by kind (surface/plain), age, parked, orphaned surfaces, and per process (pages)");
SYSCTL_PROC(_debug, OID_AUTO, nmx_pin_by_proc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
            0, 0, nmx_pin_by_proc_sysctl, "A", "Step139: user-page pin calls and pages per process name");
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
extern "C" volatile SInt64 gNmxPinWriteFail, gNmxPinWriteRetryOk, gNmxPinRoRetry, gNmxPinLastRc, gNmxPinLastRc2;
NMX_CNT(nmx_pin_write_fail, gNmxPinWriteFail, "Step132: writable user-page wires that failed the first time");
NMX_CNT(nmx_pin_write_retry_ok, gNmxPinWriteRetryOk, "Step132: of those, wires that worked after a read fault-in");
NMX_CNT(nmx_pin_ro_retry, gNmxPinRoRetry, "Step96/132: userptr BOs pinned read-only after a failed writable pin");
NMX_CNT(nmx_pin_last_rc, gNmxPinLastRc, "Step132: IOReturn of the last failed first writable wire");
NMX_CNT(nmx_pin_last_rc2, gNmxPinLastRc2, "Step132: IOReturn of the last writable wire that failed again after fault-in");
extern "C" volatile SInt64 gNmxPinOtherFail, gNmxPinOtherRc;
extern "C" char gNmxPinOtherName[32];
NMX_CNT(nmx_pin_other_fail, gNmxPinOtherFail, "Step132b: failed first writable wires outside WindowServer");
NMX_CNT(nmx_pin_other_rc, gNmxPinOtherRc, "Step132b: IOReturn of the last of those");
extern "C" volatile SInt64 gNmxSurfHint, gNmxSurfWire, gNmxSurfMiss, gNmxSurfBadPage, gNmxSurfNoSurf, gNmxSurfShort,
    gNmxSurfPrepFail;
extern "C" volatile SInt64 gNmxUserWires, gNmxUserWirePages;
extern "C" uint64_t gNmxPagesLive;
NMX_CNT(nmx_pages_live, gNmxPagesLive, "Step138: port page records handed out now (GTT/system pages, all kinds)");
extern "C" uint64_t gNmxPageChunks, gNmxPageFree, gNmxPageChunksFreed, gNmxPageChunksFilled, gNmxPageChunksInline, gNmxPageHwm;
NMX_CNT(nmx_page_chunks, gNmxPageChunks, "Step141: 2 MB page chunks held now");
NMX_CNT(nmx_page_free, gNmxPageFree, "Step141: free pages pooled in those chunks");
NMX_CNT(nmx_page_chunks_freed, gNmxPageChunksFreed, "Step141: chunks given back to the system");
NMX_CNT(nmx_page_chunks_filled, gNmxPageChunksFilled, "Step141c: chunks made ahead by the fill thread call");
NMX_CNT(nmx_page_chunks_inline, gNmxPageChunksInline, "Step141c: chunks made in the allocation path");
NMX_CNT(nmx_page_hwm, gNmxPageHwm, "Step141d: peak chunk pages in use over the last 60 s (kept pooled)");
NMX_CNT(nmx_userwire_count, gNmxUserWires, "Step138: user-memory wirings (IOMemoryDescriptors) held for the GPU now");
NMX_CNT(nmx_userwire_pages, gNmxUserWirePages, "Step138: pages covered by those wirings");
extern "C" volatile SInt64 gNmxSurfWires, gNmxSurfWirePages, gNmxUserParked, gNmxUserParkedPages, gNmxUserParks,
    gNmxUserUnparks, gNmxUserUnparkFail;
NMX_CNT(nmx_surfwire_count, gNmxSurfWires, "Step142: of the user wirings, IOSurface descriptors wired now");
NMX_CNT(nmx_surfwire_pages, gNmxSurfWirePages, "Step142: pages of those (the NMX share of zprint's IOSurface tag)");
NMX_CNT(nmx_userwire_parked, gNmxUserParked, "Step142: IOSurface wirings parked now (BO idle: held, not wired)");
NMX_CNT(nmx_userwire_parked_pages, gNmxUserParkedPages, "Step142: pages of those");
NMX_CNT(nmx_userwire_parks, gNmxUserParks, "Step142: wirings parked since boot");
NMX_CNT(nmx_userwire_unparks, gNmxUserUnparks, "Step142: parked wirings wired again for the GPU since boot");
NMX_CNT(nmx_userwire_unpark_fail, gNmxUserUnparkFail, "Step142: parked wirings that could not be wired again");
extern "C" volatile SInt64 gNmxUpinRestoreFail, gNmxUpinDestroyRec, gNmxParkSkipBusy, gNmxParkFail;
NMX_CNT(nmx_upin_restore_fail, gNmxUpinRestoreFail, "Step142: userptr BO page restores that failed");
NMX_CNT(nmx_upin_destroy_rec, gNmxUpinDestroyRec,
        "Step142: userptr BOs destroyed still holding their pages (never populated: they leaked before Step142)");
NMX_CNT(nmx_park_skip_busy, gNmxParkSkipBusy, "Step142: idle-park candidates skipped (locked, busy, pinned or shared)");
NMX_CNT(nmx_park_fail, gNmxParkFail, "Step142: idle parks whose move to system memory failed");
extern "C" int nmx_userptr_park_ms;
SYSCTL_INT(_debug, OID_AUTO, nmx_userptr_park_ms, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &nmx_userptr_park_ms, 0,
           "Step142: unwire an IOSurface userptr BO's memory after this many ms without GPU use; 0 = never");
extern "C" volatile SInt64 gNmxUserRecLive, gNmxUserRecMade, gNmxUserRecFreed, gNmxUserRecKept;
NMX_CNT(nmx_userrec_live, gNmxUserRecLive, "Step138: page records of pinned user pages alive now (freed when unpinned)");
NMX_CNT(nmx_userrec_made, gNmxUserRecMade, "Step138: user page records made");
NMX_CNT(nmx_userrec_freed, gNmxUserRecFreed, "Step138: user page records freed after their last unpin");
NMX_CNT(nmx_userrec_kept, gNmxUserRecKept, "Step138: user page records made permanent (reached by another path)");
NMX_CNT(nmx_surf_hint, gNmxSurfHint, "Step137: IOSurface hints for a userptr range (DRM_AMDGPU_NMX_SURFACE_HINT)");
NMX_CNT(nmx_surf_wire, gNmxSurfWire, "Step137: userptr imports that pinned the IOSurface's own memory");
NMX_CNT(nmx_surf_miss, gNmxSurfMiss, "Step137: hinted imports that fell back to the plain user wire");
NMX_CNT(nmx_surf_bad_page, gNmxSurfBadPage, "Step137: of those, the range's first page was not the surface's");
NMX_CNT(nmx_surf_no_surf, gNmxSurfNoSurf, "Step137: of those, the task could not look the surface up");
NMX_CNT(nmx_surf_short, gNmxSurfShort, "Step137: of those, the surface's memory was shorter than the range");
NMX_CNT(nmx_surf_prep_fail, gNmxSurfPrepFail, "Step137: of those, the surface's memory could not be prepared");
SYSCTL_STRING(_debug, OID_AUTO, nmx_pin_other_name, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, gNmxPinOtherName,
              sizeof(gNmxPinOtherName), "Step132b: process of the last of those");
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

/* ga403wm Step91: default 0 = the driver's own attribute. With snooped GTT (Step88) a cached BO maps write-back and
 * a USWC BO maps WC; forced UC/WC made every CPU read of a BO run at ~108 MB/s. */
extern "C" int nmx_umap_cache = 0;
SYSCTL_INT(_debug, OID_AUTO, nmx_umap_cache, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &nmx_umap_cache, 0,
           "user BO mapping cache: 0 driver default (default), 1 uncached, 2 write-combined");

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
    sysctl_register_oid(&sysctl__debug_nmx_pin_write_fail);
    sysctl_register_oid(&sysctl__debug_nmx_pin_write_retry_ok);
    sysctl_register_oid(&sysctl__debug_nmx_pin_ro_retry);
    sysctl_register_oid(&sysctl__debug_nmx_pin_last_rc);
    sysctl_register_oid(&sysctl__debug_nmx_pin_last_rc2);
    sysctl_register_oid(&sysctl__debug_nmx_pin_other_fail);
    sysctl_register_oid(&sysctl__debug_nmx_pin_other_rc);
    sysctl_register_oid(&sysctl__debug_nmx_pin_other_name);
    sysctl_register_oid(&sysctl__debug_nmx_pin_by_proc);
    sysctl_register_oid(&sysctl__debug_nmx_pages_live);
    sysctl_register_oid(&sysctl__debug_nmx_page_chunks);
    sysctl_register_oid(&sysctl__debug_nmx_page_free);
    sysctl_register_oid(&sysctl__debug_nmx_page_chunks_freed);
    sysctl_register_oid(&sysctl__debug_nmx_page_chunks_filled);
    sysctl_register_oid(&sysctl__debug_nmx_page_chunks_inline);
    sysctl_register_oid(&sysctl__debug_nmx_page_hwm);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_count);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_pages);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_by_kind);
    sysctl_register_oid(&sysctl__debug_nmx_surfwire_count);
    sysctl_register_oid(&sysctl__debug_nmx_surfwire_pages);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_parked);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_parked_pages);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_parks);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_unparks);
    sysctl_register_oid(&sysctl__debug_nmx_userwire_unpark_fail);
    sysctl_register_oid(&sysctl__debug_nmx_upin_restore_fail);
    sysctl_register_oid(&sysctl__debug_nmx_upin_destroy_rec);
    sysctl_register_oid(&sysctl__debug_nmx_park_skip_busy);
    sysctl_register_oid(&sysctl__debug_nmx_park_fail);
    sysctl_register_oid(&sysctl__debug_nmx_userptr_park_ms);
    sysctl_register_oid(&sysctl__debug_nmx_userrec_live);
    sysctl_register_oid(&sysctl__debug_nmx_userrec_made);
    sysctl_register_oid(&sysctl__debug_nmx_userrec_freed);
    sysctl_register_oid(&sysctl__debug_nmx_userrec_kept);
    sysctl_register_oid(&sysctl__debug_nmx_surf_hint);
    sysctl_register_oid(&sysctl__debug_nmx_surf_wire);
    sysctl_register_oid(&sysctl__debug_nmx_surf_miss);
    sysctl_register_oid(&sysctl__debug_nmx_surf_bad_page);
    sysctl_register_oid(&sysctl__debug_nmx_surf_no_surf);
    sysctl_register_oid(&sysctl__debug_nmx_surf_short);
    sysctl_register_oid(&sysctl__debug_nmx_surf_prep_fail);
    sysctl_register_oid(&sysctl__debug_nmx_umap_cache);
    sysctl_register_oid(&sysctl__debug_nmx_wbinvd);
    sysctl_register_oid(&sysctl__debug_nmx_umap_flush);
    installed = true;
}

/*
 * ga403wm Step128 boot stage marks. The Step125 source build hung boots at ~12 s with the disk already stopped, so
 * the kernel log of the hang never reached it. NVRAM survives: each stage is appended to "nmxgpu-stage" (the last
 * ~180 bytes, "<uptime ms>:<stage>|" entries, rewritten whole) and forced to flash (IONVRAM-FORCESYNCNOW; the plain
 * SYNCNOW is rate limited and an unsynced write stays in RAM). After NMXAMDFB is published a 1 s heartbeat writes
 * "nmxgpu-alive" for 40 s (uptime, beats, 890M IRQs dispatched, NVRM's nvrm-nmx-wait), so the last one dates the hang.
 * Both live in the Apple NVRAM GUID (7C436110-AB2A-4BBB-A880-FE41995C9F82). Boot-arg nmxstage=0 turns them off.
 * Never from interrupt context or with interrupts off (NVRAM writes take locks and may sleep). No memchr and no
 * ml_get_interrupts_enabled: no other kext here imports them, so their kext export is unproven (Step128 lesson).
 */
static IOLock *g_stage_lock;
static char g_stage_buf[192];
static size_t g_stage_len;
static int g_stage_on = -1;
extern "C" unsigned long nmx_irq_dispatched;

static void nmx_nvram_put(const char *key, const char *val, size_t len)
{
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (options == nullptr)
        return;
    if (OSData *d = OSData::withBytes(val, (unsigned)len)) {
        options->setProperty(key, d);
        d->release();
    }
    if (OSData *one = OSData::withBytes("1", 1)) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", one);
        one->release();
    }
    options->release();
}

static bool nmx_stage_enabled()
{
    if (g_stage_on < 0) {
        int v = 1;
        if (PE_parse_boot_argn("nmxstage", &v, sizeof(v)) && v == 0)
            g_stage_on = 0;
        else
            g_stage_on = 1;
    }
    return g_stage_on == 1 && g_stage_lock != nullptr;
}

static uint64_t nmx_uptime_ms()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}

extern "C" void nmx_xnu_stage(const char *what)
{
    /* the call sites are all sleepable (attach thread, IOKit matching, thread_call); interrupt context is refused */
    if (!nmx_stage_enabled() || ml_at_interrupt_context())
        return;
    char e[64], copy[sizeof(g_stage_buf)];
    int n = snprintf(e, sizeof(e), "%llu:%s|", nmx_uptime_ms(), what);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(e))
        n = sizeof(e) - 1;
    IOLockLock(g_stage_lock);
    /* drop the oldest entries until the new one fits */
    while (g_stage_len + n > sizeof(g_stage_buf) - 1 && g_stage_len > 0) {
        size_t cut = 0;
        while (cut < g_stage_len && g_stage_buf[cut] != '|')
            cut++;
        cut = cut < g_stage_len ? cut + 1 : g_stage_len;
        memmove(g_stage_buf, g_stage_buf + cut, g_stage_len - cut);
        g_stage_len -= cut;
    }
    memcpy(g_stage_buf + g_stage_len, e, n);
    g_stage_len += n;
    size_t len = g_stage_len;
    memcpy(copy, g_stage_buf, len);
    nmx_nvram_put("nmxgpu-stage", copy, len);     /* under the lock: marks reach NVRAM in order */
    IOLockUnlock(g_stage_lock);
}

static thread_call_t g_beat_call;
static unsigned g_beats;

static void nmx_beat(thread_call_param_t, thread_call_param_t)
{
    char nv[48] = "-", b[160];
    OSDictionary *m = IOService::serviceMatching("NVRM");
    if (m != nullptr) {
        if (IOService *s = IOService::copyMatchingService(m)) {
            if (OSString *w = OSDynamicCast(OSString, s->getProperty("nvrm-nmx-wait")))
                strlcpy(nv, w->getCStringNoCopy(), sizeof(nv));
            else
                strlcpy(nv, "started, no wait yet", sizeof(nv));
            s->release();
        }
        m->release();
    }
    int n = snprintf(b, sizeof(b), "%llu ms beat %u irq %lu nvrm %s", nmx_uptime_ms(), ++g_beats,
                     nmx_irq_dispatched, nv);
    nmx_nvram_put("nmxgpu-alive", b, n > 0 ? (size_t)n : 0);
    if (g_beats < 40) {
        uint64_t deadline;
        clock_interval_to_deadline(1, kSecondScale, &deadline);
        thread_call_enter_delayed(g_beat_call, deadline);
    }
}

static void nmx_beat_start()
{
    if (!nmx_stage_enabled() || g_beat_call != nullptr)
        return;
    g_beat_call = thread_call_allocate(&nmx_beat, nullptr);   /* kept for the kext's life: the cap is 40 beats */
    if (g_beat_call != nullptr)
        thread_call_enter(g_beat_call);
}

/*
 * ga403wm Step130 (was the NMXPowerKeep personality of the NMXBacklight kext, Step106): keeps NMXGPU powered while the
 * system is awake. NMXGPU registers two power states but never asks for one itself, so its state follows its
 * children: when display sleep takes the framebuffer down, NMXGPU dropped to 0 and ran a full amdgpu suspend under
 * WindowServer and NMXAccel (the 2026-10-17 hang). This child of NMXGPU asks for state 1, which needs NMXGPU's
 * kIOPMPowerOn, so display sleep is the framebuffer's own DPMS off/on only. On system sleep the power domain goes down
 * from the top and this child follows it to 0 as any device does. -nmxkeepoff leaves it out; nmxkeep_dsleep=0 keeps
 * the logging but lets display sleep suspend NMXGPU. It also logs the system sleep/wake timeline.
 */
class NMXPowerKeep : public IOService {
    OSDeclareDefaultStructors(NMXPowerKeep)
    bool pm_ = false;
    UInt32 changes_ = 0, sleeps_ = 0, wakes_ = 0;
    uint64_t tSleep_ = 0;
    IONotifier *sysNote_ = nullptr;
    static IOReturn sysPower(void *target, void *, UInt32 type, IOService *, void *arg, vm_size_t);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
};
OSDefineMetaClassAndStructors(NMXPowerKeep, IOService)

bool NMXPowerKeep::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    static IOPMPowerState states[2] = {
        { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    PMinit();
    provider->joinPMtree(this);
    if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) {
        IOLog("NMXPowerKeep: registerPowerDriver failed, inert\n");
        PMstop();
        return false;
    }
    pm_ = true;
    UInt32 hold = 1;
    PE_parse_boot_argn("nmxkeep_dsleep", &hold, sizeof hold);
    if (hold)
        changePowerStateTo(1);
    setProperty("PowerKeep,Wanted", (unsigned long long)(hold ? 1 : 0), 32);
    sysNote_ = registerPrioritySleepWakeInterest(&NMXPowerKeep::sysPower, this, nullptr);
    setProperty("PowerKeep,SysSleeps", 0ULL, 32);
    IOLog("NMXPowerKeep: %s %s while the system is awake; sleep/wake interest %s\n", hold ? "holding" : "NOT holding",
          provider->getName(), sysNote_ ? "on" : "FAILED");
    registerService();
    return true;
}

void NMXPowerKeep::stop(IOService *provider)
{
    if (sysNote_) { sysNote_->remove(); sysNote_ = nullptr; }
    if (pm_)
        PMstop();
    pm_ = false;
    IOService::stop(provider);
}

IOReturn NMXPowerKeep::setPowerState(unsigned long state, IOService *)
{
    UInt32 n = ++changes_;
    setProperty("PowerKeep,State", (unsigned long long)state, 32);
    setProperty("PowerKeep,Changes", n, 32);
    if (n <= 64)
        IOLog("NMXPowerKeep: power state %lu (change %u)\n", state, (unsigned)n);
    return kIOPMAckImplied;
}

IOReturn NMXPowerKeep::sysPower(void *target, void *, UInt32 type, IOService *, void *arg, vm_size_t)
{
    NMXPowerKeep *me = static_cast<NMXPowerKeep *>(target);
    IOPowerStateChangeNotification *note = static_cast<IOPowerStateChangeNotification *>(arg);
    uint64_t now = mach_absolute_time(), ns = 0;
    switch (type) {
    case kIOMessageCanSystemSleep:
        if (note) note->returnValue = 0;
        break;
    case kIOMessageSystemWillSleep:
        me->tSleep_ = now;
        me->sleeps_++;
        me->setProperty("PowerKeep,SysSleeps", me->sleeps_, 32);
        IOLog("NMXPowerKeep: system will sleep (#%u), keep state %u\n", (unsigned)me->sleeps_,
              (unsigned)me->getPowerState());
        if (note) note->returnValue = 0;
        break;
    case kIOMessageSystemWillPowerOn:
        if (me->tSleep_) absolutetime_to_nanoseconds(now - me->tSleep_, &ns);
        IOLog("NMXPowerKeep: system will power on, %llu ms after will-sleep\n", ns / 1000000);
        break;
    case kIOMessageSystemHasPoweredOn:
        me->wakes_++;
        if (me->tSleep_) absolutetime_to_nanoseconds(now - me->tSleep_, &ns);
        me->setProperty("PowerKeep,SysWakes", me->wakes_, 32);
        me->setProperty("PowerKeep,LastWakeMs", ns / 1000000, 64);
        IOLog("NMXPowerKeep: system has powered on (#%u), %llu ms after will-sleep, keep state %u\n",
              (unsigned)me->wakes_, ns / 1000000, (unsigned)me->getPowerState());
        break;
    default:
        break;
    }
    return kIOReturnSuccess;
}

class NMXGPU : public IOService {
    OSDeclareDefaultStructors(NMXGPU)
    void *dev_;
    IOService *fb_;
    IOPCIDevice *pci_;
    NMXPowerKeep *keep_;
    volatile int attach_;   /* 0 = attach pending on its thread, 1 = attached, -1 = not attached */
    bool pm_;
    volatile SInt32 hold_;          /* 1 while the boot hold keeps this service busy */
    uint64_t holdT0_;
    thread_call_t holdCall_;
    static void attachThread(void *arg, wait_result_t);
    static void holdCap(thread_call_param_t self, thread_call_param_t);
    void attachNow();
    void holdArm();
    void holdRelease(const char *why);
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
    if (g_stage_lock == nullptr)
        g_stage_lock = IOLockAlloc();
    nmx_xnu_stage("start");
    IOLockLock(g_lock);
    if (!g_started) {
        nmx_xnu_stage("port_start");
        int err = nmx_port_start();
        nmx_xnu_stage(err ? "port_start FAILED" : "port_started");
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
    keep_ = nullptr;
    pm_ = false;
    hold_ = 0;
    holdT0_ = 0;
    holdCall_ = nullptr;
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

/*
 * Boot hold: from the moment the firmware folder is there until NMXAMDFB is published, this service stays busy, so
 * whatever waits for the GPU's branch of the registry to settle (WindowServer's display discovery, NVRM's
 * waitQuiet) does not take the boot firmware framebuffer for the only display. Not in start(): the attach thread
 * first waits for the root volume, and a busy service before that would hold up the mount (the logo hang). Capped at
 * 30 s; released on every exit of attachNow. The outcome is in the "nmxgpu-boot-hold" property.
 */
void NMXGPU::holdArm()
{
    int v = 1;
    /* ga403wm Step128: boot-arg nmxhold=0 skips the hold (bisects the ~12 s boot hang without a rebuild) */
    if (PE_parse_boot_argn("nmxhold", &v, sizeof(v)) && v == 0) {
        setProperty("nmxgpu-boot-hold", "off (nmxhold=0)");
        nmx_xnu_stage("hold off");
        return;
    }
    holdT0_ = mach_absolute_time();
    hold_ = 1;
    adjustBusy(1);
    holdCall_ = thread_call_allocate(&NMXGPU::holdCap, this);
    if (holdCall_ != nullptr) {
        uint64_t deadline;
        retain();                   /* dropped by holdCap, or by holdRelease when it cancels the cap */
        clock_interval_to_deadline(30, kSecondScale, &deadline);
        thread_call_enter_delayed(holdCall_, deadline);
    }
    setProperty("nmxgpu-boot-hold", "held");
}

void NMXGPU::holdCap(thread_call_param_t p, thread_call_param_t)
{
    NMXGPU *self = (NMXGPU *)p;
    self->holdRelease("30 s cap");
    self->release();
}

void NMXGPU::holdRelease(const char *why)
{
    uint64_t ns;
    char text[96];

    if (!OSCompareAndSwap(1, 0, &hold_))
        return;
    adjustBusy(-1);
    absolutetime_to_nanoseconds(mach_absolute_time() - holdT0_, &ns);
    snprintf(text, sizeof(text), "released: %s after %llu ms", why, ns / 1000000);
    setProperty("nmxgpu-boot-hold", text);
    IOLog("NullMoth GPU: boot hold %s\n", text);
    /* the thread_call itself is kept (one per device per boot): freeing it here could race a cap that is running */
    if (holdCall_ != nullptr && thread_call_cancel(holdCall_))
        release();
}

void NMXGPU::attachNow()
{
    int err = nmx_xnu_wait_firmware_root(300);
    if (err) {
        IOLog("NullMoth GPU: the firmware folder never appeared (%d); this GPU stays with macOS\n", err);
        attach_ = -1;
        return;
    }
    holdArm();
    nmx_xnu_stage("fw ok, hold, attach");
    dev_ = nmx_port_attach(pci_);
    nmx_xnu_stage(dev_ ? "attached" : "attach FAILED");
    if (dev_ == nullptr) {
        IOLog("NullMoth GPU: no driver took %04x:%04x\n", pci_->configRead16(kIOPCIConfigVendorID),
              pci_->configRead16(kIOPCIConfigDeviceID));
        attach_ = -1;
        holdRelease("no driver");
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
    if (PE_parse_boot_argn("-nmxkeepoff", nullptr, 0)) {
        IOLog("NMXPowerKeep: -nmxkeepoff, not started\n");
    } else if ((keep_ = OSTypeAlloc(NMXPowerKeep)) != nullptr) {
        if (!keep_->init() || !keep_->attach(this) || !keep_->start(this)) {
            IOLog("NMXPowerKeep: start failed\n");
            if (keep_->getProvider() != nullptr) keep_->detach(this);
            OSSafeReleaseNULL(keep_);
        }
    }
    /* NMXAccel (the Metal accelerator for this GPU) starts only on a device this driver has claimed; it declined while
       the claim was pending, so matching on the device runs again now that it is set */
    pci_->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci_->registerService();
    /* light the outputs and hand them to macOS; without it the device still renders, with no screen of its own */
    nmx_xnu_stage("registered, fb publish");
    fb_ = nmx_fb_publish(this, dev_);
    nmx_xnu_stage(fb_ ? "fb published" : "fb FAILED");
    holdRelease(fb_ != nullptr ? "NMXAMDFB published" : "no framebuffer");
    nmx_xnu_stage("hold released");
    nmx_beat_start();
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
    if (keep_ != nullptr) {
        keep_->stop(this);
        keep_->detach(this);
        OSSafeReleaseNULL(keep_);
    }
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
