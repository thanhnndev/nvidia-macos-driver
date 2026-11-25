// NVMTLFlipCensus.m - included by NVMTLObjects.m. 2026-11-25 GFX power: who drives WindowServer's presents, and what
// each present draws into the screen. Census only, no behaviour change. Off unless NVMTL_FLIP_CENSUS=1 (environment or
// /Library/GPUBundles/nvmtl/nvrm610.conf); off, every hook is one predictable branch.
//
// Per present (commitAndWaitUntilSubmitted): which IOSurfaces were sampled by a fragment shader with a seed that changed
// since the last time this process sampled them (a client drew; our plugin bumps the seed on GPU writes, CPU writers bump
// it on unlock), and whether any of those was a client's (not a surface this process renders into, not a WindowServer
// name). Per full-screen pass (attachment >= 4 Mpx): target surface, its Metal load action, its fragment shaders.
// SPIR-V of every hashed shader goes to $TMPDIR/nvmtl-pstat (per user; WindowServer's own temp dir).
// Cost: IOSurfaceGetSeed / GetID are user-space reads (2.8 ns, no syscall); the name is read once per surface; one
// unfair lock per sampled surface bind. The pid owning a surface is not readable from this process without a private
// IOSurfaceRoot selector, so the report names surfaces by id + IOSurface name + size and
// tools/890m/flip-census/resolve.sh maps ids to processes with /usr/bin/IOSDebug (unprivileged, read-only).

#define FLC_N 2048                       // surface slots (power of two); rebuilt every report
#define FLC_TGT 16                       // distinct full-screen target surfaces per window
#define FLC_FS 32                        // distinct fragment shaders on full-screen passes per window
enum { FLC_TINY, FLC_SMALL, FLC_MED, FLC_LARGE, FLC_FULL, FLC_NCLS };
static const char *const kFlcCls[FLC_NCLS] = { "tiny<64K", "small<0.5M", "medium<2M", "large<4M", "full>=4M" };

typedef struct { uint32_t sid, w, h, seed; uint64_t chg_seq, smp_seq; uint32_t chg, smp; uint8_t used, own, cls; char name[40]; } flc_surf;
typedef struct { uint32_t sid, passes, load[3]; uint8_t scanout; char name[32]; } flc_tgt;
typedef struct { uint32_t fs, passes, load[3]; char name[240]; } flc_fs;   // name: non-zero function constants of the variant

static int g_flc = -1;
static const void *g_flc_nz_key;            // associated-object key of a pipeline's constants summary (NVMTLObjects.m)
static os_unfair_lock g_flc_lock = OS_UNFAIR_LOCK_INIT;
static flc_surf g_flc_s[FLC_N];
static uint32_t g_flc_used;
static flc_tgt g_flc_t[FLC_TGT]; static uint32_t g_flc_tn, g_flc_t_over;
static flc_fs g_flc_f[FLC_FS]; static uint32_t g_flc_fn, g_flc_f_over;
static uint64_t g_flc_seq = 1;                         // current present (incremented at commitAndWaitUntilSubmitted)
static uint32_t g_flc_cur_client, g_flc_cur_own;        // changed surfaces sampled in the current present
static uint32_t g_flc_cur_big, g_flc_last_big_sid, g_flc_prev_present_sid;
static uint32_t g_flc_pres, g_flc_pres_client, g_flc_pres_ownonly, g_flc_pres_none, g_flc_pres_nobig;
static uint32_t g_flc_same, g_flc_diff, g_flc_big_switch, g_flc_cls_pres[FLC_NCLS];
static uint8_t g_flc_cur_cls;                           // bit per class: a changed client surface of that class this present

