/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef NVMTL_DRM_XNU_H
#define NVMTL_DRM_XNU_H
/*
 * The AMD/Intel DRM render node for sandboxed processes.
 *
 * App Sandbox and browser GPU sandboxes refuse /dev/dri ("deny file-read-data /dev/dri"), so RADV's open(2) of the
 * render node fails, vkEnumeratePhysicalDevices finds nothing and the app draws on the CPU. The same sandboxes let a
 * process open user clients of an IOAccelerator, so NMXAccel hands out an NMXDrmClient (IOServiceOpen type 'NMDR',
 * kexts/NVRM/accel/nmx/nmx_drm_uc.cpp): the kernel opens the node for the caller and runs its ioctls on it.
 *
 * This file points the libc imports of RADV, libdrm and libdrm_amdgpu that touch the node at wrappers (the plugin's
 * nvmtl_rebind_import; nothing else in the process is touched):
 *   open      /dev/dri/<node> refused by the sandbox -> an NMDR connection, opened for that node; the descriptor handed
 *             back is a read-only /dev/null (an owned number every sandbox allows, as the NVIDIA RM bridge does)
 *   ioctl     on such a descriptor -> the connection's ioctl selector (same command numbers, same argument bytes)
 *   close, dup, fcntl(F_DUPFD*) keep a reference count per DRM file; fstat/stat answer a character device with the
 *             node's real device number; opendir/readdir of a refused /dev/dri list the render node
 * The WebKit GPU process profile also refuses reading /dev/null, pipe(2), fcntl(F_DUPFD/F_DUPFD_CLOEXEC) and the
 * page-size sysctl: the handle falls back to /dev/random (readable there), a refused F_DUPFD* becomes dup(2)
 * (+ F_SETFD), and getpagesize/sysconf(_SC_PAGESIZE) answer vm_page_size (docs/890M-ROADMAP.md, "Safari WebGL").
 * A process whose sandbox lets it open the node (WindowServer, Terminal, tools) never takes this path: every wrapper
 * tries the real call first and only steps in when it fails with EPERM/EACCES. NVMTL_DRM_XNU=1 forces it (tests).
 */
#include <IOKit/IOKitLib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/dyld.h>

#define NVMTL_DRMX_UC_TYPE 0x4E4D4452u   /* 'NMDR' */
enum { NVMTL_DRMX_SEL_OPEN = 0, NVMTL_DRMX_SEL_IOCTL = 1, NVMTL_DRMX_SEL_STAT = 2 };
#define NVMTL_DRMX_MAX_FD 4096

struct nvmtl_drmx_file {
    io_connect_t conn;
    int refs;               /* descriptors that name this DRM file (dup/F_DUPFD share it) */
    int major, minor;       /* the kernel open's own device number, as fstat of a real open would give */
};
static struct nvmtl_drmx_file *g_drmx_fd[NVMTL_DRMX_MAX_FD];
static pthread_mutex_t g_drmx_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_drmx_force = -1;
static volatile int g_drmx_major = -1;
static unsigned long g_drmx_opens, g_drmx_ioctls, g_drmx_logged;

static int nvmtl_drmx_forced(void)
{
    if (g_drmx_force < 0) { const char *e = getenv("NVMTL_DRM_XNU"); g_drmx_force = (e && *e == '1') ? 1 : 0; }
    return g_drmx_force;
}
static int g_drmx_trace = -1;
#define DRMX_TRACE(...) do { if (g_drmx_trace < 0) g_drmx_trace = getenv("NVMTL_DRM_XNU_TRACE") ? 1 : 0; \
                             if (g_drmx_trace) nvlog("drm-xnu trace: " __VA_ARGS__); } while (0)
static int nvmtl_drmx_refused(int err) { return err == EPERM || err == EACCES; }

