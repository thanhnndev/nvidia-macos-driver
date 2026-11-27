// buffer-list audit (docs/890M-TRACE.md part 12): a compute kernel reads a no-copy buffer through a buffer binding,
// 5000 command buffers. The audit must count the uses and find every one named by the part's resources (0 misses).
//   NVMTL_BL_AUDIT=1 NVMTL_LOG_PATH=/tmp/x.log NM_TEST_PLUGIN=/path/NVMTLDriver ./audit_test
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <mach/mach.h>
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) return 3;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    NSError *err = nil;
    id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void k(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]], uint i [[thread_position_in_grid]]) { dst[i] = src[i] + 1; }"
        options:nil error:&err];
    if (!lib) { printf("no library: %s\n", err.description.UTF8String); return 6; }
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
    if (!ps) { printf("no pipeline: %s\n", err.description.UTF8String); return 7; }
    id<MTLCommandQueue> q = [d newCommandQueue];
    const size_t n = 65536; vm_address_t a = 0; vm_allocate(mach_task_self(), &a, n, VM_FLAGS_ANYWHERE);
    id<MTLBuffer> src = [d newBufferWithBytesNoCopy:(void *)a length:n options:MTLResourceStorageModeShared
                                       deallocator:^(void *p, NSUInteger l) { vm_deallocate(mach_task_self(), (vm_address_t)p, l); }];
    id<MTLBuffer> dst = [d newBufferWithLength:n options:MTLResourceStorageModeShared];
    uint32_t *s = src.contents; for (size_t i = 0; i < n / 4; i++) s[i] = (uint32_t)i;
    int fails = 0;
    for (int r = 0; r < 5000; r++) { @autoreleasepool {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:ps]; [e setBuffer:src offset:0 atIndex:0]; [e setBuffer:dst offset:0 atIndex:1];
        [e dispatchThreads:MTLSizeMake(n / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [e endEncoding];
        [cb commit]; if (r % 500 == 499 || r == 0) [cb waitUntilCompleted];
        if (r == 0 || r == 4999) { const uint32_t *o = dst.contents; for (size_t i = 0; i < n / 4; i++) if (o[i] != (uint32_t)i + 1) { printf("FAIL at %zu: %u\n", i, o[i]); fails++; break; } }
    } }
    printf("%s\n", fails ? "FAILED" : "PASS"); return fails;
}}
