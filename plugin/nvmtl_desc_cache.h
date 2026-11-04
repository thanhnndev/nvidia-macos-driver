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

#define NVMTL_DC_CAP     128                       /* entries per command buffer, power of two */
#define NVMTL_DC_KEYMAX  192                       /* words; a longer key is simply not cached */
#define NVMTL_DC_ARENA   (NVMTL_DC_CAP * 16)       /* key words per command buffer */
enum { NVMTL_DC_DEP_IMG = 1, NVMTL_DC_DEP_BUF = 2 };
typedef struct { uint64_t h; VkDescriptorSet set; VkBuffer pbuf; uint32_t koff, klen, idx, hi, poff, ie, be;
                 uint8_t used, addr, dep; } nvmtl_dc_ent;
typedef struct { unsigned n, an; nvmtl_dc_ent e[NVMTL_DC_CAP]; uint64_t arena[NVMTL_DC_ARENA]; } nvmtl_dcache;

static uint32_t g_dc_epoch_img = 1, g_dc_epoch_buf = 1;
static uint64_t g_dc_hit, g_dc_miss, g_dc_stale, g_dc_page, g_dc_long, g_dc_full, g_dc_ins;
static int g_dc_env = -1, g_dc_live = 1;
static uint32_t g_dc_tick; static uint64_t g_dc_next_check, g_dc_next_log;