/* "/dev/dri/renderD128" -> 128, "/dev/dri/card0" -> 0; -1 for any other path */
static int nvmtl_drmx_node(const char *path)
{
    if (!path || strncmp(path, "/dev/dri/", 9)) return -1;
    const char *n = path + 9, *d;
    int render;
    if (!strncmp(n, "renderD", 7)) { d = n + 7; render = 1; }
    else if (!strncmp(n, "card", 4)) { d = n + 4; render = 0; }
    else return -1;
    if (!*d) return -1;
    int v = 0;
    for (; *d; d++) { if (*d < '0' || *d > '9') return -1; v = v * 10 + (*d - '0'); if (v > 255) return -1; }
    /* DRM minors: card0..63 primary, renderD128..191 render. libdrm on macOS names a node from its minor and asks
       access("/dev/dri/card<minor>") to tell primary from render, so a wrong pairing must not look like a node */
    if (render ? (v < 128 || v > 191) : v > 63) return -1;
    return v;
}

static struct nvmtl_drmx_file *nvmtl_drmx_get(int fd)
{
    if (fd < 0 || fd >= NVMTL_DRMX_MAX_FD) return NULL;
    return __atomic_load_n(&g_drmx_fd[fd], __ATOMIC_ACQUIRE);   /* every ioctl asks: no lock on this path */
}

static io_connect_t nvmtl_drmx_connect(void)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!svc) return 0;
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), NVMTL_DRMX_UC_TYPE, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        if (g_drmx_logged++ < 8) nvlog("drm-xnu: IOServiceOpen(NMXAccel, NMDR) 0x%08x - no sandbox-safe DRM path", kr);
        return 0;
    }
    return conn;
}

/* a read-only descriptor the sandbox allows: /dev/null, /dev/random (the WebKit GPU process refuses /dev/null but
   reads /dev/random), else the read end of a closed pipe */
static int nvmtl_drmx_handle_fd(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) return fd;
    fd = open("/dev/random", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) return fd;
    int ends[2];
    if (pipe(ends)) return -1;
    close(ends[1]);
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    return ends[0];
}

static int nvmtl_drmx_open_node(int node, int flags)
{
    io_connect_t conn = nvmtl_drmx_connect();
    if (!conn) { errno = EACCES; return -1; }
    uint64_t in = (uint64_t)node, out[3] = {0}; uint32_t n = 3;
    kern_return_t kr = IOConnectCallScalarMethod(conn, NVMTL_DRMX_SEL_OPEN, &in, 1, out, &n);
    if (kr != KERN_SUCCESS || out[0]) {
        nvlog("drm-xnu: kernel open of node %d failed (kr 0x%08x errno %llu)", node, kr, (unsigned long long)out[0]);
        IOServiceClose(conn);
        errno = kr != KERN_SUCCESS ? EIO : (int)out[0];
        return -1;
    }
    struct nvmtl_drmx_file *f = calloc(1, sizeof(*f));
    int fd = f ? nvmtl_drmx_handle_fd() : -1;
    if (fd < 0 || fd >= NVMTL_DRMX_MAX_FD) {
        if (fd >= 0) close(fd);
        free(f); IOServiceClose(conn);
        errno = EMFILE; return -1;
    }
    if (!(flags & O_CLOEXEC)) fcntl(fd, F_SETFD, 0);
    f->conn = conn; f->refs = 1; f->major = (int)out[1]; f->minor = (int)out[2];
    g_drmx_major = f->major;
    pthread_mutex_lock(&g_drmx_lock);
    g_drmx_fd[fd] = f;
    unsigned long opens = ++g_drmx_opens;
    pthread_mutex_unlock(&g_drmx_lock);
    if (opens <= 8)
        nvlog("drm-xnu: %s: node %d opened through NMXAccel (the sandbox refuses /dev/dri) -> fd %d, dev %d,0x%x",
              getprogname(), node, fd, f->major, f->minor);
    return fd;
}

/* the node's device number for stat(2) of its path when the sandbox refuses the real one */
static int nvmtl_drmx_node_major(int node)
{
    if (g_drmx_major >= 0) return g_drmx_major;
    io_connect_t conn = nvmtl_drmx_connect();
    if (!conn) return -1;
    uint64_t in = (uint64_t)node, out[3] = {0}; uint32_t n = 3;
    kern_return_t kr = IOConnectCallScalarMethod(conn, NVMTL_DRMX_SEL_STAT, &in, 1, out, &n);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS || out[0]) return -1;
    g_drmx_major = (int)out[1];
    return g_drmx_major;
}
static void nvmtl_drmx_fake_stat(struct stat *st, int major, int minor)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_rdev = makedev(major, minor);
    st->st_nlink = 1;
}

