// keep timer test: a surface wrapped once, then released by this process; no wrapper is made afterwards. With the idle
// sweep timer the keep entry goes within NVMTL_KEEP_MS + one timer period and the surface is freed.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    if (!plugin || !dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) return 3;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return 4;
    id<MTLDevice> d = ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
    if (!d) return 5;
    // KT_TWO=1: a second device wraps (and keeps) another surface first, so the first device's table is not the only
    // non-empty one: each device must get its own sweep timer (review of 7ceff16)
    id<MTLDevice> d2 = getenv("KT_TWO") ? ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc) : nil;
    IOSurfaceRef keep2 = NULL;
    if (d2) { keep2 = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@256, (id)kIOSurfaceHeight:@256,
                  (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
              MTLTextureDescriptor *td2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
              td2.storageMode = MTLStorageModeShared; td2.usage = MTLTextureUsageShaderRead;
              @autoreleasepool { id<MTLTexture> t2 = [d2 newTextureWithDescriptor:td2 iosurface:keep2 plane:0]; printf("second device wrapped: %s\n", t2 ? "ok" : "FAILED"); } }
    IOSurfaceID sid;
    @autoreleasepool {
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@1024, (id)kIOSurfaceHeight:@1024,
            (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
        sid = IOSurfaceGetID(s);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1024 height:1024 mipmapped:NO];
        td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
        printf("wrapped surface %u: %s\n", sid, t ? "ok" : "FAILED");
        t = nil; CFRelease(s);
    }
    for (int i = 0; i <= 8; i += 2) {
        IOSurfaceRef l = IOSurfaceLookup(sid);
        printf("t=%d s: surface %u %s\n", i, sid, l ? "still alive" : "freed");
        if (l) CFRelease(l);
        if (!l) break;
        usleep(2000000);
    }
    return 0;
}}
