// Import reuse (WS-CPU item 2 cut): userspace test of nmx_page.c's idle BK_USER records and pin entry blocks.
// run.sh extracts the code under test from bsdcompat/src/nmx_page.c (s1..s4.inc); this file stubs the kernel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/queue.h>
#include "nmx_import_trace.h"
static void raw_free(void *p) { free(p); }
typedef unsigned int u_int;
typedef uint64_t vm_paddr_t;
typedef uint64_t vm_pindex_t;
typedef int vm_memattr_t;
typedef void *vm_object_t;
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096ull
#define PAGE_MASK (PAGE_SIZE - 1)
#define PG_FICTITIOUS 0x0004
#define VPO_UNMANAGED 0x04
#define VM_MEMATTR_WRITE_BACK 6
struct vm_page {                    /* bsdcompat/include/vm/vm_page.h */
    vm_paddr_t phys_addr; void *nmx_kva; void *nmx_alloc; u_int nmx_npages; volatile u_int ref_count;
    vm_object_t object; vm_pindex_t pindex; uint16_t flags; uint8_t oflags, a_flags, order, pool;
    struct { vm_memattr_t pat_mode; } md;
    TAILQ_ENTRY(vm_page) listq;
    union { TAILQ_ENTRY(vm_page) q; struct { SLIST_ENTRY(vm_page) ss; } s; } plinks;
    void *nmx_lkpi; void *nmx_pins;
};
typedef struct vm_page *vm_page_t;
/* locks: single-threaded; count holds so free() under a spin lock is caught */
typedef struct { int held; } lck_spin_t;
static int held_total;
static lck_spin_t *lck_spin_alloc_init(void *g, void *a) { return calloc(1, sizeof(lck_spin_t)); }
static void lck_spin_free(lck_spin_t *l, void *g) { free(l); }
static void lck_spin_lock(lck_spin_t *l) { if (l->held) { printf("FAIL recursive lock\n"); exit(1); } l->held = 1; held_total++; }
static void lck_spin_unlock(lck_spin_t *l) { l->held = 0; held_total--; }
static void *nmx_lck_grp;
#define LCK_ATTR_NULL NULL
typedef void *thread_call_t;
typedef void (*thread_call_func_t)(void *, void *);
static thread_call_t thread_call_allocate(thread_call_func_t f, void *p) { return (void *)1; }
static uint64_t mach_absolute_time(void) { return 1; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *r) { *r = ns; }
/* malloc(9) */
struct malloc_type { int x; };
static struct malloc_type M_NMXPAGE_s;
#define M_NMXPAGE (&M_NMXPAGE_s)
#define M_WAITOK 1
#define M_ZERO 2
static long live_allocs, n_mallocs, fail_after = -1;
static void *kmalloc(size_t n, struct malloc_type *t, int fl)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    live_allocs++; n_mallocs++;
    return (fl & M_ZERO) ? calloc(1, n) : malloc(n);
}
static void kfree(void *p, struct malloc_type *t)
{
    if (p == NULL) return;
    if (held_total) { printf("FAIL free under a spin lock\n"); exit(1); }
    live_allocs--; free(p);
}
#define malloc kmalloc
#define free kfree
#define bzero(p, n) memset((p), 0, (n))
static void *nmx_xnu_mem_alloc(size_t s, int c, uint64_t m, size_t a, void **h) { return NULL; }
static void nmx_xnu_mem_free(void *h) { }
static vm_paddr_t nmx_xnu_kva_to_phys(const void *v) { return 0; }
/* the wire: phys handed in by the test; counts unwires */
static int unwires, wires;
static uint64_t *wire_phys; static int wire_n;
static int nmx_xnu_user_wire(void *mm, uintptr_t start, size_t n, int w, uint64_t *phys, void **h)
{ for (size_t i = 0; i < n; i++) phys[i] = wire_phys[i]; *h = (void *)(uintptr_t)(++wires); return 0; }
static void nmx_xnu_user_unwire(void *h) { unwires++; }
static int nmx_xnu_user_parkable(void *h) { return 0; }
static int nmx_xnu_user_park(void *h) { return 0; }
static int nmx_xnu_user_rewire(void *h, size_t n, uint64_t *phys) { for (size_t i = 0; i < n; i++) phys[i] = wire_phys[i]; return 0; }
uint32_t nmx_xnu_user_sid(void *h) { return 0; }
void nmx_ft_stall(int k, uint64_t t0, uint64_t p) { }
long long test_impc[NMX_IMPC_N];
void vm_page_free(vm_page_t m);
#include "s1.inc"
static void release_worker(void *a, void *b) { }
static void fill_worker(void *a, void *b) { }
#include "s5.inc"
#include "s2.inc"
#include "s3.inc"
#include "s4.inc"
void vm_page_free(vm_page_t m) { if (((struct backing *)m->nmx_alloc)->kind == BK_FICTITIOUS) return; /* as nmx_page.c */ printf("FAIL vm_page_free kind %d ref %u\n", ((struct backing *)m->nmx_alloc)->kind, m->ref_count); __builtin_trap(); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); fails++; } } while (0)
static int reg_count(vm_paddr_t pfn)
{
    int n = 0;
    for (struct reg_entry *e = reg_table[reg_bucket(pfn)]; e; e = e->next) n += e->pfn == pfn;
    return n;
}
/* sets fields LinuxKPI or a caller may leave on a record, so a reset can be checked */
static void dirty(vm_page_t m)
{
    m->nmx_lkpi = (void *)0xdead; m->a_flags = 0x55; m->pindex = 77; m->oflags = 0; m->md.pat_mode = 1;
    m->order = 3; m->pool = 2; m->flags |= 0x8000; m->nmx_npages = 9;
}
/* the record as out-55 makes it: page_record_init + PG_FICTITIOUS + ref 1 */
static int fresh(vm_page_t m, vm_paddr_t pa)
{
    return m->phys_addr == pa && m->ref_count == 1 && m->flags == PG_FICTITIOUS && m->nmx_lkpi == NULL &&
           m->a_flags == 0 && m->pindex == 0 && m->oflags == VPO_UNMANAGED && m->md.pat_mode == VM_MEMATTR_WRITE_BACK &&
           m->order == 0 && m->pool == 0 && m->nmx_npages == 0 && m->nmx_kva == NULL && m->object == NULL;
}
/* one owned-pin import + put_pages + destroy of npages at phys base, the BO cycle of a re-imported IOSurface */
static void cycle(uint64_t *phys, int n)
{
    vm_page_t pages[600]; void *pin;
    wire_phys = phys; wire_n = n;
    CHECK(nmx_upin_create(0x1000, n, 1, pages, &pin) == n);
    for (int i = 0; i < n; i++) {
        CHECK(pages[i] && pages[i]->phys_addr == phys[i]);
        if (((struct backing *)pages[i]->nmx_alloc)->kind != BK_USER)
            continue;           /* the permanent record of phys[3]: out-55 keeps its state, not under test here */
        CHECK(fresh(pages[i], phys[i]));    /* new, or revived after it was dirtied: identical */
        dirty(pages[i]);
    }
    nmx_upin_put_pages(pin, pages, n);
    nmx_upin_destroy(pin);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(nmx_page_init() == 0);
    CHECK(sizeof(struct pin_blk) + PIN_BLK_REFS * sizeof(struct pin_ref) <= 3072);
    uint64_t phys[600];
    for (int i = 0; i < 600; i++) phys[i] = (0x100000ull + (uint64_t)i * 7) << PAGE_SHIFT;
    long base = live_allocs;

    /* gate 0: the out-55 path, records made and freed every cycle, one pin entry malloc per page */
    nmx_import_reuse = 0;
    long m0 = n_mallocs;
    cycle(phys, 500);
    CHECK(live_allocs == base && gNmxUserRecLive == 0 && test_impc[NMX_IMPC_REC_NEW] == 500);
    long per_cycle_off = n_mallocs - m0;
    CHECK(per_cycle_off >= 3 * 500);

    /* bit 1|2: first cycle makes records and keeps them idle; second revives every one */
    nmx_import_reuse = 3;
    cycle(phys, 500);
    CHECK(gNmxReuseKept == 500 && gNmxReuseIdle == 500 && gNmxUserRecLive == 500);
    m0 = n_mallocs;
    long new0 = test_impc[NMX_IMPC_REC_NEW];
    cycle(phys, 500);
    long per_cycle_on = n_mallocs - m0;
    CHECK(test_impc[NMX_IMPC_REC_NEW] == new0 && gNmxReuseRevived == 500 && gNmxReuseIdle == 500);
    CHECK(per_cycle_on <= 3 + 4 + 2);  /* phys + pin + refs array + 4 blocks of 128 */
    printf("mallocs per 500-page re-import cycle: gate 0 = %ld, gate 3 = %ld\n", per_cycle_off, per_cycle_on);
    for (int i = 0; i < 500; i++) CHECK(reg_count(phys[i] >> PAGE_SHIFT) == 1);

    /* a page pinned twice (two plain pins), released in either order: nothing idle while one holds it */
    vm_page_t p1[1], p2[1];
    wire_phys = phys;
    CHECK(nmx_user_pages_pin(NULL, 0, 1, 1, p1) == 1);
    CHECK(nmx_user_pages_pin(NULL, 0, 1, 1, p2) == 1);
    CHECK(p1[0] == p2[0] && p1[0]->ref_count == 2 && gNmxReuseIdle == 499);
    vm_page_unwire_noq(p1[0]);
    CHECK(gNmxReuseIdle == 499);
    vm_page_unwire_noq(p2[0]);
    CHECK(gNmxReuseIdle == 500 && unwires >= 2);

    /* nmx_phys_to_page on an idle record: leaves the list, turns permanent */
    vm_page_t f = nmx_phys_to_page(phys[3]);
    CHECK(f && ((struct backing *)f->nmx_alloc)->kind == BK_FICTITIOUS && gNmxReuseIdle == 499 && gNmxUserRecKept == 1);
    CHECK(f && fresh(f, phys[3]));     /* dirtied before it went idle (cycle), handed out as out-55's fresh one */
    CHECK(gNmxReuseBlocksLive == 0);

    /* a chunk page registered later at the same frame shadows the idle record; its removal leaves the idle one */
    struct backing *ck = calloc(1, sizeof(*ck) + sizeof(struct vm_page));
    struct reg_entry ce;
    ck->kind = BK_CHUNK; ck->npages = 1;
    page_record_init(&ck->pages[0], ck, phys[5], (void *)0x1);
    ck->pages[0].flags = PG_FICTITIOUS;   /* so the plain put below pops its pin (test only: a chunk page is never user-pinned) */
    lck_spin_lock(reg_lock_of(phys[5] >> PAGE_SHIFT));
    reg_insert_locked(&ck->pages[0], &ce);
    lck_spin_unlock(reg_lock_of(phys[5] >> PAGE_SHIFT));
    vm_page_t c[1];
    wire_phys = &phys[5];
    CHECK(nmx_user_pages_pin(NULL, 0, 1, 1, c) == 1 && c[0] == &ck->pages[0]);
    vm_page_unwire_noq(c[0]);
    CHECK(reg_count(phys[5] >> PAGE_SHIFT) == 2);
    lck_spin_lock(reg_lock_of(phys[5] >> PAGE_SHIFT));
    CHECK(reg_remove_locked(phys[5] >> PAGE_SHIFT) == &ce);   /* backing_unregister's first match = the chunk's */
    lck_spin_unlock(reg_lock_of(phys[5] >> PAGE_SHIFT));
    raw_free(ck);
    CHECK(nmx_user_pages_pin(NULL, 0, 1, 1, c) == 1 && ((struct backing *)c[0]->nmx_alloc)->kind == BK_USER);
    vm_page_unwire_noq(c[0]);

    /* cap: stripes keep at most cap/256 (here 1); the oldest goes, by its own entry */
    nmx_import_reuse_cap = 256;
    long ev0 = gNmxReuseEvicted;
    cycle(phys, 500);
    CHECK(gNmxReuseEvicted > ev0);
    for (unsigned s = 0; s < REG_LOCKS; s++) CHECK(idle_n[s] <= 1);
    CHECK(gNmxReuseIdle == gNmxUserRecLive);
    nmx_import_reuse_cap = 65536;

    /* a lower cap frees what is over it at once; cap 0 frees every idle record and keeps none */
    nmx_import_reuse_cap = 65536;
    cycle(phys, 500);
    CHECK(gNmxReuseIdle == 499);
    nmx_import_reuse_cap_set(256);
    for (unsigned s = 0; s < REG_LOCKS; s++) CHECK(idle_n[s] <= 1);
    nmx_import_reuse_cap_set(0);
    CHECK(gNmxReuseIdle == 0 && gNmxUserRecLive == 0);
    cycle(phys, 100);
    CHECK(gNmxReuseIdle == 0 && gNmxUserRecLive == 0);
    nmx_import_reuse_cap_set(65536);
    cycle(phys, 500);
    CHECK(gNmxReuseIdle == 499 && gNmxReuseBlocksLive == 0);

    /* clearing bit 1 frees every idle record; the registry holds only the permanent one */
    nmx_import_reuse_set(2);
    CHECK(gNmxReuseIdle == 0 && gNmxUserRecLive == 0);
    for (int i = 0; i < 500; i++) CHECK(reg_count(phys[i] >> PAGE_SHIFT) == (i == 3));

    /* bit 2 alone: blocks freed with their last entry, also when entries are freed by different puts */
    cycle(phys, 300);
    CHECK(gNmxUserRecLive == 0);

    /* allocation failure in the middle of the blocks and of the records: nothing left behind */
    for (int bits = 0; bits <= 3; bits++) {
        nmx_import_reuse_set(bits);
        for (int k = 0; k < 40; k++) {
            vm_page_t pages[300]; void *pin = NULL;
            long before = live_allocs;
            wire_phys = phys;
            fail_after = k;
            int r = nmx_upin_create(0x1000, 300, 1, pages, &pin);
            fail_after = -1;
            if (r > 0) { nmx_upin_put_pages(pin, pages, 300); nmx_upin_destroy(pin); }
            nmx_import_reuse_drain();
            CHECK(live_allocs == before);
        }
    }
    nmx_import_reuse_drain();
    /* only the permanent record (phys[3]) and its entry remain */
    if (live_allocs != base + 2) printf("live %ld base %ld idle %lld reclive %lld\n", live_allocs, base, gNmxReuseIdle, gNmxUserRecLive);
    CHECK(live_allocs == base + 2);
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASS", fails);
    return fails != 0;
}