/* ---- the wrappers ------------------------------------------------------------------------------------------------ */

static int nvmtl_drmx_w_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    int node = nvmtl_drmx_node(path);
    if (node < 0) return open(path, flags, mode);
    if (!nvmtl_drmx_forced()) {
        int fd = open(path, flags, mode);
        if (fd >= 0 || !nvmtl_drmx_refused(errno)) return fd;
    }
    return nvmtl_drmx_open_node(node, flags);
}

static int nvmtl_drmx_w_ioctl(int fd, unsigned long cmd, ...)
{
    va_list ap; va_start(ap, cmd); void *arg = va_arg(ap, void *); va_end(ap);
    struct nvmtl_drmx_file *f = nvmtl_drmx_get(fd);
    if (!f) return ioctl(fd, cmd, arg);
    DRMX_TRACE("ioctl fd %d cmd 0x%lx", fd, cmd);
    size_t len = (cmd & (IOC_IN | IOC_OUT)) ? IOCPARM_LEN(cmd) : 0;
    uint64_t in = (uint64_t)cmd, err = 0; uint32_t n = 1; size_t outlen = len;
    kern_return_t kr = IOConnectCallMethod(f->conn, NVMTL_DRMX_SEL_IOCTL, &in, 1, len ? arg : NULL, len, &err, &n,
                                           len ? arg : NULL, len ? &outlen : NULL);
    g_drmx_ioctls++;
    if (kr != KERN_SUCCESS) {
        if (g_drmx_logged++ < 16) nvlog("drm-xnu: ioctl 0x%lx (%zu bytes) transport 0x%08x", cmd, len, kr);
        errno = EIO; return -1;
    }
    if (err) { DRMX_TRACE("ioctl fd %d cmd 0x%lx -> errno %llu", fd, cmd, (unsigned long long)err); errno = (int)err; return -1; }
    return 0;
}

static void nvmtl_drmx_drop(struct nvmtl_drmx_file *f)
{
    pthread_mutex_lock(&g_drmx_lock);
    int last = --f->refs == 0;
    pthread_mutex_unlock(&g_drmx_lock);
    if (last) { IOServiceClose(f->conn); free(f); }
}

static int nvmtl_drmx_w_close(int fd)
{
    struct nvmtl_drmx_file *f = NULL;
    if (fd >= 0 && fd < NVMTL_DRMX_MAX_FD) {
        pthread_mutex_lock(&g_drmx_lock);
        f = g_drmx_fd[fd]; g_drmx_fd[fd] = NULL;
        pthread_mutex_unlock(&g_drmx_lock);
    }
    if (f) { DRMX_TRACE("close fd %d", fd); nvmtl_drmx_drop(f); }
    return close(fd);
}

static int nvmtl_drmx_share(struct nvmtl_drmx_file *f, int nfd)
{
    if (nfd < 0) return nfd;
    if (nfd >= NVMTL_DRMX_MAX_FD) { close(nfd); errno = EMFILE; return -1; }
    pthread_mutex_lock(&g_drmx_lock);
    f->refs++;
    g_drmx_fd[nfd] = f;
    pthread_mutex_unlock(&g_drmx_lock);
    return nfd;
}

static int nvmtl_drmx_w_dup(int fd)
{
    struct nvmtl_drmx_file *f = nvmtl_drmx_get(fd);
    int nfd = dup(fd);
    return f ? nvmtl_drmx_share(f, nfd) : nfd;
}

static int nvmtl_drmx_w_fcntl(int fd, int cmd, ...)
{
    va_list ap; va_start(ap, cmd); long arg = va_arg(ap, long); va_end(ap);
    int is_dup = cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC;
    struct nvmtl_drmx_file *f = is_dup ? nvmtl_drmx_get(fd) : NULL;
    int r = fcntl(fd, cmd, arg);
    if (r < 0 && is_dup && nvmtl_drmx_refused(errno)) {
        /* the sandbox refuses the F_DUPFD* command (WebKit GPU process); dup(2) and F_SETFD are allowed. dup takes the
           lowest free number, which can be below `arg`: callers here (RADV os_dupfd_cloexec, arg 3) only need >= 3,
           and 0..2 are always taken. */
        r = dup(fd);
        if (r >= 0 && cmd == F_DUPFD_CLOEXEC) fcntl(r, F_SETFD, FD_CLOEXEC);
        DRMX_TRACE("fcntl F_DUPFD%s refused: dup fd %d -> %d", cmd == F_DUPFD_CLOEXEC ? "_CLOEXEC" : "", fd, r);
    }
    if (f) DRMX_TRACE("fcntl dup fd %d -> %d", fd, r);
    return f ? nvmtl_drmx_share(f, r) : r;
}

