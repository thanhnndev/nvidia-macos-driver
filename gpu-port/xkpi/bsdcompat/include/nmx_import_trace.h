/*
 * WS-CPU item 2: per-import cost trace (debug.nmx_import_trace, default 0; debug.nmx_import_cost to read).
 * One userptr import (DRM_AMDGPU_GEM_USERPTR), one GEM_VA map/unmap and one userptr BO free are each a "record",
 * opened on the calling thread; the stages below it on the same thread are timed into it and into per-stage sums.
 * Stages reached on a thread with no open record (a parked BO restored in CS, a delayed BO destroy on a worker) go to
 * the "unscoped" row. With the trace off every probe is one predicted branch on nmx_imp_on: no clock read, no call.
 * The ring and the sums are static (fixed size); nothing is allocated. Plan: ws-cpu-item2-import-trace-plan-2026-11-27.md.
 */
#ifndef NMX_IMPORT_TRACE_H
#define NMX_IMPORT_TRACE_H
#include <stdint.h>

enum nmx_imp_side { NMX_IMP_IMPORT = 0, NMX_IMP_VA_MAP, NMX_IMP_VA_UNMAP, NMX_IMP_FREE, NMX_IMP_SIDES };
enum nmx_imp_stage {
    /* import: DRM_AMDGPU_GEM_USERPTR */
    NMX_IMP_C0_TOTAL = 0, NMX_IMP_C1_LOOKUP, NMX_IMP_C2_PREPARE, NMX_IMP_C3_FIRSTPAGE, NMX_IMP_C4_PHYS,
    NMX_IMP_C5_WIREMAKE, NMX_IMP_C6_ATTACH, NMX_IMP_C7_VALIDATE, NMX_IMP_C8_SGTABLE, NMX_IMP_C9_DMAMAP,
    NMX_IMP_C9A_DMALOCK, NMX_IMP_C10_DMAADDR, NMX_IMP_C11_HANDLE,
    /* GEM_VA */
    NMX_IMP_V0_TOTAL, NMX_IMP_V1_TREE, NMX_IMP_V2_CLEARFREED, NMX_IMP_V3_BOUPDATE, NMX_IMP_V4_PDES, NMX_IMP_V5_SDMA,
    /* free */
    NMX_IMP_D0_TOTAL, NMX_IMP_D1_DMAUNMAP, NMX_IMP_D2_PUTPAGES, NMX_IMP_D3_UNWIRE,
    NMX_IMP_STAGES
};
enum nmx_imp_count {
    NMX_IMPC_REC_NEW = 0,     /* page records made in pin_attach (user_page_get miss) */
    NMX_IMPC_REC_REUSED,      /* page records found (another pin holds the page) */
    NMX_IMPC_PIN_MULTI,       /* put_pages: page whose pin list had more than this BO's entry */
    NMX_IMPC_REC_FREED,       /* put_pages: page records freed */
    NMX_IMPC_FLUSHTLB_UPDATE, /* amdgpu_vm_update_range with flush_tlb, from amdgpu_vm_bo_update */
    NMX_IMPC_FLUSHTLB_FREED,  /* ... from amdgpu_vm_clear_freed */
    NMX_IMPC_FLUSHTLB_OTHER,  /* ... from anything else */
    NMX_IMPC_TLBSEQ,          /* vm->tlb_seq bumps (amdgpu_vm_tlb_seq_cb) */
    NMX_IMPC_VMFLUSH_JOB,     /* amdgpu_vm_flush with a VM flush emitted (job->vm_needs_flush) */
    NMX_IMPC_PASID_FLUSH,     /* amdgpu_gmc_flush_gpu_tlb_pasid calls */
    NMX_IMPC_PTES,            /* PTEs written by amdgpu_vm_bo_update (pages of the mapping) */
    NMX_IMPC_SDMA_JOBS,       /* amdgpu_vm_sdma_commit calls */
    NMX_IMPC_SDMA_DW,         /* IB dwords those jobs carried */
    NMX_IMPC_COUNTS
};

#ifdef __cplusplus
extern "C" {
#endif
extern int nmx_imp_on;
uint64_t mach_absolute_time(void);
/* open / close a record on this thread (t0 from NMX_IMP_T0) */
void nmx_imp_open(int side, uint32_t pages, int keep);   /* keep 0: dropped at close unless nmx_imp_keep ran */
void nmx_imp_keep(void);
void nmx_imp_close(int side, uint64_t t0, uint32_t pages);
void nmx_imp_stage(int stage, uint64_t t0, uint32_t units);
void nmx_imp_count(int ctr, uint64_t n);
void nmx_imp_units2(uint32_t nents);       /* the open record's sg entries */
int nmx_import_cost_format(char *buf, __SIZE_TYPE__ size);
#ifdef __cplusplus
}
#endif

#define NMX_IMP_ON() __builtin_expect(nmx_imp_on != 0, 0)
/* t0 = 0 when the trace is off: the matching NMX_IMP_STAGE is then one more predicted branch */
#define NMX_IMP_T0(v) uint64_t v = NMX_IMP_ON() ? mach_absolute_time() : 0
#define NMX_IMP_STAGE(st, v, units) do { if (__builtin_expect((v) != 0, 0)) nmx_imp_stage((st), (v), (uint32_t)(units)); } while (0)
#define NMX_IMP_COUNT(c, n) do { if (NMX_IMP_ON()) nmx_imp_count((c), (uint64_t)(n)); } while (0)
#endif
