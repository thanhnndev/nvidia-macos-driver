// parked keep test (WS-CPU 9.10, NVMTL_KEEP_PARKED): surfaces kept alive by a mach port only (as a client's surfaces are
// for WindowServer), wrapped, left idle past a stand-in park time, wrapped again. Build and run:
//   xcrun clang -fobjc-arc -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
//     tests/parkedkeep_test.m -o /tmp/parkedkeep_test
//   NM_TEST_PLUGIN=<out>/NVMTLDriver NVMTL_LOG_PATH=<log> PK_MODE=<mode> <env below> /tmp/parkedkeep_test
// Common env: NVMTL_KEEP_PARK_MS=2000 NVMTL_KEEP_PARK_SLACK_MS=0 NVMTL_KEEP_IDLE=0 NVMTL_KEEP_VIEW_MS=1
//   NVMTL_KEEP_TIMER_MS=200 NVMTL_KEEP_ADAPT=0 NVMTL_KEEP_DEAD=0 (as in WindowServer), plus NVMTL_KEEP_PARKED=1 (on) or unset (off).
// PK_MODE=small (NVMTL_KEEP_MS=1000 NVMTL_KEEP_SMALL_MS=3000 NVMTL_KEEP_PARKED_MS=10000): 6 tiles of 256x256, idle 6 s,
//   wrapped again (on: 6 hits, 6 of them "past the small timeout"; off: 6 expired misses), then idle 12 s and wrapped
//   (both: 6 expired misses).
// PK_MODE=cap (as small, plus NVMTL_KEEP_PARKED_MAX=4): idle 6 s: 2 parked-cap drops (on), the 2 oldest.
// PK_MODE=large (NVMTL_KEEP_MS=30000 NVMTL_KEEP_IDLE_MB=8): 6 surfaces of 1024x1024 (4 MB) wrapped 2.5 s apart (each
//   earlier one is parked by the time the next goes idle), idle 4 s, wrapped again (on: the parked ones are out of the
//   8 MB cap: no idle-cap drop before the re-wrap, 6 hits; off: 3 idle > 8 MB: the 4 oldest dropped, 4 idle-cap misses).
//   Then only the first surface is wrapped (all 6 idle and unparked would be over the cap in both modes).
// Every mode ends with wraps until the 30 s keep report is written; read it from NVMTL_LOG_PATH.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
enum { NS = 6 };
static void wrap_one(id<MTLDevice> d, mach_port_t port, int sz) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:sz height:sz mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    @autoreleasepool {
        IOSurfaceRef s = IOSurfaceLookupFromMachPort(port); if (!s) { printf("lookup FAILED\n"); return; }
        id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
        if (!t) printf("wrap FAILED\n");
        t = nil; CFRelease(s);
    }
}
static void wrap_all(id<MTLDevice> d, mach_port_t *ports, int sz, long gapMs) {
    for (int i = 0; i < NS; i++) { wrap_one(d, ports[i], sz);
        if (gapMs) { struct timespec ts = { gapMs / 1000, (gapMs % 1000) * 1000000L }; nanosleep(&ts, NULL); } }
}
static void make_ports(mach_port_t *ports, int sz) {
    for (int i = 0; i < NS; i++) {
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(sz), (id)kIOSurfaceHeight:@(sz),
            (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
        ports[i] = IOSurfaceCreateMachPort(s); CFRelease(s);
    }
}
static void wait_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static double now_s(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9; }
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    void *h = plugin ? dlopen(plugin, RTLD_NOW | RTLD_LOCAL) : NULL; if (!h) { printf("dlopen: %s\n", dlerror()); return 3; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    const char *mode = getenv("PK_MODE") ?: "small";
    const int sz = !strcmp(mode, "large") ? 1024 : 256;
    mach_port_t ports[NS]; make_ports(ports, sz);
    const double t0 = now_s();
    printf("%s: wrap %d surfaces %dx%d (cap: 250 ms apart, large: 2.5 s apart)\n", mode, NS, sz, sz); fflush(stdout);
    const BOOL large = !strcmp(mode, "large");
    wrap_all(d, ports, sz, !strcmp(mode, "cap") ? 250 : large ? 2500 : 0);
    const long idle1 = large ? 4000 : 6000;
    printf("idle %ld ms, then wrap again\n", idle1); fflush(stdout);
    wait_ms(idle1); wrap_all(d, ports, sz, 0);
    if (!large) { printf("idle 12000 ms, then wrap again\n"); fflush(stdout); wait_ms(12000); wrap_all(d, ports, sz, 0); }
    printf("wraps every 2 s until the 30 s report\n"); fflush(stdout);
    while (now_s() - t0 < 33) { wait_ms(2000); if (large) wrap_one(d, ports[0], sz); else wrap_all(d, ports, sz, 0); }
    printf("done\n");
    for (int i = 0; i < NS; i++) mach_port_deallocate(mach_task_self(), ports[i]);
    return 0; } }
