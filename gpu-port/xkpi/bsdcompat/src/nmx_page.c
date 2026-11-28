/*
 * FreeBSD's physical-page API (vm_page) on xnu, for LinuxKPI's alloc_pages/page_address/get_page/put_page.
 *
 * xnu gives kexts no page array and no direct map, so every page handed out is a struct vm_page this file creates:
 *   - single pages come from 2 MB chunks of wired kernel memory (nmx_xnu_mem_alloc), carved page by page and
 *     recycled through a per-chunk free list; a chunk whose pages have all been free for CHUNK_IDLE_NS goes back to
 *     the system once more than CHUNK_RESERVE free pages are pooled (ga403wm Step141/141b; before, chunks only went
 *     back at unload);
 *   - multi-page, 32-bit-DMA, or explicitly contiguous requests get one physically contiguous run of their own.
 * Every page is registered by physical page number, so PHYS_TO_VM_PAGE and PHYS_TO_DMAP work for all of them.
 * Pages start wired once (ref_count 1); the last unwire frees them, which is LinuxKPI's put_page/__free_page rule.
 * A physical address the port never allocated (kmalloc memory reached through virt_to_page, GPU BAR pages) gets a
 * "fictitious" record on demand; those carry no backing allocation and are never freed to a pool.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_object.h>
#include <vm/vm_pager.h>
#include <vm/pmap.h>

int nmx_malloc_note_contig(void *addr, size_t size, struct malloc_type *type);
void nmx_malloc_forget_contig(void *addr);
#include <vm/vm_extern.h>
#include <machine/cpufunc.h>
#include "nmx_xnu.h"

MALLOC_DEFINE(M_NMXPAGE, "nmxpage", "page records");

#define CHUNK_PAGES 512                    /* 2 MB of 4 KB pages */
/*
 * page registry hash: page frame number -> struct vm_page.
 * ga403wm Step140: 65536 buckets held ~1.7M records (854k port pages + 826k pinned user pages on the 2026-11-09 13:06
 * boot), so chains averaged ~26 heap nodes and every new user page walked one twice (miss, then the re-check after the
 * malloc). A kernel spindump of maximised 1080p60 Chrome had 171 of 216 import samples (79 %) on the chain compare in
 * nmx_user_pages_pin (inlined reg_find_locked), against 27 in the xnu wire. 2M buckets (16 MB of pointers) keep the
 * average chain below one at today's counts.
 */
#define REG_BUCKETS (1u << 21)
#define DMA32_LIMIT 0xffffffffULL

enum backing_kind { BK_CHUNK = 1, BK_RUN = 2, BK_FICTITIOUS = 3, BK_USER = 4 };

/*
 * ga403wm Step138: a record made for a pinned user page (BK_USER) lives only while the page is pinned or referenced.
 * Before, every user page ever pinned got a permanent fictitious record (backing + vm_page + registry entry, ~300 B):
 * after an evening of video the registry held millions of them (kalloc var4.256 ~810 MB wired and growing) and every
 * import walked ever longer hash chains (73 of 87 import samples in reg_find_locked). A BK_USER record is freed when
 * its last reference goes and no pin is left on it. If any other path reaches the same page (PHYS_TO_VM_PAGE,
 * virt_to_page), it may keep the pointer without a reference, so the record turns permanent (BK_FICTITIOUS) as before.
 */
volatile long long gNmxUserRecLive, gNmxUserRecMade, gNmxUserRecFreed, gNmxUserRecKept;
/* Step142 (owned userptr pins, see nmx_upin_create): restores that failed, BOs destroyed while still holding page
 * records (pinned but never populated: the TT unpopulate that drops them was skipped), and the park sweep's knobs and
 * counters. nmx_userptr_park_ms: idle time after which an IOSurface userptr BO is parked; 0 = never (debug.*). */
volatile long long gNmxUpinRestoreFail, gNmxUpinDestroyRec, gNmxParkSkipBusy, gNmxParkFail;
volatile long long gNmxParkSkipVm, gNmxParkAbort, gNmxParkPtUpdate;	/* 11.9, amdgpu-userptr-park.patch */
volatile long long gNmxFaultKick, gNmxFaultKickIgnored, gNmxFaultKickFail;	/* row 12, amdgpu-vm-fault-name.patch */
int nmx_fault_kick_on = 0;		/* row 12 kick: off until soft recovery at fault time is verified on the 890M */
int nmx_userptr_park_ms = 60000;      /* Step142b: 10 s re-wired whole swap chains at once mid-video (part 39) */
/* import reuse (WS-CPU item 2 cut): gate bits and counters, see the import reuse section below */
int nmx_import_reuse = 0;
int nmx_import_reuse_cap = 65536;          /* idle records in all (~300 B each: 64k = ~19 MB) */
volatile long long gNmxReuseRevived, gNmxReuseKept, gNmxReuseIdle, gNmxReuseEvicted, gNmxReuseDrained;
volatile long long gNmxReuseBlocks, gNmxReuseBlockRefs, gNmxReuseSegs, gNmxReuseSegPages, gNmxReuseBlocksLive;
#define PIN_BLK_REFS 127                   /* bit 2: 8 B head + 127 x 24 B = 3056 B, fits the 3072 B kalloc zone */

struct backing {
    int kind;
    void *handle;                          /* nmx_xnu_mem_alloc handle */
    void *kva;
    size_t npages;
    u_int live;                            /* pages handed out and not yet freed */
    struct backing *next;                  /* chunk list (and the release queue once unlinked) */
    struct backing *prev;                  /* chunk list */
    struct backing *anext, *aprev;         /* BK_CHUNK: list of chunks with free pages */
    struct vm_page *cfree;                 /* BK_CHUNK: this chunk's free pages, linked through plinks.s.ss */
    u_int nfree;                           /* BK_CHUNK: pages on cfree */
    uint64_t empty_at;                     /* BK_CHUNK: mach_absolute_time when live last dropped to 0 */
    u_int idle;                            /* BK_USER: on its stripe's idle list (import reuse bit 1, stripe lock) */
    struct vm_page pages[];                /* BK_CHUNK / BK_RUN: one record per page */
};

struct reg_entry {
    vm_paddr_t pfn;
    struct vm_page *m;
    struct reg_entry *next;
};

static struct reg_entry **reg_table;
static lck_spin_t *pg_lock;               /* chunk list, free list and the live counts */
/*
 * ga403wm Step133: the registry and the pin lists have their own locks, one per stripe of hash buckets. They were under
 * pg_lock, taken once per page by every pin (phys_to_page, pin push) and every unpin (pin pop): with WindowServer,
 * Chrome, VTDecoder and Claude pinning and releasing userptr surfaces every frame, a 10 s spindump of 1080p60 video
 * had ~4 s of CPU spinning on that one lock (1.8 s of it in WindowServer). A page's registry entry and its pin list are
 * both guarded by the stripe of its page frame number, so different pages almost never meet on a lock.
 */
#define REG_LOCKS 256                      /* power of two dividing REG_BUCKETS */
static lck_spin_t *reg_locks[REG_LOCKS];
static struct backing *chunks;
/*
 * ga403wm Step141: free single pages live on their own chunk's list, and the chunks that have any are on `avail`.
 * Before, one global free list mixed pages of every chunk, so a chunk could never be taken out and the pool only grew
 * to its peak (with TTM's pool on top, uncapped because xnu never fires vm_lowmem). Now a chunk that is wholly free
 * again is unlinked and queued for release when the pool already holds CHUNK_RESERVE free pages without it; the
 * release itself (registry removal, descriptor free) runs from a thread call, never under the caller's locks.
 */
/*
 * ga403wm Step141b: releasing a chunk the moment it emptied (above a 64 MB reserve) made 1080p60 Chrome drop 21 frames a
 * minute instead of 7: Chrome's GPU buffers come and go in ~120 MB bursts, so the pool was released and then refilled
 * (93 new chunks in the 13-drop second). Now a wholly free chunk is released only after CHUNK_IDLE_NS without use, by a
 * sweep that runs every second while there is anything to release, and the reserve is 256 MB.
 */
#define CHUNK_RESERVE 65536                /* 256 MB of free pages kept for the next allocations */
#define CHUNK_IDLE_NS (5ull * 1000000000ull) /* a free chunk must stay unused this long before it goes back */
#define CHUNK_SWEEP_MS 1000
#define CHUNK_HELD_MS 10000               /* Step141e: re-check every 10 s while only a recent peak holds chunks */
static struct backing *avail;
static thread_call_t release_call;
static int sweep_armed;                   /* release_call is scheduled (pg_lock) */
static int sweep_stopping;                /* unload: nobody arms release_call again (pg_lock) */
static int sweep_busy;                    /* sweeps/fills running + arms in flight; unload waits for 0 (pg_lock) */
/*
 * ga403wm Step141c: with 1080p60 Chrome, single_alloc made 165 chunks itself in 3 s (free 62k -> 494 pages) and the
 * frames of that second dropped. Each chunk is a 2 MB wired descriptor plus 512 registry inserts, paid by the GPU path
 * that asked for one page. Now an allocation that leaves fewer than CHUNK_RESERVE/2 free pages wakes a fill thread
 * call that tops the pool back up to CHUNK_RESERVE, so the allocation path rarely has to make a chunk itself.
 */