static inline int nvmtl_flc_on(void)
{
    if (__builtin_expect(g_flc < 0, 0)) {
        const char *e = getenv("NVMTL_FLIP_CENSUS"); g_flc = (e && *e && *e != '0') ? 1 : 0;
        if (g_flc) nvlog("flip census: NVMTL_FLIP_CENSUS set - per-present client surfaces and full-screen targets, reported with the frame census");
    }
    return g_flc;
}
static inline uint8_t nvmtl_flc_cls(uint64_t px)
{ return px < 65536 ? FLC_TINY : px < 524288 ? FLC_SMALL : px < 2097152 ? FLC_MED : px < 4194304 ? FLC_LARGE : FLC_FULL; }
static int nvmtl_flc_own_name(const char *n)
{ return !strncmp(n, "SL.", 3) || strstr(n, "CoreDisplay") || strstr(n, "flattened") || strstr(n, "WindowServer") || strstr(n, "CaptureSurface"); }

static void nvmtl_flc_init(flc_surf *e, IOSurfaceRef s, uint32_t sid)
{
    memset(e, 0, sizeof *e); e->used = 1;
    e->sid = sid; e->w = (uint32_t)IOSurfaceGetWidth(s); e->h = (uint32_t)IOSurfaceGetHeight(s);
    e->cls = nvmtl_flc_cls((uint64_t)e->w * e->h);
    CFTypeRef n = IOSurfaceCopyValue(s, kIOSurfaceName);   // once per surface
    if (n && CFGetTypeID(n) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)n, e->name, sizeof e->name, kCFStringEncodingUTF8);
    if (n) CFRelease(n);
    if (!e->name[0]) strlcpy(e->name, "-", sizeof e->name);
    e->own = nvmtl_flc_own_name(e->name) ? 1 : 0;
}
// lock held
static flc_surf *nvmtl_flc_find(IOSurfaceRef s, uint32_t sid, int *fresh)
{
    uint32_t i = (sid * 2654435761u) & (FLC_N - 1);
    for (uint32_t k = 0; k < FLC_N; k++, i = (i + 1) & (FLC_N - 1)) {
        flc_surf *e = &g_flc_s[i];
        if (e->used && e->sid == sid) {
            if (e->w == (uint32_t)IOSurfaceGetWidth(s) && e->h == (uint32_t)IOSurfaceGetHeight(s)) { *fresh = 0; return e; }
            nvmtl_flc_init(e, s, sid); *fresh = 1; return e;   // id reused by a new surface: start over in the same slot
        }
        if (!e->used) {
            if (g_flc_used >= FLC_N * 3 / 4) return NULL;      // full until the next report rebuilds the table
            nvmtl_flc_init(e, s, sid); g_flc_used++; *fresh = 1; return e;
        }
    }
    return NULL;
}

// A fragment shader samples this texture (setFragmentTexture).
static void nvmtl_flc_sample(NVMTLTexture *t)
{
    if (!nvmtl_flc_on()) return;
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf), seed = IOSurfaceGetSeed(r->_surf);
    os_unfair_lock_lock(&g_flc_lock);
    int fresh = 0; flc_surf *e = nvmtl_flc_find(r->_surf, sid, &fresh);
    if (e) {
        if (e->smp_seq != g_flc_seq) { e->smp_seq = g_flc_seq; e->smp++; }
        if (fresh || e->seed != seed) {
            e->seed = seed;
            if (e->chg_seq != g_flc_seq) { e->chg_seq = g_flc_seq; e->chg++;
                if (e->own) g_flc_cur_own++; else { g_flc_cur_client++; g_flc_cur_cls |= (uint8_t)(1u << e->cls); } }
        }
    }
    os_unfair_lock_unlock(&g_flc_lock);
}

