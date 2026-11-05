// buffer texture shadow test: textures over an MTLBuffer whose bytesPerRow is not a multiple of 256 (GFX11 linear
// pitch), which NVMTL serves through a shadow image (nvmtl_btsh).
//   NM_TEST_PLUGIN=/path/NVMTLDriver ./btsh_test
// 1. read: R8 176x48, pitch 192 (WindowServer's RenderBox masks), and RGBA8 100x20 pitch 448 at buffer offset 64:
//    blit the texture to a plain buffer, every texel must match; change the bytes, read again (fresh upload per cb).
// 2. write: render-clear an RGBA8 100x20 pitch 448 buffer texture, the texels must land in the buffer and the row
//    padding must keep its bytes.
// 3. cost: 300 command buffers that each read the R8 texture.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
static double now_us(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e3; }
static int fails;
static id<MTLTexture> btex(id<MTLBuffer> b, MTLPixelFormat f, int w, int h, size_t off, size_t pitch, MTLTextureUsage u) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = u;
    return [b newTextureWithDescriptor:td offset:off bytesPerRow:pitch]; }
static void fill(uint8_t *p, int rb, int h, size_t pitch, int seed) {
    for (int y = 0; y < h; y++) for (size_t x = 0; x < pitch; x++)
        p[y * pitch + x] = x < (size_t)rb ? (uint8_t)(x * 7 + y * 13 + seed) : 0xAA; }
static int readback(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLTexture> t, const uint8_t *src, int rb, int h, size_t pitch, const char *what) {
    id<MTLBuffer> o = [d newBufferWithLength:(NSUInteger)rb * h options:MTLResourceStorageModeShared];
    memset(o.contents, 0x55, o.length);
    id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
              toBuffer:o destinationOffset:0 destinationBytesPerRow:rb destinationBytesPerImage:(NSUInteger)rb * h];
    [e endEncoding]; [c commit]; [c waitUntilCompleted];
    int bad = 0, first = -1; const uint8_t *g = o.contents;
    for (int y = 0; y < h; y++) for (int x = 0; x < rb; x++) if (g[y * rb + x] != src[y * pitch + x]) { if (first < 0) first = y * rb + x; bad++; }
    printf("%s: %d/%d bytes differ%s", what, bad, rb * h, bad ? "" : " ok\n");
    if (bad) printf(" (first at row %d col %d: got %02x want %02x) FAIL\n", first / rb, first % rb, g[first], src[(first / rb) * pitch + first % rb]);
    fails += !!bad; return bad; }
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) { fprintf(stderr, "NM_TEST_PLUGIN: %s\n", plugin ? dlerror() : "unset"); return 3; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(getenv("NM_TEST_ACCEL") ? getenv("NM_TEST_ACCEL") : "NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    IOObjectRelease(svc); if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue]; printf("device %s\n", d.name.UTF8String);
    // 1a. R8 176x48 pitch 192
    id<MTLBuffer> m = [d newBufferWithLength:192 * 48 options:MTLResourceStorageModeShared];
    fill(m.contents, 176, 48, 192, 0);
    id<MTLTexture> mt = btex(m, MTLPixelFormatR8Unorm, 176, 48, 0, 192, MTLTextureUsageShaderRead);
    if (!mt) { printf("R8 buffer texture: nil FAIL\n"); return 1; }
    readback(d, q, mt, m.contents, 176, 48, 192, "R8 176x48 pitch 192 read");
    fill(m.contents, 176, 48, 192, 99);
    readback(d, q, mt, m.contents, 176, 48, 192, "R8 176x48 pitch 192 read after CPU change");
    // 1b. RGBA8 100x20 pitch 448 at offset 64
    id<MTLBuffer> r = [d newBufferWithLength:64 + 448 * 20 options:MTLResourceStorageModeShared];
    memset(r.contents, 0x33, 64); fill((uint8_t *)r.contents + 64, 400, 20, 448, 5);
    id<MTLTexture> rt = btex(r, MTLPixelFormatRGBA8Unorm, 100, 20, 64, 448, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    if (!rt) { printf("RGBA8 buffer texture: nil FAIL\n"); return 1; }
    readback(d, q, rt, (uint8_t *)r.contents + 64, 400, 20, 448, "RGBA8 100x20 pitch 448 offset 64 read");
    // 2. render clear to (1, 0.5, 0, 1)
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = rt; p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore; p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0.5, 0, 1);
    id<MTLCommandBuffer> c = [q commandBuffer]; [[c renderCommandEncoderWithDescriptor:p] endEncoding]; [c commit]; [c waitUntilCompleted];
    const uint8_t *rb = r.contents; int tex_bad = 0, pad_bad = 0, head_bad = 0;
    for (int i = 0; i < 64; i++) head_bad += rb[i] != 0x33;
    for (int y = 0; y < 20; y++) for (int x = 0; x < 448; x++) { uint8_t v = rb[64 + y * 448 + x];
        if (x < 400) { int ch = x & 3; uint8_t want = ch == 0 ? 255 : ch == 1 ? 128 : ch == 2 ? 0 : 255;
                       if (v != want && !(ch == 1 && (v == 127 || v == 128))) tex_bad++; }
        else if (v != 0xAA) pad_bad++; }
    printf("RGBA8 render clear writeback: texels bad %d/8000, row padding changed %d/960, bytes before offset changed %d/64 %s\n",
           tex_bad, pad_bad, head_bad, tex_bad || pad_bad || head_bad ? "FAIL" : "ok");
    fails += tex_bad || pad_bad || head_bad;
    // 3. cost: like WindowServer, 6 shadowed masks per command buffer; CPU time of commit (the prologue records the uploads)
    id<MTLTexture> ms[6]; for (int i = 0; i < 6; i++) ms[i] = btex(m, MTLPixelFormatR8Unorm, 176, 48, 0, 192, MTLTextureUsageShaderRead);
    id<MTLBuffer> o = [d newBufferWithLength:176 * 48 * 6 options:MTLResourceStorageModeShared];
    const int N = 300; double t0 = now_us(), gpu = 0, cpu = 0;
    for (int i = 0; i < N; i++) { @autoreleasepool {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        for (int k = 0; k < 6; k++)
            [e copyFromTexture:ms[k] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(176, 48, 1)
                      toBuffer:o destinationOffset:176 * 48 * k destinationBytesPerRow:176 destinationBytesPerImage:176 * 48];
        [e endEncoding];
        uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); [cb commit]; cpu += (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0) / 1e3;
        [cb waitUntilCompleted]; gpu += (cb.GPUEndTime - cb.GPUStartTime) * 1e6; } }
    printf("cost: %d command buffers, 6 R8 shadows each: commit %.1f us CPU, %.0f us wall, %.0f us GPU\n", N, cpu / N, (now_us() - t0) / N, gpu / N);
    for (int k = 0; k < 6; k++) { int bad = memcmp((uint8_t *)o.contents + 176 * 48 * k, (uint8_t *)o.contents, 176 * 48) != 0; fails += bad; }
    printf("%s\n", fails ? "FAIL" : "PASS"); return fails ? 1 : 0;
}}
