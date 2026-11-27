#!/usr/bin/env python3
"""Exercise the real b15 CS helper with mocked mapping/kmap, never the GPU.
Usage: python3 ibprepare-host.py /path/to/patched/amdgpu_cs.c
"""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]).read_text()
a = source.index('static int nmx_cs_prepare_ibs(')
b = source.index('\nstatic int amdgpu_cs_patch_ibs(', a)
helper = source[a:b]
stubs = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define AMDGPU_GMC_HOLE_MASK 0x0000ffffffffffffULL
#define AMDGPU_GPU_PAGE_SHIFT 12
#define AMDGPU_RING_TYPE_GFX 1
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define mb() __asm__ __volatile__("mfence" ::: "memory")
struct amdgpu_ib { u64 gpu_addr; u32 length_dw; void *ptr; };
struct amdgpu_job { unsigned num_ibs; u64 nmx_ib_va; u32 nmx_ib_length_dw, nmx_ib_snapshot_dw, nmx_ib_snapshot[96]; struct amdgpu_ib ibs[2]; };
struct amdgpu_cs_parser { int unused; };
struct amdgpu_bo { u64 size; };
struct amdgpu_bo_va_mapping { u64 start, last, offset; };
struct funcs { int type; };
struct ring { struct funcs *funcs; };
static struct funcs funcs = {1};
static struct ring ring = {&funcs};
static struct amdgpu_bo bo;
static struct amdgpu_bo_va_mapping map;
static _Alignas(64) uint8_t memory[8192];
static unsigned maps, unmaps;
static int lookup_error, kmap_error, null_kmap;
static u64 looked_up;
static struct ring *amdgpu_job_ring(struct amdgpu_job *j) { return &ring; }
static int amdgpu_cs_find_mapping(struct amdgpu_cs_parser *p, u64 va, struct amdgpu_bo **b, struct amdgpu_bo_va_mapping **m) {
    looked_up = va; *b = &bo; *m = &map; return lookup_error;
}
static u64 amdgpu_bo_size(struct amdgpu_bo *b) { return b->size; }
static int amdgpu_bo_kmap(struct amdgpu_bo *b, void **out) { ++maps; *out = null_kmap ? NULL : memory; return kmap_error; }
static void amdgpu_bo_kunmap(struct amdgpu_bo *b) { ++unmaps; }
'''
cases = r'''
static struct amdgpu_job job;
static struct amdgpu_cs_parser parser;
static void reset(void) {
    memset(&job, 0, sizeof(job)); memset(memory, 0, sizeof(memory));
    maps = unmaps = lookup_error = kmap_error = null_kmap = 0; looked_up = 0; funcs.type = 1;
    map.start = 0x800100000ULL; map.last = map.start; map.offset = 4096; bo.size = sizeof(memory);
    job.num_ibs = 1; job.ibs[0].gpu_addr = 0xffff80010000003cULL; job.ibs[0].length_dw = 2;
    memcpy(memory + 4096 + 60, (u32[]){0xc0023f00,0x12345678}, 8);
}
int main(void) {
    reset(); assert(nmx_cs_prepare_ibs(&parser,&job) == 0);
    assert(looked_up == 0x80010000003cULL && maps == 1 && unmaps == 1);
    assert(job.nmx_ib_snapshot_dw == 2 && job.nmx_ib_snapshot[0] == 0xc0023f00 && job.nmx_ib_snapshot[1] == 0x12345678);
    reset(); job.ibs[0].length_dw = 1024; assert(nmx_cs_prepare_ibs(&parser,&job) == -EINVAL && !maps);
    reset(); bo.size = 4096; assert(nmx_cs_prepare_ibs(&parser,&job) == -EINVAL && !maps);
    reset(); map.offset = UINT64_MAX; assert(nmx_cs_prepare_ibs(&parser,&job) == -EINVAL && !maps);
    reset(); lookup_error = -EINVAL; assert(nmx_cs_prepare_ibs(&parser,&job) == -EINVAL && !maps);
    reset(); kmap_error = -ENOMEM; assert(nmx_cs_prepare_ibs(&parser,&job) == -ENOMEM && !unmaps);
    reset(); null_kmap = 1; assert(nmx_cs_prepare_ibs(&parser,&job) == -EFAULT && unmaps == 1);
    reset(); funcs.type = 2; assert(nmx_cs_prepare_ibs(&parser,&job) == 0 && !maps);
    reset(); job.ibs[0].length_dw = 0; assert(nmx_cs_prepare_ibs(&parser,&job) == 0 && !maps);
    reset(); job.ibs[0].ptr = memory; assert(nmx_cs_prepare_ibs(&parser,&job) == 0 && !maps);
    reset(); job.ibs[0].length_dw = 128; assert(nmx_cs_prepare_ibs(&parser,&job) == 0 && job.nmx_ib_snapshot_dw == 96);
    puts("PASS: 11 IB preparation cases (canonical VA, offset, snapshot, bounds, lookup/kmap errors, skips)");
}
'''
with tempfile.TemporaryDirectory(prefix='nmx-ibprepare-') as temp:
    p = pathlib.Path(temp)
    (p/'test.c').write_text(stubs + helper + cases)
    subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror', str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
