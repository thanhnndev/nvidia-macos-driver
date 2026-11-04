// The three shapes RenderBox (macOS 26 layered icons, iconservicesagent) needs from the driver:
//   1. a function constant gating a call to an extern [[visible]] function the pipeline links, both called
//      directly and passed as a function pointer to a helper (RenderBox's sample_custom takes custom_fn);
//   2. [[color(0)]] framebuffer fetch;
//   3. textures made from a buffer (newTextureWithDescriptor:offset:bytesPerRow:), with row pitches that are
//      and are not multiples of 256 B.
// Every arm checks the GPU's output against a value computed here.
//
//   clang -fobjc-arc -O1 -o rbx rbx.m -framework Foundation -framework Metal && ./rbx
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static int fails;
static void arm(const char *name, BOOL ok, NSString *detail)
{
    fails += !ok;
    printf("%s %-62s %s\n", ok ? "ok  " : "FAIL", name, detail.UTF8String ?: "");
    fflush(stdout);
}

static NSString *const kSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"constant bool use_custom [[function_constant(0)]];\n"
"[[visible]] float custom_fn(float x);\n"
"kernel void k(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
"  float v = float(i); if (use_custom) v = custom_fn(v); o[i] = v; }\n"
"typedef half4 (*cfn_t)(float2, constant float *);\n"
"[[visible]] half4 custom_px(float2 p, constant float *a);\n"
"__attribute__((noinline)) static half4 sample_custom(cfn_t f, float2 p, constant float *a) { return f(p, a); }\n"
"struct V { float4 pos [[position]]; float2 uv; };\n"
"vertex V vs(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2);\n"
"  V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = p; return o; }\n"
"fragment half4 fs(V in [[stage_in]], constant float *a [[buffer(0)]]) {\n"
"  if (use_custom) return sample_custom(custom_px, in.pos.xy, a);\n"
"  return half4(1, 0, 0, 1); }\n"
"fragment half4 fetch(V in [[stage_in]], half4 dst [[color(0)]]) { return half4(dst.b, dst.g, dst.r, 1.0h) * 0.5h; }\n"
"kernel void bt(texture2d<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]],\n"
"               uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x < t.get_width() && g.y < t.get_height()) o[g.y * t.get_width() + g.x] = t.read(g); }\n"
"fragment float4 bts(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
"  constexpr sampler s(filter::nearest); return t.sample(s, in.uv); }\n";

static NSString *const kVis = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"[[visible]] float custom_fn(float x) { return x * 3.0f + 1.0f; }\n"
"[[visible]] half4 custom_px(float2 p, constant float *a) {\n"
"  return half4(half(a[0]), half(floor(p.x) / 64.0), half(floor(p.y) / 64.0), 1.0h); }\n";

static id<MTLDevice> dev; static id<MTLCommandQueue> q; static id<MTLLibrary> lib, vis;

static BOOL done(id<MTLCommandBuffer> cb, NSString **why) {
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { *why = [NSString stringWithFormat:@"command buffer status %ld %@", (long)cb.status, cb.error]; return NO; }
    return YES;
}
static id<MTLFunction> spec(NSString *name, BOOL use, NSString **why) {
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    [cv setConstantValue:&use type:MTLDataTypeBool atIndex:0];
    NSError *e = nil; id<MTLFunction> f = [lib newFunctionWithName:name constantValues:cv error:&e];
    if (!f) *why = [NSString stringWithFormat:@"newFunctionWithName:constantValues: nil (%@)", e.localizedDescription];
    return f;
}

// compute: o[i] = use ? 3i+1 : i
static int runK(BOOL use, BOOL link, NSString **why) {
    id<MTLFunction> k = spec(@"k", use, why); if (!k) return -1;
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = k;
    if (link) { MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions]; lf.functions = @[[vis newFunctionWithName:@"custom_fn"]]; d.linkedFunctions = lf; }
    NSError *e = nil;
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithDescriptor:d options:0 reflection:nil error:&e];
    if (!ps) { *why = [NSString stringWithFormat:@"pipeline refused (%@)", e.localizedDescription]; return -1; }
    id<MTLBuffer> b = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; memset(b.contents, 0xff, 256);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:ps]; [en setBuffer:b offset:0 atIndex:0];
    [en dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [en endEncoding];
    if (!done(cb, why)) return -1;
    const float *o = b.contents; int good = 0;
    for (int i = 0; i < 64; i++) good += o[i] == (use ? i * 3.0f + 1.0f : (float)i);
    *why = [NSString stringWithFormat:@"%d/64, o[5]=%g", good, o[5]]; return good;
}

