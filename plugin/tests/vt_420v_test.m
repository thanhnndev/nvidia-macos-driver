// VTPixelTransferSession: 420v (video range, two planes) 1920x1080 with a smooth horizontal luma ramp -> 32BGRA
// 1440x960 (Photo Booth's camera conversion). Banding shows as few distinct output levels / big steps.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#include <stdlib.h>
static CVPixelBufferRef mk(int w, int h, OSType f) { CVPixelBufferRef p = NULL;
    CVPixelBufferCreate(NULL, w, h, f, (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}, (id)kCVPixelBufferMetalCompatibilityKey:@YES}, &p); return p; }
int main(int argc, char **argv) {
    @autoreleasepool {
        const int W = 1920, H = 1080, OW = argc > 1 ? atoi(argv[1]) : 1440, OH = argc > 2 ? atoi(argv[2]) : 960;
        CVPixelBufferRef src = mk(W, H, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        CVPixelBufferLockBaseAddress(src, 0);
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(src, 0); size_t yr = CVPixelBufferGetBytesPerRowOfPlane(src, 0);
        uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(src, 1); size_t uvr = CVPixelBufferGetBytesPerRowOfPlane(src, 1);
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) y[j * yr + i] = (uint8_t)(120 + i * 100 / W);   // 120..219
        for (int j = 0; j < H / 2; j++) for (int i = 0; i < W; i++) uv[j * uvr + i] = 128;
        CVPixelBufferUnlockBaseAddress(src, 0);
        CVBufferSetAttachment(src, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(src, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(src, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVPixelBufferRef dst = mk(OW, OH, kCVPixelFormatType_32BGRA);
        VTPixelTransferSessionRef s = NULL; VTPixelTransferSessionCreate(NULL, &s);
        OSStatus st = VTPixelTransferSessionTransferImage(s, src, dst);
        CVPixelBufferLockBaseAddress(dst, kCVPixelBufferLock_ReadOnly);
        uint8_t *o = CVPixelBufferGetBaseAddress(dst); size_t orow = CVPixelBufferGetBytesPerRow(dst);
        int seen[256] = {0}, uniq = 0, maxstep = 0, prev = -1;
        for (int i = 0; i < OW; i++) { int v = o[(OH / 2) * orow + i * 4 + 1]; if (!seen[v]++) uniq++; if (prev >= 0 && abs(v - prev) > maxstep) maxstep = abs(v - prev); prev = v; }
        printf("transfer status %d; %dx%d: distinct green levels %d (ramp spans ~116 levels), biggest step between neighbours %d\n  row:", (int)st, OW, OH, uniq, maxstep);
        for (int i = 0; i < OW; i += OW / 16) printf(" %d", o[(OH / 2) * orow + i * 4 + 1]); printf("\n");
        CVPixelBufferUnlockBaseAddress(dst, kCVPixelBufferLock_ReadOnly);
        printf(uniq < 80 || maxstep > 3 ? "FAIL\n" : "PASS\n"); return 0;
    }
}
