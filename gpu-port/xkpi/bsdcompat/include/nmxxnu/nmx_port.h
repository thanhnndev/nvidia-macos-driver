/* The kext's interface to the port (implemented in src/nmx_port.c); plain types only, shared with the C++ side. */
#ifndef NMX_PORT_H
#define NMX_PORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int nmx_port_start(void);
void nmx_port_stop(void);
/* device-node opens still outstanding: the kext must not stop while this is nonzero */
int nmx_port_busy(void);
void *nmx_port_attach(void *iopci);
void *nmx_port_attach_pci(void *iopci);
void nmx_port_detach(void *dev);
int nmx_port_power(void *dev, int on);
int nmx_port_shutdown(void *dev);

/* the display bridge (override/src/nmx_fb_bridge.c): one scan-out buffer on every lit output of the device */
struct nmx_fb_info {
    int ok;
    uint32_t width, height, pitch;      /* the buffer: XRGB8888, 'pitch' bytes per row */
    uint64_t phys, size;                /* its physical range (contiguous, through the GPU aperture) */
    uint64_t kva;                       /* its kernel mapping */
    uint32_t pclk_khz;                  /* timing of the largest output's mode */
    uint32_t htotal, vtotal, hsync_start, hsync_end, vsync_start, vsync_end;
};
/* before the driver attaches: settings the bridge needs from the driver (scan-out buffers in contiguous VRAM) */
void nmx_fb_before_attach(void);
int nmx_fb_open(void *dev, struct nmx_fb_info *out);
/* Immutable identity snapshot of the connector whose mode sized the scanout.
 * Separate from nmx_fb_info to preserve its existing layout. No synthesized EDID. */
struct nmx_fb_display_info {
    uint32_t edid_size, internal, width_mm, height_mm;
    uint32_t hsync_positive, vsync_positive;
    uint8_t edid[1024];
};
int nmx_fb_get_display_info(void *dev, struct nmx_fb_display_info *out);
void nmx_fb_close(void *dev);
int nmx_fb_power(void *dev, int on);
/* hardware cursor on the CRTC's cursor plane (nmx_fb_bridge.c, end of file); NMXAMDFB is the only caller */
int nmx_fb_cursor_init(uint32_t *side);
/* the kInvertingEncoding value NMXAMDFB offers IOFramebuffer (see nmx_fb_cursor.h) */
#define NMX_CUR_INVERT_PIXEL 0x00ffffffu
void nmx_fb_cursor_set_kick(void (*fn)(void *), void *arg);
int nmx_fb_cursor_apply(const uint32_t *img, uint32_t w, uint32_t h, uint32_t seq, int32_t x, int32_t y, int visible);
int nmx_fb_cursor_off(void);
extern uint64_t gNmxCurCommits, gNmxCurFull, gNmxCurImages, gNmxCurErrors, gNmxCurRearms, gNmxCurOff;
/* hardware colour stages (nmx_fb_bridge.c, end of file): CRTC DEGAMMA_LUT / CTM / GAMMA_LUT staged for the next flip */
struct nmx_color_lut;
int nmx_fb_color_stage(const struct nmx_color_lut *degamma, const uint64_t *ctm, const struct nmx_color_lut *gamma);
int nmx_fb_color_commit(void);
extern uint64_t gNmxColorStaged, gNmxColorFlip, gNmxColorAlone, gNmxColorErrors, gNmxColorRejected;

#ifdef __cplusplus
}
#endif
#endif
