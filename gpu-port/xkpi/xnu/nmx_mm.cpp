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

/* Step138: user-memory wirings held for the GPU right now (descriptors and their bytes): debug.nmx_userwire_* */
extern "C" volatile SInt64 gNmxUserWires, gNmxUserWirePages;
volatile SInt64 gNmxUserWires, gNmxUserWirePages;
/*
 * ga403wm Step142: every wiring is a record on one list (kind, owner process, age) instead of a bare descriptor.
 * - debug.nmx_userwire_by_kind splits the wired bytes by kind (IOSurface descriptor / plain user range) and process,
 *   and counts IOSurface wirings whose descriptor nobody but NMX holds any more (the surface itself is gone).
 * - An IOSurface wiring can be dropped while its BO sits idle and taken again when the GPU needs the BO
 *   (nmx_xnu_user_park / nmx_xnu_user_rewire): the descriptor is the surface's own memory, with no task behind it, so
 *   it can be prepared again at any time. The descriptor stays retained while parked, so the memory stays allocated;
 *   it is only pageable/compressible instead of wired. A plain user range is not parkable (re-wiring it needs the
 *   task's map).
 * The handle the callers keep is the record. Park, rewire and unwire of one handle are serialised by its owner (the
 * BO's reservation, or its destruction).
 */
extern "C" volatile SInt64 gNmxSurfWires, gNmxSurfWirePages, gNmxUserParked, gNmxUserParkedPages, gNmxUserParks,
    gNmxUserUnparks, gNmxUserUnparkFail;
volatile SInt64 gNmxSurfWires, gNmxSurfWirePages, gNmxUserParked, gNmxUserParkedPages, gNmxUserParks, gNmxUserUnparks,
    gNmxUserUnparkFail;
struct UserWire {
    IOMemoryDescriptor *md;
    UserWire *prev, *next;
    uint64_t when;          /* mach_absolute_time of the wire */
    uint32_t pages;         /* the descriptor's length in pages: what prepare() wires */
    uint8_t surface;        /* the IOSurface's own descriptor */
    uint8_t parked;         /* complete()d while the descriptor is still held */
    char who[22];           /* process that made the wiring */
};
static UserWire *gWires;
static IOLock *gWireLock;

static IOLock *wire_lock()
{
    if (gWireLock == nullptr) {
        IOLock *l = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gWireLock))
            IOLockFree(l);
    }
    return gWireLock;
}

/* the wired-now counters; parked records are counted apart */
static void wire_count(const UserWire *w, int dir)
{
    __atomic_add_fetch(&gNmxUserWires, dir, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gNmxUserWirePages, dir * (SInt64)w->pages, __ATOMIC_RELAXED);
    if (w->surface) {
        __atomic_add_fetch(&gNmxSurfWires, dir, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gNmxSurfWirePages, dir * (SInt64)w->pages, __ATOMIC_RELAXED);
    }
}

/* the record for a prepared descriptor, or nullptr (the caller then completes and releases it) */
static UserWire *wire_make(IOMemoryDescriptor *md, bool surface)
{
    IOLock *l = wire_lock();
    UserWire *w = l != nullptr ? (UserWire *)IOMalloc(sizeof(UserWire)) : nullptr;
    if (w == nullptr)
        return nullptr;
    *w = UserWire{};
    w->md = md;
    w->when = mach_absolute_time();
    w->pages = (uint32_t)((md->getLength() + PAGE_SIZE - 1) / PAGE_SIZE);
    w->surface = surface;
    proc_name(proc_pid(current_proc()), w->who, (int)sizeof w->who);
    IOLockLock(l);
    w->next = gWires;
    if (gWires != nullptr)
        gWires->prev = w;
    gWires = w;
    wire_count(w, 1);
    IOLockUnlock(l);
    return w;
}

/* 0 and the surface wiring's record in *handle, or an errno (the caller then wires the user range) */

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
    UserWire *w = wire_make(md, true);
    if (w == nullptr) {
        md->complete();
        md->release();
        return ENOMEM;
    }
    *handle = w;            /* nmx_xnu_user_unwire: complete + release, as for a user range */
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

/* Step139: who pins user pages, and how many (debug.nmx_pin_by_proc). One row per process name, 8 rows, the
 * least-used row is recycled. Counted at every nmx_xnu_user_wire call, before the surface/user split. */