static PFN_vkDestroyImageView  g_dc_real_dv;  static PFN_vkDestroyImage  g_dc_real_di;
static PFN_vkDestroySampler    g_dc_real_ds;  static PFN_vkDestroyBufferView g_dc_real_dbv;
static PFN_vkDestroyBuffer     g_dc_real_db;  static PFN_vkFreeMemory    g_dc_real_fm;
#define NVMTL_DC_BUMP(e) __atomic_add_fetch(&(e), 1, __ATOMIC_SEQ_CST)
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_dv(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{ if (v) NVMTL_DC_BUMP(g_dc_epoch_img); g_dc_real_dv(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_di(VkDevice d, VkImage v, const VkAllocationCallbacks *a)
{ if (v) NVMTL_DC_BUMP(g_dc_epoch_img); g_dc_real_di(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_ds(VkDevice d, VkSampler v, const VkAllocationCallbacks *a)
{ if (v) NVMTL_DC_BUMP(g_dc_epoch_img); g_dc_real_ds(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_dbv(VkDevice d, VkBufferView v, const VkAllocationCallbacks *a)
{ if (v) NVMTL_DC_BUMP(g_dc_epoch_img); g_dc_real_dbv(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL nvmtl_dc_db(VkDevice d, VkBuffer v, const VkAllocationCallbacks *a)
{ if (v) NVMTL_DC_BUMP(g_dc_epoch_buf); g_dc_real_db(d, v, a); }
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
    nvlog("desc-cache %s: hit %llu miss %llu (stale %llu, page %llu, long %llu, full %llu) inserted %llu, epochs img %u buf %u",
          why, (unsigned long long)g_dc_hit, (unsigned long long)g_dc_miss, (unsigned long long)g_dc_stale,
          (unsigned long long)g_dc_page, (unsigned long long)g_dc_long, (unsigned long long)g_dc_full,
          (unsigned long long)g_dc_ins, __atomic_load_n(&g_dc_epoch_img, __ATOMIC_RELAXED),
          __atomic_load_n(&g_dc_epoch_buf, __ATOMIC_RELAXED));
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
 * table slot. In SSBO mode the table content itself is keyed. Returns the length, 0 when it does not fit. */
static unsigned nvmtl_dc_key(const nvmtl_bindtable *t, uint32_t which, const uint64_t pm[4], uint32_t hi, int ubo,
                             uint64_t *k, uint8_t *dep)
{
    unsigned n = 0; *dep = 0;
    k[n++] = (uint64_t)which | (uint64_t)hi << 8 | (uint64_t)(ubo != 0) << 40;
    for (int w = 0; w < 4; w++) k[n++] = pm[w];
    for (int w = 0; w < 4; w++) for (uint64_t b = pm[w]; b; b &= b - 1) {
        const uint32_t i = (uint32_t)(w * 64 + __builtin_ctzll(b));
        if (i >= NVMTL_STEX_BASE) continue;
        const nvmtl_slot *sl = &t->s[which][i];
        if (i == NVMTL_ADDRESS_BINDING) {
            if (ubo) continue;
            if (n + NVMTL_NBUF + NVMTL_NTEX > NVMTL_DC_KEYMAX) return 0;
            for (unsigned j = 0; j < NVMTL_NBUF; j++) { const nvmtl_slot *s = &t->s[which][j];
                k[n++] = s->kind == NVMTL_SLOT_BUF ? s->address + s->off : 1; if (s->kind == NVMTL_SLOT_BUF) *dep |= NVMTL_DC_DEP_BUF; }
            for (unsigned j = 0; j < NVMTL_NTEX; j++) { const nvmtl_slot *s = &t->s[which][NVMTL_NBUF + j];
                k[n++] = (s->kind == NVMTL_SLOT_TEX && s->h) ? (uint64_t)s->size : 0; }
            continue;
        }
        if (n + 2 > NVMTL_DC_KEYMAX) return 0;
        const VkDescriptorType ty = binding_type(i);
        if (ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            if (sl->kind == NVMTL_SLOT_BUF && (sl->off % NVMTL_SSBO_ALIGN) == 0) {
                k[n++] = (uint64_t)(uintptr_t)sl->h; k[n++] = (uint64_t)sl->off; *dep |= NVMTL_DC_DEP_BUF;
            } else { k[n++] = 0; k[n++] = ~0ull; }
        } else if (ty == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            const int on = sl->kind == NVMTL_SLOT_TEX; k[n++] = on ? (uint64_t)(uintptr_t)sl->h : 0; if (on) *dep |= NVMTL_DC_DEP_IMG;
        } else {
            const int on = sl->kind == NVMTL_SLOT_SAMP; k[n++] = on ? (uint64_t)(uintptr_t)sl->h : 0; if (on) *dep |= NVMTL_DC_DEP_IMG;
        }
    }
    if (n + hi > NVMTL_DC_KEYMAX) return 0;
    for (uint32_t j = 0; j < hi; j++) {
        const nvmtl_slot *sl = &t->s[which][NVMTL_STEX_BASE + j];
        const int on = sl->kind == NVMTL_SLOT_TEX && sl->h; k[n++] = on ? (uint64_t)(uintptr_t)sl->h : 0; if (on) *dep |= NVMTL_DC_DEP_IMG;
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
/* A lookup in flight between the key and the insert after a miss. */
typedef struct { nvmtl_dcache *dc; uint64_t h; unsigned n, slot; uint8_t dep; uint32_t ie, be; uint64_t k[NVMTL_DC_KEYMAX]; } nvmtl_dc_probe;

/* Returns a reusable set (and its dynamic offset for binding 640 in *dyn), or VK_NULL_HANDLE with pr filled in for
 * nvmtl_dc_insert. The caller has already updated nothing. */
static VkDescriptorSet nvmtl_dc_lookup(nvk_cmdbuf *c, nvmtl_bindtable *t, uint32_t which, const uint64_t pm[4], uint32_t hi,
                                       int on, uint32_t *dyn, nvmtl_dc_probe *pr)
{
    /* Epochs are read before the key: a destroy racing the key build makes the new entry stale, never a stale one fresh.
       Without a key the set's handle classes are unknown, so it depends on both. */
    pr->dc = NULL; pr->n = 0; pr->dep = NVMTL_DC_DEP_IMG | NVMTL_DC_DEP_BUF;
    pr->ie = __atomic_load_n(&g_dc_epoch_img, __ATOMIC_ACQUIRE); pr->be = __atomic_load_n(&g_dc_epoch_buf, __ATOMIC_ACQUIRE);
    if (!on) return VK_NULL_HANDLE;
    const int ubo = nvmtl_table_ubo();
    pr->n = nvmtl_dc_key(t, which, pm, hi, ubo, pr->k, &pr->dep);
    if (!pr->n) {
        pr->dep = NVMTL_DC_DEP_IMG | NVMTL_DC_DEP_BUF; __atomic_add_fetch(&g_dc_long, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
    nvmtl_dcache *dc = (nvmtl_dcache *)t->dc;
    if (!dc) {
        dc = (nvmtl_dcache *)malloc(sizeof *dc);
        if (!dc) return VK_NULL_HANDLE;
        dc->n = dc->an = 0; memset(dc->e, 0, sizeof dc->e); t->dc = dc;
    }
    pr->dc = dc; pr->h = nvmtl_dc_hash(pr->k, pr->n);
    nvmtl_dc_ent *e = nvmtl_dc_find(dc, pr->h, pr->k, pr->n, &pr->slot);
    if (!e) { __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
    if (nvmtl_dc_stale(e) || e->idx >= c->nsets || (VkDescriptorSet)c->sets[e->idx].set != e->set) {
        __atomic_add_fetch(&g_dc_stale, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
    uint32_t d = 0;
    if (e->addr && ubo) {
        /* The set's binding 640 points at its own table slot. The next slot must land in the same page, after it. */
        const unsigned at = c->address_count % NVMTL_TABLES_PER_PAGE;
        const nvmtl_address_page *pg = (const nvmtl_address_page *)c->address_pages;
        if (at == 0 || !pg || (VkBuffer)pg->buffer.buf != e->pbuf || at * NVMTL_TABLE_BYTES <= e->poff) {
            __atomic_add_fetch(&g_dc_page, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_dc_miss, 1, __ATOMIC_RELAXED); return VK_NULL_HANDLE; }
        VkDescriptorBufferInfo ai;
        if (nvmtl_address_snapshot(c, which, &ai) || (VkBuffer)ai.buffer != e->pbuf || ai.offset <= e->poff) {
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
static void nvmtl_dc_insert(nvk_cmdbuf *c, const nvmtl_dc_probe *pr, VkDescriptorSet s, uint32_t hi, int addr)
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
        e->pbuf = (VkBuffer)pg->buffer.buf; e->poff = (uint32_t)(((c->address_count - 1) % NVMTL_TABLES_PER_PAGE) * NVMTL_TABLE_BYTES);
    }
    __atomic_add_fetch(&g_dc_ins, 1, __ATOMIC_RELAXED);
}
