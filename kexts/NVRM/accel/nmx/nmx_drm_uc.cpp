/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * NMXDrmClient: the DRM render node of NMXGPU reached through an IOKit connection on the accelerator, for processes
 * whose sandbox refuses /dev/dri (App Sandbox, browser GPU processes: "deny file-read-data /dev/dri"). The sandbox
 * lets those processes open user clients of an IOAccelerator (system.sb system-graphics), so NMXAccel hands them this
 * client (IOServiceOpen type NMX_DRM_UC_TYPE) and the client opens the node for them:
 *   - the node is opened on a kernel thread, with the kernel's vfs context, so the caller's sandbox is not asked;
 *     the open makes a devfs clone of its own, i.e. a DRM file of its own, exactly like open(2) of the node.
 *   - every ioctl is VNOP_IOCTL on that vnode from the caller's own thread, so NMXGPU sees the caller as current
 *     (GPU memory mappings land in the caller's task, user pointers inside ioctl arguments are the caller's).
 *   - close (or the process dying) closes the vnode: the DRM file is released as on close(2).
 * The userland side is the NVMTL plugin's nvmtl_drm_xnu.c (the selectors below must stay identical).
 */
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <kern/clock.h>
#include <string.h>
#include <libkern/OSAtomic.h>
#include "nmx_fence_table.h"

extern "C" {
typedef void (*nmx_thread_fn)(void *param, int wait_result);
kern_return_t kernel_thread_start(nmx_thread_fn continuation, void *parameter, thread_t *new_thread);
void thread_deallocate(thread_t thread);
}