// A render pass begins on this target (renderCommandEncoderWithDescriptor). Marks it as this process's own surface and,
// for a full-screen target, counts it by surface and load action.
static void nvmtl_flc_target(NVMTLTexture *sr, MTLLoadAction la)
{
    if (!nvmtl_flc_on() || !sr->_surf) return;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(sr->_surf);
    const uint64_t px = (uint64_t)[sr nvi]->w * [sr nvi]->h;
    const int li = la == MTLLoadActionLoad ? 0 : la == MTLLoadActionClear ? 1 : 2;
    os_unfair_lock_lock(&g_flc_lock);
    int fresh = 0; flc_surf *e = nvmtl_flc_find(sr->_surf, sid, &fresh);
    if (e) e->own = 1;
    if (px >= 4194304) {
        g_flc_cur_big++;
        if (g_flc_last_big_sid && sid != g_flc_last_big_sid) g_flc_big_switch++;
        g_flc_last_big_sid = sid;
        uint32_t k = 0; for (; k < g_flc_tn; k++) if (g_flc_t[k].sid == sid) break;
        if (k == g_flc_tn) { if (g_flc_tn < FLC_TGT) { memset(&g_flc_t[k], 0, sizeof g_flc_t[k]); g_flc_t[k].sid = sid; g_flc_tn++;
                                 if (e) { strlcpy(g_flc_t[k].name, e->name, sizeof g_flc_t[k].name); g_flc_t[k].scanout = strstr(e->name, "Scanout") != NULL; } }
                             else { g_flc_t_over++; k = FLC_TGT; } }
        if (k < FLC_TGT) { g_flc_t[k].passes++; g_flc_t[k].load[li]++; }
    }
    os_unfair_lock_unlock(&g_flc_lock);
}

// A pipeline with fragment shader hash fs (fname: its non-zero function constants) draws in a pass on a full-screen target with load
// action la; counted once per pass.
static void nvmtl_flc_fs(uint32_t fs, MTLLoadAction la, const char *fname)
{
    const int li = la == MTLLoadActionLoad ? 0 : la == MTLLoadActionClear ? 1 : 2;
    os_unfair_lock_lock(&g_flc_lock);
    uint32_t k = 0; for (; k < g_flc_fn; k++) if (g_flc_f[k].fs == fs) break;
    if (k == g_flc_fn) { if (g_flc_fn < FLC_FS) { memset(&g_flc_f[k], 0, sizeof g_flc_f[k]); g_flc_f[k].fs = fs; strlcpy(g_flc_f[k].name, fname && *fname ? fname : "?", sizeof g_flc_f[k].name); g_flc_fn++; } else { g_flc_f_over++; k = FLC_FS; } }
    if (k < FLC_FS) { g_flc_f[k].passes++; g_flc_f[k].load[li]++; }
    os_unfair_lock_unlock(&g_flc_lock);
}

// commitAndWaitUntilSubmitted: one present ends.
static void nvmtl_flc_present(void)
{
    if (!nvmtl_flc_on()) return;
    os_unfair_lock_lock(&g_flc_lock);
    g_flc_pres++;
    if (g_flc_cur_client) g_flc_pres_client++;
    else if (g_flc_cur_own) g_flc_pres_ownonly++;
    else g_flc_pres_none++;
    for (int c = 0; c < FLC_NCLS; c++) if (g_flc_cur_cls & (1u << c)) g_flc_cls_pres[c]++;
    if (!g_flc_cur_big) g_flc_pres_nobig++;
    else if (g_flc_prev_present_sid) { if (g_flc_last_big_sid == g_flc_prev_present_sid) g_flc_same++; else g_flc_diff++; }
    if (g_flc_cur_big) g_flc_prev_present_sid = g_flc_last_big_sid;
    g_flc_cur_client = g_flc_cur_own = g_flc_cur_big = 0; g_flc_cur_cls = 0;
    g_flc_seq++;
    os_unfair_lock_unlock(&g_flc_lock);
}

static int nvmtl_flc_cmp(const void *a, const void *b)
{ const flc_surf *x = *(const flc_surf *const *)a, *y = *(const flc_surf *const *)b; return x->chg < y->chg ? 1 : x->chg > y->chg ? -1 : 0; }

