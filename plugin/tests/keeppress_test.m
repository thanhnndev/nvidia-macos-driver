// keep pressure test (roadmap 4 F1): idle keep entries go on a memory-pressure event, held ones never do, an entry a
// command buffer in flight uses survives the event and goes only after completion, and nothing changes at NORMAL.
// Own process, on the 890M, nothing installed:
//   NM_TEST_PLUGIN=<built NVMTLDriver> NVMTL_KEEP_DEAD=0 ./keeppress_test   (dead entries kept, as in WindowServer)
// Events through nvmtl_test_keep_pressure (same queue, same lock and same handler as the dispatch source). Surfaces
// (4 MB each, over the 2 MB small-tile size):
//   A wrapped and released 6 s before the WARN event      -> dropped by WARN (idle >= 5 s), surface freed
//   B wrapped, its texture kept by the test (held)        -> kept through WARN and CRITICAL; a re-wrap is a keep hit
//   C wrapped and released just before the WARN event     -> kept by WARN (idle < 5 s), dropped by CRITICAL
//   E wrapped, then re-wrapped (keep hit: an alias) and read by a blit in a command buffer that waits on an event;
//     the alias and our surface reference are dropped, CRITICAL is sent while the command buffer is still waiting
//     -> E survives; the event is signalled, the command buffer completes without error and copied E's pattern;
//     a second CRITICAL then frees E (freed only after completion)
//   D wrapped and released, idle 6 s, then NORMAL         -> still alive: NORMAL changes nothing
// KEEPPRESS_SOURCE=1: the real dispatch source path instead. Wraps A (idle) and B (held), then waits up to 180 s for a
// system memory-pressure event. NVMTL_KEEP_MS / NVMTL_KEEP_TIMER_MS are raised to 10 min so the idle expiry cannot free A
// (d70ae280's first source run passed at 29.6 s after the wrap: that was the 30 s expiry, no event). The plugin's log
// goes to the unified log and to NVMTL_LOG_PATH, set here to keeppress-source.log next to the binary's cwd; pass
// = A freed, B alive, and a "memory pressure WARN|CRITICAL" line from this pid in that file.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static IOSurfaceRef mk(uint8_t fill) {
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@1024, (id)kIOSurfaceHeight:@1024,
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'BGRA')});
    if (s && fill) { IOSurfaceLock(s, 0, NULL); memset(IOSurfaceGetBaseAddress(s), fill, IOSurfaceGetAllocSize(s)); IOSurfaceUnlock(s, 0, NULL); }
    return s;
}
static id<MTLTexture> wrap(id<MTLDevice> d, IOSurfaceRef s) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1024 height:1024 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    return [d newTextureWithDescriptor:td iosurface:s plane:0];
}
// a keep hit returns an alias whose _parent is the kept texture; a fresh import has none
static id parentOf(id t) { Ivar v = class_getInstanceVariable(object_getClass(t), "_parent"); return v ? object_getIvar(t, v) : nil; }
// wrap, release the texture and our surface reference: only the keep holds the surface afterwards
static IOSurfaceID wrapRelease(id<MTLDevice> d) {
    IOSurfaceID sid;
    @autoreleasepool { IOSurfaceRef s = mk(0); sid = IOSurfaceGetID(s); id<MTLTexture> t = wrap(d, s);
        if (!t) printf("wrap of %u FAILED\n", sid); t = nil; CFRelease(s); }
    return sid;
}
static int alive(IOSurfaceID sid) { IOSurfaceRef l = IOSurfaceLookup(sid); if (l) CFRelease(l); return l != NULL; }
static const char *st(IOSurfaceID sid) { return alive(sid) ? "alive" : "freed"; }
static void settle(void) { usleep(400000); }   // drops are released on the keep's release queue
static id<MTLDevice> device(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel")); if (!svc) return nil;
    return ((id(*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
}
static int sourceRun(id<MTLDevice> d, const char *logPath) {
    const IOSurfaceID a = wrapRelease(d);
    IOSurfaceRef bs = mk(0); const IOSurfaceID b = IOSurfaceGetID(bs); id<MTLTexture> bt = wrap(d, bs);
    usleep(6000000);
    printf("source run: A %s, B %s; waiting up to 180 s for a WARN or CRITICAL event (pid %d)\n", st(a), st(b), getpid()); fflush(stdout);
    int i = 0; for (; i < 1800 && alive(a); i++) usleep(100000);
    settle();
    printf("after %.1f s: A %s, B %s\n", i / 10.0, st(a), st(b));
    char line[512]; int ev = 0; FILE *f = fopen(logPath, "r");
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "memory pressure ")) { ev = 1; printf("plugin: %s", line); }
    if (f) fclose(f);
    if (!ev) printf("no memory pressure line in %s: %s\n", logPath, alive(a) ? "no event arrived" : "A went for another reason");
    const int ok = ev && !alive(a) && alive(b) && bt.width == 1024;
    CFRelease(bs); bt = nil;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
int main(void) { @autoreleasepool {
    const char *plugin = getenv("NM_TEST_PLUGIN");
    const int src = getenv("KEEPPRESS_SOURCE") != NULL; char logPath[1100] = "";
    if (src) {   // before the plugin's first keep call reads them
        setenv("NVMTL_KEEP_MS", "600000", 1); setenv("NVMTL_KEEP_TIMER_MS", "600000", 1);
        char cwd[1024]; snprintf(logPath, sizeof logPath, "%s/keeppress-source.log", getcwd(cwd, sizeof cwd) ? cwd : "/private/tmp");
        unlink(logPath); setenv("NVMTL_LOG_PATH", logPath, 1); printf("plugin log: %s (and the unified log)\n", logPath); }
    void *h = plugin ? dlopen(plugin, RTLD_NOW | RTLD_LOCAL) : NULL; if (!h) return 3;
    id<MTLDevice> d = device(); if (!d) return 5;
    if (src) return sourceRun(d, logPath);
    void (*press)(id, unsigned long) = (void (*)(id, unsigned long))dlsym(h, "nvmtl_test_keep_pressure"); if (!press) return 6;
    int fail = 0;
    const IOSurfaceID a = wrapRelease(d);
    IOSurfaceRef bs = mk(0); const IOSurfaceID b = IOSurfaceGetID(bs); id<MTLTexture> bt = wrap(d, bs);
    usleep(6000000);
    const IOSurfaceID c = wrapRelease(d);
    settle();
    printf("before WARN:     A %s, B %s, C %s\n", st(a), st(b), st(c));
    if (!alive(a) || !alive(c)) { printf("FAIL: idle entries went without an event\n"); fail++; }
    press(d, DISPATCH_MEMORYPRESSURE_WARN); settle();
    printf("after WARN:      A %s, B %s, C %s\n", st(a), st(b), st(c));
    if (alive(a)) { printf("FAIL: A (idle 6 s) kept by WARN\n"); fail++; }
    if (!alive(c)) { printf("FAIL: C (idle < 5 s) dropped by WARN\n"); fail++; }
    press(d, DISPATCH_MEMORYPRESSURE_CRITICAL); settle();
    printf("after CRITICAL:  B %s, C %s\n", st(b), st(c));
    if (alive(c)) { printf("FAIL: C kept by CRITICAL\n"); fail++; }
    // B is held by our texture: it must survive both events, stay usable, and a re-wrap must hit the kept entry
    @autoreleasepool {
        id<MTLTexture> b2 = wrap(d, bs);
        const int hit = b2 && parentOf(b2) == bt;
        printf("B re-wrap after both events: %s\n", hit ? "keep hit (alias of the kept texture)" : "MISS (new import)");
        if (!hit) { printf("FAIL: held B's entry was dropped (re-wrap missed)\n"); fail++; }
        b2 = nil;
    }
    CFRelease(bs);
    if (!alive(b)) { printf("FAIL: held B dropped\n"); fail++; }
    if (bt.width != 1024) { printf("FAIL: held B's texture unusable\n"); fail++; }

    // E: a keep entry used only by a command buffer still in flight
    IOSurfaceRef es = mk(0x5a); const IOSurfaceID e = IOSurfaceGetID(es);
    @autoreleasepool { id<MTLTexture> t = wrap(d, es); t = nil; }            // the keep entry
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> out = [d newBufferWithLength:4u << 20 options:MTLResourceStorageModeShared];
    memset(out.contents, 0, out.length);
    id<MTLSharedEvent> ev = [d newSharedEvent];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    @autoreleasepool {
        id<MTLTexture> alias = wrap(d, es);
        const int hit = alias && parentOf(alias) != nil;
        printf("E re-wrap: %s\n", hit ? "keep hit (alias)" : "MISS");
        if (!hit) { printf("FAIL: E re-wrap was not a keep hit\n"); fail++; }
        [cb encodeWaitForEvent:ev value:1];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromTexture:alias sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1024, 1024, 1)
                   toBuffer:out destinationOffset:0 destinationBytesPerRow:4096 destinationBytesPerImage:4u << 20];
        [be endEncoding];
        [cb commit];
        alias = nil;                                                       // only the command buffer holds the alias now
    }
    CFRelease(es);                                                         // and only the keep (and the CB) the surface
    settle();
    printf("E in flight: status %lu (4 = completed), E %s\n", (unsigned long)cb.status, st(e));
    if (cb.status >= MTLCommandBufferStatusCompleted) { printf("FAIL: command buffer did not wait for the event\n"); fail++; }
    press(d, DISPATCH_MEMORYPRESSURE_CRITICAL); settle();
    printf("CRITICAL with E's command buffer in flight: status %lu, E %s\n", (unsigned long)cb.status, st(e));
    if (!alive(e)) { printf("FAIL: E freed under an in-flight command buffer\n"); fail++; }
    ev.signaledValue = 1;
    [cb waitUntilCompleted];
    const uint8_t *o = out.contents; size_t bad = 0; for (size_t i = 0; i < out.length; i++) bad += o[i] != 0x5a;
    printf("E's command buffer: status %lu, error %s, copied bytes not 0x5a: %zu of %lu\n", (unsigned long)cb.status,
           cb.error ? cb.error.localizedDescription.UTF8String : "none", bad, (unsigned long)out.length);
    if (cb.status != MTLCommandBufferStatusCompleted || cb.error || bad) { printf("FAIL: E's command buffer\n"); fail++; }
    cb = nil; settle();
    printf("E after completion, before an event: %s\n", st(e));
    press(d, DISPATCH_MEMORYPRESSURE_CRITICAL); settle();
    printf("E after a second CRITICAL: %s\n", st(e));
    if (alive(e)) { printf("FAIL: E kept after its command buffer completed\n"); fail++; }

    // D: idle 6 s (WARN would drop it), then NORMAL: nothing may change
    const IOSurfaceID dd = wrapRelease(d);
    usleep(6000000);
    press(d, DISPATCH_MEMORYPRESSURE_NORMAL); settle();
    printf("D idle 6 s, after NORMAL: %s\n", st(dd));
    if (!alive(dd)) { printf("FAIL: D dropped by NORMAL\n"); fail++; }
    bt = nil;
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}}
