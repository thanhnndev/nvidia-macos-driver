// Managed buffers (docs/890M-TRACE.md part 11): CPU write + didModifyRange -> GPU read, GPU write + synchronizeResource
// -> CPU read, for newBufferWithLength and newBufferWithBytesNoCopy (>= 1 MiB), and the commit cost of 50 Managed
// buffers modified per frame. Run with NVMTL_MANAGED_SYSMEM=0 (VRAM copy) and unset (one allocation on an iGPU).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <mach/mach.h>
static double now_us(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e3; }
static int fails;
static void check(const char *what, const uint32_t *p, size_t from, size_t to, uint32_t (^want)(size_t)) {
    for (size_t i = from; i < to; i++) if (p[i] != want(i)) { printf("FAIL %s: word %zu = %08x want %08x\n", what, i, p[i], want(i)); fails++; return; }
    printf("ok   %s\n", what);
}
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) return 3;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    const size_t N = 4 << 20, W = N / 4;
    id<MTLBuffer> out = [d newBufferWithLength:N options:MTLResourceStorageModeShared];
    void (^copy)(id<MTLBuffer>, id<MTLBuffer>) = ^(id<MTLBuffer> s, id<MTLBuffer> t) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:s sourceOffset:0 toBuffer:t destinationOffset:0 size:MIN(s.length, t.length)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; };
    for (int nc = 0; nc < 2; nc++) {
        id<MTLBuffer> m;
        if (!nc) m = [d newBufferWithLength:N options:MTLResourceStorageModeManaged];
        else { vm_address_t a = 0; vm_allocate(mach_task_self(), &a, N, VM_FLAGS_ANYWHERE);
               m = [d newBufferWithBytesNoCopy:(void *)a length:N options:MTLResourceStorageModeManaged
                                   deallocator:^(void *p, NSUInteger l) { vm_deallocate(mach_task_self(), (vm_address_t)p, l); }]; }
        const char *k = nc ? "no-copy managed" : "managed";
        uint32_t *c = m.contents; char what[96];
        for (size_t i = 0; i < W; i++) c[i] = 0xA0000000u + (uint32_t)i;
        [m didModifyRange:NSMakeRange(0, N)]; copy(m, out);
        snprintf(what, sizeof what, "%s: CPU write + didModifyRange -> GPU read", k);
        check(what, out.contents, 0, W, ^uint32_t(size_t i) { return 0xA0000000u + (uint32_t)i; });
        for (size_t i = W / 4; i < W / 2; i++) c[i] = 0xB0000000u + (uint32_t)i;
        [m didModifyRange:NSMakeRange(N / 4, N / 4)]; copy(m, out);
        snprintf(what, sizeof what, "%s: partial didModifyRange", k);
        check(what, out.contents, 0, W, ^uint32_t(size_t i) { return (i >= W / 4 && i < W / 2 ? 0xB0000000u : 0xA0000000u) + (uint32_t)i; });
        { id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
          [e fillBuffer:m range:NSMakeRange(N / 2, N / 4) value:0x5A]; [e synchronizeResource:m]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; }
        snprintf(what, sizeof what, "%s: GPU fill + synchronizeResource -> CPU read", k);
        check(what, m.contents, 0, W, ^uint32_t(size_t i) { return i >= W / 2 && i < 3 * W / 4 ? 0x5A5A5A5Au : ((i >= W / 4 && i < W / 2 ? 0xB0000000u : 0xA0000000u) + (uint32_t)i); });
    }
    NSMutableArray<id<MTLBuffer>> *vb = [NSMutableArray new];
    for (int i = 0; i < 50; i++) [vb addObject:[d newBufferWithLength:65536 options:MTLResourceStorageModeManaged]];
    id<MTLBuffer> sink = [d newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
    double cpu = 0, wall = 0; const int F = 200;
    for (int f = 0; f < F + 20; f++) { @autoreleasepool {
        uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); double w0 = now_us();
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        for (id<MTLBuffer> b in vb) { memset(b.contents, f, 16384); [b didModifyRange:NSMakeRange(0, 16384)];
                                      [e copyFromBuffer:b sourceOffset:0 toBuffer:sink destinationOffset:0 size:256]; }
        [e endEncoding]; [cb commit]; [cb waitUntilScheduled];
        if (f >= 20) { cpu += (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0) / 1e3; wall += now_us() - w0; }
        [cb waitUntilCompleted]; } }
    printf("50 managed buffers modified per frame: %.0f us CPU, %.0f us to scheduled\n", cpu / F, wall / F);
    printf("%s\n", fails ? "FAILED" : "PASS"); return fails ? 1 : 0;
}}
