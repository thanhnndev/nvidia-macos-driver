// surface keep with a pool working set (docs/890M-TRACE.md part 10): like WindowServer, the test holds each surface
// only as a mach port between frames (the app's side), looks it up, wraps it, renders into it and lets go. With more
// idle surfaces than the cap, every round re-imports (pin + GPU map); with room, every round after the first hits.
//   NM_TEST_PLUGIN=/path/NVMTLDriver ./surfpool_test [napps] [rounds]      NVMTL_KEEP_IDLE_MB=0 for the old flat cap of 48
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static double now_us(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e3; }
int main(int argc, char **argv) { @autoreleasepool {
    const int napps = argc > 1 ? atoi(argv[1]) : 30, rounds = argc > 2 ? atoi(argv[2]) : 10, pool = 3, n = napps * pool;
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) return 3;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    mach_port_t *ports = calloc(n, sizeof *ports);
    for (int i = 0; i < n; i++) {
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@1440, (id)kIOSurfaceHeight:@900,
            (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
        if (!s) return 6;
        ports[i] = IOSurfaceCreateMachPort(s); CFRelease(s);   // the "app" keeps the port, not a reference here
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1440 height:900 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    int fails = 0;
    for (int r = 0; r < rounds; r++) {
        double wrap = 0, worst = 0;
        for (int i = 0; i < n; i++) { @autoreleasepool {
            IOSurfaceRef s = IOSurfaceLookupFromMachPort(ports[i]); if (!s) return 7;
            double t0 = now_us();
            id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
            double dt = now_us() - t0; wrap += dt; if (dt > worst) worst = dt;
            if (!t) { fails++; CFRelease(s); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake((i % 7) / 7.0, r / 16.0, 0.5, 1);
            id<MTLCommandBuffer> cb = [q commandBuffer]; [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit];
            if (i == n - 1) [cb waitUntilCompleted];
            t = nil; CFRelease(s);
        } }
        printf("round %2d: %d wraps, %.0f us each, worst %.0f us\n", r, n, wrap / n, worst); fflush(stdout);
    }
    // the last colour written must be what the surface holds
    { IOSurfaceRef s = IOSurfaceLookupFromMachPort(ports[n - 1]); IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
      const uint8_t *p = IOSurfaceGetBaseAddress(s);
      const int want_g = (int)((rounds - 1) / 16.0 * 255 + 0.5), want_b = 128;
      printf("last surface pixel BGRA %u %u %u %u (want G~%d R~%d)\n", p[0], p[1], p[2], p[3], want_g, want_b);
      if (abs((int)p[1] - want_g) > 2) fails++;
      IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL); CFRelease(s); }
    printf("%s\n", fails ? "FAILED" : "PASS"); return fails ? 1 : 0;
}}