static int nvmtl_drmx_w_fstat(int fd, struct stat *st)
{
    struct nvmtl_drmx_file *f = nvmtl_drmx_get(fd);
    if (!f) return fstat(fd, st);
    nvmtl_drmx_fake_stat(st, f->major, f->minor);
    DRMX_TRACE("fstat fd %d -> chr %d,0x%x", fd, f->major, f->minor);
    return 0;
}

static int nvmtl_drmx_w_stat(const char *path, struct stat *st)
{
    int node = nvmtl_drmx_node(path);
    if (node < 128) return stat(path, st);
    if (!nvmtl_drmx_forced()) {
        int r = stat(path, st);
        if (r == 0 || !nvmtl_drmx_refused(errno)) return r;
    }
    int maj = nvmtl_drmx_node_major(node);
    if (maj < 0) { errno = EACCES; return -1; }
    nvmtl_drmx_fake_stat(st, maj, node);
    DRMX_TRACE("stat %s -> chr %d,0x%x", path, maj, node);
    return 0;
}

static int nvmtl_drmx_w_access(const char *path, int mode)
{
    int node = nvmtl_drmx_node(path);
    if (node < 128) return access(path, mode);
    if (!nvmtl_drmx_forced()) {
        int r = access(path, mode);
        if (r == 0 || !nvmtl_drmx_refused(errno)) return r;
    }
    if (nvmtl_drmx_node_major(node) < 0) { errno = EACCES; return -1; }
    return 0;
}

/* a refused /dev/dri lists the render node only (what RADV opens; libdrm groups nodes per PCI device) */
struct nvmtl_drmx_dir { uint32_t magic; int pos; struct dirent ent; };
#define NVMTL_DRMX_DIR_MAGIC 0x4E4D4452u
static void *g_drmx_dirs[16];

static int nvmtl_drmx_is_fake_dir(void *d)
{
    if (!d) return 0;
    pthread_mutex_lock(&g_drmx_lock);
    int found = 0;
    for (int i = 0; i < 16 && !found; i++) found = g_drmx_dirs[i] == d;
    pthread_mutex_unlock(&g_drmx_lock);
    return found;
}

static DIR *nvmtl_drmx_w_opendir(const char *path)
{
    int is_dri = path && (!strcmp(path, "/dev/dri") || !strcmp(path, "/dev/dri/"));
    if (!is_dri) return opendir(path);
    if (!nvmtl_drmx_forced()) {
        DIR *d = opendir(path);
        if (d || !nvmtl_drmx_refused(errno)) return d;
    }
    if (nvmtl_drmx_node_major(128) < 0) { errno = EACCES; return NULL; }
    struct nvmtl_drmx_dir *fd = calloc(1, sizeof(*fd));
    if (!fd) { errno = ENOMEM; return NULL; }
    fd->magic = NVMTL_DRMX_DIR_MAGIC;
    pthread_mutex_lock(&g_drmx_lock);
    int slot = -1;
    for (int i = 0; i < 16 && slot < 0; i++) if (!g_drmx_dirs[i]) { g_drmx_dirs[i] = fd; slot = i; }
    pthread_mutex_unlock(&g_drmx_lock);
    if (slot < 0) { free(fd); errno = EMFILE; return NULL; }
    return (DIR *)fd;
}

