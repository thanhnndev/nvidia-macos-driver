/* test stub of bsdcompat/include/nmx_import_trace.h: probes off, counters kept for the checks */
#pragma once
enum { NMX_IMPC_REC_NEW, NMX_IMPC_REC_REUSED, NMX_IMPC_PIN_MULTI, NMX_IMPC_REC_FREED, NMX_IMPC_N };
extern long long test_impc[NMX_IMPC_N];
#define NMX_IMP_ON() 1
#define NMX_IMP_T0(v) uint64_t v = 0; (void)v
#define NMX_IMP_STAGE(s, t, n) do { } while (0)
#define NMX_IMP_COUNT(c, n) (test_impc[(c)] += (n))
static inline void nmx_imp_count(int c, long long n) { test_impc[c] += n; }
