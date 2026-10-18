/*
 * Device nodes for the port on xnu devfs. Compiled with Apple's headers only.
 *
 * One xnu cdevsw serves every node. Each node is a clone node (devfs_make_node_clone): every open() of it is given
 * a minor number of its own, and that minor identifies the open in all later calls, which is how per-open state (a
 * Linux struct file) exists although an xnu cdevsw entry point receives only a dev_t.
 *
 * devfs clones at every lookup, stat(2) included, so the dev_t a process sees changes from one stat to the next. libdrm
 * (patches/libdrm-macos.patch) identifies a DRM node by its minor and names it from it (card<minor>, renderD<minor>), so
 * an open minor is (open sequence << 8) | node minor, and a node's minor is its DRM minor number: dri/card0 is 0,
 * dri/renderD128 is 128. libdrm on macOS reads only the low byte of a minor; other nodes take free minors from the top.
 *
 * Entry points call into the LinuxKPI side (nmx_cdev_*). mmap is not a cdevsw operation kexts can implement on
 * macOS, so user mappings of GPU memory are made by an ioctl through nmx_xnu_map_user (an IOKit mapping in the
 * calling task).
 */
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include "nmx_xnu.h"

/* exported to kexts (xnu config: Mach.exports task_reference/task_deallocate; Unsupported.exports get_task_map,
   vm_deallocate) but not declared in Kernel.framework's headers */
extern "C" {
void task_reference(task_t task);
void task_deallocate(task_t task);
vm_map_t get_task_map(task_t task);
kern_return_t vm_deallocate(vm_map_t target, vm_offset_t address, vm_size_t size);
}

#define NODE_MINORS 256
#define OPEN_SEQ_MAX 65536                       /* open minor = seq << 8 | node, seq 1..65535 (minor is 24 bits) */
#define OPEN_MINOR(seq, node) (((seq) << 8) | (node))

static int g_major = -1;
static IOLock *g_lock;
static void *g_node_cookie[NODE_MINORS];       /* node minor -> LinuxKPI cookie (struct cdev *) */
static void *g_node_handle[NODE_MINORS];       /* devfs node handles */
static uint16_t g_open_node[OPEN_SEQ_MAX];     /* open sequence -> node minor + 1 (0 = free) */
static int g_open_next = 1;

/* the sequence of open minor m, or -1 when m is not a live open */
static int open_seq(int m)
{
    int seq = m >> 8;
    if (seq <= 0 || seq >= OPEN_SEQ_MAX || g_open_node[seq] != (uint16_t)((m & 0xff) + 1))
        return -1;
    return seq;
}

extern "C" void nmx_thread_crit_check(const char *where);   /* nmx_kernel.c */

/* ---- clone minors ------------------------------------------------------------------------------------------------- */

static int clone_minor(dev_t dev, int action)
{
    int node = minor(dev);
    if (action == DEVFS_CLONE_ALLOC) {
        IOLockLock(g_lock);
        for (int tries = 0; tries < OPEN_SEQ_MAX - 1; tries++) {
            int seq = g_open_next;
            g_open_next = (g_open_next + 1 < OPEN_SEQ_MAX) ? g_open_next + 1 : 1;
            if (g_open_node[seq] == 0) {
                g_open_node[seq] = (uint16_t)(node + 1);
                IOLockUnlock(g_lock);
                return OPEN_MINOR(seq, node);
            }
        }
        IOLockUnlock(g_lock);
        return -1;
    }
    /* DEVFS_CLONE_FREE: the open minor is released by close() */
    return 0;
}

static void *cookie_for_open(int m, int *node_out)
{
    void *cookie = nullptr;
    IOLockLock(g_lock);
    if (open_seq(m) > 0) {
        int node = m & 0xff;
        cookie = g_node_cookie[node];
        if (node_out)
            *node_out = node;
    }
    IOLockUnlock(g_lock);
    return cookie;
}

/* ---- entry points ------------------------------------------------------------------------------------------------- */

static int nmx_open(dev_t dev, int flags, int devtype, struct proc *p)
{
    int m = minor(dev);
    void *cookie = cookie_for_open(m, nullptr);
    if (cookie == nullptr)
        return ENXIO;
    int err = nmx_cdev_open(cookie, m, flags);
    nmx_thread_crit_check("open");
    if (err) {
        IOLockLock(g_lock);
        int seq = open_seq(m);
        if (seq > 0)
            g_open_node[seq] = 0;
        IOLockUnlock(g_lock);
    }
    return err;
}

