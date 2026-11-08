/*
 * Reading and writing another process's memory from a driver thread. Compiled with Apple's headers only.
 *
 * Linux drivers do this with kthread_use_mm() followed by copy_to_user(): the worker borrows the process's address
 * space. xnu has no such borrowing for a kext, so the port remembers each mm's Mach task (recorded on the process's
 * own thread when the mm is created) and copies through an IOMemoryDescriptor over the user range in that task,
 * which wires the pages for the duration of the copy. When the caller is already that process, plain copyin/copyout
 * is used. The record holds a reference on the task until the mm is freed (an mm can outlive its process: a userptr
 * BO keeps the task_struct and so the mm), and each use takes its own under the lock, so a task that exited is never
 * used after xnu freed it.
 */
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include "nmx_xnu.h"

/* Step132 pin counters (debug.nmx_pin_*, registered in nmx_kext.cpp) */
extern "C" {
volatile SInt64 gNmxPinWriteFail, gNmxPinWriteRetryOk, gNmxPinRoRetry, gNmxPinLastRc, gNmxPinLastRc2;
volatile SInt64 gNmxPinOtherFail, gNmxPinOtherRc;
char gNmxPinOtherName[32];
/* already imported by NMXGPU (no new kernel symbols, Step128 rule); declared here to keep sys/proc.h out */
struct proc;
struct proc *current_proc(void);
int proc_pid(struct proc *);
void proc_name(int pid, char *buf, int size);
}
static volatile SInt32 gNmxPinSay, gNmxPinSayWs;

namespace {
struct MmTask {
    void *mm;
    task_t task;
    MmTask *next;
};
MmTask *gList;
IOLock *gLock;

IOLock *lock()
{
    if (gLock == nullptr) {
        IOLock *l = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gLock))
            IOLockFree(l);
    }
    return gLock;
}

/* the mm's task with a reference the caller drops (task_deallocate), or null */
task_t find_ref(void *mm)
{
    task_t task = nullptr;
    IOLockLock(lock());
    for (MmTask *e = gList; e != nullptr; e = e->next)
        if (e->mm == mm) {
            task = e->task;
            task_reference(task);
            break;
        }
    IOLockUnlock(gLock);
    return task;
}
}

extern "C" void nmx_xnu_mm_note_task(void *mm)
{
    MmTask *e = IONew(MmTask, 1);
    if (e == nullptr)
        return;     /* the copy helpers then report EFAULT for this process: nothing is written blindly */
    e->mm = mm;
    e->task = current_task();
    task_reference(e->task);
    IOLockLock(lock());
    e->next = gList;
    gList = e;
    IOLockUnlock(gLock);
}

extern "C" void nmx_xnu_mm_forget(void *mm)
{
    IOLockLock(lock());
    for (MmTask **pp = &gList; *pp != nullptr; pp = &(*pp)->next) {
        if ((*pp)->mm == mm) {
            MmTask *e = *pp;
            *pp = e->next;
            IOLockUnlock(gLock);
            task_deallocate(e->task);
            IODelete(e, MmTask, 1);
            return;
        }
    }
    IOLockUnlock(gLock);
}

extern "C" int nmx_xnu_mm_copy(void *mm, uint64_t uaddr, void *kbuf, size_t len, int to_user)
{
    if (len == 0)
        return 0;
    task_t task = find_ref(mm);
    if (task == nullptr)
        return EFAULT;

    if (task == current_task()) {
        task_deallocate(task);
        return to_user ? copyout(kbuf, (user_addr_t)uaddr, len) : copyin((user_addr_t)uaddr, kbuf, len);
    }

    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)uaddr, len,
                                                                  kIODirectionOutIn, task);
    task_deallocate(task);
    if (md == nullptr)
        return ENOMEM;
    if (md->prepare() != kIOReturnSuccess) {
        md->release();
        return EFAULT;
    }
    IOByteCount done = to_user ? md->writeBytes(0, kbuf, len) : md->readBytes(0, kbuf, len);
    md->complete();
    md->release();
    return done == len ? 0 : EFAULT;
}

