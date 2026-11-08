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
    *handle = md;
    return 0;
}

extern "C" void nmx_xnu_user_unwire(void *handle)
{
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)handle;
    if (md == nullptr)
        return;
    md->complete();
    md->release();
}