static struct dirent *nvmtl_drmx_w_readdir(DIR *d)
{
    if (!nvmtl_drmx_is_fake_dir(d)) { struct dirent *e = readdir(d); return e; }
    DRMX_TRACE("readdir (fake /dev/dri)");
    struct nvmtl_drmx_dir *fd = (struct nvmtl_drmx_dir *)d;
    if (fd->pos++ != 0) return NULL;
    memset(&fd->ent, 0, sizeof(fd->ent));
    fd->ent.d_ino = 1;
    fd->ent.d_type = DT_CHR;
    strlcpy(fd->ent.d_name, "renderD128", sizeof(fd->ent.d_name));
    fd->ent.d_namlen = (uint16_t)strlen(fd->ent.d_name);
    fd->ent.d_reclen = sizeof(fd->ent);
    return &fd->ent;
}

static int nvmtl_drmx_w_closedir(DIR *d)
{
    if (!nvmtl_drmx_is_fake_dir(d)) return closedir(d);
    pthread_mutex_lock(&g_drmx_lock);
    for (int i = 0; i < 16; i++) if (g_drmx_dirs[i] == d) g_drmx_dirs[i] = NULL;
    pthread_mutex_unlock(&g_drmx_lock);
    free(d);
    return 0;
}

static int nvmtl_drmx_w_getpagesize(void) { return (int)nvmtl_page_size(); }

/* Point the node-touching libc imports of one image at the wrappers; returns how many pointers changed. */
static int nvmtl_drmx_rebind_image(const void *inside)
{
    static const struct { const char *sym; void *fn; } k[] = {
        { "_open", (void *)nvmtl_drmx_w_open },
        { "_ioctl", (void *)nvmtl_drmx_w_ioctl },
        { "_close", (void *)nvmtl_drmx_w_close },
        { "_dup", (void *)nvmtl_drmx_w_dup },
        { "_fcntl", (void *)nvmtl_drmx_w_fcntl },
        { "_fstat$INODE64", (void *)nvmtl_drmx_w_fstat },
        { "_stat$INODE64", (void *)nvmtl_drmx_w_stat },
        { "_access", (void *)nvmtl_drmx_w_access },
        { "_opendir$INODE64", (void *)nvmtl_drmx_w_opendir },
        { "_readdir$INODE64", (void *)nvmtl_drmx_w_readdir },
        { "_closedir", (void *)nvmtl_drmx_w_closedir },
        { "_getpagesize", (void *)nvmtl_drmx_w_getpagesize },
        { "_sysconf", (void *)nvmtl_sysconf },
    };
    int total = 0;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        int r = nvmtl_rebind_import(inside, k[i].sym, k[i].fn);
        if (r > 0) total += r;
    }
    return total;
}

/* libsystem_sandbox: nonzero when the process runs in a sandbox (operation NULL) */
extern int sandbox_check(pid_t pid, const char *operation, int type, ...);

/* RADV (the image holding `driver`) and the libdrm images it loaded. Unsandboxed processes (WindowServer, Terminal)
   keep the plain libc imports, so their path stays exactly what it was. */
static void nvmtl_drmx_install(const void *driver)
{
    /* WindowServer is sandboxed too, but its profile lets it open the node: arm only where the sandbox refuses the
       render node itself (App Sandbox, browser GPU processes) or its ioctls (NotificationCenter: read allowed,
       file-ioctl denied). There the real calls are not tried first, so the process does not file a sandbox
       violation report on every open/stat/ioctl. */
    if (!nvmtl_drmx_forced()) {
        if (sandbox_check(getpid(), "file-read-data", 1 /* SANDBOX_FILTER_PATH */, "/dev/dri/renderD128") == 0 &&
            sandbox_check(getpid(), "file-ioctl", 1, "/dev/dri/renderD128") == 0) return;
        g_drmx_force = 1;
    }
    int n = nvmtl_drmx_rebind_image(driver), libs = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name) continue;
        const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
        if (strcmp(b, "libdrm.2.dylib") && strcmp(b, "libdrm_amdgpu.1.dylib")) continue;
        int r = nvmtl_drmx_rebind_image((const void *)_dyld_get_image_header(i));
        if (r > 0) { n += r; libs++; }
    }
    nvlog("drm-xnu: sandbox-safe DRM path armed (%d imports in the driver and %d libdrm images; %s)", n, libs,
          getenv("NVMTL_DRM_XNU") ? "forced by NVMTL_DRM_XNU=1" : "this process's sandbox refuses /dev/dri");
}
#endif
