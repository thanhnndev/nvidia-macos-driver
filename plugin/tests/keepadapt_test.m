// keep adapt test (WS-CPU 9.9): surfaces kept alive by a mach port only (as a client's surfaces are for WindowServer),
// wrapped again after a gap longer than the base idle timeout. Run with NVMTL_KEEP_MS=1000 NVMTL_KEEP_MAX_MS=10000
// NVMTL_KEEP_VIEW_MS=1 NVMTL_KEEP_TIMER_MS=200 and NVMTL_LOG_PATH set. Expected in the log:
//   A (0-24 s, a wrap every 2 s): expired returns -> "idle timeout 1000 -> N ms", then hits only the raise kept;
//   B (24-60 s, no wraps): the table empties, no sweeps; C's first wrap sweeps and sets "idle timeout N -> 1000 ms"
//     (one halving per quiet 10 s, so the 36 s gap takes it to the base);
//   C (60-76 s, wraps every 2 s, then a WARN event): raised again, then "-> 1000 ms (memory pressure)".
// Needs a non-release plugin (nvmtl_test_keep_pressure).
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
enum { NS = 6 };
static void wrap_all(id<MTLDevice> d, mach_port_t *ports) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1024 height:1024 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    for (int i = 0; i < NS; i++) @autoreleasepool {
        IOSurfaceRef s = IOSurfaceLookupFromMachPort(ports[i]); if (!s) { printf("lookup %d FAILED\n", i); continue; }
        id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
        if (!t) printf("wrap %d FAILED\n", i);
        t = nil; CFRelease(s);
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
    mach_port_t ports[NS];
    for (int i = 0; i < NS; i++) {
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@1024, (id)kIOSurfaceHeight:@1024,
            (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
        ports[i] = IOSurfaceCreateMachPort(s); CFRelease(s);
    }
    printf("A: wraps every 2 s\n"); fflush(stdout);
    for (int k = 0; k < 12; k++) { wrap_all(d, ports); wait_ms(2000); }
    printf("B: quiet 36 s\n"); fflush(stdout);
    wait_ms(36000);
    printf("C: wraps every 2 s, then WARN\n"); fflush(stdout);
    for (int k = 0; k < 8; k++) { wrap_all(d, ports); wait_ms(2000); }
    press(d, DISPATCH_MEMORYPRESSURE_WARN);
    wrap_all(d, ports); wait_ms(1000);
    printf("done\n");
    for (int i = 0; i < NS; i++) mach_port_deallocate(mach_task_self(), ports[i]);
    return 0; } }
