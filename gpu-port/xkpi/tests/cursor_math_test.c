/*
 * Hardware cursor (2026-11-25): user-space test of the pure cursor math against the code it has to agree with.
 *
 *   cc -O2 -Wall -o /tmp/cursor_math_test tests/cursor_math_test.c && /tmp/cursor_math_test
 *
 * 1. nmx_cur_premul against IOFramebuffer::convertCursorImage's un-premultiply (IOGraphics IOFramebuffer.cpp,
 *    "red = 0xff * red / alpha", stored "& 0xff"): every premultiplied ARGB pixel CoreGraphics can hand over (c <= a)
 *    comes back within 1 of itself after the round trip (no dark or bright fringe on anti-aliased edges).
 * 2. The position path end to end: IOFramebuffer's hot-spot padding (convertCursorImage adjX/adjY, _setCursorState
 *    "x -= cursorHotSpotAdjust") -> our crtc_x/crtc_y -> amdgpu_dm_plane_get_cursor_position (copied below verbatim in
 *    logic) -> what DCN shows: the screen pixel at the pointer location always shows the image's hot-spot pixel, for
 *    every location including the top/left edges where DM moves the cursor to 0 and programs x_hotspot/y_hotspot, and
 *    the hidden position (nmx_cur_hidden_xy) makes DM report the cursor disabled.
 * 3. nmx_cur_fill (top-left aligned, rest transparent, refuses an image larger than the buffer) and nmx_cur_buf_side
 *    (64/128/256 within DC's max, as dm_check_cursor_fb accepts).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bsdcompat/include/nmx_fb_cursor.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* IOFramebuffer::convertCursorImage, 32-bit direct path, one pixel (alpha 0 non-white = 0; inverting not offered) */
