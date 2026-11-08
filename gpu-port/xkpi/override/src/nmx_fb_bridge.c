/*
 * The display bridge, DRM side: lights the GPU's outputs with one scan-out buffer that macOS draws into.
 *
 * It is an in-kernel DRM client, the same mechanism Linux's fbdev emulation uses: probe the connectors, take the
 * internal panel's preferred mode, create an XRGB8888 buffer that size, point the panel's CRTC at it and commit.
 * External outputs are left off: the console bridge drives the panel, and a monitor attached at boot must not be able
 * to change the panel's mode (see bridge_restrict_to_panel). The buffer's CPU view is the GPU aperture (a CPU-visible,
 * contiguous VRAM buffer read through the BAR), so the kext publishes its physical range as the IOFramebuffer's VRAM
 * and WindowServer draws straight into what the display engine scans out.
 *
 * Any step that fails leaves no client behind and reports why; the kext then publishes no framebuffer.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/iosys-map.h>

#include <drm/drm_client.h>
#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_crtc.h>
#include <drm/drm_connector.h>
#include <drm/drm_property.h>
#include "nmx_fb_edid.h"

#include <sys/bus.h>

#include "nmx_xnu.h"
#include "nmx_port.h"

struct nmx_fb {
    struct drm_client_dev client;
    struct drm_client_buffer *buffer;
    struct iosys_map map;
    int mapped;
    struct nmx_fb_info info;
    struct nmx_fb_display_info display;
};

/* one bridge per PCI function the port drives */
#define MAX_FB 8
static struct { void *dev; struct nmx_fb *fb; } fbs[MAX_FB];

/*
 * amdgpu may put scan-out buffers in system memory on APUs (scatter-gather display); the bridge needs them in
 * contiguous VRAM so one physical range describes the whole frame. Set before the driver initialises its display.
 */
extern int amdgpu_sg_display;

/*
 * ga403wm Step99: DC is brought up with GPU VM (amdgpu_sg_display keeps its default -1), so it can scan out GTT
 * buffers (flip imports). The bridge's own buffers still go to contiguous VRAM through fb_create_vram.
 */
void nmx_fb_before_attach(void)
{
}

struct drm_client_buffer;
static struct drm_client_buffer *fb_create_vram(struct nmx_fb *nfb, uint32_t w, uint32_t h, uint32_t fmt);

/* the DRM device of the driver attached to the PCI function 'dev' (amdgpu keeps it as the PCI driver data) */
static struct drm_device *drm_of(device_t dev)
{
    device_t *kids = NULL;
    int n = 0;
    struct drm_device *drm = NULL;
    if (device_get_children(dev, &kids, &n) != 0)
        return NULL;
    for (int i = 0; i < n && drm == NULL; i++) {
        if (!device_is_attached(kids[i]))
            continue;
        struct pci_dev *pdev = device_get_softc(kids[i]);
        if (pdev != NULL)
            drm = pci_get_drvdata(pdev);
    }
    free(kids, M_TEMP);
    return drm;
}

/* the connector behind a mode set, or NULL when the set has none */
static struct drm_connector *modeset_connector(const struct drm_mode_set *ms)
{
    return (ms->num_connectors > 0 && ms->connectors != NULL) ? ms->connectors[0] : NULL;
}

static int connector_is_panel(const struct drm_connector *conn)
{
    if (conn == NULL)
        return 0;
    return conn->connector_type == DRM_MODE_CONNECTOR_eDP || conn->connector_type == DRM_MODE_CONNECTOR_LVDS;
}

/*
 * The console bridge lights the panel only.
 *
 * It used to point every lit CRTC at one buffer as large as the largest connected mode, which is what fbdev does.
 * That works while the panel is alone; with an HDMI monitor plugged in at boot the panel's CRTC was programmed for
 * the monitor's mode (or a second CRTC on the same buffer failed the commit) and the machine never reached the
 * desktop. An external output is now left disabled (mode = NULL makes the atomic commit turn that CRTC off) so a
 * boot with a monitor attached behaves exactly like one without, and the panel always gets its own mode.
 *
 * Caller holds client->modeset_mutex. Returns the panel's mode set with a mode, or NULL when no panel is connected.
 */
static struct drm_mode_set *bridge_restrict_to_panel(struct drm_client_dev *client)
{
    struct drm_mode_set *ms, *panel = NULL;
    int have_panel = 0;

    drm_client_for_each_modeset(ms, client)
        if (ms->mode != NULL && connector_is_panel(modeset_connector(ms)))
            have_panel = 1;

    drm_client_for_each_modeset(ms, client) {
        if (ms->mode == NULL)
            continue;
        if (have_panel && !connector_is_panel(modeset_connector(ms))) {
            struct drm_connector *conn = modeset_connector(ms);
            printf("NullMoth display: leaving external output %s off (the panel owns the console bridge)\n",
                   conn != NULL && conn->name[0] != '\0' ? conn->name : "?");
            ms->mode = NULL;
            continue;
        }
        if (panel == NULL || (u64)ms->mode->hdisplay * ms->mode->vdisplay >
                             (u64)panel->mode->hdisplay * panel->mode->vdisplay)
            panel = ms;
    }
    return panel;
}

