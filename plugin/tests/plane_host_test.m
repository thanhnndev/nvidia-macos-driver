// plane host test: two-plane (420v) IOSurfaces whose planes change every frame must read back correctly on the GPU.
//   NM_TEST_PLUGIN=/path/NVMTLDriver ./plane_host_test [frames]
// On an AMD APU each plane wrapper is a host import of the surface's own pages, shadowed on the GPU when its row pitch
// is not a multiple of 256 B (nvmtl_plane_host_create in NVMTLDevice.m). Cases:
//   - a CVPixelBuffer-style surface (1920x1080, rows as IOSurface picks them) and a camera-like one (rows 1928/1944 B,
//     planes at offsets that are not page aligned);
//   - per frame: the CPU rewrites both planes, then the GPU copies each plane (R8Unorm / RG8Unorm wrappers, read-only
//     usage) into a buffer; checked byte for byte. One set of wrappers is kept across frames (Core Image's cache), and a
//     fresh set is made every other frame;
//   - a writable wrapper (RenderTarget) of plane 0 is cleared on the GPU: the CPU must see the clear in the surface,
//     and plane 1 must be untouched.
// Run it also with NVMTL_PLANE_HOST=0 (the private-copy path) - both must pass.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
static void on_alarm(int s) { (void)s; static const char m[] = "plane host test: TIMEOUT\n"; write(2, m, sizeof m - 1); _exit(9); }
static IOSurfaceRef mk420(int w, int h, size_t yrow, size_t uvrow, size_t uvoff) {
    NSMutableDictionary *p0 = [@{(id)kIOSurfacePlaneWidth:@(w), (id)kIOSurfacePlaneHeight:@(h), (id)kIOSurfacePlaneBytesPerElement:@1} mutableCopy];
    NSMutableDictionary *p1 = [@{(id)kIOSurfacePlaneWidth:@(w / 2), (id)kIOSurfacePlaneHeight:@(h / 2), (id)kIOSurfacePlaneBytesPerElement:@2} mutableCopy];
    NSMutableDictionary *d = [@{(id)kIOSurfaceWidth:@(w), (id)kIOSurfaceHeight:@(h), (id)kIOSurfacePixelFormat:@((uint32_t)'420v'),
                                (id)kIOSurfacePlaneInfo:@[p0, p1]} mutableCopy];
    if (yrow) {
        p0[(id)kIOSurfacePlaneBytesPerRow] = @(yrow); p0[(id)kIOSurfacePlaneOffset] = @0; p0[(id)kIOSurfacePlaneSize] = @(yrow * h);
        p1[(id)kIOSurfacePlaneBytesPerRow] = @(uvrow); p1[(id)kIOSurfacePlaneOffset] = @(uvoff); p1[(id)kIOSurfacePlaneSize] = @(uvrow * h / 2);
        d[(id)kIOSurfaceAllocSize] = @(uvoff + uvrow * h / 2);
        d[(id)kIOSurfacePlaneInfo] = @[p0, p1];
    }
    return IOSurfaceCreate((__bridge CFDictionaryRef)d);
}
static uint8_t ypx(int i, int j, int f) { return (uint8_t)(i * 3 + j * 5 + f * 37); }
static uint8_t uvpx(int i, int j, int f) { return (uint8_t)(i * 7 + j * 11 + f * 53 + 1); }
static void fill(IOSurfaceRef s, int w, int h, int f) {
    IOSurfaceLock(s, 0, NULL);
    uint8_t *y = IOSurfaceGetBaseAddressOfPlane(s, 0), *uv = IOSurfaceGetBaseAddressOfPlane(s, 1);
    const size_t yr = IOSurfaceGetBytesPerRowOfPlane(s, 0), uvr = IOSurfaceGetBytesPerRowOfPlane(s, 1);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) y[j * yr + i] = ypx(i, j, f);
    for (int j = 0; j < h / 2; j++) for (int i = 0; i < w; i++) uv[j * uvr + i] = uvpx(i, j, f);
    IOSurfaceUnlock(s, 0, NULL);
}
static MTLTextureDescriptor *desc(int w, int h, MTLPixelFormat f, MTLTextureUsage u) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = u; return td; }
// GPU copy of a plane wrapper into a buffer, compared with the expected bytes; returns the number of wrong bytes
static long check(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLTexture> t, int bpe, int f, int uv) {
    const int w = (int)t.width, h = (int)t.height; const size_t row = (size_t)w * bpe;
    id<MTLBuffer> b = [d newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    memset(b.contents, 0xEE, row * h);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = b.contents; long bad = 0; int fx = -1, fy = -1;
    for (int j = 0; j < h; j++) for (size_t i = 0; i < row; i++) {
        const uint8_t want = uv ? uvpx((int)i, j, f) : ypx((int)i, j, f);
        if (p[j * row + i] != want) { if (!bad) { fx = (int)i; fy = j; } bad++; } }
    if (bad) printf("    frame %d plane %d: %ld wrong bytes, first at (%d,%d): got %u want %u\n", f, uv, bad, fx, fy,
                    p[fy * row + fx], uv ? uvpx(fx, fy, f) : ypx(fx, fy, f));
    return bad;
}
static int run_case(id<MTLDevice> d, id<MTLCommandQueue> q, const char *name, IOSurfaceRef s, int w, int h, int frames) {
    printf("%s: %dx%d, rows %zu / %zu B, plane 1 at +%zu, alloc %zu\n", name, w, h, IOSurfaceGetBytesPerRowOfPlane(s, 0),
           IOSurfaceGetBytesPerRowOfPlane(s, 1), (size_t)((uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 1) - (uint8_t *)IOSurfaceGetBaseAddress(s)),
           IOSurfaceGetAllocSize(s));
    int fails = 0;
    id<MTLTexture> ky = [d newTextureWithDescriptor:desc(w, h, MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead) iosurface:s plane:0];
    id<MTLTexture> kuv = [d newTextureWithDescriptor:desc(w / 2, h / 2, MTLPixelFormatRG8Unorm, MTLTextureUsageShaderRead) iosurface:s plane:1];
    if (!ky || !kuv) { printf("  wrappers: FAIL (nil)\n"); return 1; }
    long bad = 0;
    for (int f = 0; f < frames; f++) {
        fill(s, w, h, f);
        bad += check(d, q, ky, 1, f, 0) + check(d, q, kuv, 2, f, 1);
        if (f & 1) @autoreleasepool {
            id<MTLTexture> fy = [d newTextureWithDescriptor:desc(w, h, MTLPixelFormatR8Unorm, MTLTextureUsageShaderRead) iosurface:s plane:0];
            id<MTLTexture> fuv = [d newTextureWithDescriptor:desc(w / 2, h / 2, MTLPixelFormatRG8Unorm, MTLTextureUsageShaderRead) iosurface:s plane:1];
            bad += check(d, q, fy, 1, f, 0) + check(d, q, fuv, 2, f, 1);
        }
    }
    printf("  %d frames, kept and fresh read-only wrappers: %s (%ld wrong bytes)\n", frames, bad ? "FAIL" : "ok", bad); fails += bad != 0;
    // writable wrapper of plane 0: a GPU clear must land in the surface; plane 1 stays as the CPU wrote it
    const int f = frames;
    fill(s, w, h, f);
    @autoreleasepool {
        id<MTLTexture> wy = [d newTextureWithDescriptor:desc(w, h, MTLPixelFormatR8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)
                                               iosurface:s plane:0];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = wy; p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore; p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0, 0, 1);
        id<MTLCommandBuffer> cb = [q commandBuffer]; [[cb renderCommandEncoderWithDescriptor:p] endEncoding]; [cb commit]; [cb waitUntilCompleted];
    }
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *y = IOSurfaceGetBaseAddressOfPlane(s, 0), *uv = IOSurfaceGetBaseAddressOfPlane(s, 1);
    const size_t yr = IOSurfaceGetBytesPerRowOfPlane(s, 0), uvr = IOSurfaceGetBytesPerRowOfPlane(s, 1);
    long ybad = 0, uvbad = 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) ybad += y[j * yr + i] < 127 || y[j * yr + i] > 128;
    for (int j = 0; j < h / 2; j++) for (int i = 0; i < w; i++) uvbad += uv[j * uvr + i] != uvpx(i, j, f);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    printf("  writable plane-0 wrapper: clear reached the surface %s (%ld wrong), plane 1 untouched %s (%ld wrong)\n",
           ybad ? "FAIL" : "ok", ybad, uvbad ? "FAIL" : "ok", uvbad);
    fails += ybad != 0; fails += uvbad != 0;
    // and the kept read-only wrapper sees the cleared plane on the GPU
    long kb = 0;
    { const size_t row = (size_t)w; id<MTLBuffer> b = [d newBufferWithLength:row * h options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
      [e copyFromTexture:ky sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
      [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      const uint8_t *pp = b.contents; for (size_t i = 0; i < row * h; i++) kb += pp[i] < 127 || pp[i] > 128; }
    printf("  kept read-only wrapper sees the clear: %s (%ld wrong)\n", kb ? "FAIL" : "ok", kb); fails += kb != 0;
    return fails;
}
int main(int argc, char **argv) { @autoreleasepool {
    signal(SIGALRM, on_alarm); alarm(120);
    const int frames = argc > 1 ? atoi(argv[1]) : 12;
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) { fprintf(stderr, "NM_TEST_PLUGIN: %s\n", plugin ? dlerror() : "unset"); return 3; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(getenv("NM_TEST_ACCEL") ? getenv("NM_TEST_ACCEL") : "NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc],
        NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    IOObjectRelease(svc); if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    printf("device %s\n", d.name.UTF8String);
    int fails = 0;
    IOSurfaceRef a = mk420(1920, 1080, 0, 0, 0);
    fails += run_case(d, q, "default rows", a, 1920, 1080, frames); CFRelease(a);
    IOSurfaceRef b = mk420(1920, 1080, 1928, 1944, 1928 * 1080 + 200);   // camera-like: odd rows, plane 1 not page aligned
    fails += run_case(d, q, "camera-like rows", b, 1920, 1080, frames); CFRelease(b);
    IOSurfaceRef c = mk420(640, 480, 0, 0, 0);
    fails += run_case(d, q, "small", c, 640, 480, frames); CFRelease(c);
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
} }