static uint32_t iofb_unpremul(uint32_t s)
{
    uint32_t a = s >> 24, r = (s >> 16) & 0xff, g = (s >> 8) & 0xff, b = s & 0xff;
    if (a == 0)
        return 0;
    if (a != 0xff) {
        r = 0xff * r / a;
        g = 0xff * g / a;
        b = 0xff * b / a;
    }
    return (a << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
}

/* amdgpu_dm_plane_get_cursor_position (amdgpu_dm_plane.c), the position part */
struct dc_pos { int enable, x, y, x_hotspot, y_hotspot; };
static void dm_cursor_position(int crtc_x, int crtc_y, int max_w, int max_h, struct dc_pos *p)
{
    int x = crtc_x, y = crtc_y, xorigin = 0, yorigin = 0;

    memset(p, 0, sizeof *p);
    if (x <= -max_w || y <= -max_h)
        return;                                     /* enable stays false: DM programs the cursor off */
    if (x < 0) {
        xorigin = -x < max_w - 1 ? -x : max_w - 1;
        x = 0;
    }
    if (y < 0) {
        yorigin = -y < max_h - 1 ? -y : max_h - 1;
        y = 0;
    }
    p->enable = 1;
    p->x = x;
    p->y = y;
    p->x_hotspot = xorigin;
    p->y_hotspot = yorigin;
}

/* DCN: cursor buffer pixel (bx, by) is drawn at screen (pos.x - x_hotspot + bx, pos.y - y_hotspot + by), clipped to
 * the screen (dcn10_set_cursor_position: hot spot subtracted from the position) */
static int dcn_buffer_at(const struct dc_pos *p, int sx, int sy, int side, int *bx, int *by)
{
    if (!p->enable)
        return 0;
    *bx = sx - p->x + p->x_hotspot;
    *by = sy - p->y + p->y_hotspot;
    return *bx >= 0 && *by >= 0 && *bx < side && *by < side;
}

/* IOFramebuffer::convertCursorImage's hot-spot padding: adj = 4 - hotspot when that is >= 0 and the padded image still
 * fits the (halved) descriptor */
static int iofb_adj(int hs, int img, int out)
{
    int adj = 4 - hs;
    if (adj < 0 || img + adj > out)
        adj = 0;
    return adj;
}

int main(void)
{
    const int W = 2880, H = 1800, SIDE = 256, MAX = 256;

    /* 1: premultiply round trip, every alpha and colour value with c <= a */
    for (uint32_t a = 0; a <= 255; a++)
        for (uint32_t c = 0; c <= a; c++) {
            const uint32_t src = (a << 24) | (c << 16) | ((a - c) << 8) | (c / 2);
            const uint32_t out = nmx_cur_premul(iofb_unpremul(src));
            for (int sh = 0; sh <= 16; sh += 8) {
                const int d = (int)((out >> sh) & 0xff) - (int)((src >> sh) & 0xff);
                CHECK(d >= -1 && d <= 1, "premul a=%u c=%u shift %d: %08x -> %08x", a, c, sh, src, out);
            }
            CHECK((out >> 24) == a, "alpha changed %08x -> %08x", src, out);
        }
    CHECK(nmx_cur_premul(0x00ffffff) == 0, "alpha 0 must be fully transparent");
    CHECK(nmx_cur_premul(0xff123456) == 0xff123456, "opaque unchanged");

    /* 2: pointer location -> screen pixel shows the hot spot; image sizes as IOFramebuffer halves them */
    const int imgs[][2] = { { 32, 32 }, { 64, 64 }, { 128, 128 }, { 100, 37 }, { 252, 252 } };
    const int hss[][2] = { { 0, 0 }, { 3, 1 }, { 4, 4 }, { 15, 15 }, { 31, 2 }, { 99, 36 } };
    long cases = 0;
    for (unsigned i = 0; i < sizeof imgs / sizeof *imgs; i++)
        for (unsigned j = 0; j < sizeof hss / sizeof *hss; j++) {
            const int iw = imgs[i][0], ih = imgs[i][1], hx = hss[j][0], hy = hss[j][1];
            if (hx >= iw || hy >= ih)
                continue;
            int ow = SIDE, oh = SIDE;
            const int ax = iofb_adj(hx, iw, ow), ay = iofb_adj(hy, ih, oh);
            while ((ow >> 1) >= iw + ax) ow >>= 1;          /* convertCursorImage: halve while the padded image fits */
            while ((oh >> 1) >= ih + ay) oh >>= 1;
            for (int ly = -2; ly < H + 2; ly += (ly < 300 || ly > H - 300) ? 1 : 97)
                for (int lx = -2; lx < W + 2; lx += (lx < 300 || lx > W - 300) ? 1 : 89) {
                    /* deferredMoveCursor: loc - hotspot; _setCursorState: - adj */
                    const int x = lx - hx - ax, y = ly - hy - ay;
                    struct dc_pos p;
                    int bx, by;
                    dm_cursor_position(x, y, MAX, MAX, &p);
                    cases++;
                    if (lx < 0 || ly < 0 || lx >= W || ly >= H)
                        continue;                            /* pointer off screen: nothing to compare */
                    CHECK(p.enable, "loc %d,%d img %dx%d hs %d,%d: cursor disabled", lx, ly, iw, ih, hx, hy);
                    CHECK(dcn_buffer_at(&p, lx, ly, SIDE, &bx, &by), "loc %d,%d: hot spot not on screen", lx, ly);
                    CHECK(bx == hx + ax && by == hy + ay, "loc %d,%d img %dx%d hs %d,%d: shows buffer %d,%d, want %d,%d",
                          lx, ly, iw, ih, hx, hy, bx, by, hx + ax, hy + ay);
                    /* and every other image pixel lands where the software cursor would put it */
                    for (int k = 0; k < 4; k++) {
                        const int sx = lx + (k & 1 ? 7 : -3), sy = ly + (k & 2 ? 5 : -2);
                        if (sx < 0 || sy < 0 || sx >= W || sy >= H)
                            continue;
                        const int ix = sx - (lx - hx), iy = sy - (ly - hy);     /* software cursor's image pixel */
                        const int on = dcn_buffer_at(&p, sx, sy, SIDE, &bx, &by);
                        if (ix >= 0 && iy >= 0 && ix < iw && iy < ih)
                            CHECK(on && bx == ix + ax && by == iy + ay, "loc %d,%d pixel %d,%d: buffer %d,%d want %d,%d",
                                  lx, ly, sx, sy, bx, by, ix + ax, iy + ay);
                    }
                }
        }
    /* hidden: DM reports the cursor off, on either axis */
    {
        struct dc_pos p;
        dm_cursor_position(nmx_cur_hidden_xy(MAX), nmx_cur_hidden_xy(MAX), MAX, MAX, &p);
        CHECK(!p.enable, "hidden position must disable the cursor");
        dm_cursor_position(nmx_cur_hidden_xy(MAX) + 1, nmx_cur_hidden_xy(MAX) + 1, MAX, MAX, &p);
        CHECK(p.enable && p.x_hotspot == MAX - 1, "one pixel in from hidden: enabled, hot spot %d", p.x_hotspot);
    }

    /* 3: fill and buffer side */
    {
        static uint32_t buf[256 * 256];
        uint32_t img[3 * 2] = { 0xff000000, 0x80404040, 0x00ffffff, 0xffffffff, 0x01010101, 0x7f7f0000 };
        memset(buf, 0xab, sizeof buf);
        CHECK(nmx_cur_fill(buf, 64, img, 3, 2) == 0, "fill 3x2 into 64");
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 64; x++) {
                const uint32_t want = (x < 3 && y < 2) ? nmx_cur_premul(img[y * 3 + x]) : 0;
                CHECK(buf[y * 64 + x] == want, "fill %u,%u = %08x want %08x", x, y, buf[y * 64 + x], want);
            }
        CHECK(buf[64 * 64] == 0xabababab, "fill wrote past side*side");
        CHECK(nmx_cur_fill(buf, 64, img, 65, 1) == -1 && nmx_cur_fill(buf, 64, img, 1, 65) == -1, "oversize refused");
        CHECK(nmx_cur_buf_side(256, 256) == 256 && nmx_cur_buf_side(512, 256) == 256 && nmx_cur_buf_side(128, 128) == 128,
              "side 256/128");
        CHECK(nmx_cur_buf_side(255, 255) == 128 && nmx_cur_buf_side(64, 64) == 64 && nmx_cur_buf_side(63, 256) == 0,
              "side rounding");
    }

    printf("%s: %ld positions, %d failures\n", fails ? "FAIL" : "PASS", cases, fails);
    return fails != 0;
}