static thread_call_t fill_call;
static int fill_armed;                    /* fill_call is scheduled or running (pg_lock) */
uint64_t gNmxPageChunksFilled, gNmxPageChunksInline; /* debug.nmx_page_chunks_filled / _inline */
/*
 * ga403wm cs-enomem: a failed 2 MB chunk used to return NULL at once, so one transient shortage under memory pressure
 * became -ENOMEM from TTM populate -> amdgpu_cs_ioctl -> "Vulkan backend is down" -> WindowServer exit (2026-11-18
 * 10:35:15). Linux's order-0 GFP_KERNEL/GFP_USER allocation reclaims and retries instead of failing, and FreeBSD's
 * linux_alloc_pages loops on vm_wait. Now: a one-page run first, then, for callers that may sleep (VM_ALLOC_WAITOK, set
 * by linux_alloc_pages for M_WAITOK without __GFP_NORETRY), back off and retry (pool, chunk, page) for up to 2 s.
 */
uint64_t gNmxPageSingleRuns, gNmxPageAllocWaits, gNmxPageAllocFails;   /* debug.nmx_page_single_runs / _alloc_waits / _alloc_fails */
extern void IOSleep(unsigned int milliseconds);
static u_int free_count;
/*
 * ga403wm Step141d: the aged release gave back 194 chunks in a short pause between two videos; the next video start then
 * took ~490 MB in 3 s and the allocation path made 107 chunks (142 drops). The pool now keeps, besides the reserve, what
 * chunk pages were in use at their peak over the last CHUNK_HWM_NS: a chunk is released only while
 * free - its pages >= CHUNK_RESERVE + (hwm - chunk_live). The peak is re-based to the current use once it is older.
 */
#define CHUNK_HWM_NS (60ull * 1000000000ull)
#define CHUNK_SLOW_NS (600ull * 1000000000ull)
#define CHUNK_FAST_CAP 262144              /* Step141e: the 60 s peak keeps at most 1 GB above the reserve */
#define CHUNK_SLOW_CAP 131072              /* Step141e: the 10 min peak keeps at most 512 MB above the reserve */
/*
 * Step141e (review of 141d): each peak is the max of two back-to-back windows, so a roll-over that lands on a dip in
 * use does not drop the floor mid-video; a slow 10 min peak (capped) covers a video restart after a short break; both
 * are capped so that closing a big client does not keep gigabytes wired.
 */
static u_int chunk_live;                  /* chunk pages handed out (pg_lock) */
static u_int hwm_cur, hwm_prev, slow_cur, slow_prev;   /* peaks of chunk_live per window (pg_lock) */
static uint64_t hwm_start, slow_start;
uint64_t gNmxPageHwm;                     /* debug.nmx_page_hwm: pages kept above the reserve right now */

static void window_roll(u_int *cur, u_int *prev, uint64_t *start, uint64_t now, uint64_t len)
{
    if (now - *start < len)
        return;
    *prev = (now - *start < 2 * len) ? *cur : chunk_live;   /* two windows gone: the old peak is stale too */
    *cur = chunk_live;
    *start = now;
}

/* free pages the pool must keep (pg_lock); rolls the peak windows */
static u_int pool_floor(void)
{
    uint64_t now = mach_absolute_time(), fast, slow;
    nanoseconds_to_absolutetime(CHUNK_HWM_NS, &fast);
    nanoseconds_to_absolutetime(CHUNK_SLOW_NS, &slow);
    window_roll(&hwm_cur, &hwm_prev, &hwm_start, now, fast);
    window_roll(&slow_cur, &slow_prev, &slow_start, now, slow);
    u_int f = hwm_cur > hwm_prev ? hwm_cur : hwm_prev;
    u_int sl = slow_cur > slow_prev ? slow_cur : slow_prev;
    f = f > chunk_live ? f - chunk_live : 0;
    sl = sl > chunk_live ? sl - chunk_live : 0;
    if (f > CHUNK_FAST_CAP)
        f = CHUNK_FAST_CAP;
    if (sl > CHUNK_SLOW_CAP)
        sl = CHUNK_SLOW_CAP;
    u_int keep = f > sl ? f : sl;
    gNmxPageHwm = keep;
    return CHUNK_RESERVE + keep;
}
uint64_t gNmxPageChunks, gNmxPageFree, gNmxPageChunksFreed;   /* debug.nmx_page_chunks / _free / _chunks_freed */
uint64_t gNmxPagesLive;                   /* pages currently handed out (all kinds); debug.nmx_pages_live */
#define pages_live gNmxPagesLive

static unsigned reg_bucket(vm_paddr_t pfn)
{
    return (unsigned)((pfn ^ (pfn >> 21)) & (REG_BUCKETS - 1));
}

static lck_spin_t *reg_lock_of(vm_paddr_t pfn)
{
    return reg_locks[reg_bucket(pfn) & (REG_LOCKS - 1)];
}

/* the *_locked registry helpers below need reg_lock_of(pfn) held */
static void reg_insert_locked(struct vm_page *m, struct reg_entry *e)
{
    vm_paddr_t pfn = m->phys_addr >> PAGE_SHIFT;
    unsigned b = reg_bucket(pfn);
    e->pfn = pfn;
    e->m = m;
    e->next = reg_table[b];
    reg_table[b] = e;
}

static struct reg_entry *reg_remove_locked(vm_paddr_t pfn)
{
    unsigned b = reg_bucket(pfn);
    for (struct reg_entry **pp = &reg_table[b]; *pp; pp = &(*pp)->next) {
        if ((*pp)->pfn == pfn) {
            struct reg_entry *e = *pp;
            *pp = e->next;
            return e;
        }
    }
    return NULL;
}

/* removes the entry of pfn that maps m (not just the first entry of pfn); NULL when m is not registered under pfn */
static struct reg_entry *reg_remove_entry_locked(vm_paddr_t pfn, struct vm_page *m)
{
    unsigned b = reg_bucket(pfn);
    for (struct reg_entry **pp = &reg_table[b]; *pp; pp = &(*pp)->next) {
        if ((*pp)->pfn == pfn && (*pp)->m == m) {
            struct reg_entry *e = *pp;
            *pp = e->next;
            return e;
        }
    }
    return NULL;
}

static struct vm_page *reg_find_locked(vm_paddr_t pfn)
{
    for (struct reg_entry *e = reg_table[reg_bucket(pfn)]; e; e = e->next)
        if (e->pfn == pfn)
            return e->m;
    return NULL;
}

static void backing_release(struct backing *bk);
void nmx_ft_stall(int kind, uint64_t t0, uint64_t pages);    /* nmx_fb_bridge.c, FRAME-PATH row 10 */
static void release_worker(void *a, void *b);
static void fill_worker(void *a, void *b);

int nmx_page_init(void)
{
    release_call = thread_call_allocate((thread_call_func_t)release_worker, NULL);
    fill_call = thread_call_allocate((thread_call_func_t)fill_worker, NULL);
    pg_lock = lck_spin_alloc_init(nmx_lck_grp, LCK_ATTR_NULL);
    for (int i = 0; i < REG_LOCKS; i++)
        if ((reg_locks[i] = lck_spin_alloc_init(nmx_lck_grp, LCK_ATTR_NULL)) == NULL)
            return ENOMEM;
    reg_table = malloc(sizeof(*reg_table) * REG_BUCKETS, M_NMXPAGE, M_WAITOK | M_ZERO);
    return (pg_lock && reg_table) ? 0 : ENOMEM;
}

static void page_record_init(struct vm_page *m, struct backing *bk, vm_paddr_t pa, void *kva)
{
    bzero(m, sizeof(*m));
    m->phys_addr = pa;
    m->nmx_kva = kva;
    m->nmx_alloc = bk;
    m->oflags = VPO_UNMANAGED;
    m->md.pat_mode = VM_MEMATTR_WRITE_BACK;
}

/* registers every page of a new backing; the registry entries are allocated before taking the spin lock */
static int backing_register(struct backing *bk)
{
    struct reg_entry *ents = malloc(sizeof(*ents) * bk->npages, M_NMXPAGE, M_WAITOK);
    if (ents == NULL)
        return ENOMEM;
    for (size_t i = 0; i < bk->npages; i++) {
        lck_spin_t *l = reg_lock_of(bk->pages[i].phys_addr >> PAGE_SHIFT);
        lck_spin_lock(l);
        reg_insert_locked(&bk->pages[i], &ents[i]);
        lck_spin_unlock(l);
    }
    return 0;
}

/* removes a backing's pages from the registry (takes each page's stripe); returns the entry array for freeing */
static struct reg_entry *backing_unregister(struct backing *bk)
{
    struct reg_entry *first = NULL;
    for (size_t i = 0; i < bk->npages; i++) {
        vm_paddr_t pfn = bk->pages[i].phys_addr >> PAGE_SHIFT;
        lck_spin_t *l = reg_lock_of(pfn);
        lck_spin_lock(l);
        struct reg_entry *e = reg_remove_locked(pfn);
        lck_spin_unlock(l);
        if (i == 0)
            first = e;
    }
    return first;   /* entries were allocated as one array whose first element maps page 0 */
}

static struct backing *backing_alloc(int kind, size_t npages, int contiguous, uint64_t max_phys, size_t align)
{
    struct backing *bk = malloc(sizeof(*bk) + sizeof(struct vm_page) * npages, M_NMXPAGE, M_WAITOK | M_ZERO);
    if (bk == NULL)
        return NULL;
    bk->kind = kind;
    bk->npages = npages;
    bk->kva = nmx_xnu_mem_alloc(npages * PAGE_SIZE, contiguous, max_phys, align, &bk->handle);
    if (bk->kva == NULL) {
        free(bk, M_NMXPAGE);
        return NULL;
    }
    for (size_t i = 0; i < npages; i++) {
        void *kva = (char *)bk->kva + i * PAGE_SIZE;
        page_record_init(&bk->pages[i], bk, nmx_xnu_kva_to_phys(kva), kva);
    }
    if (backing_register(bk) != 0) {
        nmx_xnu_mem_free(bk->handle);
        free(bk, M_NMXPAGE);
        return NULL;
    }
    return bk;
}

