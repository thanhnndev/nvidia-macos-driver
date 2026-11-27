// NVMTL_NMX_FENCE_CALL (WS-CPU cut 1 item 1): fence marks through NMXAccel's fence table instead of IOSurface properties.
// A scratch NVMTLDevice in this process renders into IOSurface-backed textures (NVMTL_NMX_FENCE=2 marks them outside
// WindowServer), then reads the surfaces' NMXFence* properties back:
//   FCALL_EXPECT=props   the marks went to the properties (Done == Pending on every surface)   - call off, or fell back
//   FCALL_EXPECT=none    no surface carries NMXFencePending (every mark went through the call)  - SIM=ok
//   FCALL_EXPECT=resume  surfaces 0-2 carry NMXFencePending (written while the gate read 0), the 20 of phase 2 none
//                        (calls resumed)                                                       - SIM=gateoff=5:40
// The runner also checks the plugin log (NVMTL_LOG_PATH) for the pause contract: every pause closes the (simulated)
// connection, so NMXAccel would drop all of the process's rows: refuse=20 and gateoff=5:40 must log
// "connection closed", and gateoff=5:40 must log "resumed" with a second open after it.
// Phase 1: 3 surfaces, FCALL_FRAMES frames (one surface per command buffer); phase 2: one command buffer writing 20
// surfaces (two calls: 16 + 4).
//   xcrun clang -fobjc-arc -o fcall_test fcall_test.m -framework Foundation -framework Metal -framework IOSurface -framework IOKit
//   NVMTL_NMX_FENCE=2 NVMTL_NMX_FENCE_CALL=1 NVMTL_NMX_FENCE_CALL_SIM=ok FCALL_EXPECT=none NM_TEST_PLUGIN=.../NVMTLDriver ./fcall_test
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
static IOSurfaceRef mksurf(int w, int h) {
    return IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(w), (id)kIOSurfaceHeight:@(h),
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
}
static unsigned long long prop(IOSurfaceRef s, NSString *k, BOOL *has) {
    CFTypeRef v = IOSurfaceCopyValue(s, (__bridge CFStringRef)k);
    *has = v != NULL;
    unsigned long long n = v ? [(__bridge NSNumber *)v unsignedLongLongValue] : 0;
    if (v) CFRelease(v);
    return n;
}
static void pass(id<MTLCommandBuffer> cb, id<MTLTexture> t, double g) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, g, 0.3, 1);
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
}
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    void *h = plugin ? dlopen(plugin, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h) { fprintf(stderr, "no plugin: %s\n", dlerror()); return 3; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    const int frames = getenv("FCALL_FRAMES") ? atoi(getenv("FCALL_FRAMES")) : 60;
    const char *expect = getenv("FCALL_EXPECT") ?: "props";
    enum { NS = 23 };
    IOSurfaceRef s[NS]; id<MTLTexture> t[NS];
    for (int i = 0; i < NS; i++) {
        s[i] = mksurf(256, 128);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:128 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        t[i] = [d newTextureWithDescriptor:td iosurface:s[i] plane:0];
        if (!t[i]) { fprintf(stderr, "no IOSurface texture %d\n", i); return 6; }
    }
    id<MTLCommandBuffer> last = nil;
    for (int f = 0; f < frames; f++) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        pass(cb, t[f % 3], (f % 10) / 10.0);
        [cb commit]; last = cb;
    }
    [last waitUntilCompleted];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    for (int i = 3; i < NS; i++) pass(cb, t[i], 0.5);
    [cb commit]; [cb waitUntilCompleted];
    usleep(200000);   /* the completion thread publishes Done after the completed handler */
    int withProps = 0, consistent = 0, early = 0;
    for (int i = 0; i < NS; i++) {
        BOOL hp, hd; unsigned long long p = prop(s[i], @"NMXFencePending", &hp), dn = prop(s[i], @"NMXFenceDone", &hd);
        if (hp) withProps++;
        if (hp && i < 3) early++;
        if (hp && hd && dn >= p) consistent++;
    }
    int ok = !strcmp(expect, "none") ? withProps == 0
           : !strcmp(expect, "resume") ? (early == 3 && withProps == 3)
           : (withProps == NS && consistent == NS);
    printf("%s: expect %s, %d/%d surfaces carry NMXFencePending, %d with Done >= Pending\n", ok ? "PASS" : "FAIL", expect,
           withProps, NS, consistent);
    for (int i = 0; i < NS; i++) CFRelease(s[i]);
    return ok ? 0 : 1;
} }