/*
 * ga403wm Step137: IOSurface imports pin the surface's own memory.
 *
 * NVMTL imports every IOSurface it wraps as a userptr BO over the process's own mapping of the surface. Wiring that
 * range faults each page into the process's map first: a fresh mapping (every process, every re-import) costs one
 * VM fault per page in user space (NVMTL's write prefault) and again in the wire, ~7 % of a core in WindowServer
 * while an app animates (docs/CPU-THERMALS.md, 2026-11-08). The pages are already the IOSurface's: before the import
 * the plugin says which surface the range is (DRM_AMDGPU_NMX_SURFACE_HINT), and the wire then prepares the surface's
 * memory descriptor (IOSurfaceRoot::lookupSurface for the calling task, so only a surface that task may use) and
 * takes its pages, without touching the process's map.
 *
 * The claim is checked, not trusted: one page at the start of the range is wired through the process's map (with
 * the access asked for, so a read-only mapping still refuses a writable pin) and must be the surface's first page.
 * Any mismatch, unknown surface or short descriptor falls back to the plain user wire below, as before Step137.
 * Hints are kept per task in a small ring and expire after 2 s; a stale one fails the page check.
 * Boot-arg nmxsurfwire=0 turns the surface path off.
 */
extern "C" {
volatile SInt64 gNmxSurfHint, gNmxSurfWire, gNmxSurfMiss, gNmxSurfBadPage, gNmxSurfNoSurf, gNmxSurfShort, gNmxSurfPrepFail;
/* exported by com.apple.iokit.IOSurface; already imported by NMXAccel (Step128 rule: no new kernel symbols) */
void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *root, unsigned int id, task_t task);
void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface);
}
namespace {
struct SurfHint {
    task_t task;            /* compared only, no reference held */
    uint64_t addr, size;
    uint32_t id;
    uint64_t when;          /* mach_absolute_time */
};
constexpr unsigned kSurfHints = 64;
SurfHint gHints[kSurfHints];
unsigned gHintNext;
IOLock *gHintLock;
IOService *gSurfRoot;
int gSurfOn = -1;

bool surf_on()
{
    if (gSurfOn < 0) {
        int v = 1;
        if (PE_parse_boot_argn("nmxsurfwire", &v, sizeof v) && v == 0)
            gSurfOn = 0;
        else
            gSurfOn = 1;
    }
    return gSurfOn != 0;
}

IOLock *hint_lock()
{
    if (gHintLock == nullptr) {
        IOLock *l = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gHintLock))
            IOLockFree(l);
    }
    return gHintLock;
}

/* the hinted surface for exactly this range of this task, or 0. `take` clears the hint (after a successful pin): a
 * failed attempt keeps it for the read-only retry (Step96), which is how WindowServer's r-- client surfaces arrive. */
uint32_t hint_find(task_t task, uint64_t addr, uint64_t size, bool take)
{
    uint64_t now = mach_absolute_time(), limit;
    nanoseconds_to_absolutetime(2ULL * 1000 * 1000 * 1000, &limit);
    uint32_t id = 0;
    IOLockLock(hint_lock());
    for (unsigned i = 0; i < kSurfHints; i++) {
        SurfHint &h = gHints[i];
        if (h.task == task && h.addr == addr && h.size == size && h.id != 0 && now - h.when <= limit) {
            id = h.id;
            if (take)
                h.id = 0;   /* one import per hint */
            break;
        }
    }
    IOLockUnlock(gHintLock);
    return id;
}

IOService *surf_root()
{
    if (gSurfRoot == nullptr) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m == nullptr)
            return nullptr;
        IOService *r = IOService::copyMatchingService(m);    /* retained */
        m->release();
        if (r != nullptr && !OSCompareAndSwapPtr(nullptr, r, (void * volatile *)&gSurfRoot))
            r->release();
    }
    return gSurfRoot;
}

/* 0 and the prepared surface descriptor in *handle, or an errno (the caller then wires the user range) */
/* Step138: user-memory wirings held for the GPU right now (descriptors and their bytes): debug.nmx_userwire_* */
extern "C" volatile SInt64 gNmxUserWires, gNmxUserWirePages;
volatile SInt64 gNmxUserWires, gNmxUserWirePages;
static void wire_note(IOMemoryDescriptor *md, int dir)
{
    SInt64 pages = (SInt64)((md->getLength() + PAGE_SIZE - 1) / PAGE_SIZE);
    __atomic_add_fetch(&gNmxUserWires, dir, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gNmxUserWirePages, dir * pages, __ATOMIC_RELAXED);
}