struct PinAcct { char name[24]; SInt64 calls, pages; };
static PinAcct gPinAcct[8];
static IOLock *gPinAcctLock;
static void pin_account(size_t npages)
{
    char who[24] = "";
    proc_name(proc_pid(current_proc()), who, (int)sizeof who);
    if (gPinAcctLock == nullptr) {
        IOLock *l = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, l, (void * volatile *)&gPinAcctLock))
            IOLockFree(l);
    }
    if (gPinAcctLock == nullptr)
        return;
    IOLockLock(gPinAcctLock);
    int hit = -1, low = 0;
    for (int i = 0; i < 8; i++) {
        if (strncmp(gPinAcct[i].name, who, sizeof who) == 0) { hit = i; break; }
        if (gPinAcct[i].pages < gPinAcct[low].pages) low = i;
    }
    if (hit < 0) {
        hit = low;
        strlcpy(gPinAcct[hit].name, who, sizeof gPinAcct[hit].name);
        gPinAcct[hit].calls = gPinAcct[hit].pages = 0;
    }
    gPinAcct[hit].calls++;
    gPinAcct[hit].pages += (SInt64)npages;
    IOLockUnlock(gPinAcctLock);
}
extern "C" int nmx_pin_by_proc_format(char *buf, size_t size)
{
    int n = snprintf(buf, size, "name calls pages\n");
    if (gPinAcctLock == nullptr)
        return n;
    IOLockLock(gPinAcctLock);
    for (int i = 0; i < 8 && n > 0 && (size_t)n < size - 1; i++)
        if (gPinAcct[i].name[0] != 0 || gPinAcct[i].calls != 0)
            n += snprintf(buf + n, size - (size_t)n, "%s %lld %lld\n", gPinAcct[i].name[0] ? gPinAcct[i].name : "?",
                          (long long)gPinAcct[i].calls, (long long)gPinAcct[i].pages);
    IOLockUnlock(gPinAcctLock);
    return n;
}

extern "C" int nmx_xnu_user_wire(void *mm, uint64_t uaddr, size_t npages, int write, uint64_t *phys, void **handle)
{
    *handle = nullptr;
    if (npages == 0)
        return 0;
    if ((uaddr & PAGE_MASK) != 0)
        return EINVAL;
    pin_account(npages);
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
    UserWire *w = wire_make(md, false);
    if (w == nullptr) {
        md->complete();
        md->release();
        return ENOMEM;
    }
    *handle = w;
    return 0;
}

extern "C" void nmx_xnu_user_unwire(void *handle)
{
    UserWire *w = (UserWire *)handle;
    if (w == nullptr)
        return;
    IOLockLock(gWireLock);
    if (w->prev != nullptr)
        w->prev->next = w->next;
    else
        gWires = w->next;
    if (w->next != nullptr)
        w->next->prev = w->prev;
    if (w->parked) {
        __atomic_sub_fetch(&gNmxUserParked, 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&gNmxUserParkedPages, (SInt64)w->pages, __ATOMIC_RELAXED);
    } else {
        wire_count(w, -1);
    }
    IOLockUnlock(gWireLock);
    if (!w->parked)
        w->md->complete();
    w->md->release();
    IOFree(w, sizeof(UserWire));
}

/* Step142: 1 when the wiring can be dropped and taken again later (an IOSurface's own descriptor). Boot-arg
 * nmxpark=0 makes nothing parkable (every wiring then lives as long as its BO, as before Step142). */
static int gParkOn = -1;
extern "C" int nmx_xnu_user_parkable(void *handle)
{
    if (gParkOn < 0) {
        int v = 1;
        gParkOn = (PE_parse_boot_argn("nmxpark", &v, sizeof v) && v == 0) ? 0 : 1;
    }
    UserWire *w = (UserWire *)handle;
    return gParkOn && w != nullptr && w->surface;
}

/* Step142: drops the wiring and keeps the descriptor (the memory stays allocated, now pageable). 0 or ENOTSUP. */
extern "C" int nmx_xnu_user_park(void *handle)
{
    UserWire *w = (UserWire *)handle;
    if (!nmx_xnu_user_parkable(handle))
        return ENOTSUP;
    if (w->parked)
        return 0;
    IOLockLock(gWireLock);
    w->parked = 1;
    wire_count(w, -1);
    __atomic_add_fetch(&gNmxUserParked, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gNmxUserParkedPages, (SInt64)w->pages, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gNmxUserParks, 1, __ATOMIC_RELAXED);
    IOLockUnlock(gWireLock);
    w->md->complete();
    return 0;
}

/*
 * Step142: wires a parked record again (a wired one is left as it is) and returns the physical address of each of
 * its first npages pages; they may differ from the ones it had before it was parked. 0 or an errno; on an error the
 * record keeps whatever state it reached and nmx_xnu_user_unwire still releases it.
 */
