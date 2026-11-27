// submit cost vs live allocations: commit an almost empty command buffer with N shared MTLBuffers and M IOSurface
// textures alive (none used by the command buffer). If the kernel CS ioctl gets every live BO, the cost grows with N+M.
//   NM_TEST_PLUGIN=/path/NVMTLDriver ./submit_scale_test
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
static double now_us(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e3; }
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) return 3;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> a = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared], b = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    NSMutableArray *keep = [NSMutableArray new]; NSMutableArray *surfs = [NSMutableArray new];
    int steps[][2] = { {0, 0}, {250, 0}, {500, 0}, {1000, 0}, {1000, 250}, {1000, 500} };
    int nb = 0, ns = 0;
    for (int s = 0; s < 6; s++) {
        while (nb < steps[s][0]) { [keep addObject:[d newBufferWithLength:65536 options:MTLResourceStorageModeShared]]; nb++; }
        while (ns < steps[s][1]) {
            IOSurfaceRef x = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@256, (id)kIOSurfaceHeight:@256,
                (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            [surfs addObject:[d newTextureWithDescriptor:td iosurface:x plane:0]]; CFRelease(x); ns++; }
        // warm up, then time
        double cpu = 0, wall = 0; const int N = 200;
        for (int i = 0; i < N + 20; i++) { @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:256]; [e endEncoding];
            uint64_t c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); double w0 = now_us();
            [cb commit]; [cb waitUntilScheduled];
            if (i >= 20) { cpu += (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0) / 1e3; wall += now_us() - w0; }
            [cb waitUntilCompleted]; } }
        printf("live buffers %4d, live surface textures %4d: commit %.0f us CPU, %.0f us to scheduled\n", nb, ns, cpu / N, wall / N);
        fflush(stdout);
    }
    return 0;
}}