static void backing_release(struct backing *bk)
{
    struct reg_entry *ents = backing_unregister(bk);
    free(ents, M_NMXPAGE);
    nmx_xnu_mem_free(bk->handle);
    free(bk, M_NMXPAGE);
}

/* ---- allocation --------------------------------------------------------------------------------------------------- */

static void avail_add(struct backing *bk)
{
    bk->aprev = NULL;
    bk->anext = avail;
    if (avail)
        avail->aprev = bk;
    avail = bk;
}

static void avail_del(struct backing *bk)
{
    if (bk->aprev)
        bk->aprev->anext = bk->anext;
    else
        avail = bk->anext;
    if (bk->anext)
        bk->anext->aprev = bk->aprev;
    bk->anext = bk->aprev = NULL;
}

static void chunk_unlink(struct backing *bk)
{
    if (bk->prev)
        bk->prev->next = bk->next;
    else
        chunks = bk->next;
    if (bk->next)
        bk->next->prev = bk->prev;
    bk->next = bk->prev = NULL;
}

/* call: release_call as read under pg_lock with sweep_busy raised, so unload cannot free it under us */
static void sweep_arm(thread_call_t call, uint32_t ms)
{
    uint64_t deadline;
    clock_interval_to_deadline(ms, 1000000 /* ns per ms */, &deadline);
    thread_call_enter_delayed(call, deadline);
    lck_spin_lock(pg_lock);
    sweep_busy--;
    lck_spin_unlock(pg_lock);
}

/*
 * the release sweep (thread call): unlinks every chunk that has been wholly free for CHUNK_IDLE_NS while the pool keeps
 * CHUNK_RESERVE free pages without it, frees them outside pg_lock, and runs again in a second while free chunks above
 * the reserve are still waiting. Also called once at unload with release_call already gone.
 */
static void release_worker(void *a, void *b)
{
    struct backing *q = NULL;
    uint64_t idle;
    nanoseconds_to_absolutetime(CHUNK_IDLE_NS, &idle);
    lck_spin_lock(pg_lock);
    sweep_busy++;
    uint64_t now = mach_absolute_time();
    u_int floor = pool_floor();
    int waiting = 0, held = 0;
    for (struct backing *c = avail, *n; c; c = n) {
        n = c->anext;
        if (c->live != 0)
            continue;
        if (free_count - c->nfree < CHUNK_RESERVE)
            continue;
        if (free_count - c->nfree < floor) {
            held = 1;                     /* kept by a recent peak; look again later (slowly) */
            continue;
        }
        if (now - c->empty_at < idle) {
            waiting = 1;
            continue;
        }
        avail_del(c);
        chunk_unlink(c);
        free_count -= c->nfree;
        gNmxPageChunks--;
        gNmxPageChunksFreed++;
        c->next = q;
        q = c;
    }
    gNmxPageFree = free_count;
    thread_call_t call = release_call;
    int rearm = (waiting || held) && call != NULL && !sweep_stopping;
    uint32_t next_ms = waiting ? CHUNK_SWEEP_MS : CHUNK_HELD_MS;
    sweep_armed = rearm;
    if (rearm)
        sweep_busy++;                     /* dropped by sweep_arm */
    lck_spin_unlock(pg_lock);
    if (rearm)
        sweep_arm(call, next_ms);
    while (q) {
        struct backing *n = q->next;
        backing_release(q);
        q = n;
    }
    lck_spin_lock(pg_lock);
    sweep_busy--;
    lck_spin_unlock(pg_lock);
}

/* makes one chunk and puts all its pages in the pool; thread context (may block) */
static int chunk_new(uint64_t *counter)
{
    struct backing *bk = backing_alloc(BK_CHUNK, CHUNK_PAGES, 0, 0, PAGE_SIZE);
    if (bk == NULL)
        return ENOMEM;
    bk->cfree = NULL;
    for (size_t i = bk->npages; i-- > 0;) {
        bk->pages[i].plinks.s.ss.sle_next = (void *)bk->cfree;
        bk->cfree = &bk->pages[i];
    }
    bk->nfree = (u_int)bk->npages;
    bk->empty_at = mach_absolute_time();
    lck_spin_lock(pg_lock);
    bk->prev = NULL;
    bk->next = chunks;
    if (chunks)
        chunks->prev = bk;
    chunks = bk;
    avail_add(bk);
    free_count += bk->nfree;
    gNmxPageFree = free_count;
    gNmxPageChunks++;
    (*counter)++;
    lck_spin_unlock(pg_lock);
    return 0;
}

/* the fill thread call: tops the pool up to CHUNK_RESERVE free pages (bounded per run) */
static void fill_worker(void *a, void *b)
{
    lck_spin_lock(pg_lock);
    sweep_busy++;
    lck_spin_unlock(pg_lock);
    for (int i = 0; i < CHUNK_RESERVE / CHUNK_PAGES; i++) {
        lck_spin_lock(pg_lock);
        int need = free_count < CHUNK_RESERVE && !sweep_stopping;
        lck_spin_unlock(pg_lock);
        if (!need)
            break;
        uint64_t t0 = mach_absolute_time();
        int r = chunk_new(&gNmxPageChunksFilled);
        nmx_ft_stall(2, t0, CHUNK_PAGES);
        if (r != 0)
            break;
    }
    lck_spin_lock(pg_lock);
    fill_armed = 0;
    sweep_busy--;
    lck_spin_unlock(pg_lock);
}

/* one page in a run of its own (no chunk could be made): freed with its backing when the page comes back */
static struct vm_page *single_run(void)
{
    struct backing *bk = backing_alloc(BK_RUN, 1, 0, 0, PAGE_SIZE);
    if (bk == NULL)
        return NULL;
    lck_spin_lock(pg_lock);
    bk->live = 1;
    pages_live++;
    lck_spin_unlock(pg_lock);
    __atomic_add_fetch(&gNmxPageSingleRuns, 1, __ATOMIC_RELAXED);
    return &bk->pages[0];
}

static struct vm_page *single_alloc(int req)
{
    unsigned waited_ms = 0, step_ms = 1;
    for (;;) {
        lck_spin_lock(pg_lock);
        struct backing *c = avail;
        if (c) {
            struct vm_page *m = c->cfree;
            c->cfree = (struct vm_page *)(uintptr_t)m->plinks.s.ss.sle_next;
            if (--c->nfree == 0)
                avail_del(c);
            free_count--;
            gNmxPageFree = free_count;
            c->live++;
            pages_live++;
            if (++chunk_live > hwm_cur)
                hwm_cur = chunk_live;
            if (chunk_live > slow_cur)
                slow_cur = chunk_live;
            thread_call_t call = fill_call;
            int fill = free_count < CHUNK_RESERVE / 2 && !fill_armed && !sweep_stopping && call != NULL;
            if (fill) {
                fill_armed = 1;
                sweep_busy++;
            }
            lck_spin_unlock(pg_lock);
            m->plinks.s.ss.sle_next = NULL;
            if (fill) {
                thread_call_enter(call);
                lck_spin_lock(pg_lock);
                sweep_busy--;
                lck_spin_unlock(pg_lock);
            }
            return m;
        }
        lck_spin_unlock(pg_lock);
        uint64_t t0 = mach_absolute_time();
        int r = chunk_new(&gNmxPageChunksInline);
        nmx_ft_stall(1, t0, CHUNK_PAGES);
        if (r == 0)
            continue;
        struct vm_page *m = single_run();
        if (m != NULL)
            return m;
        if (!(req & VM_ALLOC_WAITOK) || waited_ms >= 2000) {
            __atomic_add_fetch(&gNmxPageAllocFails, 1, __ATOMIC_RELAXED);
            return NULL;
        }
        /* the system is short of free pages: let the pageout daemon and compressor work, then look again (another
         * thread or the fill call may have refilled the pool meanwhile) */
        __atomic_add_fetch(&gNmxPageAllocWaits, 1, __ATOMIC_RELAXED);
        IOSleep(step_ms);
        waited_ms += step_ms;
        if (step_ms < 64)
            step_ms *= 2;
    }
}

static void page_prepare(struct vm_page *m, int req)
{
    m->ref_count = 1;               /* every page starts wired once; LinuxKPI always asks for wired pages */
    m->flags = 0;
    m->md.pat_mode = VM_MEMATTR_WRITE_BACK;
    if (req & VM_ALLOC_ZERO)
        bzero(m->nmx_kva, PAGE_SIZE);
}

vm_page_t vm_page_alloc_noobj(int req)
{
    struct vm_page *m = single_alloc(req);
    if (m)
        page_prepare(m, req);
    return m;
}

vm_page_t vm_page_alloc_noobj_contig(int req, u_long npages, vm_paddr_t low, vm_paddr_t high, u_long alignment,
                                     vm_paddr_t boundary, vm_memattr_t memattr)
{
    /* a single page with no address limit can come from the chunk pool; any limit needs its own placed run */
    if (npages == 1 && high >= BUS_SPACE_MAXADDR && alignment <= PAGE_SIZE)
        return vm_page_alloc_noobj(req);
    struct backing *bk = backing_alloc(BK_RUN, npages, 1, high, alignment > PAGE_SIZE ? alignment : PAGE_SIZE);
    if (bk == NULL)
        return NULL;
    lck_spin_lock(pg_lock);
    bk->live = (u_int)npages;
    pages_live += npages;
    lck_spin_unlock(pg_lock);
    for (u_long i = 0; i < npages; i++) {
        page_prepare(&bk->pages[i], req);
        bk->pages[i].md.pat_mode = memattr;
    }
    return &bk->pages[0];
}