#define NMX_DRM_LOG(fmt, ...) do { kprintf("NMXAccel: drm-uc: " fmt "\n", ##__VA_ARGS__); \
                                   IOLog("NMXAccel: drm-uc: " fmt "\n", ##__VA_ARGS__); } while (0)

enum {
    kNmxDrmSelOpen  = 0,   /* in: node minor (128 = renderD128); out: errno, dev major, dev minor of the open */
    kNmxDrmSelIoctl = 1,   /* in: cmd; struct in/out: the ioctl argument (IOCPARM_LEN(cmd) bytes); out: errno */
    kNmxDrmSelStat  = 2,   /* in: node minor; out: errno, dev major, node minor (stat(2) of the node, no DRM file) */
    kNmxDrmSelFence = 3,   /* struct in: nmx_fence_call_v1 (nmx_fence_table.h); no output. debug.nmxaccel_fence_table */
    kNmxDrmSelCount
};


/* ---- fence table (nmx_fence_table.h) ----
 * 64 rows keyed by IOSurface ID. One spin lock, held only inside nmx_fence_table_get and the selector's apply step:
 * no IOSurface (or any other) call is made with it held. A row is owned by the client that last wrote it; clientClose
 * clears the rows it owns. A new surface takes a free row, else the least recently written row that is not in flight
 * (Done >= Pending, or no Pending); when every row is in flight the call is refused and nothing is written. */
int nmx_fence_table_on = 0;
int64_t nmx_fence_table_stale, nmx_fence_table_denied;
int64_t nmx_fence_table_calls, nmx_fence_table_entries, nmx_fence_table_refused, nmx_fence_table_bad,
        nmx_fence_table_hits, nmx_fence_table_misses, nmx_fence_table_evicted, nmx_fence_table_cleared;
#define NMX_FT_ROWS 64
struct NmxFenceRow {
    uint32_t sid;              /* 0 = free */
    uint32_t valid;            /* bit k: v[k] was written through the table */
    const void *owner;
    uint64_t tick;             /* last write, for eviction */
    uint64_t when;             /* mach_absolute_time of the last write: an in-flight row older than 2 s is stale */
    unsigned long long v[kNmxFkCount];
};
static NmxFenceRow gFt[NMX_FT_ROWS];
static uint64_t gFtTick;
static IOSimpleLock *volatile gFtLock;
static IOSimpleLock *nmxFtLock(void)
{
    IOSimpleLock *l = gFtLock;
    if (l) return l;
    IOSimpleLock *n = IOSimpleLockAlloc();
    if (!n) return nullptr;
    if (!OSCompareAndSwapPtr(nullptr, n, (void *volatile *)&gFtLock)) IOSimpleLockFree(n);
    return gFtLock;
}
static inline bool nmxFtBusy(const NmxFenceRow &r)   /* in flight: a Pending the row has not seen Done for */
{
    if (!(r.valid & (1u << kNmxFkPending))) return false;
    return !(r.valid & (1u << kNmxFkDone)) || r.v[kNmxFkDone] < r.v[kNmxFkPending];
}
extern "C" int nmx_fence_key_index(const char *key)
{
    if (!key || strncmp(key, "NMXFence", 8) != 0) return -1;
    const char *k = key + 8;
    if (!strcmp(k, "Pending")) return kNmxFkPending;
    if (!strcmp(k, "PendingT")) return kNmxFkPendingT;
    if (!strcmp(k, "Submitted")) return kNmxFkSubmitted;
    if (!strcmp(k, "Done")) return kNmxFkDone;
    if (!strcmp(k, "DoneT")) return kNmxFkDoneT;
    if (!strcmp(k, "GpuEndT")) return kNmxFkGpuEndT;
    return -1;
}
extern "C" bool nmx_fence_table_get(uint32_t sid, int ki, unsigned long long *v)
{
    if (!nmx_fence_table_on || !sid || ki < 0 || ki >= kNmxFkCount) return false;
    IOSimpleLock *l = gFtLock;
    if (!l) return false;
    bool hit = false;
    IOSimpleLockLock(l);
    for (int i = 0; i < NMX_FT_ROWS; i++)
        if (gFt[i].sid == sid) {
            if (gFt[i].valid & (1u << ki)) { *v = gFt[i].v[ki]; hit = true; }
            break;
        }
    IOSimpleLockUnlock(l);
    OSAddAtomic64(1, hit ? &nmx_fence_table_hits : &nmx_fence_table_misses);
    return hit;
}
static bool nmxFtFlagsOk(uint32_t f)
{
    return f == NMX_FENCE_F_PENDING || f == (NMX_FENCE_F_PENDING | NMX_FENCE_F_SUBMITTED) || f == NMX_FENCE_F_SUBMITTED ||
           f == NMX_FENCE_F_DONE;
}
/* the call's entries, already validated and copied to kernel memory: all applied, or none (false = no row free).
 * Rows are chosen first without touching the table, then written:
 *   1. every entry whose surface already has a row claims it (a bitmap of the rows taken),
 *   2. then the others, in order, get a free row, else the least recently written row that is not in flight, else the
 *      oldest in-flight row not written for 2 s (stale: its surface was freed, or its Done was lost), never a row
 *      step 1 claimed. Claiming first matters: a new surface must not evict the row of a surface later in the same call
 *      (that entry would then land on the new surface's row and overwrite it, losing its Pending). */
static bool nmxFtApply(const nmx_fence_entry_v1 *e, uint32_t n, const void *owner)
{
    IOSimpleLock *l = nmxFtLock();
    if (!l) return false;
    static uint64_t staleAbs;
    if (!staleAbs) nanoseconds_to_absolutetime(2000000000ull, &staleAbs);
    const uint64_t now = mach_absolute_time();
    int slot[NMX_FENCE_CALL_MAX];
    uint64_t taken = 0, staleRows = 0;   /* staleRows: rows step 2 took as stale (counted there, not as evicted) */
    uint32_t evicted = 0, stale = 0;
    bool ok = true;
    IOSimpleLockLock(l);
    for (uint32_t j = 0; j < n; j++) {                       /* 1: surfaces that have a row */
        slot[j] = -1;
        for (int i = 0; i < NMX_FT_ROWS; i++)
            if (gFt[i].sid == e[j].surfaceID) { slot[j] = i; taken |= 1ull << i; break; }
    }
    for (uint32_t j = 0; j < n && ok; j++) {                 /* 2: the others */
        if (slot[j] >= 0) continue;
        for (uint32_t p = 0; p < j && slot[j] < 0; p++) if (e[p].surfaceID == e[j].surfaceID) slot[j] = slot[p];
        if (slot[j] >= 0) continue;
        int freeAt = -1, evictAt = -1, staleAt = -1;
        for (int i = 0; i < NMX_FT_ROWS; i++) {
            if (taken & (1ull << i)) continue;
            if (!gFt[i].sid) { freeAt = i; break; }
            if (!nmxFtBusy(gFt[i])) { if (evictAt < 0 || gFt[i].tick < gFt[evictAt].tick) evictAt = i; }
            else if (now - gFt[i].when > staleAbs && (staleAt < 0 || gFt[i].tick < gFt[staleAt].tick)) staleAt = i;
        }
        const int at = freeAt >= 0 ? freeAt : evictAt >= 0 ? evictAt : staleAt;
        if (at < 0) { ok = false; break; }
        if (at == staleAt && freeAt < 0 && evictAt < 0) { stale++; staleRows |= 1ull << at; }
        slot[j] = at; taken |= 1ull << at;
    }
    if (ok) {
        const uint64_t tick = ++gFtTick;
        for (uint32_t j = 0; j < n; j++) {
            NmxFenceRow &r = gFt[slot[j]];
            if (r.sid != e[j].surfaceID) {
                if (r.sid && !(staleRows & (1ull << slot[j]))) evicted++;
                bzero(&r, sizeof r);
                r.sid = e[j].surfaceID;
            }
            const uint32_t f = e[j].flags;
            if (f & NMX_FENCE_F_PENDING) {
                r.v[kNmxFkPending] = e[j].seq; r.v[kNmxFkPendingT] = e[j].tNs;
                r.valid |= (1u << kNmxFkPending) | (1u << kNmxFkPendingT);
            }
            if (f & NMX_FENCE_F_SUBMITTED) { r.v[kNmxFkSubmitted] = e[j].seq; r.valid |= 1u << kNmxFkSubmitted; }
            if (f & NMX_FENCE_F_DONE) {
                r.v[kNmxFkDone] = e[j].seq; r.v[kNmxFkDoneT] = e[j].tNs; r.v[kNmxFkGpuEndT] = e[j].gpuEndNs;
                r.valid |= (1u << kNmxFkDone) | (1u << kNmxFkDoneT) | (1u << kNmxFkGpuEndT);
            }
            r.owner = owner;
            r.tick = tick;
            r.when = now;
        }
    } else {
        stale = 0;
    }
    IOSimpleLockUnlock(l);
    if (evicted) OSAddAtomic64(evicted, &nmx_fence_table_evicted);
    if (stale) OSAddAtomic64(stale, &nmx_fence_table_stale);
    return ok;
}
extern "C" void nmx_fence_table_set(int on)
{
    IOSimpleLock *l = nmxFtLock();
    if (l) IOSimpleLockLock(l);
    nmx_fence_table_on = on;
    bzero(gFt, sizeof gFt);
    if (l) IOSimpleLockUnlock(l);
}
/* a refused call: drop the rows of its surfaces (whoever wrote them), so none can shadow the property values the caller
 * writes for them instead; the caller's other rows stay valid */
static void nmxFtClearSurfaces(const nmx_fence_entry_v1 *e, uint32_t n)
{
    IOSimpleLock *l = gFtLock;
    if (!l) return;
    uint32_t cleared = 0;
    IOSimpleLockLock(l);
    for (int i = 0; i < NMX_FT_ROWS; i++) {
        if (!gFt[i].sid) continue;
        for (uint32_t j = 0; j < n; j++)
            if (gFt[i].sid == e[j].surfaceID) { bzero(&gFt[i], sizeof gFt[i]); cleared++; break; }
    }
    IOSimpleLockUnlock(l);
    if (cleared) OSAddAtomic64(cleared, &nmx_fence_table_cleared);
}
static void nmxFtClearOwner(const void *owner)
{
    IOSimpleLock *l = gFtLock;
    if (!l) return;
    uint32_t n = 0;
    IOSimpleLockLock(l);
    for (int i = 0; i < NMX_FT_ROWS; i++) if (gFt[i].sid && gFt[i].owner == owner) { bzero(&gFt[i], sizeof gFt[i]); n++; }
    IOSimpleLockUnlock(l);
    if (n) OSAddAtomic64(n, &nmx_fence_table_cleared);
}

class NMXDrmClient : public IOUserClient {
    OSDeclareDefaultStructors(NMXDrmClient)
    task_t   fTask = nullptr;
    vnode_t  fVp = nullptr;
    IORWLock *fLock = nullptr;
    int      fMajor = -1, fMinor = -1;
    bool     fFenceWs = false;           /* opened by WindowServer (uid 88 _windowserver, name WindowServer) */
    static volatile SInt32 sOpens;
    static volatile SInt32 sMajor;           /* NMXGPU's devfs major, learnt from the first open (-1 = not yet) */
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE {
        if (!IOUserClient::initWithTask(owningTask, securityID, type)) return false;
        fTask = owningTask;
        /* IOServiceOpen runs on the opening thread: current proc/cred are the client's */
        char name[32] = "";
        proc_selfname(name, (int)sizeof name);
        fFenceWs = owningTask == current_task() && kauth_getuid() == 88 && strcmp(name, "WindowServer") == 0;
        fLock = IORWLockAlloc();
        return fLock != nullptr;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fLock) { IORWLockFree(fLock); fLock = nullptr; }
        IOUserClient::free();
    }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {
        nmxFtClearOwner(this);
        closeNode("close");
        if (!isInactive()) terminate();
        return kIOReturnSuccess;
    }
    IOReturn clientDied() APPLE_KEXT_OVERRIDE { return clientClose(); }

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *,
                            OSObject *, void *) APPLE_KEXT_OVERRIDE {
        switch (selector) {
        case kNmxDrmSelOpen:  return selOpen(a);
        case kNmxDrmSelIoctl: return selIoctl(a);
        case kNmxDrmSelStat:  return selStat(a);
        case kNmxDrmSelFence: return selFence(a);
        default: return kIOReturnBadArgument;
        }
    }

