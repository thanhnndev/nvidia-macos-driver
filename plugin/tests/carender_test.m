// CoreAnimation's renderer (CARenderer, the same CA render code WindowServer composites with) driving a scratch
// NVMTLDevice in this process: windows = IOSurface-backed layers with corner radius and shadow, one "video" layer
// whose contents flip between surfaces every frame, a text layer, and a moving layer. Each frame calls the plugin's
// frame census as one present, so its per-present counts compare with the WindowServer census line.
//   NM_TEST_PLUGIN=/path/NVMTLDriver NVMTL_CENSUS_MS=5000 ./carender_test [frames] [video 0/1] [nwin]
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
static double sys_s(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6; }
static double cpu_s(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6; }

static IOSurfaceRef mksurf(int w, int h, uint32_t fmt, uint8_t fill)
{
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(w), (id)kIOSurfaceHeight:@(h),
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@(fmt)});
    if (!s) return NULL;
    IOSurfaceLock(s, 0, NULL); memset(IOSurfaceGetBaseAddress(s), fill, IOSurfaceGetAllocSize(s)); IOSurfaceUnlock(s, 0, NULL);
    return s;
}
static CVPixelBufferRef mk420(int w, int h, uint8_t y)
{
    CVPixelBufferRef pb = NULL;
    NSDictionary *a = @{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    if (CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, (__bridge CFDictionaryRef)a, &pb)) return NULL;
    CVPixelBufferLockBaseAddress(pb, 0);
    memset(CVPixelBufferGetBaseAddressOfPlane(pb, 0), y, CVPixelBufferGetBytesPerRowOfPlane(pb, 0) * h);
    memset(CVPixelBufferGetBaseAddressOfPlane(pb, 1), 128, CVPixelBufferGetBytesPerRowOfPlane(pb, 1) * h / 2);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}