/* returns one page to its backing; the backing is released when its last page comes back */
void vm_page_free(vm_page_t m)
{
    struct backing *bk = m->nmx_alloc;
    if (bk == NULL || bk->kind == BK_FICTITIOUS || bk->kind == BK_USER)   /* BK_USER: page_unref frees it */
        return;
    lck_spin_lock(pg_lock);
    pages_live--;
    bk->live--;
    if (bk->kind == BK_CHUNK) {
        chunk_live--;
        m->plinks.s.ss.sle_next = (void *)bk->cfree;
        bk->cfree = m;
        if (bk->nfree++ == 0)
            avail_add(bk);
        free_count++;
        int arm = 0;
        thread_call_t call = release_call;
        if (bk->live == 0) {
            bk->empty_at = mach_absolute_time();
            arm = !sweep_armed && !sweep_stopping && call != NULL && free_count - bk->nfree >= CHUNK_RESERVE;
            /* (CHUNK_RESERVE, not pool_floor: the sweep re-checks the floor and keeps running while the peak holds) */
            if (arm) {
                sweep_armed = 1;
                sweep_busy++;             /* dropped by sweep_arm */
            }
        }
        gNmxPageFree = free_count;
        lck_spin_unlock(pg_lock);
        if (arm)
            sweep_arm(call, CHUNK_SWEEP_MS);
        return;
    }
    int last = bk->live == 0;
    lck_spin_unlock(pg_lock);
    if (last)
        backing_release(bk);
}

void vm_page_free_contig(vm_page_t m, u_long npages)
{
    for (u_long i = 0; i < npages; i++)
        vm_page_free(m + i);
}

void vm_page_wire(vm_page_t m)
{
    __atomic_fetch_add(&m->ref_count, 1, __ATOMIC_SEQ_CST);
}

/*
 * drops one wiring; true when it was the last and the page belongs to no object, so the caller frees it. A page in
 * an object stays with the object at zero wirings, as in FreeBSD; the object frees it.
 */
/*
 * ---- a process's pages pinned for a device (Linux get_user_pages) -------------------------------------------------------
 * xnu wires a user range through an IOMemoryDescriptor on the process's task (nmx_xnu_user_wire), one descriptor per
 * request. Each page of the request gets the port's page record (a fictitious one: the memory is the process's) and
 * one extra wiring, plus an entry on the record's pin list naming the request. Linux drops a pin with put_page; here
 * the matching unwire pops one entry from that page's list and, when every page of a request has been put, the
 * descriptor is released and xnu unwires the range. A page pinned by several requests carries one entry per pin,
 * and any of them may be popped: each request is only released after as many puts as it has pages, and a page's
 * entries are only consumed by puts of that page, so no page is unwired while one of its pins is still held.
 * The release can block (xnu unwires the range), so put_page on a pinned user page must come from thread context,
 * which is where Linux drivers release userptr pages.
 */
struct user_pin {
    void *handle;                          /* nmx_xnu_user_wire descriptor */
    volatile u_int remaining;              /* page puts still to come */
    u_int owned;                           /* Step142: owned by one BO, released by nmx_upin_destroy only */
    int npages, write;
};
struct pin_ref {
    struct user_pin *pin;
    struct pin_ref *next;
    struct pin_blk *blk;                   /* import reuse bit 2: the block it was carved from, NULL = own malloc */
};
struct pin_blk {
    volatile u_int live;                   /* entries of this block not yet freed */
    struct pin_ref refs[];
};

static void pin_ref_free(struct pin_ref *r)
{
    struct pin_blk *b = r->blk;
    if (b == NULL)
        free(r, M_NMXPAGE);
    else if (__atomic_sub_fetch(&b->live, 1, __ATOMIC_SEQ_CST) == 0) {
        free(b, M_NMXPAGE);
        __atomic_sub_fetch(&gNmxReuseBlocksLive, 1, __ATOMIC_RELAXED);
    }
}

/* refs[0..n) = n new pin entries: blocks of PIN_BLK_REFS with bit 2, else one malloc each. 0, or ENOMEM with none left */
static int pin_refs_alloc(struct pin_ref **refs, int n)
{
    int i = 0;
    if (__atomic_load_n(&nmx_import_reuse, __ATOMIC_RELAXED) & 2) {
        while (i < n) {
            int k = n - i < PIN_BLK_REFS ? n - i : PIN_BLK_REFS;
            struct pin_blk *b = malloc(sizeof(*b) + sizeof(struct pin_ref) * (size_t)k, M_NMXPAGE, M_WAITOK);
            if (b == NULL)
                goto fail;
            b->live = (u_int)k;
            for (int j = 0; j < k; j++) {
                b->refs[j].blk = b;
                refs[i++] = &b->refs[j];
            }
            __atomic_add_fetch(&gNmxReuseBlocks, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&gNmxReuseBlocksLive, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&gNmxReuseBlockRefs, k, __ATOMIC_RELAXED);
        }
        return 0;
    }
    for (; i < n; i++) {
        if ((refs[i] = malloc(sizeof(struct pin_ref), M_NMXPAGE, M_WAITOK)) == NULL)
            goto fail;
        refs[i]->blk = NULL;
    }
    return 0;
fail:
    for (int j = 0; j < i; j++)
        pin_ref_free(refs[j]);
    return ENOMEM;
}

/* frees refs[0..n) that were never pushed on a page */
static void pin_refs_free_unused(struct pin_ref **refs, int n)
{
    for (int i = 0; i < n; i++)
        if (refs[i] != NULL)
            pin_ref_free(refs[i]);
}

static void pin_put(struct user_pin *pin)
{
    if (__atomic_sub_fetch(&pin->remaining, 1, __ATOMIC_SEQ_CST) == 0 && !pin->owned) {
        nmx_xnu_user_unwire(pin->handle);
        free(pin, M_NMXPAGE);
    }
}

/* takes one pin entry off a page, if it has any; the request it named, or NULL */
static struct user_pin *page_pop_pin(vm_page_t m)
{
    struct pin_ref *r;
    lck_spin_t *l = reg_lock_of(m->phys_addr >> PAGE_SHIFT);
    lck_spin_lock(l);
    r = m->nmx_pins;
    if (r != NULL)
        m->nmx_pins = r->next;
    lck_spin_unlock(l);
    if (r == NULL)
        return NULL;
    struct user_pin *pin = r->pin;
    pin_ref_free(r);
    return pin;
}

static bool page_unref(vm_page_t m);

#include "nmx_import_trace.h"   /* WS-CPU item 2 probes: C6 attach, D2 put_pages, D3 unwire, record counts */

/*
 * ---- import reuse (WS-CPU item 2 cut, debug.nmx_import_reuse, default 0) ---------------------------------------------
 * The out-54 counters: every IOSurface re-import rebuilt its page records (8.8k/s: backing+vm_page malloc, registry
 * entry malloc, two stripe-lock rounds, and two frees at put_pages), and pin_attach made one pin entry malloc per page.
 * Bits, each read where it acts, so the gate can be flipped at runtime for an A/B:
 *   1  a BK_USER record whose last reference and pin go stays registered on an idle list of its stripe instead of being
 *      freed; the next pin of the same page frame (same surface re-imported, or any process) revives it. A record's
 *      content is its page frame only, and a revived record is re-initialised exactly like a new one, so a page frame
 *      the process freed and the kernel handed to someone else is still correct: the record never says whose it is.
 *      The list is LRU, capped at debug.nmx_import_reuse_cap records in all (per stripe cap/256, at least 1); over it,
 *      the oldest idle record of the stripe is freed. A page frame that our own chunk allocator gets later is
 *      registered at the chain head and shadows the idle record (lookups return the first match); the idle record is
 *      removed by its own entry, never by "first match of pfn". An idle record reached by another path
 *      (nmx_phys_to_page) leaves the list and turns permanent, as a live BK_USER record does (Step138).
 *      Clearing bit 1 frees every idle record (sysctl handler), and so does unload.
 *   2  pin entries are allocated in blocks of up to PIN_BLK_REFS per import (one malloc per block, a live count frees the
 *      block with its last entry) instead of one malloc per page.
 *   4  (xnu/nmx_mm.cpp) the physical address loop of a wire / rewire takes each segment's length and fills the
 *      contiguous pages from one getPhysicalSegment call instead of one call per page.
 * Counters: debug.nmx_import_reuse_{revived,kept,idle,evicted,drained,blocks,blocks_live,block_refs,segs,seg_pages}.
 * Writing debug.nmx_import_reuse_cap frees the idle records over the new cap at once (0: all of them).
 */
static struct backing *idle_head[REG_LOCKS], *idle_tail[REG_LOCKS];   /* newest first; links bk->next/prev */
static u_int idle_n[REG_LOCKS];
#define REUSE_EVICT_MAX 8                  /* idle records freed per push at most (a lowered cap shrinks gradually) */

static unsigned stripe_of(vm_paddr_t pfn)
{
    return reg_bucket(pfn) & (REG_LOCKS - 1);
}

/* the *idle*_locked helpers need the stripe's lock */
static void idle_unlink_locked(unsigned s, struct backing *bk)
{
    if (bk->prev)
        bk->prev->next = bk->next;
    else
        idle_head[s] = bk->next;
    if (bk->next)
        bk->next->prev = bk->prev;
    else
        idle_tail[s] = bk->prev;
    bk->next = bk->prev = NULL;
    bk->idle = 0;
    idle_n[s]--;
    __atomic_sub_fetch(&gNmxReuseIdle, 1, __ATOMIC_RELAXED);
}