private:
    struct OpenJob {
        char path[64];
        int fmode;               /* 0: lookup only (stat), the major is read and the vnode let go */
        int major;
        vnode_t vp;
        int err;
        bool done;
        IOLock *lock;
    };
    /* runs on a kernel thread: vfs_context_current() is the kernel's context there */
    static void openThread(void *param, int) {
        OpenJob *j = (OpenJob *)param;
        vnode_t vp = nullptr;
        vfs_context_t ctx = vfs_context_current();
        int err;
        if (j->fmode == 0) {
            err = vnode_lookup(j->path, 0, &vp, ctx);
            if (err == 0) { j->major = (int)major((uint32_t)vnode_specrdev(vp)); vnode_put(vp); vp = nullptr; }
        } else {
            err = vnode_open(j->path, j->fmode, 0, 0, &vp, ctx);
            if (err == 0) vnode_put(vp);   /* keep the open's use count; iocounts are taken per call */
        }
        IOLockLock(j->lock);
        j->vp = err ? nullptr : vp;
        j->err = err;
        j->done = true;
        IOLockWakeup(j->lock, j, false);
        IOLockUnlock(j->lock);
    }

    /* run j on a kernel thread and wait for it; false when no thread could be started */
    static bool runJob(OpenJob *j) {
        j->lock = IOLockAlloc();
        if (!j->lock) return false;
        thread_t thr = nullptr;
        IOLockLock(j->lock);
        if (kernel_thread_start(openThread, j, &thr) != KERN_SUCCESS) {
            IOLockUnlock(j->lock); IOLockFree(j->lock); j->lock = nullptr;
            return false;
        }
        while (!j->done) IOLockSleep(j->lock, j, THREAD_UNINT);
        IOLockUnlock(j->lock);
        thread_deallocate(thr);
        IOLockFree(j->lock); j->lock = nullptr;
        return true;
    }
    static void nodePath(char *buf, size_t n, uint64_t node) {
        snprintf(buf, n, node >= 128 ? "/dev/dri/renderD%u" : "/dev/dri/card%u", (unsigned)node);
    }

    /* the devfs node's identity without opening it (one lookup per boot; later answers come from sMajor) */
    IOReturn selStat(IOExternalMethodArguments *a) {
        if (a->scalarInputCount < 1 || a->scalarOutputCount < 3) return kIOReturnBadArgument;
        uint64_t node = a->scalarInput[0];
        if (node > 255) return kIOReturnBadArgument;
        int err = 0;
        if (sMajor < 0) {
            OpenJob *j = IONew(OpenJob, 1);
            if (!j) return kIOReturnNoMemory;
            bzero(j, sizeof(*j));
            nodePath(j->path, sizeof(j->path), node);
            j->fmode = 0;
            if (!runJob(j)) { IODelete(j, OpenJob, 1); return kIOReturnNoResources; }
            err = j->err;
            if (err == 0) sMajor = j->major;
            IODelete(j, OpenJob, 1);
        }
        a->scalarOutput[0] = (uint64_t)err;
        a->scalarOutput[1] = err ? 0 : (uint64_t)sMajor;
        a->scalarOutput[2] = err ? 0 : node;
        return kIOReturnSuccess;
    }

    IOReturn selOpen(IOExternalMethodArguments *a) {
        if (a->scalarInputCount < 1 || a->scalarOutputCount < 3) return kIOReturnBadArgument;
        uint64_t node = a->scalarInput[0];
        if (node > 255) return kIOReturnBadArgument;
        IORWLockWrite(fLock);
        if (fVp) {                           /* one DRM file per connection */
            IORWLockUnlock(fLock);
            a->scalarOutput[0] = EBUSY; a->scalarOutput[1] = 0; a->scalarOutput[2] = 0;
            return kIOReturnSuccess;
        }
        OpenJob *j = IONew(OpenJob, 1);
        if (!j) { IORWLockUnlock(fLock); return kIOReturnNoMemory; }
        bzero(j, sizeof(*j));
        nodePath(j->path, sizeof(j->path), node);
        j->fmode = FREAD | FWRITE;
        if (!runJob(j)) { IODelete(j, OpenJob, 1); IORWLockUnlock(fLock); return kIOReturnNoResources; }
        int err = j->err;
        if (err == 0) {
            fVp = j->vp;
            dev_t d = vnode_specrdev(fVp);
            fMajor = (int)major((uint32_t)d); fMinor = (int)minor((uint32_t)d);
            sMajor = fMajor;
            OSIncrementAtomic(&sOpens);
        }
        char path[64]; strlcpy(path, j->path, sizeof(path));
        IODelete(j, OpenJob, 1);
        IORWLockUnlock(fLock);
        if (err == 0) {
            if (sOpens <= 64)
                NMX_DRM_LOG("pid %d opened %s for its sandbox (dev %d,0x%x, %d opens so far)",
                            proc_selfpid(), path, fMajor, fMinor, (int)sOpens);
        } else {
            NMX_DRM_LOG("pid %d: %s open failed (%d)", proc_selfpid(), path, err);
        }
        a->scalarOutput[0] = (uint64_t)err;
        a->scalarOutput[1] = err ? 0 : (uint64_t)fMajor;
        a->scalarOutput[2] = err ? 0 : (uint64_t)fMinor;
        return kIOReturnSuccess;
    }

    IOReturn selIoctl(IOExternalMethodArguments *a) {
        if (a->scalarInputCount < 1 || a->scalarOutputCount < 1) return kIOReturnBadArgument;
        u_long cmd = (u_long)a->scalarInput[0];
        uint32_t len = IOCPARM_LEN(cmd);
        if (!(cmd & (IOC_IN | IOC_OUT))) len = 0;
        if (len > 4096) return kIOReturnBadArgument;        /* inline structures only; DRM ioctls are far smaller */
        if ((cmd & IOC_IN) && a->structureInputSize < len) return kIOReturnBadArgument;
        if (len && a->structureOutputSize < len) return kIOReturnBadArgument;
        uint8_t *data = len ? (uint8_t *)a->structureOutput : nullptr;
        if (len) {
            if (cmd & IOC_IN) memcpy(data, a->structureInput, len);
            else bzero(data, len);
        }
        IORWLockRead(fLock);
        vnode_t vp = fVp;
        int err;
        if (!vp) err = EBADF;
        else if (vnode_getwithref(vp) != 0) err = EBADF;
        else {
            err = VNOP_IOCTL(vp, cmd, (caddr_t)data, FREAD | FWRITE, vfs_context_current());
            vnode_put(vp);
        }
        IORWLockUnlock(fLock);
        a->structureOutputSize = len;
        a->scalarOutput[0] = (uint64_t)err;
        return kIOReturnSuccess;
    }

    /* fence marks for up to 16 surfaces (nmx_fence_table.h). Strict: exact size for the version, n 1..16, known flag
     * combinations only, no scalars, no struct output, no out-of-line input. */
    IOReturn selFence(IOExternalMethodArguments *a) {
        if (!nmx_fence_table_on) return kIOReturnUnsupported;
        /* only WindowServer may write fence rows (any other process could make WindowServer's frames wait or flip
         * early); gate 2 lets any process call, for own-process tests only */
        if (nmx_fence_table_on != 2 && !fFenceWs) { OSAddAtomic64(1, &nmx_fence_table_denied); return kIOReturnNotPrivileged; }
        /* the connection's own task only (fFenceWs was decided for it; a port handed to another task must not inherit it) */
        if (current_task() != fTask) { OSAddAtomic64(1, &nmx_fence_table_denied); return kIOReturnNotPrivileged; }
        OSAddAtomic64(1, &nmx_fence_table_calls);
        const uint32_t sz = a->structureInputSize;
        if (a->scalarInputCount || a->scalarOutputCount || a->structureOutputSize || a->structureInputDescriptor ||
            a->structureOutputDescriptor || !a->structureInput || sz < 8 || sz > sizeof(nmx_fence_call_v1)) {
            OSAddAtomic64(1, &nmx_fence_table_bad); return kIOReturnBadArgument;
        }
        nmx_fence_call_v1 c;                     /* 520 bytes, copied once: no field is read twice from the caller */
        memcpy(&c, a->structureInput, sz);
        if (c.version != NMX_FENCE_CALL_VERSION || c.n < 1 || c.n > NMX_FENCE_CALL_MAX || sz != NMX_FENCE_CALL_SIZE(c.n)) {
            OSAddAtomic64(1, &nmx_fence_table_bad); return kIOReturnBadArgument;
        }
        for (uint32_t j = 0; j < c.n; j++)
            if (!c.e[j].surfaceID || !nmxFtFlagsOk(c.e[j].flags)) { OSAddAtomic64(1, &nmx_fence_table_bad); return kIOReturnBadArgument; }
        if (!nmxFtApply(c.e, c.n, this)) {
            /* Every row in flight. The caller writes these marks to IOSurface properties instead, so the rows of this
             * call's surfaces are dropped now: one left behind would shadow those property values. The NVMTL plugin also
             * pauses and closes its connection on this (clientClose then drops its other rows too, since all of its
             * marks go to properties while paused); dropping these here keeps a client that does not close correct. */
            nmxFtClearSurfaces(c.e, c.n);
            const SInt64 k = OSAddAtomic64(1, &nmx_fence_table_refused);
            if (k < 4) NMX_DRM_LOG("pid %d: fence table full (%d rows in flight), call refused, the rows of its surfaces dropped", proc_selfpid(), NMX_FT_ROWS);
            return kIOReturnNoSpace;
        }
        OSAddAtomic64((SInt64)c.n, &nmx_fence_table_entries);
        return kIOReturnSuccess;
    }

    void closeNode(const char *why) {
        if (!fLock) return;
        IORWLockWrite(fLock);
        vnode_t vp = fVp;
        fVp = nullptr;
        IORWLockUnlock(fLock);
        if (!vp) return;
        if (vnode_getwithref(vp) == 0)
            vnode_close(vp, FREAD | FWRITE, vfs_context_current());   /* drops the open's use count and our iocount */
        OSDecrementAtomic(&sOpens);
    }
};
volatile SInt32 NMXDrmClient::sOpens = 0;
volatile SInt32 NMXDrmClient::sMajor = -1;
OSDefineMetaClassAndStructors(NMXDrmClient, IOUserClient)

/* NMXAccel::newUserClient for type NMX_DRM_UC_TYPE */
extern "C" IOReturn nmx_drm_new_client(IOService *provider, task_t owningTask, void *securityID, UInt32 type,
                                       IOUserClient **handler)
{
    NMXDrmClient *uc = OSTypeAlloc(NMXDrmClient);
    if (!uc) return kIOReturnNoMemory;
    if (!uc->initWithTask(owningTask, securityID, type)) { uc->release(); return kIOReturnNoResources; }
    if (!uc->attach(provider)) { uc->release(); return kIOReturnInternalError; }
    if (!uc->start(provider)) { uc->detach(provider); uc->release(); return kIOReturnInternalError; }
    *handler = uc;
    return kIOReturnSuccess;
}
