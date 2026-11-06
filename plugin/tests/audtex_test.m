// Buffer-list audit, texture side: Core Image samples a texture made straight on a no-copy buffer's pages. The audit
// must count that binding as a use of the import (log "first texture binding on no-copy pages") and find it named by
// the part's resources (no "AUDIT MISS"); the rendered pixels must match the buffer.
//   NVMTL_BL_AUDIT=1 NVMTL_LOG_PATH=/tmp/x.log ./audtex_test   (installed plugin; AUDTEX_PLAIN=1: an ordinary texture)
#import <Metal/Metal.h>
#import <CoreImage/CoreImage.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <mach/mach.h>
int main(void) { @autoreleasepool {
    // Core Image loads the installed plugin itself, so this test runs on the installed build (MTLCreateSystemDefaultDevice).
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) return 5;
    id<MTLCommandQueue> q = [d newCommandQueue];
    const int W = 64, H = 64; const size_t n = 65536;
    vm_address_t a = 0; if (vm_allocate(mach_task_self(), &a, n, VM_FLAGS_ANYWHERE)) return 6;
    uint32_t *px = (uint32_t *)a; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) px[y * W + x] = 0xff000000u | (uint32_t)(x * 4) << 16 | (uint32_t)(y * 4) << 8 | 0x40;
    id<MTLBuffer> src = [d newBufferWithBytesNoCopy:(void *)a length:n options:MTLResourceStorageModeShared deallocator:nil];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tin = [src newTextureWithDescriptor:td offset:0 bytesPerRow:W * 4];
    if (getenv("AUDTEX_PLAIN")) { tin = [d newTextureWithDescriptor:td]; [tin replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4]; }
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    id<MTLTexture> tout = [d newTextureWithDescriptor:td];
    id<MTLBuffer> rb = [d newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    if (!src || !tin || !tout || !rb) { printf("FAIL setup\n"); return 7; }
    CIContext *ci = [CIContext contextWithMTLCommandQueue:q options:@{kCIContextCacheIntermediates:@NO, kCIContextWorkingColorSpace:[NSNull null], kCIContextOutputColorSpace:[NSNull null]}];
    int fails = 0;
    for (int it = 0; it < 4; it++) {
        CIImage *img = [CIImage imageWithMTLTexture:tin options:@{kCIImageColorSpace:[NSNull null]}];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        const int blit = getenv("AUDTEX_BLIT") != NULL;   // no Core Image: the GPU copies the buffer texture itself
        if (!blit) [ci render:img toMTLTexture:tout commandBuffer:cb bounds:CGRectMake(0, 0, W, H) colorSpace:(CGColorSpaceRef)nil];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:blit ? tin : tout sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                  toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("FAIL command buffer status %ld\n", (long)cb.status); return 8; }
        const uint32_t *o = rb.contents; int bad = 0, first = -1;
        for (int i = 0; i < W * H; i++) {   // CI may flip rows: accept either orientation per row
            int y = i / W, x = i % W; uint32_t want = px[i], flip = px[(H - 1 - y) * W + x];
            if (o[i] != want && o[i] != flip) { if (first < 0) first = i; bad++; } }
        if (bad) { printf("FAIL pass %d: %d of %d pixels wrong (first %d: %08x want %08x)\n", it, bad, W * H, first, o[first], px[first]); fails++; }
        else printf("ok   pass %d: %s the no-copy buffer texture\n", it, blit ? "a blit copied" : "Core Image sampled");
        if (blit) px[5] ^= 0x00101000u;   // the CPU changes the pages between passes (Core Image keeps its own copy of a
                                          // source texture it has seen, so only the blit pass checks this)
    }
    printf(fails ? "FAIL\n" : "PASS\n"); return fails ? 1 : 0;
} }