static int nmx_close(dev_t dev, int flags, int devtype, struct proc *p)
{
    int m = minor(dev);
    int err = nmx_cdev_close(m, flags);
    nmx_thread_crit_check("close");
    IOLockLock(g_lock);
    int seq = open_seq(m);
    if (seq > 0)
        g_open_node[seq] = 0;
    IOLockUnlock(g_lock);
    return err;
}

static int nmx_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc *p)
{
    int err = nmx_cdev_ioctl(minor(dev), cmd, data, fflag);
    nmx_thread_crit_check("ioctl");
    return err;
}

/*
 * read: the driver's read handler copies straight to the caller's buffer (Linux copy_to_user), so the glue hands
 * it the current user segment of the xnu uio and advances the uio by what was read. Only user-space reads reach a
 * device node from read(2).
 */
static int nmx_read(dev_t dev, struct uio *uio, int ioflag)
{
    if (uio_resid(uio) <= 0)
        return 0;
    if (!uio_isuserspace(uio))
        return EFAULT;
    user_addr_t base = uio_curriovbase(uio);
    user_size_t len = uio_curriovlen(uio);
    size_t done = 0;
    int err = nmx_cdev_read(minor(dev), (uint64_t)base, (size_t)len, &done, (ioflag & IO_NDELAY) ? 1 : 0);
    nmx_thread_crit_check("read");
    if (done > 0)
        uio_update(uio, (user_size_t)done);
    return err;
}

/*
 * select/poll: xnu keeps struct selinfo opaque to kexts, so no waiter can be recorded. Readiness is reported from
 * the driver's own poll; a not-ready answer makes poll() fall back to its timeout. The render path does not poll.
 */
static int nmx_select(dev_t dev, int which, void *wql, struct proc *p)
{
    int events = which == FREAD ? 0x0001 /* POLLIN */ : which == FWRITE ? 0x0004 /* POLLOUT */ : 0x0002 /* POLLPRI */;
    int ready = nmx_cdev_poll(minor(dev), events) != 0;
    nmx_thread_crit_check("select");
    return ready;
}

static struct cdevsw g_cdevsw = {
    /* d_open */ nmx_open,
    /* d_close */ nmx_close,
    /* d_read */ nmx_read,
    /* d_write */ eno_rdwrt,
    /* d_ioctl */ nmx_ioctl,
    /* d_stop */ eno_stop,
    /* d_reset */ eno_reset,
    /* d_ttys */ nullptr,
    /* d_select */ nmx_select,
    /* d_mmap */ eno_mmap,
    /* d_strategy */ eno_strat,
    /* d_reserved_1 */ eno_getc,
    /* d_reserved_2 */ eno_putc,
    /* d_type */ 0,
};

/* ---- nodes -------------------------------------------------------------------------------------------------------- */

extern "C" int nmx_xnu_devfs_init(void)
{
    g_lock = IOLockAlloc();
    if (g_lock == nullptr)
        return ENOMEM;
    g_major = cdevsw_add(-1, &g_cdevsw);
    if (g_major < 0) {
        IOLockFree(g_lock);
        g_lock = nullptr;
        return ENXIO;
    }
    return 0;
}

extern "C" int nmx_xnu_devfs_busy(void)
{
    int busy = 0;
    IOLockLock(g_lock);
    for (int seq = 1; seq < OPEN_SEQ_MAX; seq++)
        if (g_open_node[seq]) { busy++; }
    IOLockUnlock(g_lock);
    return busy;
}

extern "C" void nmx_xnu_devfs_fini(void)
{
    if (g_major >= 0)
        cdevsw_remove(g_major, &g_cdevsw);
    g_major = -1;
    if (g_lock)
        IOLockFree(g_lock);
    g_lock = nullptr;
}

extern "C" void *nmx_xnu_devfs_make(const char *path, uint32_t uid, uint32_t gid, int mode, void *cookie)
{
    if (g_major < 0)
        return nullptr;
    /* dri/card<N> and dri/renderD<N> take minor N (the DRM minor libdrm derives the node name from) */
    int want = -1;
    if (strncmp(path, "dri/", 4) == 0) {
        const char *d = path + strlen(path);
        while (d > path + 4 && d[-1] >= '0' && d[-1] <= '9')
            d--;
        if (*d != '\0') {
            want = 0;
            for (; *d && want < NODE_MINORS; d++)
                want = want * 10 + (*d - '0');
            if (want >= NODE_MINORS)
                want = -1;
        }
    }
    IOLockLock(g_lock);
    int node = -1;
    if (want >= 0 && g_node_cookie[want] == nullptr)
        node = want;
    for (int i = NODE_MINORS - 1; node < 0 && i >= 0; i--)
        if (g_node_cookie[i] == nullptr)
            node = i;
    if (node >= 0)
        g_node_cookie[node] = cookie;
    IOLockUnlock(g_lock);
    if (node < 0)
        return nullptr;
    void *h = devfs_make_node_clone(makedev(g_major, node), DEVFS_CHAR, uid, gid, mode, clone_minor, "%s", path);
    IOLockLock(g_lock);
    if (h == nullptr)
        g_node_cookie[node] = nullptr;
    else
        g_node_handle[node] = h;
    IOLockUnlock(g_lock);
    return h;
}

