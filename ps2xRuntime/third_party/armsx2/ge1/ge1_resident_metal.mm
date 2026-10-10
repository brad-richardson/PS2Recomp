// RZV1 S4b: the resident generator on Metal (Mac): the shared pool buffer (the
// CPU writes slot loads straight into it), the int kernel from
// ge1_rz_core.inc + ge1_resident_gen.inc compiled at first use, and one
// dispatch per vsync batch, waited and read back for GE1_RESIDENT's check.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

#include "ge1_resident_msl.h"

namespace
{
id<MTLDevice> s_dev;
id<MTLCommandQueue> s_queue;
id<MTLComputePipelineState> s_pipe;
id<MTLBuffer> s_pool;

const char* const kPrelude = R"RZG(#include <metal_stdlib>
using namespace metal;
#define F2U(x) as_type<uint>(x)
#define U2F(x) as_type<float>(x)
#define F2I(x) int(x)
#define U2FLT(x) float(x)
#define U32C(x) uint(x)
#define I32C(x) int(x)
#define PREC
#define MSB(x) (31 - int(clz(x)))
#define UMUL(a, b, hi, lo) { lo = (a) * (b); hi = mulhi((a), (b)); }
#define VARIANT 0
#define DEVC device const
#define THR thread
)RZG";

const char* const kKernel = R"RZG(
kernel void rz_resident(device const uint* pool [[buffer(0)]], device const uint* jobs [[buffer(1)]],
                        device const uint* items [[buffer(2)]], device uint* outv [[buffer(3)]],
                        constant uint& count [[buffer(4)]], uint gid [[thread_position_in_grid]])
{
    if (gid >= count)
        return;
    device const uint* it = items + gid * 16u;
    device const uint* blob = pool + it[0] * (448u * 4u);
    device const uint* job = jobs + it[1] * 80u;
    uint w[8];
    rzg_vertex(blob, job, it[2], it[3], w);
    uint m[8];
    rzg_gsvertex(w, it[4], it[5], it[6], it[7], it[8], it[9], it[10], it[11], it[12], m);
    for (uint l = 0u; l < 8u; ++l)
    {
        outv[gid * 16u + l] = w[l];
        outv[gid * 16u + 8u + l] = m[l];
    }
}
)RZG";
} // namespace

bool ge1rm_init(size_t poolWords)
{
    @autoreleasepool
    {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev)
            return false;
        s_queue = [s_dev newCommandQueue];
        std::string src = std::string(kPrelude) + kGe1RzCoreSrc + kGe1RzGenSrc + kKernel;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        opt.mathMode = MTLMathModeSafe;
        NSError* err = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:opt error:&err];
        if (!lib)
        {
            std::fprintf(stderr, "[ge1] resident: MSL compile failed: %s\n", err.localizedDescription.UTF8String);
            return false;
        }
        id<MTLFunction> fn = [lib newFunctionWithName:@"rz_resident"];
        s_pipe = [s_dev newComputePipelineStateWithFunction:fn error:&err];
        if (!s_pipe)
            return false;
        s_pool = [s_dev newBufferWithLength:poolWords * 4 options:MTLResourceStorageModeShared];
        return s_pool != nil;
    }
}

uint32_t* ge1rm_pool()
{
    return s_pool ? static_cast<uint32_t*>(s_pool.contents) : nullptr;
}

bool ge1rm_run(const uint32_t* jobs, size_t jobWords, const uint32_t* items, uint32_t nitems, uint32_t* out, double* gpuMs)
{
    @autoreleasepool
    {
        id<MTLBuffer> bj = [s_dev newBufferWithBytes:jobs length:jobWords * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bi = [s_dev newBufferWithBytes:items length:size_t(nitems) * 64 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bo = [s_dev newBufferWithLength:size_t(nitems) * 64 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:s_pipe];
        [e setBuffer:s_pool offset:0 atIndex:0];
        [e setBuffer:bj offset:0 atIndex:1];
        [e setBuffer:bi offset:0 atIndex:2];
        [e setBuffer:bo offset:0 atIndex:3];
        [e setBytes:&nitems length:4 atIndex:4];
        [e dispatchThreads:MTLSizeMake(nitems, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error)
            return false;
        std::memcpy(out, bo.contents, size_t(nitems) * 64);
        *gpuMs = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
        return true;
    }
}