static void idle_push_locked(unsigned s, struct backing *bk)
{
    bk->idle = 1;
    bk->prev = NULL;
    bk->next = idle_head[s];
    if (idle_head[s])
        idle_head[s]->prev = bk;
    else
        idle_tail[s] = bk;
    idle_head[s] = bk;
    idle_n[s]++;
    __atomic_add_fetch(&gNmxReuseIdle, 1, __ATOMIC_RELAXED);
}

/* takes the stripe's oldest idle record off the list and out of the registry; *ep = its entry. NULL if none */
static struct backing *idle_pop_oldest_locked(unsigned s, struct reg_entry **ep)
{
    struct backing *bk = idle_tail[s];
    if (bk == NULL)
        return NULL;
    idle_unlink_locked(s, bk);
    *ep = reg_remove_entry_locked(bk->pages[0].phys_addr >> PAGE_SHIFT, &bk->pages[0]);
    return bk;
}

/* frees a chain of records taken off an idle list (linked through bk->next, entries through e->next) */
static void idle_free_chain(struct backing *bk, struct reg_entry *e, volatile long long *counter)
{
    while (bk) {
        struct backing *n = bk->next;
        free(bk, M_NMXPAGE);
        __atomic_sub_fetch(&gNmxUserRecLive, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gNmxUserRecFreed, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
        bk = n;
    }
    while (e) {
        struct reg_entry *n = e->next;
        free(e, M_NMXPAGE);
        e = n;
    }
}

/* per-stripe idle limit for a total cap: cap/256, at least 1 while cap > 0; 0 = keep none */
static u_int stripe_cap(int cap)
{
    if (cap <= 0)
        return 0;
    return (u_int)cap / REG_LOCKS ? (u_int)cap / REG_LOCKS : 1;
}

/* frees the oldest idle records of every stripe until each holds at most per_stripe; revived records are not touched */
static void idle_trim(u_int per_stripe)
{
    if (reg_table == NULL)
        return;
    for (unsigned s = 0; s < REG_LOCKS; s++) {
        if (reg_locks[s] == NULL)
            continue;
        struct backing *chain = NULL;
        struct reg_entry *ents = NULL;
        lck_spin_lock(reg_locks[s]);
        struct reg_entry *e;
        struct backing *bk;
        while (idle_n[s] > per_stripe && (bk = idle_pop_oldest_locked(s, &e)) != NULL) {
            bk->next = chain;
            chain = bk;
            if (e) {
                e->next = ents;
                ents = e;
            }
        }
        lck_spin_unlock(reg_locks[s]);
        idle_free_chain(chain, ents, &gNmxReuseDrained);
    }
}

/* frees every idle record (bit 1 cleared, cap 0, unload) */
void nmx_import_reuse_drain(void)
{
    idle_trim(0);
}

/* sysctl write of debug.nmx_import_reuse_cap: a lower cap frees what is over it now (0 frees every idle record) */
void nmx_import_reuse_cap_set(int cap)
{
    if (cap < 0)
        cap = 0;
    __atomic_store_n(&nmx_import_reuse_cap, cap, __ATOMIC_SEQ_CST);
    idle_trim(stripe_cap(cap));
}

/* sysctl write of debug.nmx_import_reuse (xnu/nmx_kext.cpp): clearing bit 1 frees the idle records */
void nmx_import_reuse_set(int v)
{
    int old = __atomic_exchange_n(&nmx_import_reuse, v, __ATOMIC_SEQ_CST);
    if ((old & 1) && !(v & 1))
        nmx_import_reuse_drain();
}

/*
 * an idle record about to be handed out without a reference (nmx_phys_to_page): off the list and re-initialised as
 * out-55's fresh fictitious record is (ref 1, PG_FICTITIOUS, nothing else), then the caller makes it permanent
 */
static void idle_forget_locked(struct vm_page *m)
{
    struct backing *bk = m->nmx_alloc;
    if (!bk->idle)
        return;
    vm_paddr_t pa = m->phys_addr;
    idle_unlink_locked(stripe_of(pa >> PAGE_SHIFT), bk);
    page_record_init(m, bk, pa, NULL);
    m->flags = PG_FICTITIOUS;
    m->ref_count = 1;
}

/* one more reference on a registered record (stripe lock held); an idle one (import reuse bit 1: ref 0, no pins,
 * nobody holds it) leaves the idle list and is re-initialised exactly as a new BK_USER record */
static void rec_ref_locked(struct vm_page *m, vm_paddr_t pfn)
{
    struct backing *ob = m->nmx_alloc;
    if (ob != NULL && ob->idle) {
        idle_unlink_locked(stripe_of(pfn), ob);
        page_record_init(m, ob, pfn << PAGE_SHIFT, NULL);
        m->flags = PG_FICTITIOUS;
        m->ref_count = 1;
        __atomic_add_fetch(&gNmxReuseRevived, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&m->ref_count, 1, __ATOMIC_SEQ_CST);
}

/* the record for a pinned user page, with one more reference; a new one is a BK_USER record (Step138) */
static vm_page_t user_page_get(vm_paddr_t pa)
{
    vm_paddr_t pfn = pa >> PAGE_SHIFT;
    lck_spin_t *l = reg_lock_of(pfn);
    lck_spin_lock(l);
    struct vm_page *m = reg_find_locked(pfn);
    if (m != NULL)
        rec_ref_locked(m, pfn);
    lck_spin_unlock(l);
    if (m != NULL) {
        NMX_IMP_COUNT(NMX_IMPC_REC_REUSED, 1);
        return m;
    }
    NMX_IMP_COUNT(NMX_IMPC_REC_NEW, 1);
    struct backing *bk = malloc(sizeof(*bk) + sizeof(struct vm_page), M_NMXPAGE, M_WAITOK | M_ZERO);
    struct reg_entry *e = malloc(sizeof(*e), M_NMXPAGE, M_WAITOK);
    if (bk == NULL || e == NULL) {
        free(bk, M_NMXPAGE);
        free(e, M_NMXPAGE);
        return NULL;
    }
    bk->kind = BK_USER;
    bk->npages = 1;
    page_record_init(&bk->pages[0], bk, pfn << PAGE_SHIFT, NULL);
    bk->pages[0].flags = PG_FICTITIOUS;
    bk->pages[0].ref_count = 1;
    lck_spin_lock(l);
    m = reg_find_locked(pfn);
    if (m == NULL) {
        reg_insert_locked(&bk->pages[0], e);
        m = &bk->pages[0];
        bk = NULL;
        e = NULL;
    } else {
        rec_ref_locked(m, pfn);     /* made by another thread meanwhile, and maybe idle again already */
    }
    lck_spin_unlock(l);
    if (bk == NULL) {
        __atomic_add_fetch(&gNmxUserRecLive, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gNmxUserRecMade, 1, __ATOMIC_RELAXED);
    }
    free(bk, M_NMXPAGE);
    free(e, M_NMXPAGE);
    return m;
}

int nmx_user_pages_pin(void *mm, uintptr_t start, int npages, int write, vm_page_t *pages)
{
    if (npages <= 0)
        return 0;
    uint64_t *phys = malloc(sizeof(uint64_t) * (size_t)npages, M_NMXPAGE, M_WAITOK);
    struct user_pin *pin = malloc(sizeof(*pin), M_NMXPAGE, M_WAITOK | M_ZERO);
    struct pin_ref **refs = malloc(sizeof(*refs) * (size_t)npages, M_NMXPAGE, M_WAITOK | M_ZERO);
    int err = ENOMEM, i;
    if (phys == NULL || pin == NULL || refs == NULL)
        goto fail;
    if (pin_refs_alloc(refs, npages) != 0) {
        free(refs, M_NMXPAGE);              /* nothing in it: pin_refs_alloc freed what it made */
        refs = NULL;
        goto fail;
    }
    err = nmx_xnu_user_wire(mm, start, (size_t)npages, write, phys, &pin->handle);
    if (err != 0)
        goto fail;
    for (i = 0; i < npages; i++) {
        pages[i] = user_page_get(phys[i]);
        if (pages[i] == NULL) {
            /*
             * no record could be made: drop the references taken so far (no pin entry is on any page yet, so none is
             * popped: a page's list may hold other requests' pins), then release the wiring
             */
            for (int k = 0; k < i; k++)
                if (page_unref(pages[k]))
                    vm_page_free(pages[k]);
            pin->remaining = 1;
            pin_put(pin);
            pin = NULL;
            pin_refs_free_unused(refs, npages);   /* none was pushed yet */
            free(refs, M_NMXPAGE);
            free(phys, M_NMXPAGE);
            return -ENOMEM;
        }
    }
    pin->remaining = (u_int)npages;
    for (i = 0; i < npages; i++) {
        refs[i]->pin = pin;
        lck_spin_t *l = reg_lock_of(pages[i]->phys_addr >> PAGE_SHIFT);
        lck_spin_lock(l);
        refs[i]->next = pages[i]->nmx_pins;
        pages[i]->nmx_pins = refs[i];
        lck_spin_unlock(l);                     /* the reference was taken by user_page_get */
    }
    free(refs, M_NMXPAGE);
    free(phys, M_NMXPAGE);
    return npages;
fail:
    if (refs != NULL)
        pin_refs_free_unused(refs, npages);
    free(refs, M_NMXPAGE);
    free(pin, M_NMXPAGE);
    free(phys, M_NMXPAGE);
    return -err;
}

/* drops one reference of a page; a BK_USER record goes once nothing holds it (see Step138 above). True: caller frees */
static bool page_unref(vm_page_t m)
{
    struct backing *bk = m->nmx_alloc;
    if (bk == NULL || bk->kind != BK_USER)
        return __atomic_sub_fetch(&m->ref_count, 1, __ATOMIC_SEQ_CST) == 0 && m->object == NULL;
    vm_paddr_t pfn = m->phys_addr >> PAGE_SHIFT;
    lck_spin_t *l = reg_lock_of(pfn);
    struct reg_entry *e = NULL;
    struct backing *victims = NULL;
    struct reg_entry *vents = NULL;
    lck_spin_lock(l);
    u_int left = __atomic_sub_fetch(&m->ref_count, 1, __ATOMIC_SEQ_CST);
    if (left == 0 && m->nmx_pins == NULL && bk->kind == BK_USER) {
        int cap = __atomic_load_n(&nmx_import_reuse_cap, __ATOMIC_RELAXED);
        if ((__atomic_load_n(&nmx_import_reuse, __ATOMIC_RELAXED) & 1) && cap > 0 && !bk->idle) {
            /* import reuse bit 1: keep it registered and idle; free the stripe's oldest ones over the cap */
            unsigned s = stripe_of(pfn);
            u_int scap = stripe_cap(cap);
            idle_push_locked(s, bk);
            __atomic_add_fetch(&gNmxReuseKept, 1, __ATOMIC_RELAXED);
            for (int k = 0; k < REUSE_EVICT_MAX && idle_n[s] > scap; k++) {
                struct reg_entry *ve = NULL;
                struct backing *v = idle_pop_oldest_locked(s, &ve);
                if (v == NULL)
                    break;
                v->next = victims;
                victims = v;
                if (ve) {
                    ve->next = vents;
                    vents = ve;
                }
            }
        } else {
            e = reg_remove_locked(pfn);
            if (e != NULL && e->m != m) {               /* not ours: put it back and keep the record */
                reg_insert_locked(e->m, e);
                e = NULL;
            }
        }
    }
    lck_spin_unlock(l);
    if (victims != NULL || vents != NULL)
        idle_free_chain(victims, vents, &gNmxReuseEvicted);
    if (e != NULL) {
        NMX_IMP_COUNT(NMX_IMPC_REC_FREED, 1);
        free(e, M_NMXPAGE);
        free(bk, M_NMXPAGE);
        __atomic_sub_fetch(&gNmxUserRecLive, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gNmxUserRecFreed, 1, __ATOMIC_RELAXED);
    }
    return false;
}

bool vm_page_unwire_noq(vm_page_t m)
{
    /* a pinned user page: this put releases one pin (see nmx_user_pages_pin) */
    if ((m->flags & PG_FICTITIOUS) != 0 && m->nmx_pins != NULL) {
        struct user_pin *pin = page_pop_pin(m);
        if (pin != NULL)
            pin_put(pin);
    }
    return page_unref(m);
}

void vm_page_unwire(vm_page_t m, uint8_t queue)
{
    if (vm_page_unwire_noq(m))
        vm_page_free(m);
}

/*
 * ---- ga403wm Step142: userptr pins owned by one BO ------------------------------------------------------------------
 * The plain pin above lives exactly as long as its page puts: amdgpu's userptr BO pinned at the ioctl and put the
 * pages in TT unpopulate, so an imported IOSurface stayed wired for the BO's whole life, however long the BO sat
 * idle in a process's texture cache (1.3-1.4 GB of IOSurface wired 35 min after boot). An owned pin separates the two:
 * - the BO's page records can be dropped (put_pages) without dropping the wiring, and the wiring (an IOSurface's own
 *   descriptor) can then be dropped while the BO is idle (park) and taken again when the GPU needs it (restore);
 * - put_pages takes this pin's own entry off each page, not whichever entry is first, so a BO never consumes another
 *   request's pins (a plain put that took this pin's entry earlier is settled with that request's entry instead);
 * - only nmx_upin_destroy releases it.
 * The caller serialises calls on one pin (amdgpu: the BO's reservation, or its destruction).
 */
/* page records for phys[0..npages) with one entry each naming pin; 0, or ENOMEM with nothing left behind */
static int pin_attach(struct user_pin *pin, const uint64_t *phys, int npages, vm_page_t *pages)
{
    NMX_IMP_T0(it6);
    struct pin_ref **refs = malloc(sizeof(*refs) * (size_t)npages, M_NMXPAGE, M_WAITOK | M_ZERO);
    int i;
    if (refs == NULL)
        return ENOMEM;
    if (pin_refs_alloc(refs, npages) != 0) {
        free(refs, M_NMXPAGE);
        return ENOMEM;
    }
    for (i = 0; i < npages; i++) {
        pages[i] = user_page_get(phys[i]);
        if (pages[i] == NULL) {
            for (int k = 0; k < i; k++) {
                if (page_unref(pages[k]))
                    vm_page_free(pages[k]);
                pages[k] = NULL;
            }
            goto fail;
        }
    }
    __atomic_add_fetch(&pin->remaining, (u_int)npages, __ATOMIC_SEQ_CST);
    for (i = 0; i < npages; i++) {
        refs[i]->pin = pin;
        lck_spin_t *l = reg_lock_of(pages[i]->phys_addr >> PAGE_SHIFT);
        lck_spin_lock(l);
        refs[i]->next = pages[i]->nmx_pins;
        pages[i]->nmx_pins = refs[i];
        lck_spin_unlock(l);
    }
    free(refs, M_NMXPAGE);
    NMX_IMP_STAGE(NMX_IMP_C6_ATTACH, it6, npages);
    return 0;
fail:
    pin_refs_free_unused(refs, npages);
    free(refs, M_NMXPAGE);
    return ENOMEM;
}

/* takes pin's own entry off a page if it is there, else the first entry; the request it named, or NULL */
static struct user_pin *page_take_pin(vm_page_t m, struct user_pin *pin)
{
    struct pin_ref *r, **pp;
    lck_spin_t *l = reg_lock_of(m->phys_addr >> PAGE_SHIFT);
    lck_spin_lock(l);
    for (pp = (struct pin_ref **)&m->nmx_pins; *pp != NULL; pp = &(*pp)->next)
        if ((*pp)->pin == pin)
            break;
    if (*pp == NULL)
        pp = (struct pin_ref **)&m->nmx_pins;
    r = *pp;
    if (r != NULL)
        *pp = r->next;
    bool others = m->nmx_pins != NULL;      /* the page is still pinned by another request */
    lck_spin_unlock(l);
    if (NMX_IMP_ON() && others)
        nmx_imp_count(NMX_IMPC_PIN_MULTI, 1);
    if (r == NULL)
        return NULL;
    struct user_pin *got = r->pin;
    pin_ref_free(r);
    return got;
}

int nmx_upin_create(uintptr_t start, int npages, int write, vm_page_t *pages, void **pinp)
{
    *pinp = NULL;
    if (npages <= 0)
        return -EINVAL;
    uint64_t *phys = malloc(sizeof(uint64_t) * (size_t)npages, M_NMXPAGE, M_WAITOK);
    struct user_pin *pin = malloc(sizeof(*pin), M_NMXPAGE, M_WAITOK | M_ZERO);
    int err = ENOMEM;
    if (phys == NULL || pin == NULL)
        goto out;
    err = nmx_xnu_user_wire(NULL, start, (size_t)npages, write, phys, &pin->handle);
    if (err != 0)
        goto out;
    pin->owned = 1;
    pin->npages = npages;
    pin->write = write;
    err = pin_attach(pin, phys, npages, pages);
    if (err != 0) {
        nmx_xnu_user_unwire(pin->handle);
        goto out;
    }
    free(phys, M_NMXPAGE);
    *pinp = pin;
    return npages;
out:
    free(pin, M_NMXPAGE);
    free(phys, M_NMXPAGE);
    return -err;
}

void nmx_upin_put_pages(void *p, vm_page_t *pages, int npages)
{
    NMX_IMP_T0(itd2);
    struct user_pin *pin = p;
    for (int i = 0; i < npages; i++) {
        vm_page_t m = pages[i];
        if (m == NULL)
            continue;
        pages[i] = NULL;
        struct user_pin *got = page_take_pin(m, pin);
        if (got == pin)
            __atomic_sub_fetch(&pin->remaining, 1, __ATOMIC_SEQ_CST);
        else if (got != NULL)
            pin_put(got);
        if (page_unref(m))
            vm_page_free(m);
    }
    NMX_IMP_STAGE(NMX_IMP_D2_PUTPAGES, itd2, npages);
}

uint32_t nmx_xnu_user_sid(void *handle);    /* xnu/nmx_mm.cpp */
uint32_t nmx_upin_sid(void *p)
{
    struct user_pin *pin = p;
    return pin != NULL ? nmx_xnu_user_sid(pin->handle) : 0;
}

int nmx_upin_parkable(void *p)
{
    struct user_pin *pin = p;
    return pin != NULL && nmx_xnu_user_parkable(pin->handle);
}

int nmx_upin_park(void *p)
{
    struct user_pin *pin = p;
    return -nmx_xnu_user_park(pin->handle);
}

int nmx_upin_restore(void *p, vm_page_t *pages, int npages)
{
    struct user_pin *pin = p;
    int err = EINVAL;
    if (npages != pin->npages)
        goto fail;
    uint64_t *phys = malloc(sizeof(uint64_t) * (size_t)npages, M_NMXPAGE, M_WAITOK);
    err = ENOMEM;
    if (phys == NULL)
        goto fail;
    uint64_t t0 = mach_absolute_time();
    err = nmx_xnu_user_rewire(pin->handle, (size_t)npages, phys);
    nmx_ft_stall(0, t0, (uint64_t)npages);
    if (err == 0)
        err = pin_attach(pin, phys, npages, pages);
    free(phys, M_NMXPAGE);
    if (err == 0)
        return 0;
fail:
    __atomic_add_fetch(&gNmxUpinRestoreFail, 1, __ATOMIC_RELAXED);
    return -err;
}

void nmx_upin_destroy(void *p)
{
    struct user_pin *pin = p;
    if (pin == NULL)
        return;
    NMX_IMP_T0(itd3);
    nmx_xnu_user_unwire(pin->handle);
    NMX_IMP_STAGE(NMX_IMP_D3_UNWIRE, itd3, pin->npages);
    free(pin, M_NMXPAGE);
}

/*
 * ---- VM objects: anonymous page containers (LinuxKPI shmem files, which TTM swaps buffers out to) ----------------------
 * A page array indexed by page number, filled on demand with zeroed pages. There is no pager behind it: the pages
 * stay in memory for the object's life. The object lock (VM_OBJECT_WLOCK) guards the array.
 */
vm_object_t vm_object_allocate(objtype_t type, vm_pindex_t size)
{
    struct vm_object *o = malloc(sizeof(*o), M_NMXPAGE, M_WAITOK | M_ZERO);
    if (o == NULL)
        return NULL;
    if (size > 0) {
        o->nmx_pages = malloc(sizeof(vm_page_t) * (size_t)size, M_NMXPAGE, M_WAITOK | M_ZERO);
        if (o->nmx_pages == NULL) {
            free(o, M_NMXPAGE);
            return NULL;
        }
    }
    mtx_init(&o->lock, "vm object", NULL, MTX_DEF);
    o->type = type;
    o->size = size;
    o->ref_count = 1;
    return o;
}

vm_object_t vm_pager_allocate(objtype_t type, void *handle, vm_ooffset_t size, vm_prot_t prot, vm_ooffset_t off,
                              struct ucred *cred)
{
    return vm_object_allocate(type, atop(round_page(size)));
}

void vm_object_reference(vm_object_t o)
{
    if (o != NULL)
        __atomic_fetch_add(&o->ref_count, 1, __ATOMIC_SEQ_CST);
}

/* detaches the page at i; frees it unless someone still holds a wiring (they free it at their last unwire) */
static void object_drop_page_locked(vm_object_t o, vm_pindex_t i)
{
    vm_page_t *pages = o->nmx_pages;
    vm_page_t m = pages[i];
    if (m == NULL)
        return;
    pages[i] = NULL;
    o->resident_page_count--;
    m->object = NULL;
    if (__atomic_load_n(&m->ref_count, __ATOMIC_SEQ_CST) == 0)
        vm_page_free(m);
}

void vm_object_deallocate(vm_object_t o)
{
    if (o == NULL || __atomic_sub_fetch(&o->ref_count, 1, __ATOMIC_SEQ_CST) != 0)
        return;
    VM_OBJECT_WLOCK(o);
    for (vm_pindex_t i = 0; i < o->size; i++)
        object_drop_page_locked(o, i);
    VM_OBJECT_WUNLOCK(o);
    mtx_destroy(&o->lock);
    free(o->nmx_pages, M_NMXPAGE);
    free(o, M_NMXPAGE);
}

vm_page_t vm_page_lookup(vm_object_t o, vm_pindex_t pindex)
{
    if (o == NULL || pindex >= o->size)
        return NULL;
    return ((vm_page_t *)o->nmx_pages)[pindex];
}

vm_page_t vm_page_grab(vm_object_t o, vm_pindex_t pindex, int allocflags)
{
    if (o == NULL || pindex >= o->size)
        return NULL;
    vm_page_t *pages = o->nmx_pages;
    vm_page_t m = pages[pindex];
    if (m == NULL) {
        if (allocflags & VM_ALLOC_NOCREAT)
            return NULL;
        m = vm_page_alloc_noobj(VM_ALLOC_ZERO);         /* comes back with one wiring */
        if (m == NULL)
            return NULL;
        m->object = o;
        m->pindex = pindex;
        pages[pindex] = m;
        o->resident_page_count++;
        if (!(allocflags & VM_ALLOC_WIRED))
            __atomic_fetch_sub(&m->ref_count, 1, __ATOMIC_SEQ_CST);  /* the object owns it unwired */
        return m;
    }
    if (allocflags & VM_ALLOC_WIRED)
        vm_page_wire(m);
    return m;
}

int vm_page_grab_valid(vm_page_t *mp, vm_object_t o, vm_pindex_t pindex, int allocflags)
{
    *mp = vm_page_grab(o, pindex, allocflags & ~VM_ALLOC_NOCREAT);
    return *mp ? VM_PAGER_OK : VM_PAGER_FAIL;
}

int vm_page_grab_valid_unlocked(vm_page_t *mp, vm_object_t o, vm_pindex_t pindex, int allocflags)
{
    VM_OBJECT_WLOCK(o);
    int r = vm_page_grab_valid(mp, o, pindex, allocflags);
    VM_OBJECT_WUNLOCK(o);
    return r;
}

/*
 * Removes pages [start, end) (end 0 = to the end). OBJPR_CLEANONLY asks for clean pages only; with no record of
 * which pages were written, every page is treated as dirty and kept, so no data is ever dropped.
 */
void vm_object_page_remove(vm_object_t o, vm_pindex_t start, vm_pindex_t end, int options)
{
    if (o == NULL || (options & OBJPR_CLEANONLY))
        return;
    if (end == 0 || end > o->size)
        end = o->size;
    for (vm_pindex_t i = start; i < end; i++)
        object_drop_page_locked(o, i);
}

/* ---- physical <-> page <-> kernel address ------------------------------------------------------------------------- */

vm_page_t nmx_phys_to_page(vm_paddr_t pa)
{
    vm_paddr_t pfn = pa >> PAGE_SHIFT;
    lck_spin_t *l = reg_lock_of(pfn);
    lck_spin_lock(l);
    struct vm_page *m = reg_find_locked(pfn);
    if (m != NULL && m->nmx_alloc != NULL && ((struct backing *)m->nmx_alloc)->kind == BK_USER) {
        /* reached without a reference: the caller may keep it, so it can no longer be freed (Step138) */
        idle_forget_locked(m);
        ((struct backing *)m->nmx_alloc)->kind = BK_FICTITIOUS;
        __atomic_sub_fetch(&gNmxUserRecLive, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gNmxUserRecKept, 1, __ATOMIC_RELAXED);
    }
    lck_spin_unlock(l);
    if (m)
        return m;

    /* not one of ours: a fictitious record (GPU BAR page, or memory reached through virt_to_page) */
    struct backing *bk = malloc(sizeof(*bk) + sizeof(struct vm_page), M_NMXPAGE, M_WAITOK | M_ZERO);
    struct reg_entry *e = malloc(sizeof(*e), M_NMXPAGE, M_WAITOK);
    if (bk == NULL || e == NULL) {
        free(bk, M_NMXPAGE);
        free(e, M_NMXPAGE);
        return NULL;
    }
    bk->kind = BK_FICTITIOUS;
    bk->npages = 1;
    page_record_init(&bk->pages[0], bk, pfn << PAGE_SHIFT, NULL);
    bk->pages[0].flags = PG_FICTITIOUS;
    bk->pages[0].ref_count = 1;
    lck_spin_lock(l);
    m = reg_find_locked(pfn);
    if (m == NULL) {
        reg_insert_locked(&bk->pages[0], e);
        m = &bk->pages[0];
        bk = NULL;
        e = NULL;
    }
    lck_spin_unlock(l);
    free(bk, M_NMXPAGE);
    free(e, M_NMXPAGE);
    return m;
}

/* the page record for a kernel address, remembering that address so page_address() returns it */
vm_page_t nmx_kva_to_page(const void *va)
{
    vm_paddr_t pa = nmx_xnu_kva_to_phys(va);
    if (pa == 0)
        return NULL;
    vm_page_t m = nmx_phys_to_page(pa);
    if (m && m->nmx_kva == NULL)
        m->nmx_kva = (void *)trunc_page((uintptr_t)va);
    return m;
}

bool nmx_phys_in_dmap(vm_paddr_t pa)
{
    lck_spin_t *l = reg_lock_of(pa >> PAGE_SHIFT);
    lck_spin_lock(l);
    struct vm_page *m = reg_find_locked(pa >> PAGE_SHIFT);
    bool in = m && m->nmx_kva;
    lck_spin_unlock(l);
    return in;
}

void *nmx_phys_to_kva(vm_paddr_t pa)
{
    lck_spin_t *l = reg_lock_of(pa >> PAGE_SHIFT);
    lck_spin_lock(l);
    struct vm_page *m = reg_find_locked(pa >> PAGE_SHIFT);
    void *kva = (m && m->nmx_kva) ? (char *)m->nmx_kva + (pa & PAGE_MASK) : NULL;
    lck_spin_unlock(l);
    return kva;
}

vm_paddr_t nmx_kva_to_phys(const void *va)
{
    return nmx_xnu_kva_to_phys(va);
}

vm_paddr_t pmap_kextract(vm_offset_t va)
{
    return nmx_xnu_kva_to_phys((const void *)va);
}

/* ---- kernel memory (kmem) and device mappings (pmap) -------------------------------------------------------------- */

void *kmem_alloc_contig(vm_size_t size, int flags, vm_paddr_t low, vm_paddr_t high, u_long alignment,
                        vm_paddr_t boundary, vm_memattr_t memattr)
{
    vm_page_t m = vm_page_alloc_noobj_contig(VM_ALLOC_WIRED | ((flags & M_ZERO) ? VM_ALLOC_ZERO : 0),
                                             atop(round_page(size)), low, high, alignment, boundary, memattr);
    return m ? m->nmx_kva : NULL;
}

void *kmem_alloc_attr(vm_size_t size, int flags, vm_paddr_t low, vm_paddr_t high, vm_memattr_t memattr)
{
    return kmem_alloc_contig(size, flags, low, high, PAGE_SIZE, 0, memattr);
}

void *kmem_malloc(vm_size_t size, int flags)
{
    return kmem_alloc_contig(size, flags, 0, ~(vm_paddr_t)0, PAGE_SIZE, 0, VM_MEMATTR_DEFAULT);
}

void kmem_free(void *addr, vm_size_t size)
{
    if (addr == NULL)
        return;
    vm_page_t m = nmx_phys_to_page(nmx_xnu_kva_to_phys(addr));
    if (m)
        vm_page_free_contig(m, atop(round_page(size)));
}

void *contigmalloc(unsigned long size, struct malloc_type *type, int flags, vm_paddr_t low, vm_paddr_t high,
                   unsigned long alignment, vm_paddr_t boundary)
{
    void *p = kmem_alloc_contig(size, flags, low, high, alignment, boundary, VM_MEMATTR_DEFAULT);
    /* recorded so free() takes it back as FreeBSD's does (LinuxKPI kfree of a kmalloc above a page) */
    if (p != NULL && nmx_malloc_note_contig(p, size, type) != 0) {
        kmem_free(p, size);
        return NULL;
    }
    return p;
}

void contigfree(void *addr, unsigned long size, struct malloc_type *type)
{
    if (addr == NULL)
        return;
    nmx_malloc_forget_contig(addr);
    kmem_free(addr, size);
}

/* device mappings, tracked by address so pmap_unmapdev can release them */
struct devmap { void *va; void *handle; struct devmap *next; };
static struct devmap *devmaps;
static struct mtx devmap_mtx;
MTX_SYSINIT(nmx_devmap, &devmap_mtx, "device mappings", MTX_DEF);

void *pmap_mapdev_attr(vm_paddr_t pa, vm_size_t size, int mode)
{
    struct devmap *d = malloc(sizeof(*d), M_NMXPAGE, M_WAITOK);
    if (d == NULL)
        return NULL;
    d->va = nmx_xnu_map_phys(pa, size, mode, &d->handle);
    if (d->va == NULL) {
        free(d, M_NMXPAGE);
        return NULL;
    }
    mtx_lock(&devmap_mtx);
    d->next = devmaps;
    devmaps = d;
    mtx_unlock(&devmap_mtx);
    return d->va;
}

void pmap_unmapdev(void *va, vm_size_t size)
{
    mtx_lock(&devmap_mtx);
    for (struct devmap **pp = &devmaps; *pp; pp = &(*pp)->next) {
        if ((*pp)->va == va) {
            struct devmap *d = *pp;
            *pp = d->next;
            mtx_unlock(&devmap_mtx);
            nmx_xnu_unmap(d->handle);
            free(d, M_NMXPAGE);
            return;
        }
    }
    mtx_unlock(&devmap_mtx);
}

/*
 * Cache attributes: a kext cannot change the cache type of an existing kernel mapping on macOS. The attribute is
 * recorded on the page and applied when the page is mapped (vmap, user mappings); system memory stays cached in
 * the kernel's own view, which is coherent for a discrete GPU because PCIe accesses to system RAM snoop the CPU
 * caches (an integrated NPU's need not: see pmap_page_set_memattr).
 */
int pmap_change_attr(void *va, vm_size_t size, int mode)
{
    return 0;
}

/*
 * A page leaving write-back is written out of the CPU caches first, as FreeBSD's pmap and Linux's set_pages_array_wc
 * do. "PCIe snoops" above holds for a discrete GPU, not for the NPU: an integrated NPU reads write-combined and
 * uncached buffers without snooping, so a line still in the cache (the page was zeroed through the cached view) is
 * invisible to it and can later be written back over what was written through the WC view. The first Arrow Lake NPU
 * firmware boot never answered, its firmware and log buffers being exactly such pages.
 */
void pmap_invalidate_cache_range(vm_offset_t sva, vm_offset_t eva);
void pmap_page_set_memattr(vm_page_t m, vm_memattr_t ma)
{
    if (ma != VM_MEMATTR_WRITE_BACK && m->md.pat_mode != ma && m->nmx_kva)
        pmap_invalidate_cache_range((vm_offset_t)m->nmx_kva, (vm_offset_t)m->nmx_kva + PAGE_SIZE);
    m->md.pat_mode = ma;
}

void pmap_page_set_memattr_noflush(vm_page_t m, vm_memattr_t ma)
{
    m->md.pat_mode = ma;
}

static void wbinvd_action(void *arg)
{
    wbinvd();
}

void pmap_invalidate_cache(void)
{
    extern void smp_rendezvous(void (*)(void *), void (*)(void *), void (*)(void *), void *);
    smp_rendezvous(NULL, wbinvd_action, NULL, NULL);
}

void pmap_invalidate_cache_range(vm_offset_t sva, vm_offset_t eva)
{
    for (vm_offset_t va = sva & ~(vm_offset_t)63; va < eva; va += 64)
        clflush(va);
    mfence();
}

void pmap_force_invalidate_cache_range(vm_offset_t sva, vm_offset_t eva)
{
    pmap_invalidate_cache_range(sva, eva);
}

void pmap_invalidate_cache_pages(vm_page_t *pages, int count)
{
    for (int i = 0; i < count; i++)
        if (pages[i]->nmx_kva)
            pmap_invalidate_cache_range((vm_offset_t)pages[i]->nmx_kva, (vm_offset_t)pages[i]->nmx_kva + PAGE_SIZE);
}

void pmap_copy_page(vm_page_t src, vm_page_t dst)
{
    memcpy(dst->nmx_kva, src->nmx_kva, PAGE_SIZE);
}

/* ga403wm Step84: TTM clears pool pages through the write-back kernel alias (nmx_kva) while the GPU may read them
 * unsnooped (WC/UC pages). Without a flush the zeros stay as dirty lines and are later written back over what RADV
 * or the GPU put in the page (IBs, descriptors). Linux has no such alias (set_pages_array_wc makes it WC too). */
void pmap_zero_page(vm_page_t m)
{
    bzero(m->nmx_kva, PAGE_SIZE);
    pmap_invalidate_cache_range((vm_offset_t)m->nmx_kva, (vm_offset_t)m->nmx_kva + PAGE_SIZE);
}

/* ---- accounting --------------------------------------------------------------------------------------------------- */

uint64_t nmx_physmem_pages(void)
{
    extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, NULL, 0) != 0)
        return 0;
    return bytes >> PAGE_SHIFT;
}

