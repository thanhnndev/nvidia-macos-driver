// keep adapt test (WS-CPU 9.9): surfaces kept alive by a mach port only (as a client's surfaces are for WindowServer),
// wrapped again after a gap longer than the base idle timeout. Run with NVMTL_KEEP_MS=1000 NVMTL_KEEP_MAX_MS=10000
// NVMTL_KEEP_VIEW_MS=1 NVMTL_KEEP_TIMER_MS=200 and NVMTL_LOG_PATH set. Expected in the log:
//   A (0-24 s, a wrap every 2 s): expired returns -> "idle timeout 1000 -> N ms", then hits only the raise kept;
//   B (24-60 s, no wraps): the table empties, no sweeps; C's first wrap sweeps and sets "idle timeout N -> 1000 ms"
//     (one halving per quiet 10 s, so the 36 s gap takes it to the base);
//   C (60-76 s, wraps every 2 s, then a WARN event): raised again, then "-> 1000 ms (memory pressure)".
//   then (F2, review of c53ab829) wraps every 2 s for 12 s inside the 60 s pressure window: no raise.
// KA_MODE=small (F1): A as above, then 36 s of small 256x256 tiles (256 KB, under NVMTL_KEEP_SMALL_KB) coming back every
//   3.5 s and no large wraps; run with NVMTL_KEEP_SMALL_MS=5000 so the small rule keeps them past the 1 s base. The raised
//   timeout must still decay to 1000 (hits on small tiles are not "kept by the raise").
// KA_MODE=empty (F3): A, then 6 s with no wraps (the table empties, its timer stops), then a WARN: "-> 1000 ms (memory
//   pressure)" at once.
// Needs a non-release plugin (nvmtl_test_keep_pressure).
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
enum { NS = 6 };
static void wrap_all_sz(id<MTLDevice> d, mach_port_t *ports, int sz) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:sz height:sz mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    for (int i = 0; i < NS; i++) @autoreleasepool {
        IOSurfaceRef s = IOSurfaceLookupFromMachPort(ports[i]); if (!s) { printf("lookup %d FAILED\n", i); continue; }
        id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
        if (!t) printf("wrap %d FAILED\n", i);
        t = nil; CFRelease(s);
    }
}
static void wrap_all(id<MTLDevice> d, mach_port_t *ports) { wrap_all_sz(d, ports, 1024); }
static void make_ports(mach_port_t *ports, int sz) {
    for (int i = 0; i < NS; i++) {
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(sz), (id)kIOSurfaceHeight:@(sz),
            (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
        ports[i] = IOSurfaceCreateMachPort(s); CFRelease(s);
    }
}
static void wait_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    void *h = plugin ? dlopen(plugin, RTLD_NOW | RTLD_LOCAL) : NULL; if (!h) return 3;
    void (*press)(id, unsigned long) = (void (*)(id, unsigned long))dlsym(h, "nvmtl_test_keep_pressure"); if (!press) return 6;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    mach_port_t ports[NS]; make_ports(ports, 1024);
    const char *mode = getenv("KA_MODE") ?: "default";
    printf("A: wraps every 2 s\n"); fflush(stdout);
    for (int k = 0; k < 12; k++) { wrap_all(d, ports); wait_ms(2000); }
    if (!strcmp(mode, "small")) {
        mach_port_t tiles[NS]; make_ports(tiles, 256);
        printf("D: small tiles every 3.5 s, no large wraps, 36 s\n"); fflush(stdout);
        for (int k = 0; k < 10; k++) { wrap_all_sz(d, tiles, 256); wait_ms(3500); }
        for (int i = 0; i < NS; i++) mach_port_deallocate(mach_task_self(), tiles[i]);
    } else if (!strcmp(mode, "empty")) {
        printf("E: 6 s quiet, then WARN on the empty table\n"); fflush(stdout);
        wait_ms(6000);
        press(d, DISPATCH_MEMORYPRESSURE_WARN);
        wait_ms(500);
    } else {
        printf("B: quiet 36 s\n"); fflush(stdout);
        wait_ms(36000);
        printf("C: wraps every 2 s, then WARN\n"); fflush(stdout);
        for (int k = 0; k < 8; k++) { wrap_all(d, ports); wait_ms(2000); }
        press(d, DISPATCH_MEMORYPRESSURE_WARN);
        printf("F2: wraps every 2 s for 12 s inside the pressure window (no raise)\n"); fflush(stdout);
        for (int k = 0; k < 6; k++) { wrap_all(d, ports); wait_ms(2000); }
    }
    printf("done\n");
    for (int i = 0; i < NS; i++) mach_port_deallocate(mach_task_self(), ports[i]);
    return 0; } }
