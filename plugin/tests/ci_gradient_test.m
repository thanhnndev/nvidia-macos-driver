// Core Image renders a smooth gradient with sRGB output into an IOSurface BGRA8 texture (Photo Booth's photo/preview
// path once #471 lets CI accept surface textures); compare with CI's CPU renderer. Banding shows as large errors.
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        const int W = 256, H = 64;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CIContext *gpu = [CIContext contextWithMTLCommandQueue:q options:@{kCIContextCacheIntermediates:@NO}];
        CIContext *cpu = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer:@YES}];
        CIFilter *g = [CIFilter filterWithName:@"CILinearGradient"];
        [g setValue:[CIVector vectorWithX:0 Y:0] forKey:@"inputPoint0"]; [g setValue:[CIVector vectorWithX:W Y:0] forKey:@"inputPoint1"];
        [g setValue:[CIColor colorWithRed:0.55 green:0.55 blue:0.55 colorSpace:srgb] forKey:@"inputColor0"];
        [g setValue:[CIColor colorWithRed:0.9 green:0.9 blue:0.9 colorSpace:srgb] forKey:@"inputColor1"];
        CIImage *img = [g.outputImage imageByCroppingToRect:CGRectMake(0, 0, W, H)];
        NSMutableData *ref = [NSMutableData dataWithLength:W * H * 4];
        CIRenderDestination *cd = [[CIRenderDestination alloc] initWithBitmapData:ref.mutableBytes width:W height:H bytesPerRow:W * 4 format:kCIFormatBGRA8];
        cd.colorSpace = srgb; NSError *err = nil;
        [[cpu startTaskToRender:img toDestination:cd error:&err] waitUntilCompletedAndReturnError:&err];
        int fails = 0;
        for (int mode = 0; mode < 2; mode++) {
            CVPixelBufferRef pb = NULL; id<MTLTexture> t;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
            if (mode) { CVPixelBufferCreate(NULL, W, H, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}}, &pb);
                        t = [dev newTextureWithDescriptor:td iosurface:CVPixelBufferGetIOSurface(pb) plane:0]; }
            else t = [dev newTextureWithDescriptor:td];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            CIRenderDestination *d = [[CIRenderDestination alloc] initWithMTLTexture:t commandBuffer:cb];
            d.colorSpace = srgb; d.flipped = NO;
            CIRenderTask *task = [gpu startTaskToRender:img toDestination:d error:&err];
            [cb commit]; [cb waitUntilCompleted]; [task waitUntilCompletedAndReturnError:&err];
            NSMutableData *out = [NSMutableData dataWithLength:W * H * 4];
            [t getBytes:out.mutableBytes bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            const uint8_t *a = out.bytes, *b = ref.bytes; int maxd = 0, uniq = 0; int seen[256] = {0};
            for (int x = 0; x < W; x++) { int row = H / 2; int v = a[(row * W + x) * 4 + 1]; if (!seen[v]++) uniq++;
                int dd = abs(v - b[(row * W + x) * 4 + 1]); if (dd > maxd) maxd = dd; }
            printf("%s: max |gpu-cpu| %d, distinct green levels along the gradient %d (cpu has ~%d)\n", mode ? "IOSurface texture" : "plain texture", maxd, uniq, (int)(0.35 * 255));
            printf("  gpu row: "); for (int x = 0; x < W; x += 16) printf("%d ", a[((H / 2) * W + x) * 4 + 1]); printf("\n  cpu row: ");
            for (int x = 0; x < W; x += 16) printf("%d ", b[((H / 2) * W + x) * 4 + 1]); printf("\n");
            if (maxd > 3) fails++;
            if (pb) CVPixelBufferRelease(pb);
        }
        printf(fails ? "FAIL\n" : "PASS\n"); return fails;
    }
}
