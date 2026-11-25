/*
 * Hardware cursor (2026-11-25): the pure pieces shared by the bridge (override/src/nmx_fb_bridge.c) and its unit test
 * (tests/cursor_math_test.c). No kernel headers.
 *
 * IOFramebuffer::convertCursorImage() hands over straight (un-premultiplied) ARGB: it divides each colour by alpha
 * (IOGraphics IOFramebuffer.cpp, "red = 0xff * red / alpha"). amdgpu_dm programs the DCN cursor as
 * CURSOR_MODE_COLOR_PRE_MULTIPLIED_ALPHA (amdgpu_dm_plane_handle_cursor_update), so every pixel is multiplied back
 * before it reaches the cursor buffer; a straight pixel shown as premultiplied gives the bright/dark fringe on the
 * cursor's anti-aliased edge.
 */
#ifndef NMX_FB_CURSOR_H
#define NMX_FB_CURSOR_H

#if !defined(_KERNEL) && !defined(KERNEL)
#include <stdint.h>      /* the kernel sides have the fixed-width types already */
#endif

/* straight ARGB8888 -> premultiplied ARGB8888, rounded */
static inline uint32_t nmx_cur_premul(uint32_t s)
{
    const uint32_t a = s >> 24;
    if (a == 0xff)
        return s;
    if (a == 0)
        return 0;
    const uint32_t r = (((s >> 16) & 0xff) * a + 127) / 255;
    const uint32_t g = (((s >> 8) & 0xff) * a + 127) / 255;
    const uint32_t b = ((s & 0xff) * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* side of the square cursor buffer: amdgpu_dm accepts a cursor FB whose pitch equals its width and is 64, 128 or 256
 * pixels and no larger than the CRTC's max_cursor_width/height (dm_check_cursor_fb); 0 = no usable size */
static inline uint32_t nmx_cur_buf_side(uint32_t max_w, uint32_t max_h)
{
    const uint32_t m = max_w < max_h ? max_w : max_h;
    return m >= 256 ? 256 : m >= 128 ? 128 : m >= 64 ? 64 : 0;
}

/* position that hides the cursor without leaving the async path: amdgpu_dm_plane_get_cursor_position() reports
 * enable = false for crtc_x <= -max_cursor_width, and amdgpu_dm_plane_handle_cursor_update() then programs a disabled
 * cursor (its own "turn off cursor" branch); the plane keeps its CRTC and FB, so showing it again is async too */
static inline int32_t nmx_cur_hidden_xy(uint32_t max_cursor)
{
    return -(int32_t)max_cursor;
}

/*
 * Copy one converted image (rows of w straight pixels, h rows; IOHardwareCursorInfo cursorWidth/cursorHeight) into a
 * side x side premultiplied buffer, top-left aligned, the rest transparent. dst is written with 32-bit stores only
 * (it is VRAM behind the BAR). Returns 0, or -1 when the image does not fit.
 */
static inline int nmx_cur_fill(volatile uint32_t *dst, uint32_t side, const uint32_t *src, uint32_t w, uint32_t h)
{
    if (w > side || h > side)
        return -1;
    for (uint32_t y = 0; y < side; y++)
        for (uint32_t x = 0; x < side; x++)
            dst[(uint64_t)y * side + x] = (y < h && x < w) ? nmx_cur_premul(src[(uint64_t)y * w + x]) : 0;
    return 0;
}

#endif
