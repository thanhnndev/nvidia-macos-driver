/* CPU-only check of the descriptor snapshot path with the desc-cache (no GPU, no Vulkan driver).
 *
 * tests/desccache_extract.py copies the real code (bind table, address table, nvmtl_snapshot_set_md/_c,
 * nvmtl_desc_cache.h, nvmtl_vk_cmd_bind_set) out of nvmtl_vk.c; this file supplies a fake Vulkan descriptor store
 * and runs random bind/draw sequences. After every draw it checks, for both stages, that the bound set holds for each
 * binding the pipeline reads exactly the descriptor a fresh build would hold (handle AND its generation, so a freed
 * and reused handle is caught), and that binding 640 + the dynamic offset lands on an address-table slot whose
 * content matches the current bindings.
 *
 *   sh tests/desccache_test.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "../nvmtl_vk.h"

static int g_verbose;
void nvlog(const char *fmt, ...) { if (!g_verbose) return; va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }

/* ---- fake handles with generations ---- */
#define NH 64
static uint64_t g_gen[NH * 4];            /* generation per fake handle value */
static uint64_t hgen(const void *h) { uintptr_t v = (uintptr_t)h; return v && v < NH * 4 ? g_gen[v] : 0; }

/* ---- fake descriptor sets ---- */
#define NSETS 65536
typedef struct { uint8_t w; uint64_t a, b, g; } frec;
typedef struct { int live; frec r[384]; } fset;
static fset *g_sets; static uint32_t g_freelist[NSETS], g_nfree;
#define NVMTL_ADDRESS_BINDING_V (32 + 128 + 32)
#define NVMTL_STEX_BASE_V (NVMTL_ADDRESS_BINDING_V + 1)
static unsigned bind_index(uint32_t b)
{ return b == 640 ? NVMTL_ADDRESS_BINDING_V : (b >= 480 && b < 608) ? NVMTL_STEX_BASE_V + (b - 480) : b; }

static VkResult VKAPI_CALL f_alloc(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    (void)d;
    if (g_nfree < ai->descriptorSetCount) return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (uint32_t k = 0; k < ai->descriptorSetCount; k++) {
        uint32_t id = g_freelist[--g_nfree]; memset(&g_sets[id], 0, sizeof g_sets[id]); g_sets[id].live = 1;
        out[k] = (VkDescriptorSet)(uintptr_t)id;
    }
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL f_free(VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s)
{
    (void)d; (void)p;
    for (uint32_t k = 0; k < n; k++) { uint32_t id = (uint32_t)(uintptr_t)s[k]; if (!g_sets[id].live) { fprintf(stderr, "double free %u\n", id); exit(2); }
        g_sets[id].live = 0; g_freelist[g_nfree++] = id; }
    return VK_SUCCESS;
}
static uint64_t g_writes;
static void VKAPI_CALL f_update(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *cp)
{
    (void)d;
    for (uint32_t k = 0; k < nw; k++) {
        fset *s = &g_sets[(uintptr_t)w[k].dstSet]; frec *r = &s->r[bind_index(w[k].dstBinding)];
        r->w = 1; g_writes++;
        switch (w[k].descriptorType) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            r->a = (uint64_t)(uintptr_t)w[k].pBufferInfo->buffer; r->b = w[k].pBufferInfo->offset; r->g = hgen(w[k].pBufferInfo->buffer); break;
        case VK_DESCRIPTOR_TYPE_SAMPLER: r->a = (uint64_t)(uintptr_t)w[k].pImageInfo->sampler; r->b = 0; r->g = hgen(w[k].pImageInfo->sampler); break;
        default: r->a = (uint64_t)(uintptr_t)w[k].pImageInfo->imageView; r->b = 0; r->g = hgen(w[k].pImageInfo->imageView); break;
        }
    }
    for (uint32_t k = 0; k < nc; k++) {
        fset *src = &g_sets[(uintptr_t)cp[k].srcSet], *dst = &g_sets[(uintptr_t)cp[k].dstSet];
        dst->r[bind_index(cp[k].dstBinding)] = src->r[bind_index(cp[k].srcBinding)];
    }
}
static VkDescriptorSet g_bound[3]; static uint32_t g_bound_dyn[2], g_bound_ndyn;
static void VKAPI_CALL f_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n,
                              const VkDescriptorSet *s, uint32_t nd, const uint32_t *dyn)
{ (void)cb; (void)bp; (void)l; (void)first; for (uint32_t k = 0; k < n && k < 3; k++) g_bound[k] = s[k];
  g_bound_ndyn = nd; for (uint32_t k = 0; k < nd && k < 2; k++) g_bound_dyn[k] = dyn[k]; }