// a 64x64 RGBA8 target, filled with `fill` first when load is set; returns the pixels
static NSData *draw(id<MTLFunction> f, MTLLinkedFunctions *lf, BOOL load, const uint8_t fill[4], id<MTLTexture> tex, NSString **why) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = f;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (lf) d.fragmentLinkedFunctions = lf;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { *why = [NSString stringWithFormat:@"pipeline refused (%@)", e.localizedDescription]; return nil; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> rb = [dev newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (load) {
        // the first pass clears to `fill`, the second loads it and fetches
        MTLRenderPassDescriptor *c = [MTLRenderPassDescriptor renderPassDescriptor];
        c.colorAttachments[0].texture = rt; c.colorAttachments[0].loadAction = MTLLoadActionClear;
        c.colorAttachments[0].clearColor = MTLClearColorMake(fill[0] / 255.0, fill[1] / 255.0, fill[2] / 255.0, fill[3] / 255.0);
        c.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:c] endEncoding];
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    } else { rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); }
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps];
    float a0 = 0.5f; [en setFragmentBytes:&a0 length:sizeof a0 atIndex:0];
    if (tex) [en setFragmentTexture:tex atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1)
               toBuffer:rb destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:64 * 256];
    [bl endEncoding];
    if (!done(cb, why)) return nil;
    return [NSData dataWithBytes:rb.contents length:rb.length];
}
static BOOL near(int a, int b) { return abs(a - b) <= 2; }

static int runFS(BOOL use, NSString **why) {
    id<MTLFunction> f = spec(@"fs", use, why); if (!f) return -1;
    MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions]; lf.functions = @[[vis newFunctionWithName:@"custom_px"]];
    NSData *px = draw(f, lf, NO, NULL, nil, why); if (!px) return -1;
    const uint8_t *p = px.bytes; int good = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const uint8_t *c = p + (y * 64 + x) * 4;
        int r = use ? 128 : 255, g = use ? (int)lround(x / 64.0 * 255) : 0, b = use ? (int)lround(y / 64.0 * 255) : 0;
        good += near(c[0], r) && near(c[1], g) && near(c[2], b) && c[3] == 255;
    }
    const uint8_t *c = p + (10 * 64 + 20) * 4;
    *why = [NSString stringWithFormat:@"%d/4096, (20,10) = %u,%u,%u,%u", good, c[0], c[1], c[2], c[3]]; return good;
}

static int runFetch(NSString **why) {
    const uint8_t fill[4] = {200, 100, 40, 255};
    NSData *px = draw([lib newFunctionWithName:@"fetch"], nil, YES, fill, nil, why); if (!px) return -1;
    const uint8_t *p = px.bytes; int good = 0;
    for (int i = 0; i < 4096; i++) { const uint8_t *c = p + i * 4; good += near(c[0], 20) && near(c[1], 50) && near(c[2], 100) && near(c[3], 128); }
    *why = [NSString stringWithFormat:@"%d/4096, px0 = %u,%u,%u,%u (want 20,50,100,128)", good, p[0], p[1], p[2], p[3]]; return good;
}

