// WS-CPU item 2: userspace test of the import trace core in override/src/nmx_fb_bridge.c (no kernel). Run from tests/:
//   awk '/^ \* ---- WS-CPU item 2: per-import cost trace/{f=1} /^\/\* debug.nmx_frametrace: a text dump/{f=0} f' \
//     ../override/src/nmx_fb_bridge.c | sed '1s/^/\/*\n/' > imp_body.inc
//   xcrun clang -std=gnu11 -Wall -Wno-unused-function -I../bsdcompat/include -o /tmp/imp_test imp_trace_test.c && /tmp/imp_test
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
struct thread; struct proc;
static struct thread *gThr = (struct thread *)0x1000;
static uint64_t gNow = 1;
uint64_t mach_absolute_time(void) { return gNow; }
static struct proc *current_proc(void) { return (struct proc *)0; }
static int proc_pid(struct proc *p) { return 42; }
static const char *gName = "WindowServer";
#define current_thread test_current_thread
#define proc_name test_proc_name
struct thread *test_current_thread(void) { return gThr; }
void test_proc_name(int pid, char *buf, int size) { strncpy(buf, gName, size - 1); buf[size - 1] = 0; }
#include "imp_body.inc"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    nmx_imp_reset(); nmx_imp_on = 1;
    /* one WindowServer import: C0 1000 ns, C4 300 ns + C6 200 ns */
    uint64_t t0 = gNow; nmx_imp_open(NMX_IMP_IMPORT, 878, 1);
    uint64_t a = gNow; gNow += 300; nmx_imp_stage(NMX_IMP_C4_PHYS, a, 878);
    a = gNow; gNow += 200; nmx_imp_stage(NMX_IMP_C6_ATTACH, a, 878);
    nmx_imp_units2(870);
    gNow += 500; nmx_imp_close(NMX_IMP_IMPORT, t0, 878);
    CHECK(imp.n[0][NMX_IMP_C0_TOTAL] == 1 && imp.sum_ns[0][NMX_IMP_C0_TOTAL] == 1000);
    CHECK(imp.sum_ns[0][NMX_IMP_C4_PHYS] == 300 && imp.units[0][NMX_IMP_C4_PHYS] == 878);
    CHECK(imp.head == 1 && imp.ring[0].nents == 870);
    /* no record: row none */
    a = gNow; gNow += 50; nmx_imp_stage(NMX_IMP_C9A_DMALOCK, a, 1);
    CHECK(imp.n[2][NMX_IMP_C9A_DMALOCK] == 1);
    /* VA record not kept (not userptr): dropped, nothing summed */
    t0 = gNow; nmx_imp_open(NMX_IMP_VA_MAP, 10, 0);
    a = gNow; gNow += 70; nmx_imp_stage(NMX_IMP_V3_BOUPDATE, a, 0);
    nmx_imp_close(NMX_IMP_VA_MAP, t0, 10);
    CHECK(imp.dropped == 1 && imp.n[0][NMX_IMP_V3_BOUPDATE] == 0 && imp.n[2][NMX_IMP_V3_BOUPDATE] == 0);
    /* kept VA record from another process */
    gName = "Claude Helper";
    t0 = gNow; nmx_imp_open(NMX_IMP_VA_UNMAP, 10, 0); nmx_imp_keep();
    a = gNow; gNow += 70; nmx_imp_stage(NMX_IMP_V2_CLEARFREED, a, 0);
    nmx_imp_close(NMX_IMP_VA_UNMAP, t0, 10);
    CHECK(imp.n[1][NMX_IMP_V2_CLEARFREED] == 1 && imp.n[1][NMX_IMP_V0_TOTAL] == 1);
    /* nested open on the same thread: ignored, outer keeps going */
    t0 = gNow; nmx_imp_open(NMX_IMP_IMPORT, 5, 1); nmx_imp_open(NMX_IMP_FREE, 5, 1);
    nmx_imp_close(NMX_IMP_FREE, t0, 5);  /* side mismatch: no-op */
    nmx_imp_close(NMX_IMP_IMPORT, t0, 5);
    CHECK(imp.n[1][NMX_IMP_D0_TOTAL] == 0 && imp.n[1][NMX_IMP_C0_TOTAL] == 1);
    /* 17 threads: slot exhaustion counted */
    for (int i = 0; i < 17; i++) { gThr = (struct thread *)(uintptr_t)(0x2000 + i); nmx_imp_open(NMX_IMP_IMPORT, 1, 1); }
    CHECK(imp.busy == 1);
    for (int i = 0; i < 16; i++) { gThr = (struct thread *)(uintptr_t)(0x2000 + i); nmx_imp_close(NMX_IMP_IMPORT, gNow, 1); }
    gThr = (struct thread *)0x1000;
    /* ring wrap */
    for (int i = 0; i < 600; i++) { t0 = gNow; nmx_imp_open(NMX_IMP_FREE, 3, 1); a = gNow; gNow += 9; nmx_imp_stage(NMX_IMP_D2_PUTPAGES, a, 3); nmx_imp_close(NMX_IMP_FREE, t0, 3); }
    nmx_imp_count(NMX_IMPC_TLBSEQ, 2);
    /* bounded output: full buffer, then a tiny one with a canary */
    static char big[128 * 1024];
    int n = nmx_import_cost_format(big, sizeof big);
    CHECK(n > 0 && n < (int)sizeof big && !strstr(big, "TRUNCATED"));
    int recs = 0; for (char *p = big; (p = strstr(p, "\nrec ")); p++) recs++;
    CHECK(recs == 256);
    CHECK(strstr(big, "count tlb_seq 2") != NULL);
    for (size_t sz = 16; sz < 8192; sz += 37) {
        char *b = malloc(sz + 64); memset(b, 0x5a, sz + 64);
        int m = nmx_import_cost_format(b, sz);
        int over = 0; for (size_t k = sz; k < sz + 64; k++) if ((unsigned char)b[k] != 0x5a) over = 1;
        if (over || m < 0 || (size_t)m > sz) { printf("FAIL bound sz %zu m %d over %d\n", sz, m, over); fails++; break; }
        if (sz < 4000 && !strstr(b, "TRUNCATED")) { printf("FAIL no TRUNCATED at %zu\n", sz); fails++; break; }
        free(b);
    }
    CHECK(nmx_import_cost_format(big, 8) == 0);
    printf("output %d bytes for 256 records; sample:\n", n);
    char *l = strstr(big, "stage C0_total"); if (l) printf("  %.*s\n", (int)(strchr(l, '\n') - l), l);
    l = strstr(big, "rec import"); if (l) printf("  %.*s\n", (int)(strchr(l, '\n') - l), l);
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