int surf_wire(task_t task, uint32_t id, uint64_t uaddr, size_t npages, int write, uint64_t *phys, void **handle)
{
    IOService *root = surf_root();
    if (root == nullptr)
        return ENODEV;
    OSObject *surf = (OSObject *)_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(root, id, task);     /* retained */
    if (surf == nullptr) {
        __atomic_add_fetch(&gNmxSurfNoSurf, 1, __ATOMIC_RELAXED);
        return ENOENT;
    }
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    if (md != nullptr)
        md->retain();       /* the BO keeps the memory, not the surface object */
    surf->release();
    if (md == nullptr || md->getLength() < npages * PAGE_SIZE) {
        __atomic_add_fetch(&gNmxSurfShort, 1, __ATOMIC_RELAXED);
        if (md != nullptr)
            md->release();
        return EINVAL;
    }
    if (md->prepare() != kIOReturnSuccess) {
        __atomic_add_fetch(&gNmxSurfPrepFail, 1, __ATOMIC_RELAXED);
        md->release();
        return EFAULT;
    }
    /* the check: the range's first page, wired through the process's map with the access asked for */
    IOMemoryDescriptor *one = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)uaddr, PAGE_SIZE,
                                                                   write ? kIODirectionOutIn : kIODirectionOut, task);
    bool same = false;
    if (one != nullptr) {
        if (one->prepare() == kIOReturnSuccess) {
            IOByteCount l1 = 0, l2 = 0;
            addr64_t a = one->getPhysicalSegment(0, &l1, kIOMemoryMapperNone);
            addr64_t b = md->getPhysicalSegment(0, &l2, kIOMemoryMapperNone);
            same = a != 0 && a == b;
            one->complete();
        }
        one->release();
    }
    if (!same) {
        __atomic_add_fetch(&gNmxSurfBadPage, 1, __ATOMIC_RELAXED);
        md->complete();
        md->release();
        return EPERM;
    }
    for (size_t i = 0; i < npages; i++) {
        IOByteCount len = 0;
        addr64_t pa = md->getPhysicalSegment(i * PAGE_SIZE, &len, kIOMemoryMapperNone);
        if (pa == 0) {
            md->complete();
            md->release();
            return EFAULT;
        }
        phys[i] = pa;
    }
    wire_note(md, 1);
    *handle = md;           /* nmx_xnu_user_unwire: complete + release, as for a user range */
    return 0;
}
}

extern "C" int nmx_xnu_surface_hint(uint32_t surface_id, uint64_t addr, uint64_t size)
{
    if (!surf_on())
        return ENOTSUP;
    if (surface_id == 0)
        return EINVAL;
    task_t task = current_task();
    IOLockLock(hint_lock());
    unsigned slot = gHintNext++ % kSurfHints;
    for (unsigned i = 0; i < kSurfHints; i++)       /* the same range again replaces its hint */
        if (gHints[i].task == task && gHints[i].addr == addr) {
            slot = i;
            break;
        }
    gHints[slot] = SurfHint{ task, addr, size, surface_id, mach_absolute_time() };
    IOLockUnlock(gHintLock);
    __atomic_add_fetch(&gNmxSurfHint, 1, __ATOMIC_RELAXED);
    return 0;
}

