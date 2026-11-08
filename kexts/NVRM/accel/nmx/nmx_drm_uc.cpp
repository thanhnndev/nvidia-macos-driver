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
#include <string.h>

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
    kNmxDrmSelCount
};

class NMXDrmClient : public IOUserClient {
    OSDeclareDefaultStructors(NMXDrmClient)
    task_t   fTask = nullptr;
    vnode_t  fVp = nullptr;
    IORWLock *fLock = nullptr;
    int      fMajor = -1, fMinor = -1;
    static volatile SInt32 sOpens;
    static volatile SInt32 sMajor;           /* NMXGPU's devfs major, learnt from the first open (-1 = not yet) */
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE {
        if (!IOUserClient::initWithTask(owningTask, securityID, type)) return false;
        fTask = owningTask;
        fLock = IORWLockAlloc();
        return fLock != nullptr;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fLock) { IORWLockFree(fLock); fLock = nullptr; }
        IOUserClient::free();
    }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {
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