extern "C" void nmx_xnu_devfs_remove(void *handle)
{
    if (handle == nullptr)
        return;
    devfs_remove(handle);
    IOLockLock(g_lock);
    for (int i = 0; i < NODE_MINORS; i++)
        if (g_node_handle[i] == handle) { g_node_handle[i] = nullptr; g_node_cookie[i] = nullptr; break; }
    IOLockUnlock(g_lock);
}

/* ---- user mappings of GPU memory (the mmap ioctl) ------------------------------------------------------------------ */

static IOOptionBits cache_option(int cache)
{
    switch (cache) {
    case NMX_CACHE_UC: return kIOMapInhibitCache;
    case NMX_CACHE_WC: return kIOMapWriteCombineCache;
    case NMX_CACHE_WT: return kIOMapWriteThruCache;
    default: return kIOMapDefaultCache;
    }
}

/*
 * A fixed mapping is entered over the process's own range with kIOMapOverwrite (atomic, like Linux MAP_FIXED). IOKit
 * never deallocates an overwrite mapping's range when the map is released (IOMemoryDescriptor::doUnmap), so the
 * device pages would stay visible to the process after the buffer is freed. The record therefore keeps a reference to
 * the task and removes the range itself before releasing the map.
 */
struct UserMap {
    IOMemoryMap *map;
    task_t task;            /* fixed mappings only: the task whose range is removed at unmap */
    uint64_t addr;
    uint64_t size;
};

extern "C" int nmx_xnu_map_user(const uint64_t *pages, size_t npages, int cache, int writable, uint64_t fixed,
                                uint64_t *uaddr, void **handle)
{
    *handle = nullptr;
    *uaddr = 0;
    if (npages == 0 || (fixed & PAGE_MASK) != 0)
        return EINVAL;
    IOAddressRange *ranges = (IOAddressRange *)IOMalloc(sizeof(IOAddressRange) * npages);
    if (ranges == nullptr)
        return ENOMEM;
    size_t n = 0;
    for (size_t i = 0; i < npages; i++) {
        if (n > 0 && ranges[n - 1].address + ranges[n - 1].length == pages[i]) {
            ranges[n - 1].length += PAGE_SIZE;
        } else {
            ranges[n].address = pages[i];
            ranges[n].length = PAGE_SIZE;
            n++;
        }
    }
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRanges(ranges, (UInt32)n,
                                 kIODirectionInOut | kIOMemoryTypePhysical64, nullptr);
    IOFree(ranges, sizeof(IOAddressRange) * npages);
    if (md == nullptr)
        return ENOMEM;
    UserMap *um = IONew(UserMap, 1);
    if (um == nullptr) {
        md->release();
        return ENOMEM;
    }
    IOOptionBits opts = cache_option(cache) | (writable ? 0 : kIOMapReadOnly) | (fixed ? kIOMapOverwrite : kIOMapAnywhere);
    IOMemoryMap *map = md->createMappingInTask(current_task(), fixed, opts);
    md->release();
    if (map == nullptr) {
        IODelete(um, UserMap, 1);
        return fixed ? EINVAL : ENOMEM;
    }
    um->map = map;
    um->addr = map->getAddress();
    um->size = (uint64_t)npages * PAGE_SIZE;
    um->task = nullptr;
    if (fixed) {
        um->task = current_task();
        task_reference(um->task);
    }
    *uaddr = um->addr;
    *handle = um;
    return 0;
}

extern "C" void nmx_xnu_unmap_user(void *handle)
{
    UserMap *um = (UserMap *)handle;
    if (um == nullptr)
        return;
    if (um->task != nullptr) {
        /* the range goes first, while the map still holds the pages: the process never sees freed memory */
        vm_deallocate(get_task_map(um->task), (vm_offset_t)um->addr, (vm_size_t)um->size);
        task_deallocate(um->task);
    }
    um->map->release();
    IODelete(um, UserMap, 1);
}
