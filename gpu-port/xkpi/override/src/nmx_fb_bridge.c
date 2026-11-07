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

void nmx_fb_before_attach(void)
{
    amdgpu_sg_display = 0;
}

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

    fb->buffer = drm_client_framebuffer_create(&fb->client, out->width, out->height, DRM_FORMAT_XRGB8888);
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
    err = -drm_client_modeset_commit(&fb->client);
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