uint64_t nmx_free_pages(void)
{
    extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);
    uint32_t n = 0;
    size_t len = sizeof(n);
    if (sysctlbyname("vm.page_free_count", &n, &len, NULL, 0) != 0)
        return 0;
    return n;
}

uint64_t nmx_pages_live(void)
{
    return pages_live;
}

void nmx_page_fini(void)
{
    if (pg_lock == NULL)
        return;
    if (pages_live)
        printf("NullMoth xkpi: %llu page(s) still held at unload\n", (unsigned long long)pages_live);
    nmx_import_reuse_drain();
    /*
     * Step141b: stop new arms, then wait (bounded) until no sweep runs and no arm is in flight before freeing the call;
     * cancel_wait is not exported to kexts. thread_call_free fails while the call is still pending, so retry the cancel.
     */
    lck_spin_lock(pg_lock);
    sweep_stopping = 1;
    thread_call_t calls[2] = { release_call, fill_call };
    lck_spin_unlock(pg_lock);
    for (int k = 0; k < 2; k++) {
        thread_call_t call = calls[k];
        if (call == NULL)
            continue;
        int freed = 0;
        for (int i = 0; i < 500 && !freed; i++) {
            thread_call_cancel(call);
            lck_spin_lock(pg_lock);
            int busy = sweep_busy;
            lck_spin_unlock(pg_lock);
            if (busy == 0 && thread_call_free(call))
                freed = 1;
            else
                IOSleep(10);
        }
        if (!freed)
            printf("NullMoth xkpi: chunk sweep/fill call still busy at unload, call leaked\n");
    }
    lck_spin_lock(pg_lock);
    release_call = NULL;
    fill_call = NULL;
    lck_spin_unlock(pg_lock);
    release_worker(NULL, NULL);
    /* fully free chunks go back; a chunk with pages still held is leaked deliberately rather than freed under them */
    struct backing *bk = chunks;
    chunks = NULL;
    while (bk) {
        struct backing *n = bk->next;
        if (bk->live == 0)
            backing_release(bk);
        bk = n;
    }
    free(reg_table, M_NMXPAGE);
    reg_table = NULL;
    lck_spin_free(pg_lock, nmx_lck_grp);
    pg_lock = NULL;
    for (int i = 0; i < REG_LOCKS; i++)
        if (reg_locks[i] != NULL) {
            lck_spin_free(reg_locks[i], nmx_lck_grp);
            reg_locks[i] = NULL;
        }
}