// a w x h buffer texture with row pitch `pitch`, read back texel by texel by a kernel. RGBA16Float is the format
// RenderBox makes its buffer textures in (render target + shader write, as it uses them).
static float texel(int x, int y, int c) { return (float)((x * 7 + y * 13 + c * 3 + 1) & 255) / 255.0f; }
static int runBT(MTLPixelFormat pf, int w, int h, int pitch, NSString **why) {
    const BOOL half = pf == MTLPixelFormatRGBA16Float;
    const int ch = pf == MTLPixelFormatR8Unorm ? 1 : 4, bpp = half ? 8 : ch;
    NSUInteger align = [dev minimumLinearTextureAlignmentForPixelFormat:pf];
    id<MTLBuffer> b = [dev newBufferWithLength:align + pitch * h options:MTLResourceStorageModeShared];
    uint8_t *m = (uint8_t *)b.contents + align;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
        if (half) ((__fp16 *)(m + y * pitch))[x * 4 + c] = (__fp16)texel(x, y, c);
        else m[y * pitch + x * ch + c] = (uint8_t)lroundf(texel(x, y, c) * 255); }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead | (half ? MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite : 0);
    id<MTLTexture> t = [b newTextureWithDescriptor:td offset:align bytesPerRow:pitch];
    if (!t) { *why = @"newTextureWithDescriptor:offset:bytesPerRow: returned nil"; return -1; }
    NSError *e = nil;
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"bt"] error:&e];
    if (!ps) { *why = [NSString stringWithFormat:@"pipeline refused (%@)", e.localizedDescription]; return -1; }
    id<MTLBuffer> o = [dev newBufferWithLength:w * h * 16 options:MTLResourceStorageModeShared]; memset(o.contents, 0xff, o.length);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:ps]; [en setTexture:t atIndex:0]; [en setBuffer:o offset:0 atIndex:0];
    [en dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [en endEncoding];
    if (!done(cb, why)) return -1;
    const float *f = o.contents; int good = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        BOOL ok = YES;
        for (int c = 0; c < ch; c++) ok &= fabsf(f[(y * w + x) * 4 + c] - texel(x, y, c)) < 0.004f;
        good += ok;
    }
    *why = [NSString stringWithFormat:@"%d/%d texels, (1,0).r = %.3f want %.3f", good, w * h, f[4], texel(1, 0, 0)]; return good == w * h ? 1 : 0;
}

int main(int argc, char **argv) {
    // "nofetch" skips arm 2: on a driver without framebuffer fetch the live read can take the process down
    const BOOL nofetch = argc > 1 && !strcmp(argv[1], "nofetch");
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice(); if (!dev) { fprintf(stderr, "no Metal device\n"); return 3; }
        q = [dev newCommandQueue];
        NSError *e = nil;
        lib = [dev newLibraryWithSource:kSrc options:nil error:&e]; if (!lib) { fprintf(stderr, "lib: %s\n", e.description.UTF8String); return 3; }
        vis = [dev newLibraryWithSource:kVis options:nil error:&e]; if (!vis) { fprintf(stderr, "vis: %s\n", e.description.UTF8String); return 3; }
        printf("device: %s\n", dev.name.UTF8String);
        NSString *why = nil; int g;
        g = runK(YES, YES, &why); arm("1a compute: live call to a linked visible function", g == 64, why);
        g = runK(NO, NO, &why);   arm("1b compute: call compiled out, nothing linked", g == 64, why);
        g = runK(YES, NO, &why);  arm("1c compute: live call, nothing linked, is refused", g == -1, why);
        g = runFS(YES, &why);     arm("1d fragment: linked visible fn passed as a function pointer", g == 4096, why);
        g = runFS(NO, &why);      arm("1e fragment: same, call compiled out", g == 4096, why);
        if (!nofetch) { g = runFetch(&why); arm("2  fragment: [[color(0)]] framebuffer fetch", g == 4096, why); }
        g = runBT(MTLPixelFormatRGBA8Unorm, 64, 64, 256, &why);  arm("3a buffer texture RGBA8 64x64 pitch 256", g == 1, why);
        g = runBT(MTLPixelFormatRGBA8Unorm, 16, 16, 64, &why);   arm("3b buffer texture RGBA8 16x16 pitch 64", g == 1, why);
        g = runBT(MTLPixelFormatRGBA8Unorm, 100, 8, 448, &why);  arm("3c buffer texture RGBA8 100x8 pitch 448", g == 1, why);
        g = runBT(MTLPixelFormatR8Unorm, 256, 1, 256, &why);     arm("3d buffer texture R8 256x1 pitch 256", g == 1, why);
        g = runBT(MTLPixelFormatR8Unorm, 40, 4, 64, &why);       arm("3e buffer texture R8 40x4 pitch 64", g == 1, why);
        g = runBT(MTLPixelFormatRGBA16Float, 131, 131, 1280, &why); arm("3f buffer texture RGBA16F 131x131 pitch 1280", g == 1, why);
        g = runBT(MTLPixelFormatRGBA16Float, 131, 131, 1048, &why); arm("3g buffer texture RGBA16F 131x131 pitch 1048", g == 1, why);
        g = runBT(MTLPixelFormatRGBA16Float, 141, 141, 1136, &why); arm("3h buffer texture RGBA16F 141x141 pitch 1136", g == 1, why);
        printf("%s\n", fails ? "FAILED" : "PASS");
        return fails != 0;
    }
}