extern "C" int nmx_xnu_user_wire(void *mm, uint64_t uaddr, size_t npages, int write, uint64_t *phys, void **handle)
{
    *handle = nullptr;
    if (npages == 0)
        return 0;
    if ((uaddr & PAGE_MASK) != 0)
        return EINVAL;
    task_t task = current_task();
    if (mm != nullptr) {
        task = find_ref(mm);
        if (task == nullptr)
            return EFAULT;
    }
    if (surf_on()) {
        uint32_t sid = hint_find(task, uaddr, (uint64_t)npages * PAGE_SIZE, false);
        if (sid != 0) {
            int sr = surf_wire(task, sid, uaddr, npages, write, phys, handle);
            if (sr == 0) {
                (void)hint_find(task, uaddr, (uint64_t)npages * PAGE_SIZE, true);
                __atomic_add_fetch(&gNmxSurfWire, 1, __ATOMIC_RELAXED);
                if (mm != nullptr)
                    task_deallocate(task);
                return 0;
            }
            __atomic_add_fetch(&gNmxSurfMiss, 1, __ATOMIC_RELAXED);    /* the plain wire below */
        }
    }
    /* the device writes the pages when the mapping is writable (kIODirectionIn), and always reads them; the BO that
     * keeps this wiring keeps the mm, whose record keeps the task */
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)uaddr, npages * PAGE_SIZE,
                                                                  write ? kIODirectionOutIn : kIODirectionOut, task);
    if (md == nullptr) {
        if (mm != nullptr)
            task_deallocate(task);
        return ENOMEM;
    }
    IOReturn rc = md->prepare();
    if (rc != kIOReturnSuccess && write) {
        /* ga403wm Step132: a writable wire of a fresh IOSurface (none of its pages resident yet) failed, and the
         * userptr ioctl then fell back to a read-only pin (Step96) of memory the GPU is asked to write: the write
         * faulted (PERMISSION_FAULTS 0x5) and the gfx ring hung for the 3 s timeout (mediaanalysisd, 12:42). Wire the
         * range read-only once to fault it in, then wire it for write again; only a second failure goes on to the
         * read-only fallback. The return codes are kept in debug.nmx_pin_* so the cause stays visible.
         * Step132b: the 14:23 boot showed every WindowServer failure is KERN_PROTECTION_FAILURE (2), which a fault-in
         * cannot change, so that code skips the retry; failures outside WindowServer are kept apart (count, code,
         * process) so WindowServer's hundreds of read-only client imports do not hide them. */
        __atomic_add_fetch(&gNmxPinWriteFail, 1, __ATOMIC_RELAXED);
        gNmxPinLastRc = (SInt64)(uint32_t)rc;
        char who[32] = "";
        proc_name(proc_pid(current_proc()), who, (int)sizeof who);
        bool ws = strncmp(who, "WindowServer", 12) == 0;
        IOReturn first = rc;
        if (rc != KERN_PROTECTION_FAILURE) {
            IOMemoryDescriptor *rd = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)uaddr,
                                                                          npages * PAGE_SIZE, kIODirectionOut, task);
            if (rd != nullptr) {
                if (rd->prepare() == kIOReturnSuccess)
                    rd->complete();
                rd->release();
            }
            rc = md->prepare();
            if (rc == kIOReturnSuccess)
                __atomic_add_fetch(&gNmxPinWriteRetryOk, 1, __ATOMIC_RELAXED);
            else
                gNmxPinLastRc2 = (SInt64)(uint32_t)rc;
        }
        if (!ws) {
            __atomic_add_fetch(&gNmxPinOtherFail, 1, __ATOMIC_RELAXED);
            gNmxPinOtherRc = (SInt64)(uint32_t)first;
            strlcpy(gNmxPinOtherName, who, sizeof gNmxPinOtherName);
        }
        if (__atomic_add_fetch(ws ? &gNmxPinSayWs : &gNmxPinSay, 1, __ATOMIC_RELAXED) <= (ws ? 4 : 24))
            IOLog("NMX pin: %s write wire of %llx (%zu pages) failed %#x%s, %s %#x\n", who,
                  (unsigned long long)uaddr, npages, (unsigned)first,
                  first == KERN_PROTECTION_FAILURE ? " (protection, no retry)" : "",
                  rc == kIOReturnSuccess ? "retry ok" : "now", (unsigned)rc);
    }
    if (mm != nullptr)
        task_deallocate(task);
    if (rc != kIOReturnSuccess) {
        md->release();
        return EFAULT;
    }
    for (size_t i = 0; i < npages; i++) {
        IOByteCount len = 0;
        addr64_t pa = md->getPhysicalSegment(i * PAGE_SIZE, &len, kIOMemoryMapperNone);
        if (pa == 0) {
            md->complete();
            md->release();
            return EFAULT;
        }
        phys[i] = pa;
    }
    wire_note(md, 1);
    *handle = md;
    return 0;
}

extern "C" void nmx_xnu_user_unwire(void *handle)
{
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)handle;
    if (md == nullptr)
        return;
    wire_note(md, -1);
    md->complete();
    md->release();
}
