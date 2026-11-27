/* desc-cache: a per-command-buffer cache of descriptor sets that were already written, keyed by exactly what the
 * bound pipeline reads (its binding mask, the descriptors those bindings resolve to, and the storage-texture range).
 *
 * Why: every draw/dispatch whose bindings changed since the last set (any setBytes moves a ring offset, so in
 * WindowServer that is every draw) allocated a fresh set and ran vkUpdateDescriptorSets on it. Most of those draws
 * read the same descriptors as an earlier draw in the same command buffer: the only thing that really changed is the
 * address table (binding 640), and that table is a UNIFORM_BUFFER_DYNAMIC. So a set written earlier is reused, a
 * fresh table slot is written for this draw (in the same address page), and the set is bound with a dynamic offset
 * that points its binding 640 at the new slot: no allocation and no vkUpdateDescriptorSets.
 *
 * Correctness:
 *  - The key holds the *canonical* descriptor each read binding resolves to (what nvmtl_snapshot_set_md would write:
 *    the dummy for an empty or misaligned slot), so a hit set holds exactly the descriptors a rebuild would write for
 *    every binding the pipeline reads. Full keys are compared, not just hashes.
 *  - Sets and address pages live until the command buffer retires, and the cache lives in its bind table, which is
 *    freed at the same time, so an entry never outlives its set.
 *  - Raw handles can be reused once destroyed. Every vkDestroyImageView/Image/Sampler/BufferView bumps g_dc_epoch_img
 *    and every vkDestroyBuffer/vkFreeMemory bumps g_dc_epoch_buf (hooked once on the device's function pointers).
 *    An entry remembers the epochs of the handle classes its key contains and is not used once either moved, so a
 *    freed-and-reused handle never matches an old set. Keys made only of dummies or of the address table (the usual
 *    WindowServer case) depend on no handle and stay valid.
 *  - Off switches: NVMTL_NO_DESCCACHE=1 (environment or /Library/GPUBundles/nvmtl/nvrm610.conf, read at start), or
 *    live: touch /tmp/nvmtl-desccache-off (checked every ~2 s; remove it to turn the cache back on).
 *
 * This file is #included by nvmtl_vk.c right before nvmtl_snapshot_set_c (single translation unit). */

#define NVMTL_DC_CAP     256                       /* entries per command buffer, power of two */
#define NVMTL_DC_KEYMAX  192                       /* words; a longer key is simply not cached */
#define NVMTL_DC_ARENA   (NVMTL_DC_CAP * 24)       /* key words per command buffer */
enum { NVMTL_DC_DEP_IMG = 1, NVMTL_DC_DEP_BUF = 2 };
typedef struct { uint64_t h; VkDescriptorSet set; VkBuffer pbuf; uint32_t koff, klen, idx, hi, poff, ie, be;
                 uint8_t used, addr, dep; } nvmtl_dc_ent;
struct nvmtl_xdc_ent;
typedef struct { unsigned n, an; nvmtl_dc_ent e[NVMTL_DC_CAP]; uint64_t arena[NVMTL_DC_ARENA];
                 struct nvmtl_xdc_ent **pins; unsigned npin, cappin;   /* device-level entries this command buffer uses */
               } nvmtl_dcache;

static uint32_t g_dc_epoch_img = 1, g_dc_epoch_buf = 1;
static uint64_t g_dc_hit, g_dc_miss, g_dc_stale, g_dc_page, g_dc_long, g_dc_full, g_dc_ins;
static int g_dc_env = -1, g_dc_live = 1;
static uint32_t g_dc_tick; static uint64_t g_dc_next_check, g_dc_next_log;

static PFN_vkDestroyImageView  g_dc_real_dv;  static PFN_vkDestroyImage  g_dc_real_di;
static PFN_vkDestroySampler    g_dc_real_ds;  static PFN_vkDestroyBufferView g_dc_real_dbv;
static PFN_vkDestroyBuffer     g_dc_real_db;  static PFN_vkFreeMemory    g_dc_real_fm;
#define NVMTL_DC_BUMP(e) __atomic_add_fetch(&(e), 1, __ATOMIC_SEQ_CST)
/* which destroy moved the image epoch (Step101b trace): image view, image, sampler, buffer view */
static uint64_t g_dc_by_dv, g_dc_by_di, g_dc_by_ds, g_dc_by_dbv;
#define NVMTL_DC_BY(c) __atomic_add_fetch(&(c), 1, __ATOMIC_RELAXED)
/* Per-handle epochs for the device-level cache (WS-CPU 8.9): a destroy of a handle that can sit in a key (image view,
 * sampler, buffer view, buffer) bumps the bucket its value hashes to, before the handle is released for reuse. An entry
 * remembers the sum of its handles' bucket epochs; the sum only grows, so any destroy of one of its handles (or of a
 * handle sharing a bucket) makes it stale. Image and memory frees do not count: their views and buffers stay live
 * handles until destroyed themselves, so the raw value in a key cannot be reused before its own destroy bumps it. */