static void VKAPI_CALL f_push(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags f, uint32_t o, uint32_t n, const void *v)
{ (void)cb; (void)l; (void)f; (void)o; (void)n; (void)v; }
static void VKAPI_CALL f_dv(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)a; g_gen[(uintptr_t)v]++; }
static void VKAPI_CALL f_di(VkDevice d, VkImage v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
static void VKAPI_CALL f_ds(VkDevice d, VkSampler v, const VkAllocationCallbacks *a) { (void)d; (void)a; g_gen[(uintptr_t)v]++; }
static void VKAPI_CALL f_dbv(VkDevice d, VkBufferView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
static void VKAPI_CALL f_db(VkDevice d, VkBuffer v, const VkAllocationCallbacks *a) { (void)d; (void)a; g_gen[(uintptr_t)v]++; }
static void VKAPI_CALL f_fm(VkDevice d, VkDeviceMemory v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }

static PFN_vkAllocateDescriptorSets pvkAllocateDescriptorSets = f_alloc;
static PFN_vkFreeDescriptorSets pvkFreeDescriptorSets = f_free;
static PFN_vkUpdateDescriptorSets pvkUpdateDescriptorSets = f_update;
static PFN_vkCmdBindDescriptorSets pvkCmdBindDescriptorSets = f_bind;
static PFN_vkCmdPushConstants pvkCmdPushConstants = f_push;
static PFN_vkDestroyImageView pvkDestroyImageView = f_dv;
static PFN_vkDestroyImage pvkDestroyImage = f_di;
static PFN_vkDestroySampler pvkDestroySampler = f_ds;
static PFN_vkDestroyBufferView pvkDestroyBufferView = f_dbv;
static PFN_vkDestroyBuffer pvkDestroyBuffer = f_db;
static PFN_vkFreeMemory pvkFreeMemory = f_fm;

/* ---- the rest of what the extracted code touches ---- */
static VkDevice g_dev; static VkPhysicalDeviceLimits g_lim;
static pthread_mutex_t g_desc_lock = PTHREAD_MUTEX_INITIALIZER;
static VkDescriptorPool g_descPools[1] = { (VkDescriptorPool)(uintptr_t)1 }; static unsigned g_ndescPools = 1;
static VkDescriptorSetLayout g_setLayout;
static int ensure_descriptors_locked(void) { return 0; }
static int nvmtl_desc_pool_open(void) { return -1; }
static nvk_buffer g_dummyBuf = { .buf = (void *)(uintptr_t)200, .size = 4096 };
static nvk_image g_dummyImg = { .view = (void *)(uintptr_t)201 }, g_dummyStex = { .view = (void *)(uintptr_t)202 };
static VkSampler g_dummySampler = (VkSampler)(uintptr_t)203;
static void *g_dummyTexView = (void *)(uintptr_t)204;
static int g_mutable, g_mutable_stex, g_sample_locations;
static VkDescriptorSet g_heapSet;
static int g_ubo = 1;
static int nvmtl_table_ubo(void) { return g_ubo; }
enum { NVMTL_E78_H3 = 1 };
enum { E78C_SETNEW, E78C_SETREUSE, E78C_WRITES, E78C_N };
static uint64_t g_e78c[E78C_N];
static int nvmtl_enc78_on(int row) { (void)row; return 1; }
static void nvmtl_enc78_count(int k, uint64_t n) { g_e78c[k] += n; }
typedef struct nvmtl_address_page nvmtl_address_page;
static nvmtl_address_page *nvmtl_keep_page_pop(void) { return NULL; }
static uint32_t g_next_buf = 300;
int nvmtl_vk_buffer_create(size_t size, int host, nvk_buffer *out)
{ (void)host; memset(out, 0, sizeof *out); out->map = calloc(1, size); out->size = size; out->buf = (void *)(uintptr_t)(g_next_buf++); return out->map ? 0 : -1; }
void nvmtl_vk_buffer_destroy(nvk_buffer *b) { free(b->map); b->map = NULL; }
uint64_t nvmtl_vk_buffer_address(nvk_buffer *b) { return b && b->buf ? (uint64_t)(uintptr_t)b->buf << 24 : 0; }

#include "desccache_extracted.inc"

_Static_assert(NVMTL_ADDRESS_BINDING == NVMTL_ADDRESS_BINDING_V, "binding layout moved: update the test");

/* ---- ground truth ---- */
static int g_fail;
static void expect(fset *s, unsigned i, uint64_t a, uint64_t b, uint64_t g, const char *what, unsigned which)
{
    const frec *r = &s->r[i];
    if (!r->w || r->a != a || r->b != b || r->g != g) {
        if (g_fail++ < 3 && g_verbose) fprintf(stderr, "FAIL stage %u binding %u (%s): set has w%u %llx+%llu g%llu, want %llx+%llu g%llu\n", which, i, what,
            r->w, (unsigned long long)r->a, (unsigned long long)r->b, (unsigned long long)r->g, (unsigned long long)a, (unsigned long long)b, (unsigned long long)g);
    }
}
static nvmtl_address_page *page_of(nvk_cmdbuf *c, uint64_t buf)
{ for (nvmtl_address_page *p = c->address_pages; p; p = p->next) if ((uint64_t)(uintptr_t)p->buffer.buf == buf) return p; return NULL; }
static void verify(nvk_cmdbuf *c, const nvk_pipeline *p)
{
    static nvmtl_bindtable empty;
    nvmtl_bindtable *t = c->table ? c->table : &empty;
    for (unsigned which = 0; which < 2; which++) {
        uintptr_t id = (uintptr_t)g_bound[which];
        if (!id || !g_sets[id].live) { if (g_fail++ < 3 && g_verbose) fprintf(stderr, "FAIL stage %u: bound set %lu not live\n", which, (unsigned long)id); continue; }
        fset *s = &g_sets[id];
        const uint64_t all[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
        const uint64_t *pm = p->bmask_ok ? p->bmask : p->gmask_ok ? p->gmask[which] : all;
        for (unsigned i = 0; i < NVMTL_STEX_BASE; i++) {
            if (!((pm[i >> 6] >> (i & 63)) & 1)) continue;
            const nvmtl_slot *sl = &t->s[which][i];
            if (i == NVMTL_ADDRESS_BINDING) {
                const frec *r = &s->r[i];
                uint32_t dyn = g_ubo ? g_bound_dyn[which] : 0;
                nvmtl_address_page *pg = page_of(c, r->a);
                if (!r->w || !pg || r->b + dyn + NVMTL_TABLE_BYTES > pg->buffer.size) { if (g_fail++ < 3 && g_verbose) fprintf(stderr, "FAIL stage %u: address binding not written / page gone\n", which); continue; }
                const uint64_t *tab = (const uint64_t *)((const uint8_t *)pg->buffer.map + r->b + dyn);
                const uint64_t self = nvmtl_vk_buffer_address(&pg->buffer) + r->b + dyn;
                for (unsigned j = 0; j < NVMTL_NBUF; j++) {
                    const nvmtl_slot *b = &t->s[which][j];
                    uint64_t want = b->kind == NVMTL_SLOT_BUF ? b->address + b->off : nvmtl_vk_buffer_address(&g_dummyBuf);
                    if (tab[j] != want || tab[NVMTL_NBUF + j] != self + j * 8) { if (g_fail++ < 3 && g_verbose) fprintf(stderr, "FAIL stage %u: table buf %u = %llx want %llx (dyn %u)\n", which, j, (unsigned long long)tab[j], (unsigned long long)want, dyn); break; }
                }
                for (unsigned j = 0; j < NVMTL_NTEX; j++) {
                    const nvmtl_slot *x = &t->s[which][NVMTL_NBUF + j];
                    uint64_t want = (x->kind == NVMTL_SLOT_TEX && x->h) ? x->size : 0;
                    if (tab[2 * NVMTL_NBUF + j] != want) { if (g_fail++ < 3 && g_verbose) fprintf(stderr, "FAIL stage %u: table tex %u\n", which, j); break; }
                }
                continue;
            }
            VkDescriptorType ty = binding_type(i);
            if (ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                if (sl->kind == NVMTL_SLOT_BUF && sl->off % NVMTL_SSBO_ALIGN == 0) expect(s, i, (uintptr_t)sl->h, sl->off, hgen(sl->h), "buffer", which);
                else expect(s, i, (uintptr_t)g_dummyBuf.buf, 0, hgen(g_dummyBuf.buf), "buffer dummy", which);
            } else if (ty == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                if (sl->kind == NVMTL_SLOT_TEX) expect(s, i, (uintptr_t)sl->h, 0, hgen(sl->h), "texture", which);
                else expect(s, i, (uintptr_t)g_dummyImg.view, 0, hgen(g_dummyImg.view), "texture dummy", which);
            } else {
                if (sl->kind == NVMTL_SLOT_SAMP) expect(s, i, (uintptr_t)sl->h, 0, hgen(sl->h), "sampler", which);
                else expect(s, i, (uintptr_t)g_dummySampler, 0, hgen(g_dummySampler), "sampler dummy", which);
            }
        }
        for (unsigned j = 0; j < p->stex_hi[which]; j++) {
            const nvmtl_slot *sl = &t->s[which][NVMTL_STEX_BASE + j];
            if (sl->kind == NVMTL_SLOT_TEX && sl->h) expect(s, NVMTL_STEX_BASE + j, (uintptr_t)sl->h, 0, hgen(sl->h), "stex", which);
            else expect(s, NVMTL_STEX_BASE + j, (uintptr_t)g_dummyStex.view, 0, hgen(g_dummyStex.view), "stex dummy", which);
        }
    }
}
static void retire(nvk_cmdbuf *c)   /* the parts of nvmtl_cmd_retire that own sets, the table, its cache and the pages */
{
    if (c->ndcache) { VkDescriptorSet back[32]; for (unsigned i = 0; i < c->ndcache; i++) back[i] = (VkDescriptorSet)c->dcache[i];
                      pvkFreeDescriptorSets(g_dev, NULL, c->ndcache, back); c->ndcache = 0; }
    for (unsigned i = 0; i < c->nsets; i++) { VkDescriptorSet s = (VkDescriptorSet)c->sets[i].set; pvkFreeDescriptorSets(g_dev, NULL, 1, &s); }
    free(c->sets); c->sets = NULL; c->nsets = c->capsets = 0;
    if (c->table) { free(((nvmtl_bindtable *)c->table)->dc); ((nvmtl_bindtable *)c->table)->dc = NULL; }
    free(c->table); c->table = NULL;
    for (nvmtl_address_page *p = c->address_pages, *n; p; p = n) { n = p->next; nvmtl_vk_buffer_destroy(&p->buffer); free(p); }
    c->address_pages = NULL; c->address_count = 0;
}

static uint64_t rnd_s = 88172645463325252ull;
static uint64_t rnd(void) { rnd_s ^= rnd_s << 13; rnd_s ^= rnd_s >> 7; rnd_s ^= rnd_s << 17; return rnd_s; }
#define R(n) ((unsigned)(rnd() % (n)))

static void run(int ubo, int cache, int destroy_rate, int ws, unsigned ncb, unsigned ndraw, const char *name)
{
    g_ubo = ubo; g_dc_env = cache; g_dc_live = 1;
    g_dc_hit = g_dc_miss = g_dc_stale = g_dc_page = g_dc_long = g_dc_full = g_dc_ins = 0; g_writes = 0;
    memset(g_e78c, 0, sizeof g_e78c);
    nvk_buffer bufs[4]; for (int k = 0; k < 4; k++) { memset(&bufs[k], 0, sizeof bufs[k]); bufs[k].buf = (void *)(uintptr_t)(10 + k); bufs[k].size = 1 << 20; }
    nvk_image views[4]; for (int k = 0; k < 4; k++) { memset(&views[k], 0, sizeof views[k]); views[k].view = (void *)(uintptr_t)(20 + k); }
    nvk_sampler samps[3]; for (int k = 0; k < 3; k++) { memset(&samps[k], 0, sizeof samps[k]); samps[k].s = (void *)(uintptr_t)(30 + k); }
    /* pipelines: what WindowServer's look like (table only, table + sampler + texture), plus wider ones */
    nvk_pipeline P[7]; memset(P, 0, sizeof P);
    const unsigned A = NVMTL_ADDRESS_BINDING, T0 = NVMTL_NBUF, S0 = NVMTL_NBUF + NVMTL_NTEX;
    #define BIT(pp, i) ((pp).bmask[(i) >> 6] |= 1ull << ((i) & 63))
    P[0].bmask_ok = 1; BIT(P[0], A);
    P[1].bmask_ok = 1; BIT(P[1], A); BIT(P[1], S0); BIT(P[1], T0);
    P[2].bmask_ok = 1; BIT(P[2], A); BIT(P[2], 0); BIT(P[2], 1); BIT(P[2], S0 + 1); BIT(P[2], T0 + 1);
    P[3].bmask_ok = 1; BIT(P[3], S0); P[3].stex_hi[0] = P[3].stex_hi[1] = 2;
    P[4].bmask_ok = 1; BIT(P[4], 0); BIT(P[4], T0); BIT(P[4], T0 + 2);
    P[5].gmask_ok = 1; P[5].gmask[0][A >> 6] |= 1ull << (A & 63); P[5].gmask[1][(S0) >> 6] |= 1ull << ((S0) & 63); P[5].gmask[1][T0 >> 6] |= 1ull << (T0 & 63);
    /* P[6]: no mask at all - every binding, a key too long to cache */
    P[6].stex_hi[1] = 1;
    unsigned draws = 0;
    for (unsigned cb = 0; cb < ncb; cb++) {
        nvk_cmdbuf c; memset(&c, 0, sizeof c);
        for (unsigned d = 0; d < ndraw; d++) {
            unsigned ops = R(4);
            if (d == 0 || R(4) == 0) ops++;
            for (unsigned o = 0; o < ops; o++) {
                const unsigned set = R(2);
                switch (ws && R(8) ? 0 : R(9)) {
                case 0: case 1: case 2: { /* setBytes: same ring buffer, a new offset every time (WindowServer) */
                    size_t off = (size_t)R(4096) * (R(8) ? 16 : 4);
                    nvmtl_vk_bind_buffer(&c, set, R(3), &bufs[R(4)], off); break; }
                case 3: if (c.table) nvmtl_vk_bind_buffer_offset(&c, set, R(3), (size_t)R(1024) * 16); break;
                case 4: nvmtl_vk_bind_texture(&c, set, R(3), &views[R(4)]); break;
                case 5: nvmtl_vk_bind_sampler(&c, set, R(2), R(5) ? &samps[R(3)] : NULL); break;
                case 6: nvmtl_bind_slot(&c, set, NVMTL_STEX_BASE + R(2), R(4) ? NVMTL_SLOT_TEX : NVMTL_SLOT_NONE, views[R(4)].view, 0); break;
                case 7: if (c.table) { nvmtl_slot *x = &((nvmtl_bindtable *)c.table)->s[set][NVMTL_NBUF + R(3)];
                          if (x->kind == NVMTL_SLOT_TEX) { x->size = 1 + R(100); ((nvmtl_bindtable *)c.table)->dirty[set] = 1; } } break;
                case 8: if (destroy_rate && R(destroy_rate) == 0) {
                          /* free a handle; the same value comes back as a NEW object (generation + 1) */
                          switch (R(3)) {
                          case 0: pvkDestroyImageView(g_dev, (VkImageView)views[R(4)].view, NULL); break;
                          case 1: pvkDestroySampler(g_dev, (VkSampler)samps[R(3)].s, NULL); break;
                          default: pvkDestroyBuffer(g_dev, (VkBuffer)bufs[R(4)].buf, NULL); break;
                          } }
                        break;
                }
            }
            nvk_pipeline *p = ws == 2 ? &P[R(3) ? 6 : 1] : ws ? &P[R(2)] : &P[R(10) < 7 ? R(2) : R(7)];
            memset(g_bound, 0, sizeof g_bound);
            nvmtl_vk_cmd_bind_set(&c, p);
            if (c.nobind) { if (g_fail++ < 3 && g_verbose) fprintf(stderr, "FAIL: nobind\n"); continue; }
            verify(&c, p); draws++;
        }
        retire(&c);
    }
    printf("%-34s draws %6u  sets-new %6llu reused %6llu  descriptor writes %7llu  cache hit %6llu miss %6llu (stale %llu page %llu long %llu full %llu)  %s\n",
           name, draws, (unsigned long long)g_e78c[E78C_SETNEW], (unsigned long long)g_e78c[E78C_SETREUSE], (unsigned long long)g_writes,
           (unsigned long long)g_dc_hit, (unsigned long long)g_dc_miss, (unsigned long long)g_dc_stale, (unsigned long long)g_dc_page,
           (unsigned long long)g_dc_long, (unsigned long long)g_dc_full, g_fail ? "FAILED" : "ok");
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1;
    g_sets = calloc(NSETS, sizeof *g_sets);
    for (uint32_t i = NSETS - 1; i >= 1; i--) g_freelist[g_nfree++] = i;
    g_lim.minUniformBufferOffsetAlignment = 4;
    nvmtl_dc_hook_destroy();
    int bad = 0;
    /* informational = the old path (cache off) with handles freed and reused mid-command-buffer: it reuses last[]
       across the free, a hazard that predates the cache (the cache's epoch guard closes it when on). */
    struct { int ubo, cache, destroy, ws, info; const char *name; } cfg[] = {
        { 1, 0, 0, 1, 0, "WS-like, UBO table, cache OFF" },  { 1, 1, 0, 1, 0, "WS-like, UBO table, cache ON" },
        { 1, 1, 5, 1, 0, "WS-like, cache ON, frees" },
        { 1, 0, 0, 2, 0, "WS no-mask, cache OFF" },          { 1, 1, 0, 2, 0, "WS no-mask, cache ON" },
        { 1, 1, 5, 2, 0, "WS no-mask, cache ON, frees" },
        { 1, 0, 0, 0, 0, "mixed, UBO table, cache OFF" },    { 1, 1, 0, 0, 0, "mixed, UBO table, cache ON" },
        { 1, 1, 3, 0, 0, "mixed, UBO, cache ON, frees" },    { 0, 0, 0, 0, 0, "mixed, SSBO table, cache OFF" },
        { 0, 1, 0, 0, 0, "mixed, SSBO table, cache ON" },    { 0, 1, 3, 0, 0, "mixed, SSBO, cache ON, frees" },
        { 1, 0, 3, 0, 1, "mixed, UBO, cache OFF, frees (info)" },
    };
    for (unsigned k = 0; k < sizeof cfg / sizeof cfg[0]; k++) {
        g_fail = 0; run(cfg[k].ubo, cfg[k].cache, cfg[k].destroy, cfg[k].ws, 300, 400, cfg[k].name);
        if (!cfg[k].info) bad |= g_fail != 0;
    }
    /* the live flag file and the env switch */
    g_dc_env = -1; setenv("NVMTL_NO_DESCCACHE", "1", 1);
    if (nvmtl_dc_on()) { printf("FAIL: NVMTL_NO_DESCCACHE=1 left the cache on\n"); bad = 1; } else printf("NVMTL_NO_DESCCACHE=1 -> cache off: ok\n");
    unsetenv("NVMTL_NO_DESCCACHE"); g_dc_env = -1;
    if (!nvmtl_dc_on()) { printf("FAIL: cache off without NVMTL_NO_DESCCACHE\n"); bad = 1; } else printf("default -> cache on: ok\n");
    printf(bad ? "RESULT: FAILED\n" : "RESULT: all checks passed\n");
    return bad;
}
