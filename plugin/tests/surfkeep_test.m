// surface keep test: correctness of the IOSurface wrapper cache and the cost of a miss with many live surfaces.
//   NM_TEST_PLUGIN=/path/NVMTLDriver ./surfkeep_test [nlive]
// 1. clear a wrapper of surface S to a colour, drop the wrapper, wrap S again: the new wrapper must see that colour
//    on the GPU (copy to a fresh surface, read on the CPU) - the keep must hand back the image the first pass wrote.
// 2. hold `nlive` surfaces wrapped (like WindowServer), then wrap 200 fresh surfaces one by one and time each call.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
static IOSurfaceRef mk(int w, int h) {
    return IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(w), (id)kIOSurfaceHeight:@(h),
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')}); }
static MTLTextureDescriptor *desc(int w, int h, MTLPixelFormat f) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; return td; }
static void clear(id<MTLCommandQueue> q, id<MTLTexture> t, MTLClearColor c) {
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore; p.colorAttachments[0].clearColor = c;
    id<MTLCommandBuffer> b = [q commandBuffer]; [[b renderCommandEncoderWithDescriptor:p] endEncoding]; [b commit]; [b waitUntilCompleted]; }
static uint32_t gpu_read(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLTexture> src) {
    IOSurfaceRef o = mk((int)src.width, (int)src.height);
    id<MTLTexture> dst = [d newTextureWithDescriptor:desc((int)src.width, (int)src.height, src.pixelFormat) iosurface:o plane:0];
    id<MTLCommandBuffer> b = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [b blitCommandEncoder];
    [e copyFromTexture:src toTexture:dst]; [e endEncoding]; [b commit]; [b waitUntilCompleted];
    IOSurfaceLock(o, kIOSurfaceLockReadOnly, NULL); uint32_t px = ((uint32_t *)IOSurfaceGetBaseAddress(o))[5 * IOSurfaceGetBytesPerRow(o) / 4 + 7];
    IOSurfaceUnlock(o, kIOSurfaceLockReadOnly, NULL); dst = nil; CFRelease(o); return px; }
static double now_us(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e3; }
int main(int argc, char **argv) { @autoreleasepool {
    const int nlive = argc > 1 ? atoi(argv[1]) : 400;
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) { fprintf(stderr, "NM_TEST_PLUGIN: %s\n", plugin ? dlerror() : "unset"); return 3; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(getenv("NM_TEST_ACCEL") ? getenv("NM_TEST_ACCEL") : "NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc],
        NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    IOObjectRelease(svc); if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    printf("device %s\n", d.name.UTF8String);
    int fails = 0;
    // 1. correctness through the keep
    IOSurfaceRef s = mk(64, 64);
    @autoreleasepool { id<MTLTexture> a = [d newTextureWithDescriptor:desc(64, 64, MTLPixelFormatBGRA8Unorm) iosurface:s plane:0];
                       clear(q, a, MTLClearColorMake(0, 1, 0, 1)); }   // green, then the wrapper goes
    for (int round = 0; round < 3; round++) {
        uint32_t want = round == 1 ? 0xffff0000u : 0xff00ff00u;   // BGRA in memory -> 0xAARRGGBB
        @autoreleasepool {
            id<MTLTexture> b = [d newTextureWithDescriptor:desc(64, 64, MTLPixelFormatBGRA8Unorm) iosurface:s plane:0];
            if (round == 1) clear(q, b, MTLClearColorMake(1, 0, 0, 1));
            uint32_t got = gpu_read(d, q, b);
            if (round == 2) want = 0xffff0000u;
            printf("keep round %d: pixel %08x want %08x %s\n", round, got, want, got == want ? "ok" : "FAIL"); fails += got != want;
        }
    }
    // other key, same surface (sRGB view): must still work
    @autoreleasepool { id<MTLTexture> c = [d newTextureWithDescriptor:desc(64, 64, MTLPixelFormatBGRA8Unorm_sRGB) iosurface:s plane:0];
                       printf("other key: %s\n", c ? "ok" : "FAIL"); fails += !c; }
    // 2. cost with many live surfaces
    NSMutableArray *live = [NSMutableArray new]; CFMutableArrayRef surfs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (int i = 0; i < nlive; i++) { IOSurfaceRef x = mk(64, 64); CFArrayAppendValue(surfs, x);
        [live addObject:[d newTextureWithDescriptor:desc(64, 64, MTLPixelFormatBGRA8Unorm) iosurface:x plane:0]]; CFRelease(x); }
    double tot = 0, mx = 0; const int N = 200;
    for (int i = 0; i < N; i++) { @autoreleasepool { IOSurfaceRef x = mk(64, 64);
        double t0 = now_us(); id<MTLTexture> t = [d newTextureWithDescriptor:desc(64, 64, MTLPixelFormatBGRA8Unorm) iosurface:x plane:0];
        double dt = now_us() - t0; tot += dt; if (dt > mx) mx = dt; if (!t) fails++; t = nil; CFRelease(x); } }
    double htot = 0;
    for (int i = 0; i < N; i++) { @autoreleasepool { IOSurfaceRef x = (IOSurfaceRef)CFArrayGetValueAtIndex(surfs, i % nlive);
        double t0 = now_us(); id<MTLTexture> t = [d newTextureWithDescriptor:desc(64, 64, MTLPixelFormatBGRA8Unorm) iosurface:x plane:0];
        htot += now_us() - t0; if (!t) fails++; } }
    printf("live %d: miss avg %.0f us max %.0f us; hit avg %.1f us\n", nlive, tot / N, mx, htot / N);
    printf("%s\n", fails ? "FAIL" : "PASS"); return fails ? 1 : 0;
}}