extern "C" int nmx_xnu_user_rewire(void *handle, size_t npages, uint64_t *phys)
{
    UserWire *w = (UserWire *)handle;
    if (w == nullptr || npages > w->pages)
        return EINVAL;
    if (w->parked) {
        if (w->md->prepare() != kIOReturnSuccess) {
            __atomic_add_fetch(&gNmxUserUnparkFail, 1, __ATOMIC_RELAXED);
            return EFAULT;
        }
        IOLockLock(gWireLock);
        w->parked = 0;
        wire_count(w, 1);
        __atomic_sub_fetch(&gNmxUserParked, 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&gNmxUserParkedPages, (SInt64)w->pages, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gNmxUserUnparks, 1, __ATOMIC_RELAXED);
        IOLockUnlock(gWireLock);
    }
    for (size_t i = 0; i < npages; i++) {
        IOByteCount len = 0;
        addr64_t pa = w->md->getPhysicalSegment(i * PAGE_SIZE, &len, kIOMemoryMapperNone);
        if (pa == 0)
            return EFAULT;
        phys[i] = pa;
    }
    return 0;
}

/*
 * Step142: debug.nmx_userwire_by_kind. Per kind (surface = the IOSurface's own descriptor, plain = a user range):
 * records wired now and their pages, of those older than 60 s, and parked records. "orphan" = surface wirings whose
 * descriptor has no holder but NMX (retain count <= NMX's own records on it): the IOSurface is gone and only the BO
 * keeps its memory. Then the 8 processes with the most pages (wired + parked; surface / plain / parked).
 */
extern "C" int nmx_userwire_format(char *buf, size_t size)
{
    struct Row { char who[22]; SInt64 surf, plain, parked; };
    Row rows[8] = {};
    SInt64 cnt[2] = {}, pg[2] = {}, old[2] = {}, pold[2] = {}, pk[2] = {}, ppk[2] = {}, orph = 0, porph = 0;
    uint64_t now = mach_absolute_time(), minute;
    nanoseconds_to_absolutetime(60ULL * 1000 * 1000 * 1000, &minute);
    IOLock *l = wire_lock();
    if (l == nullptr)
        return snprintf(buf, size, "no lock\n");
    IOLockLock(l);
    unsigned total = 0;
    for (UserWire *w = gWires; w != nullptr; w = w->next)
        total++;
    for (UserWire *w = gWires; w != nullptr; w = w->next) {
        int k = w->surface ? 0 : 1;
        if (w->parked) {
            pk[k]++;
            ppk[k] += w->pages;
        } else {
            cnt[k]++;
            pg[k] += w->pages;
            if (now - w->when > minute) {
                old[k]++;
                pold[k] += w->pages;
            }
        }
        if (w->surface && total <= 4096) {
            int ours = 0;
            for (UserWire *v = gWires; v != nullptr; v = v->next)
                if (v->md == w->md)
                    ours++;
            if (w->md->getRetainCount() <= ours) {
                orph++;
                porph += w->pages;
            }
        }
        int hit = -1, low = 0;
        for (int i = 0; i < 8; i++) {
            if (strncmp(rows[i].who, w->who, sizeof rows[i].who) == 0) { hit = i; break; }
            if (rows[i].surf + rows[i].plain + rows[i].parked < rows[low].surf + rows[low].plain + rows[low].parked)
                low = i;
        }
        if (hit < 0) {
            hit = low;
            rows[hit] = Row{};
            strlcpy(rows[hit].who, w->who, sizeof rows[hit].who);
        }
        if (w->parked)
            rows[hit].parked += w->pages;
        else if (w->surface)
            rows[hit].surf += w->pages;
        else
            rows[hit].plain += w->pages;
    }
    IOLockUnlock(l);
    int n = snprintf(buf, size, "kind wired pages old60 old60pages parked parkedpages\n"
                     "surface %lld %lld %lld %lld %lld %lld\nplain %lld %lld %lld %lld %lld %lld\n"
                     "surface orphan %lld %lld\nprocess surf plain parked (pages)\n",
                     (long long)cnt[0], (long long)pg[0], (long long)old[0], (long long)pold[0], (long long)pk[0],
                     (long long)ppk[0], (long long)cnt[1], (long long)pg[1], (long long)old[1], (long long)pold[1],
                     (long long)pk[1], (long long)ppk[1], (long long)orph, (long long)porph);
    for (int i = 0; i < 8 && n > 0 && (size_t)n < size - 1; i++)
        if (rows[i].surf + rows[i].plain + rows[i].parked != 0)
            n += snprintf(buf + n, size - (size_t)n, "%s %lld %lld %lld\n", rows[i].who[0] ? rows[i].who : "?",
                          (long long)rows[i].surf, (long long)rows[i].plain, (long long)rows[i].parked);
    return n;
}
