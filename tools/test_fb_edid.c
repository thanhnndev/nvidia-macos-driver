#include "../gpu-port/xkpi/bsdcompat/include/nmx_fb_edid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void checksum(uint8_t *p) {
    uint8_t s = 0;
    for (unsigned i = 0; i < 127; i++) s += p[i];
    p[127] = (uint8_t)-s;
}
int main(void) {
    uint8_t e[256] = {0,255,255,255,255,255,255,0};
    checksum(e);
    assert(nmx_fb_edid_valid(e,128,sizeof(e)));
    assert(!nmx_fb_edid_valid(NULL,128,sizeof(e)));
    assert(!nmx_fb_edid_valid(e,127,sizeof(e)));
    assert(!nmx_fb_edid_valid(e,129,sizeof(e)));
    e[126]=1;checksum(e);e[128]=2;checksum(e+128);
    assert(nmx_fb_edid_valid(e,256,sizeof(e)));
    assert(!nmx_fb_edid_valid(e,256,128));
    assert(!nmx_fb_edid_valid(e,128,sizeof(e)));
    e[129]^=1;assert(!nmx_fb_edid_valid(e,256,sizeof(e)));e[129]^=1;
    e[8]^=1;assert(!nmx_fb_edid_valid(e,256,sizeof(e)));e[8]^=1;
    e[1]=0;checksum(e);assert(!nmx_fb_edid_valid(e,256,sizeof(e)));
    puts("PASS: EDID base/extension, capacity, truncation, header and checksum rejection");
}
