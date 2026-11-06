// buffer list: commit cost with N live newBufferWithBytesNoCopy imports that the command buffer does not use, plus
// correctness of copies through no-copy buffers (buffer -> buffer, buffer -> texture on a no-copy buffer -> buffer) and
// of a buffer whose gpuAddress was read (back on the global list).
//   NM_TEST_PLUGIN=/path/NVMTLDriver ./nocopy_list_test        NVMTL_BUFFER_LIST=0 for the old behaviour
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <mach/mach.h>
static double now_us(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e3; }
static void *pages(size_t n) { vm_address_t a = 0; return vm_allocate(mach_task_self(), &a, n, VM_FLAGS_ANYWHERE) ? NULL : (void *)a; }
static id<MTLBuffer> nocopy(id<MTLDevice> d, size_t n) {
    void *p = pages(n); if (!p) return nil;
    return [d newBufferWithBytesNoCopy:p length:n options:MTLResourceStorageModeShared
                           deallocator:^(void *q, NSUInteger l) { vm_deallocate(mach_task_self(), (vm_address_t)q, l); }];
}
static int check(const char *what, const uint32_t *got, uint32_t seed, size_t words) {
    for (size_t i = 0; i < words; i++) if (got[i] != seed + (uint32_t)i) { printf("FAIL %s: word %zu = %08x, want %08x\n", what, i, got[i], seed + (uint32_t)i); return 1; }
    printf("ok   %s\n", what); return 0;
}
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) return 3;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> a = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared], b = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    NSMutableArray *keep = [NSMutableArray new]; int fails = 0;
    int steps[] = { 0, 250, 500, 1000 };
    for (int s = 0; s < 4; s++) {
        while ((int)keep.count < steps[s]) { id<MTLBuffer> x = nocopy(d, 65536); if (!x) return 6; [keep addObject:x]; }
        double cpu = 0, wall = 0; const int N = 200;
        for (int i = 0; i < N + 20; i++) { @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:256]; [e endEncoding];
            uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); double w0 = now_us();
            [cb commit]; [cb waitUntilScheduled];
            if (i >= 20) { cpu += (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0) / 1e3; wall += now_us() - w0; }
            [cb waitUntilCompleted]; } }
        printf("live no-copy buffers %4d: commit %.0f us CPU, %.0f us to scheduled\n", (int)keep.count, cpu / N, wall / N); fflush(stdout);
    }
    // correctness, with the 1000 still alive
    const size_t n = 65536, w = n / 4;
    id<MTLBuffer> src = nocopy(d, n), dst = nocopy(d, n), out = nocopy(d, n), esc = nocopy(d, n);
    uint32_t *s32 = src.contents; for (size_t i = 0; i < w; i++) s32[i] = 0x1000 + (uint32_t)i;
    memset(dst.contents, 0, n); memset(out.contents, 0, n);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [dst newTextureWithDescriptor:td offset:0 bytesPerRow:256];   // 64x64 RGBA8 = 16 KB of dst
    { id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
      [e copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:0 size:n]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      fails += check("no-copy -> no-copy copy", dst.contents, 0x1000, w); }
    { id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
      [e copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1)
                toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:16384];
      [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      fails += check("texture on a no-copy buffer -> no-copy buffer", out.contents, 0x1000, 16384 / 4); }
    { uint32_t *e32 = esc.contents; for (size_t i = 0; i < w; i++) e32[i] = 0x5000 + (uint32_t)i;
      printf("     gpuAddress of the escaped buffer: 0x%llx\n", (unsigned long long)esc.gpuAddress);
      memset(out.contents, 0, n);
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
      [e copyFromBuffer:esc sourceOffset:0 toBuffer:out destinationOffset:0 size:n]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      fails += check("escaped (gpuAddress) no-copy -> no-copy copy", out.contents, 0x5000, w);
      printf("     command buffer status %ld error %s\n", (long)cb.status, cb.error ? cb.error.description.UTF8String : "none"); }
    printf("%s\n", fails ? "FAILED" : "PASS"); return fails ? 1 : 0;
}}