// Called from the 30 s frame census (nvmtl_vk_frame_census) with the window's length in seconds.
void nvmtl_flc_report(double secs)
{
    if (g_flc != 1) return;
    if (secs <= 0) secs = 1;
    os_unfair_lock_lock(&g_flc_lock);
    nvlog_report("flip census %.0f s: %u presents (%.1f/s): %u sampled a changed client surface, %u only changed surfaces of this process "
                 "(offscreen passes it drew itself), %u no changed surface at all (cursor, WindowServer animations, colour pass); "
                 "presents with a changed client surface by its size: %s %u, %s %u, %s %u, %s %u, %s %u; %u surfaces tracked",
                 secs, g_flc_pres, g_flc_pres / secs, g_flc_pres_client, g_flc_pres_ownonly, g_flc_pres_none,
                 kFlcCls[0], g_flc_cls_pres[0], kFlcCls[1], g_flc_cls_pres[1], kFlcCls[2], g_flc_cls_pres[2],
                 kFlcCls[3], g_flc_cls_pres[3], kFlcCls[4], g_flc_cls_pres[4], g_flc_used);
    flc_surf *top[FLC_N]; uint32_t nt = 0;
    for (uint32_t i = 0; i < FLC_N; i++) if (g_flc_s[i].used && g_flc_s[i].sid && g_flc_s[i].chg) top[nt++] = &g_flc_s[i];
    qsort(top, nt, sizeof top[0], nvmtl_flc_cmp);
    for (uint32_t i = 0; i < nt && i < 16; i++)
        nvlog_report("flip census surface 0x%x %ux%u %s <%s>%s: changed in %u presents (%.1f/s), sampled in %u",
                     top[i]->sid, top[i]->w, top[i]->h, kFlcCls[top[i]->cls], top[i]->name, top[i]->own ? " own" : " client",
                     top[i]->chg, top[i]->chg / secs, top[i]->smp);
    nvlog_report("flip census screen targets: presents drawing a >= 4 Mpx target: %u (%u drew none); its last full-screen target was the "
                 "same surface as the previous present's %u times, a different one %u times; full-screen target changes between passes %u; "
                 "%u distinct full-screen targets%s",
                 g_flc_pres - g_flc_pres_nobig, g_flc_pres_nobig, g_flc_same, g_flc_diff, g_flc_big_switch, g_flc_tn, g_flc_t_over ? " (table full)" : "");
    for (uint32_t k = 0; k < g_flc_tn; k++)
        nvlog_report("flip census target 0x%x <%s>%s: %u passes, load action Load %u Clear %u DontCare %u",
                     g_flc_t[k].sid, g_flc_t[k].name, g_flc_t[k].scanout ? " scanout" : "", g_flc_t[k].passes,
                     g_flc_t[k].load[0], g_flc_t[k].load[1], g_flc_t[k].load[2]);
    for (uint32_t k = 0; k < g_flc_fn; k++)
        nvlog_report("flip census full-screen fragment shader %08x in %u passes (load Load %u Clear %u DontCare %u); SPIR-V "
                     "$TMPDIR/nvmtl-pstat/%08x.frag.spv; constants %s", g_flc_f[k].fs, g_flc_f[k].passes,
                     g_flc_f[k].load[0], g_flc_f[k].load[1], g_flc_f[k].load[2], g_flc_f[k].fs, g_flc_f[k].name);
    // next window: counts to zero; surfaces not sampled this window dropped (table rebuilt, so probing stays intact)
    static flc_surf keep[FLC_N]; uint32_t nk = 0;
    for (uint32_t i = 0; i < FLC_N; i++) if (g_flc_s[i].used && g_flc_s[i].sid && g_flc_s[i].smp) { keep[nk] = g_flc_s[i]; keep[nk].chg = keep[nk].smp = 0; nk++; }
    memset(g_flc_s, 0, sizeof g_flc_s); g_flc_used = 0;
    for (uint32_t j = 0; j < nk; j++) {
        uint32_t i = (keep[j].sid * 2654435761u) & (FLC_N - 1);
        while (g_flc_s[i].used) i = (i + 1) & (FLC_N - 1);
        g_flc_s[i] = keep[j]; g_flc_used++;
    }
    g_flc_tn = g_flc_t_over = g_flc_fn = g_flc_f_over = 0;
    g_flc_pres = g_flc_pres_client = g_flc_pres_ownonly = g_flc_pres_none = g_flc_pres_nobig = 0;
    g_flc_same = g_flc_diff = g_flc_big_switch = 0; memset(g_flc_cls_pres, 0, sizeof g_flc_cls_pres);
    os_unfair_lock_unlock(&g_flc_lock);
}
