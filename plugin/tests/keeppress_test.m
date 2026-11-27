// keep pressure test (roadmap 4 F1): idle keep entries go on a memory-pressure event, held ones never do, and nothing
// changes without an event. Own process, on the 890M, nothing installed:
//   NM_TEST_PLUGIN=<built NVMTLDriver> NVMTL_KEEP_DEAD=0 ./keeppress_test   (dead entries kept, as in WindowServer)
// Surfaces (4 MB each, over the 2 MB small-tile size):
//   A wrapped and released 6 s before the WARN event      -> dropped by WARN (idle >= 5 s), surface freed
//   B wrapped, its texture kept by the test (held)        -> kept through WARN and CRITICAL
//   C wrapped and released just before the WARN event     -> kept by WARN (idle < 5 s), dropped by CRITICAL
//   D wrapped and released, no event (control, 3 s later) -> still alive: no behaviour change at normal pressure
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
static IOSurfaceRef mk(void) {
    return IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@1024, (id)kIOSurfaceHeight:@1024,
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
}
static id<MTLTexture> wrap(id<MTLDevice> d, IOSurfaceRef s) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1024 height:1024 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    return [d newTextureWithDescriptor:td iosurface:s plane:0];
}
// wrap, release the texture and our surface reference: only the keep holds the surface afterwards
static IOSurfaceID wrapRelease(id<MTLDevice> d) {
    IOSurfaceID sid;
    @autoreleasepool { IOSurfaceRef s = mk(); sid = IOSurfaceGetID(s); id<MTLTexture> t = wrap(d, s);
        if (!t) printf("wrap of %u FAILED\n", sid); t = nil; CFRelease(s); }
    return sid;
}
static int alive(IOSurfaceID sid) { IOSurfaceRef l = IOSurfaceLookup(sid); if (l) CFRelease(l); return l != NULL; }
static void settle(void) { usleep(400000); }   // drops are released on the keep's release queue
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    void *h = plugin ? dlopen(plugin, RTLD_NOW | RTLD_LOCAL) : NULL; if (!h) return 3;
    void (*press)(id, unsigned long) = (void (*)(id, unsigned long))dlsym(h, "nvmtl_test_keep_pressure"); if (!press) return 6;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    int fail = 0;
    const IOSurfaceID a = wrapRelease(d);
    IOSurfaceRef bs = mk(); const IOSurfaceID b = IOSurfaceGetID(bs); id<MTLTexture> bt = wrap(d, bs);
    usleep(6000000);
    const IOSurfaceID c = wrapRelease(d);
    settle();
    printf("before WARN:     A %s, B %s, C %s\n", alive(a) ? "alive" : "freed", alive(b) ? "alive" : "freed", alive(c) ? "alive" : "freed");
    if (!alive(a) || !alive(c)) { printf("FAIL: idle entries went without an event\n"); fail++; }
    press(d, DISPATCH_MEMORYPRESSURE_WARN); settle();
    printf("after WARN:      A %s, B %s, C %s\n", alive(a) ? "alive" : "freed", alive(b) ? "alive" : "freed", alive(c) ? "alive" : "freed");
    if (alive(a)) { printf("FAIL: A (idle 6 s) kept by WARN\n"); fail++; }
    if (!alive(c)) { printf("FAIL: C (idle < 5 s) dropped by WARN\n"); fail++; }
    press(d, DISPATCH_MEMORYPRESSURE_CRITICAL); settle();
    printf("after CRITICAL:  B %s, C %s\n", alive(b) ? "alive" : "freed", alive(c) ? "alive" : "freed");
    if (alive(c)) { printf("FAIL: C kept by CRITICAL\n"); fail++; }
    // B is held by our texture: it must survive both events and still be usable
    CFRelease(bs);
    if (!alive(b)) { printf("FAIL: held B dropped\n"); fail++; }
    if (bt.width != 1024) { printf("FAIL: held B's texture unusable\n"); fail++; }
    press(d, DISPATCH_MEMORYPRESSURE_NORMAL); settle();
    const IOSurfaceID dd = wrapRelease(d);
    usleep(3000000);
    printf("control (no event, 3 s idle): D %s\n", alive(dd) ? "alive" : "freed");
    if (!alive(dd)) { printf("FAIL: D dropped without an event\n"); fail++; }
    bt = nil;
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}}