/* a monitor plugged or unplugged: probe again and put the panel back on the same buffer */
static int bridge_hotplug(struct drm_client_dev *client)
{
    struct nmx_fb *fb = container_of(client, struct nmx_fb, client);
    struct drm_mode_set *ms;
    if (fb->buffer == NULL)
        return 0;
    if (drm_client_modeset_probe(client, fb->info.width, fb->info.height) != 0)
        return 0;
    mutex_lock(&client->modeset_mutex);
    bridge_restrict_to_panel(client);
    drm_client_for_each_modeset(ms, client)
        if (ms->mode != NULL)
            ms->fb = fb->buffer->fb;
    mutex_unlock(&client->modeset_mutex);
    drm_client_modeset_commit(client);
    return 0;
}

static void bridge_free(struct nmx_fb *fb);

/* a userland KMS client (a DRM master) closed: put our scan-out back on the outputs */
static int bridge_restore(struct drm_client_dev *client)
{
    return drm_client_modeset_commit(client);
}

/*
 * The DRM device is going away (driver detach): the DRM core has taken the client off its list and hands it back
 * here to free. Once registered, this is the only place a bridge is freed.
 */
static void bridge_unregister(struct drm_client_dev *client)
{
    struct nmx_fb *fb = container_of(client, struct nmx_fb, client);
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].fb == fb) {
            fbs[i].dev = NULL;
            fbs[i].fb = NULL;
        }
    bridge_free(fb);
}

static const struct drm_client_funcs bridge_funcs = {
    .owner = NULL,
    .unregister = bridge_unregister,
    .restore = bridge_restore,
    .hotplug = bridge_hotplug,
};

static void bridge_free(struct nmx_fb *fb)
{
    if (fb->mapped)
        drm_client_buffer_vunmap(fb->buffer);
    if (fb->buffer != NULL)
        drm_client_framebuffer_delete(fb->buffer);
    drm_client_release(&fb->client);
    kfree(fb);
}

int nmx_fb_open(void *devp, struct nmx_fb_info *out)
{
    device_t dev = devp;
    struct drm_device *drm = drm_of(dev);
    struct drm_mode_set *ms;
    const struct drm_display_mode *best = NULL;
    const struct drm_mode_set *best_ms = NULL;
    int slot = -1, err;

    memset(out, 0, sizeof(*out));
    if (drm == NULL)
        return ENODEV;
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == NULL) { slot = i; break; }
    if (slot < 0)
        return ENOSPC;

    struct nmx_fb *fb = kzalloc(sizeof(*fb), GFP_KERNEL);
    if (fb == NULL)
        return ENOMEM;
    err = -drm_client_init(drm, &fb->client, "nmxfb", &bridge_funcs);
    if (err) {
        printf("NullMoth display: DRM client refused (%d)\n", err);
        kfree(fb);
        return err;
    }
    err = -drm_client_modeset_probe(&fb->client, 0, 0);
    if (err) {
        printf("NullMoth display: probing the outputs failed (%d)\n", err);
        goto fail;
    }
    /* the panel's mode; external outputs are disabled before the commit (see bridge_restrict_to_panel) */
    mutex_lock(&fb->client.modeset_mutex);
    best_ms = bridge_restrict_to_panel(&fb->client);
    if (best_ms != NULL)
        best = best_ms->mode;
    if (best != NULL) {
        out->width = best->hdisplay;
        out->height = best->vdisplay;
        out->pclk_khz = (uint32_t)best->clock;
        out->htotal = best->htotal;
        out->vtotal = best->vtotal;
        out->hsync_start = best->hsync_start;
        out->hsync_end = best->hsync_end;
        out->vsync_start = best->vsync_start;
        out->vsync_end = best->vsync_end;
        fb->display.hsync_positive = !!(best->flags & DRM_MODE_FLAG_PHSYNC);
        fb->display.vsync_positive = !!(best->flags & DRM_MODE_FLAG_PVSYNC);
        /* Same lock order as drm_client_modeset_probe: client then mode_config.
         * Copy the blob while protected; never retain a connector/blob pointer. */
        mutex_lock(&drm->mode_config.mutex);
        if (best_ms && best_ms->num_connectors) {
            struct drm_connector *conn = best_ms->connectors[0];
            struct drm_property_blob *blob = conn->edid_blob_ptr;
            fb->display.internal = conn->connector_type == DRM_MODE_CONNECTOR_eDP ||
                                   conn->connector_type == DRM_MODE_CONNECTOR_LVDS;
            fb->display.width_mm = conn->display_info.width_mm;
            fb->display.height_mm = conn->display_info.height_mm;
            if (blob && nmx_fb_edid_valid(blob->data, blob->length, sizeof(fb->display.edid))) {
                fb->display.edid_size = (uint32_t)blob->length;
                memcpy(fb->display.edid, blob->data, blob->length);
            }
        }
        mutex_unlock(&drm->mode_config.mutex);
    }
    mutex_unlock(&fb->client.modeset_mutex);
    if (best == NULL) {
        printf("NullMoth display: no output is connected\n");
        err = ENXIO;
        goto fail;
    }

    fb->buffer = fb_create_vram(fb, out->width, out->height, DRM_FORMAT_XRGB8888);   /* VRAM even with GPU VM */
    if (IS_ERR(fb->buffer)) {
        err = -PTR_ERR(fb->buffer);
        fb->buffer = NULL;
        printf("NullMoth display: no %ux%u scan-out buffer (%d)\n", out->width, out->height, err);
        goto fail;
    }
    out->pitch = fb->buffer->fb->pitches[0];

    err = -drm_client_buffer_vmap(fb->buffer, &fb->map);
    if (err) {
        printf("NullMoth display: the scan-out buffer would not map (%d)\n", err);
        goto fail;
    }
    fb->mapped = 1;

    /* the published range must be one physical run: check every page of the CPU view */
    const char *va = fb->map.is_iomem ? (const char *)fb->map.vaddr_iomem : (const char *)fb->map.vaddr;
    uint64_t size = (uint64_t)out->pitch * out->height;
    uint64_t base = nmx_xnu_kva_to_phys(va);
    for (uint64_t off = PAGE_SIZE; off < size; off += PAGE_SIZE)
        if (nmx_xnu_kva_to_phys(va + off) != base + off) {
            printf("NullMoth display: the scan-out buffer is not physically contiguous (page %llu)\n",
                   (unsigned long long)(off / PAGE_SIZE));
            err = EFAULT;
            goto fail;
        }
    if (base == 0) {
        err = EFAULT;
        goto fail;
    }
    out->phys = base;
    out->size = round_page(size);
    out->kva = (uint64_t)(uintptr_t)va;
    memset(__DECONST(char *, va), 0, size);       /* black until WindowServer draws */

    mutex_lock(&fb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &fb->client)
        if (ms->mode != NULL)
            ms->fb = fb->buffer->fb;
    mutex_unlock(&fb->client.modeset_mutex);
    nmx_xnu_stage("fb modeset");
    err = -drm_client_modeset_commit(&fb->client);
    nmx_xnu_stage(err ? "fb modeset FAILED" : "fb modeset done");
    if (err) {
        printf("NullMoth display: the mode set failed (%d)\n", err);
        goto fail;
    }
    drm_client_register(&fb->client);       /* from here on hotplug reaches bridge_hotplug */

    fb->info = *out;
    fbs[slot].dev = dev;
    fbs[slot].fb = fb;
    out->ok = 1;
    printf("NullMoth display: %ux%u, pitch %u, phys 0x%llx, %u kHz pixel clock\n", out->width, out->height,
           out->pitch, (unsigned long long)out->phys, out->pclk_khz);
    return 0;