int main(int argc, char **argv) { @autoreleasepool {
    const int frames = argc > 1 ? atoi(argv[1]) : 600, video = argc > 2 ? atoi(argv[2]) : 1, nwin = argc > 3 ? atoi(argv[3]) : 6, churn = argc > 4 ? atoi(argv[4]) : 0;
    const char *plugin = getenv("NM_TEST_PLUGIN");
    void *h = plugin ? dlopen(plugin, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h) { fprintf(stderr, "no plugin: %s\n", dlerror()); return 3; }
    void (*census)(int) = (void (*)(int))dlsym(h, "nvmtl_vk_frame_census");
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    const int W = 1920, H = 1200;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> fb[3]; for (int i = 0; i < 3; i++) fb[i] = [d newTextureWithDescriptor:td];
    CARenderer *r = [CARenderer rendererWithMTLTexture:fb[0] options:@{kCARendererMetalCommandQueue: q}];
    if (!r) return 6;
    CALayer *root = [CALayer layer]; root.frame = CGRectMake(0, 0, W, H); root.backgroundColor = CGColorCreateGenericRGB(0.2, 0.3, 0.4, 1);
    NSMutableArray *keep = [NSMutableArray array];
    for (int i = 0; i < nwin; i++) {
        CALayer *l = [CALayer layer]; l.frame = CGRectMake(60 + i * 110, 60 + i * 70, 900, 600);
        IOSurfaceRef s = mksurf(900, 600, 'BGRA', (uint8_t)(40 + i * 30));
        l.contents = (__bridge id)s; [keep addObject:(__bridge id)s]; CFRelease(s);
        l.cornerRadius = 10; l.masksToBounds = YES;
        CALayer *sh = [CALayer layer]; sh.frame = l.frame; sh.shadowOpacity = 0.4; sh.shadowRadius = 20; sh.shadowOffset = CGSizeMake(0, -8);
        sh.shadowPath = CGPathCreateWithRect(CGRectMake(0, 0, 900, 600), NULL); sh.backgroundColor = CGColorCreateGenericRGB(0, 0, 0, 0);
        [root addSublayer:sh]; [root addSublayer:l];
    }
    CATextLayer *tl = [CATextLayer layer]; tl.frame = CGRectMake(20, H - 40, 600, 30); tl.string = @"menu bar 12:00"; tl.fontSize = 14;
    tl.foregroundColor = CGColorCreateGenericRGB(1, 1, 1, 1); [root addSublayer:tl];
    CALayer *cursor = [CALayer layer]; cursor.frame = CGRectMake(0, 0, 24, 24); cursor.backgroundColor = CGColorCreateGenericRGB(1, 1, 1, 1);
    [root addSublayer:cursor];
    CALayer *vl = nil; CVPixelBufferRef vb[4] = {0};
    if (video) {
        vl = [CALayer layer]; vl.frame = CGRectMake(400, 200, 1280, 720); [root addSublayer:vl];
        for (int i = 0; i < 4; i++) vb[i] = mk420(1920, 1080, (uint8_t)(16 + 50 * i));
    }
    // video 2: a decoder-like pool of 8 frames, each frame handed over as a mach port and looked up again (what
    // WindowServer gets from a client); video 3: a CVPixelBufferPool, one new pool buffer per frame, released after use
    mach_port_t vport[8] = {0}; CVPixelBufferRef vpb8[8] = {0}; CVPixelBufferPoolRef pool = NULL;
    if (video == 2) for (int i = 0; i < 8; i++) { vpb8[i] = mk420(1920, 1080, (uint8_t)(16 + 25 * i)); vport[i] = IOSurfaceCreateMachPort(CVPixelBufferGetIOSurface(vpb8[i])); }
    // video 4: Edge-like 4K 10-bit video (2026-11-23 working load): a pool of CARENDER_POOL (12) 3840x2160 x420 frames
    // handed over by mach port, one "decoded" (rows written, seed moved) per frame; every CARENDER_SWITCH frames
    // (frames / 3) a new pool replaces it while the client keeps the old one alive (a seek or resolution change)
    const int vpool = getenv("CARENDER_POOL") ? atoi(getenv("CARENDER_POOL")) : 12, vswitch = getenv("CARENDER_SWITCH") ? atoi(getenv("CARENDER_SWITCH")) : frames / 3;
    // CARENDER_VW / _VH: frame size (3840x2160); CARENDER_FPS: pace the frames (0 = as fast as they go), so a pool larger
    // than fps x NVMTL_KEEP_SHADOW_MS reuses each frame later than the shadow timeout (the thrash case)
    const int vw = getenv("CARENDER_VW") ? atoi(getenv("CARENDER_VW")) : 3840, vh = getenv("CARENDER_VH") ? atoi(getenv("CARENDER_VH")) : 2160;
    const double vfps = getenv("CARENDER_FPS") ? atof(getenv("CARENDER_FPS")) : 0;
    NSMutableArray *vold = [NSMutableArray array]; CVPixelBufferRef *v4 = calloc(vpool, sizeof *v4); mach_port_t *v4port = calloc(vpool, sizeof *v4port);
    void (^mkpool)(int) = ^(int gen) {
        for (int i = 0; i < vpool; i++) {
            if (v4[i]) { [vold addObject:(__bridge id)v4[i]]; CVPixelBufferRelease(v4[i]); }   // old pool: still held by the client
            // CARENDER_PLANE_ALIGN=256: the layout VCNDec 0.2.1 asks for (planes at 256 B aligned offsets, aliased by the plugin)
            const int palign = getenv("CARENDER_PLANE_ALIGN") ? atoi(getenv("CARENDER_PLANE_ALIGN")) : 0;
            NSDictionary *a = palign ? @{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}, (id)kCVPixelBufferPlaneAlignmentKey:@(palign)}
                                     : @{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
            v4[i] = NULL; CVPixelBufferCreate(NULL, vw, vh, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, (__bridge CFDictionaryRef)a, &v4[i]);
            v4port[i] = v4[i] ? IOSurfaceCreateMachPort(CVPixelBufferGetIOSurface(v4[i])) : MACH_PORT_NULL;
            if (gen == 0 && i == 0 && v4[0]) { IOSurfaceRef sf = CVPixelBufferGetIOSurface(v4[0]); const uint8_t *b0 = IOSurfaceGetBaseAddress(sf);
                printf("video 4: pool of %d x420 %dx%d at %.0f fps, plane offsets +%ld +%ld, pitch %zu %zu, switch every %d frames\n", vpool, vw, vh, vfps,
                       (long)((const uint8_t *)IOSurfaceGetBaseAddressOfPlane(sf, 0) - b0), (long)((const uint8_t *)IOSurfaceGetBaseAddressOfPlane(sf, 1) - b0),
                       IOSurfaceGetBytesPerRowOfPlane(sf, 0), IOSurfaceGetBytesPerRowOfPlane(sf, 1), vswitch); }
        } };
    if (video == 4) mkpool(0);
    if (video == 3) {
        NSDictionary *pa = @{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), (id)kCVPixelBufferWidthKey:@1920,
                             (id)kCVPixelBufferHeightKey:@1080, (id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        CVPixelBufferPoolCreate(NULL, (__bridge CFDictionaryRef)@{(id)kCVPixelBufferPoolMinimumBufferCountKey:@6}, (__bridge CFDictionaryRef)pa, &pool);
    }
    if (video) {
    }
    r.layer = root; r.bounds = root.frame;
    [CATransaction flush];
    double t0 = CACurrentMediaTime(), c0 = 0, w0 = 0, s0 = 0; const int warm = frames / 4;
    for (int f = 0; f < frames; f++) { @autoreleasepool {
        if (f == warm) { c0 = cpu_s(); s0 = sys_s(); w0 = CACurrentMediaTime(); }
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        cursor.position = CGPointMake(100 + (f * 7) % (W - 200), 100 + (f * 3) % (H - 200));
        if (vl && video == 1) vl.contents = (__bridge id)CVPixelBufferGetIOSurface(vb[f & 3]);
        if (vl && video == 2) { IOSurfaceRef sr = IOSurfaceLookupFromMachPort(vport[f & 7]); vl.contents = (__bridge id)sr; if (sr) CFRelease(sr); }
        if (vl && video == 4) {
            if (vfps > 0) { static double next; const double nowt = CACurrentMediaTime(); if (!next) next = nowt;
                            next += 1.0 / vfps; if (next > nowt) usleep((useconds_t)((next - nowt) * 1e6)); }
            if (vswitch > 0 && f && f % vswitch == 0) mkpool(f / vswitch);
            CVPixelBufferRef pb = v4[f % vpool];
            CVPixelBufferLockBaseAddress(pb, 0);   // the decoder's write: moves the seed, so the plane rows go in again
            memset(CVPixelBufferGetBaseAddressOfPlane(pb, 0), (f * 5) & 0xff, CVPixelBufferGetBytesPerRowOfPlane(pb, 0));
            CVPixelBufferUnlockBaseAddress(pb, 0);
            IOSurfaceRef sr = IOSurfaceLookupFromMachPort(v4port[f % vpool]); vl.contents = (__bridge id)sr; if (sr) CFRelease(sr); }
        if (vl && video == 3) { CVPixelBufferRef pb = NULL; CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb);
            if (pb) { vl.contents = (__bridge id)CVPixelBufferGetIOSurface(pb); CVPixelBufferRelease(pb); } }
        if (f % 60 == 0) tl.string = [NSString stringWithFormat:@"menu bar 12:%02d", (f / 60) % 60];
        [CATransaction commit]; [CATransaction flush];
        // churn: per frame, 2 MTLBuffers and 1 texture view made and dropped elsewhere in the process (WindowServer's
        // census shows 1.2-1.5 VkBuffers and up to 1.8 views made and destroyed per present at idle)
        for (int k = 0; k < churn; k++) { id<MTLBuffer> jb = [d newBufferWithLength:65536 * (k + 1) options:MTLResourceStorageModeShared];
            id<MTLBuffer> jb2 = [d newBufferWithLength:1 << 20 options:MTLResourceStorageModePrivate]; (void)jb; (void)jb2;
            id<MTLTexture> jv = [fb[0] newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB]; (void)jv; }
        [r setDestination:fb[f % 3]];
        [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
        [r addUpdateRect:r.bounds];
        [r render];
        [r endFrame];
        if (census) census(1);   // NVC_CAWUS: one present
        if (f % 120 == 119) { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; [cb waitUntilCompleted]; }
    } }
    { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; [cb waitUntilCompleted]; }
    const double c1 = cpu_s(), w1 = CACurrentMediaTime();
    printf("%d frames in %.2f s; steady %d frames: %.1f us CPU per frame (%.1f system), %.1f us wall per frame\n", frames, w1 - t0,
           frames - warm, (c1 - c0) * 1e6 / (frames - warm), (sys_s() - s0) * 1e6 / (frames - warm), (w1 - w0) * 1e6 / (frames - warm));
    // checksum of the last frame (read back through a shared buffer)
    { id<MTLBuffer> rb = [d newBufferWithLength:(NSUInteger)W * H * 4 options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
      [be copyFromTexture:fb[(frames - 1) % 3] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:(NSUInteger)W * H * 4];
      [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
      const uint8_t *px = rb.contents; uint64_t hsh = 1469598103934665603ull; for (size_t i = 0; i < (size_t)W * H * 4; i++) hsh = (hsh ^ px[i]) * 1099511628211ull;
      printf("last frame checksum %016llx, pixel(500,500) BGRA %u %u %u %u\n", (unsigned long long)hsh, px[(500 * W + 500) * 4], px[(500 * W + 500) * 4 + 1], px[(500 * W + 500) * 4 + 2], px[(500 * W + 500) * 4 + 3]); }
    return 0;
}}