#define NVMTL_HEP_N 4096
static uint32_t g_hep[NVMTL_HEP_N];
static inline unsigned nvmtl_hep_bucket(const void *h)
{ uint64_t x = (uint64_t)(uintptr_t)h; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 32; return (unsigned)x & (NVMTL_HEP_N - 1); }
static inline void nvmtl_hep_bump(const void *h) { __atomic_add_fetch(&g_hep[nvmtl_hep_bucket(h)], 1, __ATOMIC_SEQ_CST); }
static inline uint64_t nvmtl_hep_get(const void *h) { return __atomic_load_n(&g_hep[nvmtl_hep_bucket(h)], __ATOMIC_ACQUIRE); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_dv(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{ if (v) { NVMTL_DC_BUMP(g_dc_epoch_img); NVMTL_DC_BY(g_dc_by_dv); nvmtl_hep_bump(v); } g_dc_real_dv(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_di(VkDevice d, VkImage v, const VkAllocationCallbacks *a)
{ if (v) { NVMTL_DC_BUMP(g_dc_epoch_img); NVMTL_DC_BY(g_dc_by_di); } g_dc_real_di(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_ds(VkDevice d, VkSampler v, const VkAllocationCallbacks *a)
{ if (v) { NVMTL_DC_BUMP(g_dc_epoch_img); NVMTL_DC_BY(g_dc_by_ds); nvmtl_hep_bump(v); } g_dc_real_ds(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_dbv(VkDevice d, VkBufferView v, const VkAllocationCallbacks *a)
{ if (v) { NVMTL_DC_BUMP(g_dc_epoch_img); NVMTL_DC_BY(g_dc_by_dbv); nvmtl_hep_bump(v); } g_dc_real_dbv(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_db(VkDevice d, VkBuffer v, const VkAllocationCallbacks *a)
{ if (v) { NVMTL_DC_BUMP(g_dc_epoch_buf); nvmtl_hep_bump(v); } g_dc_real_db(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_fm(VkDevice d, VkDeviceMemory v, const VkAllocationCallbacks *a)
{ if (v) NVMTL_DC_BUMP(g_dc_epoch_buf); g_dc_real_fm(d, v, a); }
/* Called once after the device's function pointers are loaded (and after nvmtl_bl_hook_destroy, which it chains). */
void nvmtl_dc_hook_destroy(void)
{
    if (pvkDestroyImageView && pvkDestroyImageView != nvmtl_dc_dv) { g_dc_real_dv = pvkDestroyImageView; pvkDestroyImageView = nvmtl_dc_dv; }
    if (pvkDestroyImage && pvkDestroyImage != nvmtl_dc_di) { g_dc_real_di = pvkDestroyImage; pvkDestroyImage = nvmtl_dc_di; }
    if (pvkDestroySampler && pvkDestroySampler != nvmtl_dc_ds) { g_dc_real_ds = pvkDestroySampler; pvkDestroySampler = nvmtl_dc_ds; }
    if (pvkDestroyBufferView && pvkDestroyBufferView != nvmtl_dc_dbv) { g_dc_real_dbv = pvkDestroyBufferView; pvkDestroyBufferView = nvmtl_dc_dbv; }
    if (pvkDestroyBuffer && pvkDestroyBuffer != nvmtl_dc_db) { g_dc_real_db = pvkDestroyBuffer; pvkDestroyBuffer = nvmtl_dc_db; }
    if (pvkFreeMemory && pvkFreeMemory != nvmtl_dc_fm) { g_dc_real_fm = pvkFreeMemory; pvkFreeMemory = nvmtl_dc_fm; }
}

static void nvmtl_dc_stats(const char *why)
{
    nvlog("desc-cache %s: hit %llu miss %llu (stale %llu, page %llu, long %llu, full %llu) inserted %llu, epochs img %u buf %u"
          " (img by view %llu image %llu sampler %llu bufview %llu)",
          why, (unsigned long long)g_dc_hit, (unsigned long long)g_dc_miss, (unsigned long long)g_dc_stale,
          (unsigned long long)g_dc_page, (unsigned long long)g_dc_long, (unsigned long long)g_dc_full,
          (unsigned long long)g_dc_ins, __atomic_load_n(&g_dc_epoch_img, __ATOMIC_RELAXED),
          __atomic_load_n(&g_dc_epoch_buf, __ATOMIC_RELAXED),
          (unsigned long long)g_dc_by_dv, (unsigned long long)g_dc_by_di, (unsigned long long)g_dc_by_ds, (unsigned long long)g_dc_by_dbv);
}
static int nvmtl_dc_on(void)
{
    if (g_dc_env < 0) {
        const char *e = getenv("NVMTL_NO_DESCCACHE");
        g_dc_env = !(e && *e && *e != '0');
        nvlog("vk: desc-cache %s (NVMTL_NO_DESCCACHE=%s; touch /tmp/nvmtl-desccache-off turns it off live)",
              g_dc_env ? "ON - a set already written for the same descriptors is reused, binding 640 moved by its dynamic offset"
                       : "OFF", e ? e : "unset");
    }
    if (!g_dc_env) return 0;
    if (!(__atomic_add_fetch(&g_dc_tick, 1, __ATOMIC_RELAXED) & 511)) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        if (now >= g_dc_next_check) {
            g_dc_next_check = now + 2000000000ull;
            const int live = access("/tmp/nvmtl-desccache-off", F_OK) != 0;
            if (live != g_dc_live) { g_dc_live = live; nvmtl_dc_stats(live ? "turned back ON (flag file gone)" : "turned OFF live (/tmp/nvmtl-desccache-off)"); }
        }
        if (now >= g_dc_next_log) { if (g_dc_next_log) nvmtl_dc_stats("stats"); g_dc_next_log = now + 60000000000ull; }
    }
    return g_dc_live;
}

static inline uint64_t nvmtl_dc_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
/* The key: what nvmtl_snapshot_set_md would write for each binding in pm (sampled/sampler/buffer bindings below
 * NVMTL_STEX_BASE) and for storage textures [0, hi). In UBO mode binding 640 adds nothing: a hit writes a fresh
 * table slot. In SSBO mode the table content itself is keyed. Returns the length, 0 when it does not fit.
 * Only bindings that write a real descriptor are in the key, as (binding, value) pairs: an empty (or misaligned)
 * binding always gets the same dummy, and pm and hi are in the header, so leaving those out keeps the key exact.
 * (d5da629 spent 1-2 words on every binding of the layout; CoreAnimation's pipelines declare ~200, so 163260 of
 * 175475 lookups on the Step100 boot overflowed NVMTL_DC_KEYMAX and were never cached.) */
static unsigned nvmtl_dc_key(const nvmtl_bindtable *t, uint32_t which, const uint64_t pm[4], uint32_t hi, int ubo,
                             uint64_t *k, uint8_t *dep, uint64_t *sig)
{
    unsigned n = 0; *dep = 0; *sig = 0;
    k[n++] = (uint64_t)which | (uint64_t)hi << 8 | (uint64_t)(ubo != 0) << 40;
    for (int w = 0; w < 4; w++) k[n++] = pm[w];
    for (int w = 0; w < 4; w++) for (uint64_t b = pm[w]; b; b &= b - 1) {
        const uint32_t i = (uint32_t)(w * 64 + __builtin_ctzll(b));
        if (i >= NVMTL_STEX_BASE) continue;
        const nvmtl_slot *sl = &t->s[which][i];
        if (i == NVMTL_ADDRESS_BINDING) {
            if (ubo) continue;
            {   /* the table content, sparse: only the used entries, then a terminator */
                if (n + 2 > NVMTL_DC_KEYMAX) return 0;
                k[n++] = (uint64_t)i << 2 | 3;
                for (unsigned j = 0; j < NVMTL_NBUF; j++) { const nvmtl_slot *s = &t->s[which][j];
                    if (s->kind != NVMTL_SLOT_BUF) continue;
                    if (n + 2 > NVMTL_DC_KEYMAX) return 0;
                    k[n++] = j; k[n++] = s->address + s->off; *dep |= NVMTL_DC_DEP_BUF; }
                for (unsigned j = 0; j < NVMTL_NTEX; j++) { const nvmtl_slot *s = &t->s[which][NVMTL_NBUF + j];
                    if (!(s->kind == NVMTL_SLOT_TEX && s->h)) continue;
                    if (n + 2 > NVMTL_DC_KEYMAX) return 0;
                    k[n++] = NVMTL_NBUF + j; k[n++] = (uint64_t)s->size; }
                if (n + 1 > NVMTL_DC_KEYMAX) return 0;
                k[n++] = ~0ull;
            }
            continue;
        }
        const VkDescriptorType ty = binding_type(i);
        if (ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            if (sl->kind == NVMTL_SLOT_BUF && (sl->off % NVMTL_SSBO_ALIGN) == 0) {
                if (n + 3 > NVMTL_DC_KEYMAX) return 0;
                k[n++] = (uint64_t)i << 2 | 1; k[n++] = (uint64_t)(uintptr_t)sl->h; k[n++] = (uint64_t)sl->off;
                *dep |= NVMTL_DC_DEP_BUF; *sig += nvmtl_hep_get(sl->h);
            }
        } else if (ty == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            if (sl->kind == NVMTL_SLOT_TEX) {
                if (n + 2 > NVMTL_DC_KEYMAX) return 0;
                k[n++] = (uint64_t)i << 2 | 2; k[n++] = (uint64_t)(uintptr_t)sl->h; *dep |= NVMTL_DC_DEP_IMG; *sig += nvmtl_hep_get(sl->h);
            }
        } else {
            if (sl->kind == NVMTL_SLOT_SAMP) {
                if (n + 2 > NVMTL_DC_KEYMAX) return 0;
                k[n++] = (uint64_t)i << 2 | 2; k[n++] = (uint64_t)(uintptr_t)sl->h; *dep |= NVMTL_DC_DEP_IMG; *sig += nvmtl_hep_get(sl->h);
            }
        }
    }
    for (uint32_t j = 0; j < hi; j++) {
        const nvmtl_slot *sl = &t->s[which][NVMTL_STEX_BASE + j];
        if (!(sl->kind == NVMTL_SLOT_TEX && sl->h)) continue;
        if (n + 2 > NVMTL_DC_KEYMAX) return 0;
        k[n++] = (uint64_t)(NVMTL_STEX_BASE + j) << 2 | 2; k[n++] = (uint64_t)(uintptr_t)sl->h; *dep |= NVMTL_DC_DEP_IMG;
        *sig += nvmtl_hep_get(sl->h);
    }
    return n;
}
static uint64_t nvmtl_dc_hash(const uint64_t *k, unsigned n)
{ uint64_t h = 0x84222325cbf29ce4ull; for (unsigned i = 0; i < n; i++) h = nvmtl_dc_mix(h, k[i]); return h ? h : 1; }

/* Finds the entry for key k (returns it, or NULL); *slot gets the bucket to insert into (or ~0u when full). */
static nvmtl_dc_ent *nvmtl_dc_find(nvmtl_dcache *dc, uint64_t h, const uint64_t *k, unsigned n, unsigned *slot)
{
    unsigned b = (unsigned)h & (NVMTL_DC_CAP - 1);
    for (unsigned probe = 0; probe < NVMTL_DC_CAP; probe++, b = (b + 1) & (NVMTL_DC_CAP - 1)) {
        nvmtl_dc_ent *e = &dc->e[b];
        if (!e->used) { *slot = b; return NULL; }
        if (e->h == h && e->klen == n && !memcmp(dc->arena + e->koff, k, (size_t)n * sizeof *k)) { *slot = b; return e; }
    }
    *slot = ~0u; return NULL;
}
static int nvmtl_dc_last_ok(const nvmtl_bindtable *t, uint32_t which)
{
    return !((t->last_dep[which] & NVMTL_DC_DEP_IMG) && t->last_ie[which] != __atomic_load_n(&g_dc_epoch_img, __ATOMIC_ACQUIRE)) &&
           !((t->last_dep[which] & NVMTL_DC_DEP_BUF) && t->last_be[which] != __atomic_load_n(&g_dc_epoch_buf, __ATOMIC_ACQUIRE));
}
static int nvmtl_dc_stale(const nvmtl_dc_ent *e)
{
    return ((e->dep & NVMTL_DC_DEP_IMG) && e->ie != __atomic_load_n(&g_dc_epoch_img, __ATOMIC_ACQUIRE)) ||
           ((e->dep & NVMTL_DC_DEP_BUF) && e->be != __atomic_load_n(&g_dc_epoch_buf, __ATOMIC_ACQUIRE));
}
/* Miss classes (WS-CPU 8.9): a plain miss is the first use of a key in this command buffer that the device-level cache
 * (below) did not have either. The seen table remembers keys written before (hash, handle signature, global epochs):
 *   new   - never written before (or fell out of the table): a new combination of descriptors,
 *   again - written before and none of its handles destroyed since: a set that could have been reused,
 *   moved - written before, but one of its handles (or a bucket neighbour) was destroyed since: really new.
 * "again" also says how many of those the old global epochs would have thrown away (img/buf moved).
 * Diagnostic only: it decides nothing. */
#define NVMTL_DC_SEEN 8192
/* Off unless NVMTL_DC_CLASSIFY=1 or the device-level cache is on: the seen table (192 KB) is not touched otherwise
   (review of 921113d). The census then prints zeros for the classes. */
static int nvmtl_xdc_on(void);
static int nvmtl_dc_classify_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_DC_CLASSIFY"); on = (e && *e == '1') || nvmtl_xdc_on(); }
    return on;
}
typedef struct { uint64_t h, sig; uint32_t ie, be; } nvmtl_dc_seen_ent;
static nvmtl_dc_seen_ent g_dc_seen[NVMTL_DC_SEEN];
static uint64_t g_dc_c_new, g_dc_c_again, g_dc_c_moved, g_dc_c_again_gimg, g_dc_c_again_gbuf;
static void nvmtl_dc_seen_classify(uint64_t h, uint64_t sig, uint8_t dep)
{
    const nvmtl_dc_seen_ent e = g_dc_seen[h & (NVMTL_DC_SEEN - 1)];   /* racy copy: a torn entry only misclassifies one miss */
    if (e.h != h) { __atomic_add_fetch(&g_dc_c_new, 1, __ATOMIC_RELAXED); return; }
    if (e.sig != sig) { __atomic_add_fetch(&g_dc_c_moved, 1, __ATOMIC_RELAXED); return; }
    __atomic_add_fetch(&g_dc_c_again, 1, __ATOMIC_RELAXED);
    if ((dep & NVMTL_DC_DEP_IMG) && e.ie != __atomic_load_n(&g_dc_epoch_img, __ATOMIC_RELAXED)) __atomic_add_fetch(&g_dc_c_again_gimg, 1, __ATOMIC_RELAXED);
    else if ((dep & NVMTL_DC_DEP_BUF) && e.be != __atomic_load_n(&g_dc_epoch_buf, __ATOMIC_RELAXED)) __atomic_add_fetch(&g_dc_c_again_gbuf, 1, __ATOMIC_RELAXED);
}
static void nvmtl_dc_seen_note(uint64_t h, uint64_t sig, uint32_t ie, uint32_t be)
{
    nvmtl_dc_seen_ent *e = &g_dc_seen[h & (NVMTL_DC_SEEN - 1)];
    e->h = h; e->sig = sig; e->ie = ie; e->be = be;
}

/* A lookup in flight between the key and the insert after a miss. */
typedef struct { nvmtl_dcache *dc; uint64_t h, sig; unsigned n, slot; uint8_t dep, plain; uint32_t ie, be; uint64_t k[NVMTL_DC_KEYMAX]; } nvmtl_dc_probe;

/* Returns a reusable set (and its dynamic offset for binding 640 in *dyn), or VK_NULL_HANDLE with pr filled in for
 * nvmtl_dc_insert. The caller has already updated nothing. */
static VkDescriptorSet nvmtl_dc_lookup(nvk_cmdbuf *c, nvmtl_bindtable *t, uint32_t which, const uint64_t pm[4], uint32_t hi,
                                       int on, uint32_t *dyn, nvmtl_dc_probe *pr)
{
    /* Epochs are read before the key: a destroy racing the key build makes the new entry stale, never a stale one fresh.
       Without a key the set's handle classes are unknown, so it depends on both. */
    pr->dc = NULL; pr->n = 0; pr->plain = 0; pr->sig = 0; pr->dep = NVMTL_DC_DEP_IMG | NVMTL_DC_DEP_BUF;
    pr->ie = __atomic_load_n(&g_dc_epoch_img, __ATOMIC_ACQUIRE); pr->be = __atomic_load_n(&g_dc_epoch_buf, __ATOMIC_ACQUIRE);
    if (!on) return VK_NULL_HANDLE;
    const int ubo = nvmtl_table_ubo();
    pr->n = nvmtl_dc_key(t, which, pm, hi, ubo, pr->k, &pr->dep, &pr->sig);
    if (!pr->n) {
        pr->dep = NVMTL_DC_DEP_IMG | NVMTL_DC_DEP_BUF; __atomic_add_fetch(&g_dc_long, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
    nvmtl_dcache *dc = (nvmtl_dcache *)t->dc;
    if (!dc) {
        dc = (nvmtl_dcache *)malloc(sizeof *dc);
        if (!dc) return VK_NULL_HANDLE;
        dc->n = dc->an = 0; memset(dc->e, 0, sizeof dc->e); dc->pins = NULL; dc->npin = dc->cappin = 0; t->dc = dc;
    }
    pr->dc = dc; pr->h = nvmtl_dc_hash(pr->k, pr->n);
    nvmtl_dc_ent *e = nvmtl_dc_find(dc, pr->h, pr->k, pr->n, &pr->slot);
    if (!e) { __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); pr->plain = 1; return VK_NULL_HANDLE; }
    if (nvmtl_dc_stale(e) || e->idx >= c->nsets || (VkDescriptorSet)c->sets[e->idx].set != e->set) {
        __atomic_add_fetch(&g_dc_stale, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
    uint32_t d = 0;
    if (e->addr && ubo) {
        /* The set's binding 640 points at its own table slot. The next slot must land in the same page, after it. */
        const unsigned at = c->address_count % NVMTL_TABLES_PER_PAGE;
        const nvmtl_address_page *pg = (const nvmtl_address_page *)c->address_pages;
        if (at == 0 || !pg || (VkBuffer)pg->buffer.buf != e->pbuf || at * NVMTL_TABLE_BYTES < e->poff) {
            __atomic_add_fetch(&g_dc_page, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
        VkDescriptorBufferInfo ai;
        if (nvmtl_address_snapshot(c, which, &ai) || (VkBuffer)ai.buffer != e->pbuf || ai.offset < e->poff) {   /* the fresh slot is always past the set's own one */
            __atomic_add_fetch(&g_dc_page, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
        d = (uint32_t)(ai.offset - e->poff);
        const VkDeviceSize al = g_lim.minUniformBufferOffsetAlignment ? g_lim.minUniformBufferOffsetAlignment : 1;
        if (d % al) { __atomic_add_fetch(&g_dc_page, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
    }
    t->last[which] = e->set; t->last_idx[which] = e->idx; t->last_hi[which] = e->hi; t->dirty[which] = 0;
    memset(t->chg[which], 0, sizeof t->chg[which]);
    memcpy(t->lastw[which], pm, sizeof t->lastw[which]);
    t->dyn[which] = d; *dyn = d;
    t->last_dep[which] = e->dep; t->last_ie[which] = pr->ie; t->last_be[which] = pr->be;
    __atomic_add_fetch(&g_dc_hit, 1, __ATOMIC_RELAXED);
    return e->set;
}
/* After a miss: remember set s (just written, at c->sets[c->nsets - 1]) under the probe's key. */
static void nvmtl_dc_insert(nvk_cmdbuf *c, const nvmtl_dc_probe *pr, VkDescriptorSet s, uint32_t hi, int addr, int base0)
{
    nvmtl_dcache *dc = pr->dc;
    if (!dc || !s || !c->nsets || (VkDescriptorSet)c->sets[c->nsets - 1].set != s) return;
    unsigned slot = pr->slot;
    if (slot == ~0u || (!dc->e[slot].used && dc->n >= NVMTL_DC_CAP * 3 / 4)) { __atomic_add_fetch(&g_dc_full, 1, __ATOMIC_RELAXED); return; }
    nvmtl_dc_ent *e = &dc->e[slot];
    if (!e->used) {
        if (dc->an + pr->n > NVMTL_DC_ARENA) { __atomic_add_fetch(&g_dc_full, 1, __ATOMIC_RELAXED); return; }
        e->koff = dc->an; dc->an += pr->n; memcpy(dc->arena + e->koff, pr->k, (size_t)pr->n * sizeof pr->k[0]);
        e->h = pr->h; e->klen = pr->n; e->used = 1; dc->n++;
    }   /* else: the same key, stale: refresh it in place */
    e->set = s; e->idx = c->nsets - 1; e->hi = hi; e->dep = pr->dep; e->ie = pr->ie; e->be = pr->be;
    e->addr = (uint8_t)(addr != 0); e->pbuf = VK_NULL_HANDLE; e->poff = 0;
    if (e->addr) {
        const nvmtl_address_page *pg = (const nvmtl_address_page *)c->address_pages;
        if (!pg || !c->address_count) { e->set = VK_NULL_HANDLE; e->idx = ~0u; return; }   /* cannot happen (md wrote a slot); never cache blind */
        e->pbuf = (VkBuffer)pg->buffer.buf; e->poff = base0 ? 0 : (uint32_t)(((c->address_count - 1) % NVMTL_TABLES_PER_PAGE) * NVMTL_TABLE_BYTES);
    }
    __atomic_add_fetch(&g_dc_ins, 1, __ATOMIC_RELAXED);
}

/* ---- Device-level desc-cache (WS-CPU 8.9) -------------------------------------------------------------------------
 * Why: the cache above lives in one command buffer, so every command buffer starts cold. In WindowServer (and in a
 * CARenderer test in our own process) every plain miss was a key an earlier command buffer had already written and
 * that was still valid: 5-7.5 misses per present = 5-7.5 vkAllocateDescriptorSets + vkUpdateDescriptorSets per
 * present that a set kept across frames avoids. A Linux compositor on RADV keeps its sets across frames the same way
 * (wlroots' Vulkan renderer writes one set per texture when the texture is made and only binds it per frame).
 *
 * How:
 *  - Entries own their set (taken out of the command buffer's list, whose retire then skips it) and are keyed by the
 *    same key plus, for sets that read binding 640, the address page buffer: those sets are written with binding 640
 *    at base offset 0 (nvmtl_snapshot_set_c), so the dynamic offset picks any slot of that page. Pages are recycled
 *    LIFO between command buffers, so a few pages carry the steady state.
 *  - Validity: the sum of the per-handle bucket epochs (g_hep) of the key's handles and of the page buffer, not the
 *    global epochs: a VkBuffer or view destroyed elsewhere no longer throws every entry away.
 *  - Lifetime: every command buffer that uses an entry pins it (ref) until it retires; only an unpinned entry is
 *    evicted (its set freed). The table is a fixed array with chained buckets under one mutex.
 *  - Default off; NVMTL_XDESCCACHE=1 (environment or nvrm610.conf, read at start) turns it on. */
#define NVMTL_XDC_CAP 1024
#define NVMTL_XDC_NB  2048
typedef struct nvmtl_xdc_ent { uint64_t h, sig; uint64_t *k; uint32_t klen, hi, ref, tick; int next;
                               VkDescriptorSet set; VkDescriptorPool pool; VkBuffer pbuf; uint8_t used, dep; } nvmtl_xdc_ent;
static nvmtl_xdc_ent g_xdc[NVMTL_XDC_CAP]; static int g_xdc_head[NVMTL_XDC_NB]; static int g_xdc_free = -2;
static unsigned g_xdc_n, g_xdc_hand; static uint32_t g_xdc_tick;
static pthread_mutex_t g_xdc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_xdc_hit, g_xdc_miss, g_xdc_ins, g_xdc_evict, g_xdc_stale, g_xdc_full;
static int nvmtl_xdc_on(void)
{
    static int on = -1;
    /* Default OFF (main, 2026-11-22): ~7 us per present does not pay for a new cross-command-buffer lifetime in
       WindowServer. NVMTL_XDESCCACHE=1 turns it on. */
    if (on < 0) { const char *e = getenv("NVMTL_XDESCCACHE"); on = e && *e == '1';
        nvlog("vk: device-level desc-cache %s (NVMTL_XDESCCACHE=%s)", on ? "ON - descriptor sets are kept across command buffers" : "OFF (default)", e ? e : "unset"); }
    return on && nvmtl_table_ubo();
}
static void nvmtl_xdc_init_locked(void)
{
    if (g_xdc_free != -2) return;
    for (int i = 0; i < NVMTL_XDC_NB; i++) g_xdc_head[i] = -1;
    for (int i = 0; i < NVMTL_XDC_CAP; i++) g_xdc[i].next = i + 1 < NVMTL_XDC_CAP ? i + 1 : -1;
    g_xdc_free = 0;
}
static inline uint64_t nvmtl_xdc_hash(uint64_t h, VkBuffer pbuf) { return nvmtl_dc_mix(h, (uint64_t)(uintptr_t)pbuf); }
static int nvmtl_xdc_pin(nvmtl_dcache *dc, nvmtl_xdc_ent *e)
{
    if (dc->npin == dc->cappin) {
        const unsigned cap = dc->cappin ? dc->cappin * 2 : 32;
        nvmtl_xdc_ent **g = (nvmtl_xdc_ent **)realloc(dc->pins, cap * sizeof *g);
        if (!g) return -1;
        dc->pins = g; dc->cappin = cap;
    }
    dc->pins[dc->npin++] = e; e->ref++;
    return 0;
}
/* Unlinks and frees an unpinned entry (its set goes back to its pool). Called with g_xdc_lock held. */
static void nvmtl_xdc_drop_locked(nvmtl_xdc_ent *e)
{
    const int i = (int)(e - g_xdc);
    int *pp = &g_xdc_head[e->h & (NVMTL_XDC_NB - 1)];
    while (*pp != -1 && *pp != i) pp = &g_xdc[*pp].next;
    if (*pp == i) *pp = e->next;
    pthread_mutex_lock(&g_desc_lock);
    pvkFreeDescriptorSets(g_dev, e->pool, 1, &e->set);
    pthread_mutex_unlock(&g_desc_lock);
    free(e->k); e->k = NULL; e->used = 0; e->set = VK_NULL_HANDLE;
    e->next = g_xdc_free; g_xdc_free = i; g_xdc_n--;
}
/* Looks the probe's key up for the page buffer pbuf (VK_NULL_HANDLE when the pipeline does not read binding 640).
   A hit is pinned to this command buffer. */
static VkDescriptorSet nvmtl_xdc_lookup(nvmtl_dc_probe *pr, VkBuffer pbuf, uint64_t sig, uint32_t *hi)
{
    if (!pr->dc || !pr->n) return VK_NULL_HANDLE;
    const uint64_t h = nvmtl_xdc_hash(pr->h, pbuf);
    VkDescriptorSet s = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_xdc_lock);
    nvmtl_xdc_init_locked();
    for (int i = g_xdc_head[h & (NVMTL_XDC_NB - 1)], nx; i != -1; i = nx) {
        nvmtl_xdc_ent *e = &g_xdc[i]; nx = e->next;
        if (e->h != h || e->pbuf != pbuf || e->klen != pr->n || memcmp(e->k, pr->k, (size_t)pr->n * sizeof *pr->k)) continue;
        if (e->sig != sig) {   /* one of its handles was destroyed since: drop it now if nobody holds it, look on */
            g_xdc_stale++;
            if (!e->ref) nvmtl_xdc_drop_locked(e);
            continue;
        }
        if (nvmtl_xdc_pin(pr->dc, e)) break;
        e->tick = ++g_xdc_tick; s = e->set; *hi = e->hi; g_xdc_hit++;
        break;
    }
    if (!s) g_xdc_miss++;
    pthread_mutex_unlock(&g_xdc_lock);
    return s;
}
/* After a miss: hand set s (written for this probe, at c->sets[c->nsets - 1]) to the device-level cache. */
static void nvmtl_xdc_insert(nvk_cmdbuf *c, const nvmtl_dc_probe *pr, VkDescriptorSet s, VkBuffer pbuf, uint64_t sig, uint32_t hi)
{
    if (!pr->dc || !pr->n || !c->nsets || (VkDescriptorSet)c->sets[c->nsets - 1].set != s || !c->sets[c->nsets - 1].pool) return;
    uint64_t *k = (uint64_t *)malloc((size_t)pr->n * sizeof *k);
    if (!k) return;
    memcpy(k, pr->k, (size_t)pr->n * sizeof *k);
    const uint64_t h = nvmtl_xdc_hash(pr->h, pbuf);
    pthread_mutex_lock(&g_xdc_lock);
    nvmtl_xdc_init_locked();
    if (g_xdc_free == -1) {   /* full: a clock sweep evicts the first unpinned entry not used in the last 256 lookups */
        for (unsigned step = 0; step < 2 * NVMTL_XDC_CAP && g_xdc_free == -1; step++) {
            nvmtl_xdc_ent *e = &g_xdc[g_xdc_hand]; g_xdc_hand = (g_xdc_hand + 1) % NVMTL_XDC_CAP;
            if (e->used && !e->ref && (step >= NVMTL_XDC_CAP || g_xdc_tick - e->tick > 256)) { nvmtl_xdc_drop_locked(e); g_xdc_evict++; }
        }
    }
    if (g_xdc_free == -1 || nvmtl_xdc_pin(pr->dc, &g_xdc[g_xdc_free])) { g_xdc_full++; pthread_mutex_unlock(&g_xdc_lock); free(k); return; }
    const int i = g_xdc_free; nvmtl_xdc_ent *e = &g_xdc[i]; g_xdc_free = e->next;
    e->h = h; e->sig = sig; e->k = k; e->klen = pr->n; e->hi = hi; e->tick = ++g_xdc_tick; e->set = s;
    e->pool = (VkDescriptorPool)c->sets[c->nsets - 1].pool; e->pbuf = pbuf; e->dep = pr->dep; e->used = 1;
    e->next = g_xdc_head[h & (NVMTL_XDC_NB - 1)]; g_xdc_head[h & (NVMTL_XDC_NB - 1)] = i; g_xdc_n++; g_xdc_ins++;
    c->sets[c->nsets - 1].pool = NULL;   /* the entry owns the set now: the command buffer's retire skips it */
    pthread_mutex_unlock(&g_xdc_lock);
}
/* At command buffer retire, before its desc-cache is freed. */
static void nvmtl_xdc_unpin(nvmtl_dcache *dc)
{
    if (!dc || !dc->npin) { if (dc) { free(dc->pins); dc->pins = NULL; dc->cappin = 0; } return; }
    pthread_mutex_lock(&g_xdc_lock);
    for (unsigned i = 0; i < dc->npin; i++) if (dc->pins[i]->ref) dc->pins[i]->ref--;
    pthread_mutex_unlock(&g_xdc_lock);
    free(dc->pins); dc->pins = NULL; dc->npin = dc->cappin = 0;
}