fail:
    bridge_free(fb);
    memset(out, 0, sizeof(*out));
    return err ? err : EIO;
}

/*
 * The framebuffer is gone from macOS: blank the outputs. The client itself is freed by the DRM core when the driver
 * detaches (bridge_unregister), which the kext does right after.
 */
void nmx_fb_close(void *dev)
{
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev && fbs[i].fb != NULL)
            drm_client_modeset_dpms(&fbs[i].fb->client, DRM_MODE_DPMS_OFF);
}

/* display sleep and wake from macOS (IOFramebuffer power) */
int nmx_fb_power(void *dev, int on)
{
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev) {
            /* traced with timings: display sleep once hung the machine ~23 s later (docs/890M-TRACE.md) */
            struct timespec t0, t1;
            int err;
            nanouptime(&t0);
            printf("NullMoth display: power %s begin\n", on ? "on" : "off");
            err = -drm_client_modeset_dpms(&fbs[i].fb->client, on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
            nanouptime(&t1);
            printf("NullMoth display: power %s done err %d in %ld ms\n", on ? "on" : "off", err,
                   (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
            return err;
        }
    return ENODEV;
}

/* Called during IOFramebuffer construction, after open published the bridge. */
int nmx_fb_get_display_info(void *dev, struct nmx_fb_display_info *out)
{
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev && fbs[i].fb) {
            *out = fbs[i].fb->display;
            return 0;
        }
    return ENODEV;
}

/* ---- page flips (ga403wm Step98/99) -----------------------------------------------------------------------------
 *
 * NMXAccel (ci/nmxaccel-flip) calls NMXAMDFB::callPlatformFunction(name, false, req, ...) with these names:
 *   nmxFlipCaps     SUBMIT | VRAM_CHAIN | XRGB8888 (+ IMPORT | XRGB2101010 when DC runs with GPU VM), the lit mode
 *   nmxFlipBuffers  home (handle 0) + up to 2 more XRGB8888 VRAM buffers from the bridge's DRM client, all pinned
 *   nmxFlipImport   mode 1: the caller's IOSurface pages as a GTT scan-out buffer (zero copy), handles 8..11
 *   nmxFlipSubmit   non-blocking atomic commit of the primary plane's fb (DC fast update, latched at vblank)
 *   nmxFlipStatus   latched = the commit's flip_done; never sleeps
 *   nmxFlipRelease  busy while on screen or pending; frees an import
 * Pinning: amdgpu GEM objects have no .pin, so a buffer is pinned only while DC scans it. Flipping away from the home
 * buffer would let TTM evict it under the kva that IOFramebuffer and the K6 copy publish, so every chain buffer, home
 * included, is pinned in VRAM once and never unpinned.
 * Generation: the bridge commits its home fb itself (restore, hotplug, display power). Status notices the plane's fb
 * is not the one it last latched and bumps generation; current then names whatever is really on screen.
 */
#include <linux/scatterlist.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/drm_gem.h>
#include <drm/drm_vblank.h>
#include <drm/drm_modeset_lock.h>
#include "drm_crtc_internal.h"
#include "amdgpu.h"

#define FLIP_R_OK          0
#define FLIP_R_UNSUPPORTED 0xe00002c7u
#define FLIP_R_BADARG      0xe00002c2u
#define FLIP_R_BUSY        0xe00002d5u
#define FLIP_R_NOTREADY    0xe00002d8u
#define FLIP_R_ERROR       0xe00002bcu
#define FLIP_NONE          0xffffffffu
#define FLIP_NBUF          3
#define FLIP_IMP0          8u
#define FLIP_NIMP          4u

/* the NMXAccel request (NMXFlipRequest, version 1) */
struct nmx_flipbuf { uint64_t phys; void *kva; uint64_t size; uint64_t gpu; uint32_t handle, pitch; };
struct nmx_flipreq {
    uint32_t version, flags, handle, format, width, height, pitch, caps, maxImports, nbuffers;
    void *md; uint64_t offset, gpuAddr, seq, seqSubmitted, seqLatched;
    uint32_t cur, pending, generation; int err;   /* NMXAccel: current */
    uint64_t vblankCount, vblankNs;
    struct nmx_flipbuf buf[FLIP_NBUF];
};
_Static_assert(__builtin_offsetof(struct nmx_flipreq, buf) == 120, "NMXFlipRequest layout");
_Static_assert(sizeof(struct nmx_flipbuf) == 40, "NMXFlipBuffer layout");

struct nmx_flip_import {
    struct drm_framebuffer *fb;
    struct drm_gem_object *gem;
    struct sg_table *sgt;
    void *md;
    uint64_t gpu;
    uint32_t fbid, gemh, fmt, used;     /* used: 0 free, 1 live, 2 being built or torn down */
};

static struct {
    spinlock_t lock;
    int lock_ready;
    uint32_t inited, gen, cur, pending, nbuf, logs, tried;
    uint64_t seq_sub, seq_lat;
    struct drm_crtc_commit *commit;     /* the pending flip's commit (one reference held) */
    struct nmx_fb *nfb;
    struct drm_crtc *crtc;
    struct drm_plane *plane;
    struct drm_framebuffer *drmfb[FLIP_NBUF];
    struct drm_client_buffer *buffer[FLIP_NBUF];   /* [0] = the bridge's home buffer */
    uint64_t kva[FLIP_NBUF], phys[FLIP_NBUF], size[FLIP_NBUF], gpu[FLIP_NBUF];
    uint32_t pitch[FLIP_NBUF];
    struct nmx_flip_import imp[FLIP_NIMP];
    uint32_t nimports, import_fail;
} flip;

#define FLIP_LOG(...) do { if (flip.logs < 40) { flip.logs++; printf(__VA_ARGS__); } } while (0)

static struct amdgpu_device *flip_adev(struct nmx_fb *nfb)
{
    return drm_to_adev(nfb->client.dev);
}

/* DC scans GTT buffers only when it was brought up with GPU VM (Step99: amdgpu_sg_display stays -1) */
static int flip_gpu_vm(struct nmx_fb *nfb)
{
    return flip_adev(nfb)->mode_info.gpu_vm_support ? 1 : 0;
}

/*
 * With GPU VM, amdgpu offers GTT for scan-out buffers and an APU may place a dumb buffer there. The bridge's own
 * buffers must stay one contiguous VRAM range (the IOFramebuffer aperture and the K6 copy depend on it), so GPU VM
 * support is hidden from amdgpu_display while they are made.
 */
static struct drm_client_buffer *fb_create_vram(struct nmx_fb *nfb, uint32_t w, uint32_t h, uint32_t fmt)
{
    struct amdgpu_device *adev = flip_adev(nfb);
    bool was = adev->mode_info.gpu_vm_support;
    struct drm_client_buffer *b;

    adev->mode_info.gpu_vm_support = false;
    b = drm_client_framebuffer_create(&nfb->client, w, h, fmt);
    adev->mode_info.gpu_vm_support = was;
    return b;
}

static struct drm_framebuffer *flip_fb_of(uint32_t h)
{
    if (h < flip.nbuf)
        return flip.drmfb[h];
    if (h >= FLIP_IMP0 && h < FLIP_IMP0 + FLIP_NIMP && flip.imp[h - FLIP_IMP0].used == 1)
        return flip.imp[h - FLIP_IMP0].fb;
    return NULL;
}

/* pin a client buffer's BO in VRAM (never unpinned); its MC address goes to gpu */
static int flip_pin(struct drm_client_buffer *buffer, uint64_t *gpu)
{
    struct amdgpu_bo *bo = gem_to_amdgpu_bo(buffer->gem);
    int e = amdgpu_bo_reserve(bo, false);

    if (e)
        return e;
    e = amdgpu_bo_pin(bo, AMDGPU_GEM_DOMAIN_VRAM);
    if (!e)
        *gpu = amdgpu_bo_gpu_offset(bo);
    amdgpu_bo_unreserve(bo);
    return e;
}

static int flip_init(void)
{
    struct nmx_fb *nfb = fbs[0].fb;
    struct drm_mode_set *ms;
    struct drm_crtc *crtc = NULL;
    int e;

    if (flip.inited)
        return 1;
    if (!nfb || !nfb->info.kva || !nfb->info.size || !nfb->buffer)
        return 0;
    mutex_lock(&nfb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &nfb->client)
        if (ms->mode != NULL && ms->crtc != NULL) { crtc = ms->crtc; break; }
    mutex_unlock(&nfb->client.modeset_mutex);
    if (!crtc || !crtc->primary)
        return 0;
    if (!flip.lock_ready) {
        spin_lock_init(&flip.lock);
        flip.lock_ready = 1;
    }
    flip.nfb = nfb;
    flip.crtc = crtc;
    flip.plane = crtc->primary;
    flip.buffer[0] = nfb->buffer;
    flip.drmfb[0] = nfb->buffer->fb;
    flip.kva[0] = nfb->info.kva;
    flip.phys[0] = nfb->info.phys;
    flip.size[0] = nfb->info.size;
    flip.pitch[0] = nfb->info.pitch;
    flip.cur = 0;
    flip.pending = FLIP_NONE;
    flip.nbuf = 1;
    /* the home buffer must stay in VRAM whatever is on screen: DC unpins it when a flip moves away from it */
    e = flip_pin(flip.buffer[0], &flip.gpu[0]);
    if (e) {
        FLIP_LOG("NMX flip: home buffer would not pin (%d), no flips\n", e);
        return 0;
    }
    __atomic_store_n(&flip.inited, 1, __ATOMIC_RELEASE);
    FLIP_LOG("NMX flip: API up, crtc %p plane %p home fb %p %ux%u pitch %u\n", crtc, flip.plane, flip.drmfb[0],
             nfb->info.width, nfb->info.height, flip.pitch[0]);
    return 1;
}

static uint32_t flip_buffers(struct nmx_flipreq *r)
{
    if (!flip.tried) {
        uint32_t w = flip.nfb->info.width, h = flip.nfb->info.height;

        flip.tried = 1;
        for (uint32_t i = 1; i < FLIP_NBUF; i++) {
            struct drm_client_buffer *b = fb_create_vram(flip.nfb, w, h, DRM_FORMAT_XRGB8888);
            struct iosys_map map;
            const char *va;
            uint64_t size, base, off;
            int e;

            if (IS_ERR_OR_NULL(b)) {
                FLIP_LOG("NMX flip: buffer %u not created (%ld)\n", i, (long)PTR_ERR(b));
                break;
            }
            if ((e = flip_pin(b, &flip.gpu[i]))) {
                FLIP_LOG("NMX flip: buffer %u would not pin (%d)\n", i, e);
                drm_client_framebuffer_delete(b);
                break;
            }
            if ((e = drm_client_buffer_vmap(b, &map))) {    /* stays pinned: never freed */
                FLIP_LOG("NMX flip: buffer %u would not map (%d)\n", i, e);
                break;
            }
            va = map.is_iomem ? (const char *)map.vaddr_iomem : (const char *)map.vaddr;
            size = (uint64_t)b->pitch * h;
            base = nmx_xnu_kva_to_phys(va);
            for (off = PAGE_SIZE; off < size; off += PAGE_SIZE)
                if (nmx_xnu_kva_to_phys(va + off) != base + off)
                    break;
            if (!base || off < size) {
                FLIP_LOG("NMX flip: buffer %u not contiguous (page %llu)\n", i, (unsigned long long)(off / PAGE_SIZE));
                break;
            }
            flip.buffer[i] = b;
            flip.drmfb[i] = b->fb;
            flip.kva[i] = (uint64_t)(uintptr_t)va;
            flip.phys[i] = base;
            flip.size[i] = round_page(size);
            flip.pitch[i] = b->pitch;
            flip.nbuf = i + 1;
            FLIP_LOG("NMX flip: buffer %u fb %p phys %llx gpu %llx pitch %u\n", i, flip.drmfb[i],
                     (unsigned long long)base, (unsigned long long)flip.gpu[i], b->pitch);
        }
        FLIP_LOG("NMX flip: chain of %u (home gpu %llx)\n", flip.nbuf, (unsigned long long)flip.gpu[0]);
    }
    r->nbuffers = flip.nbuf;
    for (uint32_t i = 0; i < flip.nbuf; i++) {
        r->buf[i].phys = flip.phys[i];
        r->buf[i].kva = (void *)(uintptr_t)flip.kva[i];
        r->buf[i].size = flip.size[i];
        r->buf[i].gpu = flip.gpu[i];
        r->buf[i].handle = i;
        r->buf[i].pitch = flip.pitch[i];
    }
    return flip.nbuf > 1 ? FLIP_R_OK : FLIP_R_ERROR;
}

/* lock held. Retire a pending flip whose commit has signalled flip_done; notice a bridge commit. */
static void flip_refresh(void)
{
    if (flip.commit && __atomic_load_n(&flip.commit->flip_done.done, __ATOMIC_ACQUIRE)) {
        drm_crtc_commit_put(flip.commit);
        flip.commit = NULL;
        flip.cur = flip.pending;
        flip.pending = FLIP_NONE;
        flip.seq_lat = flip.seq_sub;
    }
    if (flip.pending == FLIP_NONE) {
        struct drm_plane_state *ps = flip.plane->state;
        struct drm_framebuffer *fb = ps ? ps->fb : NULL;

        if (fb && fb != flip_fb_of(flip.cur)) {
            uint32_t h = FLIP_NONE;

            for (uint32_t i = 0; i < FLIP_IMP0 + FLIP_NIMP; i++)
                if (flip_fb_of(i) == fb)
                    h = i;
            flip.gen++;
            FLIP_LOG("NMX flip: on screen is fb %p (handle %d), not handle %u: generation %u\n", fb, (int)h,
                     flip.cur, flip.gen);
            flip.cur = h == FLIP_NONE ? 0 : h;
        }
    }
}

static uint32_t flip_submit(struct nmx_flipreq *r)
{
    uint32_t h = r->handle;
    struct drm_framebuffer *nfbp = flip_fb_of(h);
    struct drm_modeset_acquire_ctx ctx;
    struct drm_atomic_state *state;
    struct drm_crtc_commit *commit = NULL;
    int e = -ENOMEM;

    if (!nfbp)
        return FLIP_R_BADARG;
    spin_lock(&flip.lock);
    flip_refresh();
    if (flip.pending != FLIP_NONE) {
        spin_unlock(&flip.lock);
        return FLIP_R_BUSY;
    }
    flip.pending = h;           /* reserved; status reports it pending from here */
    spin_unlock(&flip.lock);

    drm_modeset_acquire_init(&ctx, 0);
    state = drm_atomic_state_alloc(flip.nfb->client.dev);
    if (state) {
        state->acquire_ctx = &ctx;
        for (;;) {
            struct drm_plane_state *ps = drm_atomic_get_plane_state(state, flip.plane);
            struct drm_crtc_state *cs;

            if (IS_ERR(ps)) {
                e = PTR_ERR(ps);
                goto deadlk;
            }
            drm_atomic_set_fb_for_plane(ps, nfbp);
            cs = drm_atomic_get_crtc_state(state, flip.crtc);
            if (IS_ERR(cs)) {
                e = PTR_ERR(cs);
                goto deadlk;
            }
            e = drm_atomic_nonblocking_commit(state);
            if (!e) {
                commit = cs->commit;    /* locks still held: nothing can swap the CRTC state yet */
                if (commit)
                    drm_crtc_commit_get(commit);
                break;
            }
deadlk:
            if (e != -EDEADLK)
                break;
            drm_atomic_state_clear(state);
            drm_modeset_backoff(&ctx);
        }
        drm_atomic_state_put(state);
    }
    drm_modeset_drop_locks(&ctx);
    drm_modeset_acquire_fini(&ctx);

    spin_lock(&flip.lock);
    if (e || !commit) {
        flip.pending = FLIP_NONE;
        if (commit)
            drm_crtc_commit_put(commit);
        spin_unlock(&flip.lock);
        FLIP_LOG("NMX flip: submit of handle %u failed (%d%s)\n", h, e, e ? "" : ", no commit");
        r->err = e ? e : -EINVAL;
        return e == -EBUSY ? FLIP_R_BUSY : FLIP_R_ERROR;
    }
    flip.commit = commit;
    r->seq = ++flip.seq_sub;
    spin_unlock(&flip.lock);
    return FLIP_R_OK;
}

static uint32_t flip_status(struct nmx_flipreq *r)
{
    ktime_t t = 0;

    spin_lock(&flip.lock);
    flip_refresh();
    r->seqSubmitted = flip.seq_sub;
    r->seqLatched = flip.seq_lat;
    r->cur = flip.cur;
    r->pending = flip.pending;
    r->generation = flip.gen;
    spin_unlock(&flip.lock);
    r->vblankCount = drm_crtc_vblank_count_and_time(flip.crtc, &t);
    r->vblankNs = (uint64_t)ktime_to_ns(t);
    return FLIP_R_OK;
}

static uint32_t flip_import_fail(struct nmx_flipreq *r, struct nmx_flip_import *m, const char *what, int e)
{
    if (m)
        m->used = 0;
    flip.import_fail++;
    FLIP_LOG("NMX flip: import failed at %s (%d)\n", what, e);
    r->err = e;
    return FLIP_R_ERROR;
}

/*
 * An import makes a scan-out buffer out of the caller's memory (CoreDisplay's IOSurface, a prepared descriptor):
 *   1. the descriptor's physical pages -> a flat sg_table (dma address = physical address, xkpi maps 1:1)
 *   2. amdgpu_gem_object_create(CPU domain, CPU_GTT_USWC, ttm_bo_type_sg), the sg_table set on the BO and its ttm_tt
 *      (TTM created the tt already, with TTM_TT_FLAG_EXTERNAL, so amdgpu binds the dma addresses from it)
 *   3. pinned in GTT, a GART range bound: gpu address = what DCN fetches
 *   4. a GEM handle on the bridge's DRM file, drm_mode_addfb2 (linear, the surface's pitch)
 * The descriptor stays prepared by the caller until release. Release unpins and drops the fb and the handle; the
 * sg_table is not freed (the BO may outlive the call until DRM drops its last fb reference).
 */
static uint32_t flip_import(struct nmx_flipreq *r)
{
    struct amdgpu_device *adev;
    struct nmx_flip_import *m = NULL;
    struct drm_gem_object *gem = NULL;
    struct drm_framebuffer *fb;
    struct drm_mode_fb_cmd2 c;
    struct drm_file *file = flip.nfb->client.file;
    struct drm_device *dev = flip.nfb->client.dev;
    struct sg_table *sgt;
    struct scatterlist *sg;
    struct amdgpu_bo *bo;
    uint64_t size, n, i, runs = 0, gpu = 0, prev = 0, len, pa;
    uint32_t fmt, slot = FLIP_NONE, h = 0;
    int err;

    if (!flip_gpu_vm(flip.nfb))
        return FLIP_R_UNSUPPORTED;
    if (!r->md || !r->width || !r->height || r->pitch < 4 * r->width || (r->pitch & 255))
        return FLIP_R_BADARG;
    fmt = r->format == 1 ? DRM_FORMAT_XRGB8888 : r->format == 2 ? DRM_FORMAT_XRGB2101010 : 0;
    if (!fmt)
        return FLIP_R_BADARG;
    spin_lock(&flip.lock);
    for (i = 0; i < FLIP_NIMP; i++)
        if (!flip.imp[i].used) { slot = (uint32_t)i; flip.imp[i].used = 2; break; }
    spin_unlock(&flip.lock);
    if (slot == FLIP_NONE)
        return FLIP_R_BUSY;
    m = &flip.imp[slot];

    size = round_page((uint64_t)r->pitch * r->height);
    n = size / PAGE_SIZE;
    for (i = 0; i < n; i++) {               /* runs of physically adjacent pages */
        len = 0;
        pa = nmx_xnu_md_phys(r->md, r->offset + i * PAGE_SIZE, &len);
        /* ga403wm Step131: LinuxKPI's PAGE_MASK is Linux's ~(PAGE_SIZE - 1) in this file, so "pa & PAGE_MASK" was
         * non-zero for every page and each import failed at page 0 (Step129/130: every frame took NMXAccel's CPU copy,
         * ~100 fps x 20.7 MB). The cave build tested "pa & 4095". Test the in-page bits explicitly. */
        if (!pa || (pa & (PAGE_SIZE - 1)))
            return flip_import_fail(r, m, "descriptor page", (int)i);
        if (i == 0 || pa != prev + PAGE_SIZE)
            runs++;
        prev = pa;
    }
    sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
    if (!sgt || sg_alloc_table(sgt, (unsigned int)runs, GFP_KERNEL)) {
        kfree(sgt);
        return flip_import_fail(r, m, "sg alloc", -ENOMEM);
    }
    sg = NULL;
    for (i = 0; i < n; i++) {
        len = 0;
        pa = nmx_xnu_md_phys(r->md, r->offset + i * PAGE_SIZE, &len);
        if (sg && sg_dma_address(sg) + sg_dma_len(sg) == pa) {
            sg_dma_len(sg) += PAGE_SIZE;
            sg->length += PAGE_SIZE;
            continue;
        }
        sg = sg ? sg_next(sg) : sgt->sgl;
        sg_dma_address(sg) = pa;
        sg_dma_len(sg) = PAGE_SIZE;
        sg->length = PAGE_SIZE;
    }

    adev = flip_adev(flip.nfb);
    err = amdgpu_gem_object_create(adev, size, PAGE_SIZE, AMDGPU_GEM_DOMAIN_CPU, AMDGPU_GEM_CREATE_CPU_GTT_USWC,
                                   ttm_bo_type_sg, NULL, &gem, 0);
    if (err || !gem)
        return flip_import_fail(r, m, "gem create", err);   /* sg_table leaked */
    bo = gem_to_amdgpu_bo(gem);
    bo->tbo.sg = sgt;
    if (!bo->tbo.ttm || !(bo->tbo.ttm->page_flags & TTM_TT_FLAG_EXTERNAL)) {
        int f = bo->tbo.ttm ? (int)bo->tbo.ttm->page_flags : -1;

        drm_gem_object_put(gem);
        return flip_import_fail(r, m, "no external tt", f);
    }
    bo->tbo.ttm->sg = sgt;

    err = amdgpu_bo_reserve(bo, false);
    if (!err) {
        err = amdgpu_bo_pin(bo, AMDGPU_GEM_DOMAIN_GTT);
        if (!err && (err = amdgpu_ttm_alloc_gart(&bo->tbo)))
            amdgpu_bo_unpin(bo);
        if (!err)
            gpu = amdgpu_bo_gpu_offset(bo);
        amdgpu_bo_unreserve(bo);
    }
    if (err) {
        drm_gem_object_put(gem);
        return flip_import_fail(r, m, "pin gtt", err);
    }
    if ((err = drm_gem_handle_create(file, gem, &h))) {
        if (!amdgpu_bo_reserve(bo, false)) {
            amdgpu_bo_unpin(bo);
            amdgpu_bo_unreserve(bo);
        }
        drm_gem_object_put(gem);
        return flip_import_fail(r, m, "handle", err);
    }
    drm_gem_object_put(gem);                /* the handle holds the object now */
    memset(&c, 0, sizeof(c));
    c.width = r->width;
    c.height = r->height;
    c.pixel_format = fmt;
    c.handles[0] = h;
    c.pitches[0] = r->pitch;
    if ((err = drm_mode_addfb2(dev, &c, file)) || !(fb = drm_framebuffer_lookup(dev, file, c.fb_id))) {
        if (!amdgpu_bo_reserve(bo, false)) {
            amdgpu_bo_unpin(bo);
            amdgpu_bo_unreserve(bo);
        }
        drm_gem_handle_delete(file, h);
        return flip_import_fail(r, m, "addfb2", err ? err : -ENOENT);
    }
    drm_framebuffer_put(fb);                /* the file's fb list holds it */
    m->fb = fb;
    m->gem = gem;
    m->sgt = sgt;
    m->md = r->md;
    m->gpu = gpu;
    m->fbid = c.fb_id;
    m->gemh = h;
    m->fmt = fmt;
    __atomic_store_n(&m->used, 1, __ATOMIC_RELEASE);
    flip.nimports++;
    r->handle = FLIP_IMP0 + slot;
    r->gpuAddr = gpu;
    FLIP_LOG("NMX flip: import %u: %ux%u pitch %u fmt %x, %llu pages in %llu runs, gpu %llx, fb %p\n",
             FLIP_IMP0 + slot, r->width, r->height, r->pitch, fmt, (unsigned long long)n, (unsigned long long)runs,
             (unsigned long long)gpu, fb);
    return FLIP_R_OK;
}

/* the caller has checked the handle is neither on screen nor pending */
static uint32_t flip_release_import(uint32_t h)
{
    struct nmx_flip_import *m;
    struct amdgpu_bo *bo;

    if (h < FLIP_IMP0 || h >= FLIP_IMP0 + FLIP_NIMP || flip.imp[h - FLIP_IMP0].used != 1)
        return FLIP_R_BADARG;
    m = &flip.imp[h - FLIP_IMP0];
    bo = gem_to_amdgpu_bo(m->gem);
    spin_lock(&flip.lock);
    m->used = 2;
    spin_unlock(&flip.lock);
    if (!amdgpu_bo_reserve(bo, false)) {
        amdgpu_bo_unpin(bo);
        amdgpu_bo_unreserve(bo);
    }
    drm_mode_rmfb(flip.nfb->client.dev, m->fbid, flip.nfb->client.file);
    drm_gem_handle_delete(flip.nfb->client.file, m->gemh);
    m->fb = NULL;
    m->gem = NULL;
    __atomic_store_n(&m->used, 0, __ATOMIC_RELEASE);
    FLIP_LOG("NMX flip: released import %u\n", h);
    return FLIP_R_OK;
}

/* -1: not a flip call (IOFramebuffer handles it); otherwise the IOReturn */
long long nmx_fb_flip_call(const char *name, void *req)
{
    struct nmx_flipreq *r = req;
    int fn;

    if (strncmp(name, "nmxFlip", 7) != 0)
        return -1;
    fn = !strcmp(name, "nmxFlipStatus") ? 5 : !strcmp(name, "nmxFlipSubmit") ? 4 : !strcmp(name, "nmxFlipCaps") ? 0 :
         !strcmp(name, "nmxFlipBuffers") ? 2 : !strcmp(name, "nmxFlipRelease") ? 3 :
         !strcmp(name, "nmxFlipImport") ? 1 : -1;
    if (fn < 0)
        return -1;
    {
        static int marked;          /* ga403wm Step128: the first flip call of the boot */
        if (!marked) {
            marked = 1;
            nmx_xnu_stage(name);
        }
    }
    if (!r || r->version != 1)
        return FLIP_R_BADARG;
    if (!flip_init())
        return FLIP_R_NOTREADY;
    r->err = 0;
    switch (fn) {
    case 0:
        r->caps = 1u | 4u | 0x100u;
        r->maxImports = 0;
        if (flip_gpu_vm(flip.nfb)) {
            r->caps |= 2u | 0x200u;
            r->maxImports = FLIP_NIMP;
        }
        r->width = flip.nfb->info.width;
        r->height = flip.nfb->info.height;
        r->pitch = flip.pitch[0];
        r->generation = flip.gen;
        return FLIP_R_OK;
    case 1:
        return flip_import(r);
    case 2:
        return flip_buffers(r);
    case 3: {
        uint32_t busy;

        spin_lock(&flip.lock);
        flip_refresh();
        busy = r->handle == flip.cur || r->handle == flip.pending;
        spin_unlock(&flip.lock);
        if (busy)
            return FLIP_R_BUSY;
        return r->handle >= FLIP_IMP0 ? flip_release_import(r->handle) : FLIP_R_OK;
    }
    case 4:
        return flip_submit(r);
    default:
        return flip_status(r);
    }
}
