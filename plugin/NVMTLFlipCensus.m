// NVMTLFlipCensus.m - included by NVMTLObjects.m. 2026-11-25 GFX power: who drives WindowServer's presents, and what
// each present draws into the screen. Census only, no behaviour change. Off unless NVMTL_FLIP_CENSUS=1 (environment or
// /Library/GPUBundles/nvmtl/nvrm610.conf), and then only in WindowServer (NVMTL_FLIP_CENSUS_ALL=1: every Metal process);
// off, every hook is one predictable branch. The draw-area counters in nvmtl_vk.c follow the same switch.
//
// Per present (commitAndWaitUntilSubmitted): which IOSurfaces were sampled by a fragment shader with a seed that changed
// since the last time this process sampled them (a client drew; our plugin bumps the seed on GPU writes, CPU writers bump
// it on unlock), and whether any of those was a client's (not a surface this process renders into, not a WindowServer
// name). Per full-screen pass (attachment >= 4 Mpx): target surface, its Metal load action, its fragment shaders.
// SPIR-V of the hashed shaders is written only with NVMTL_FLIP_CENSUS_SPV=1 (or the pass timer), off the render thread,
// to $TMPDIR/nvmtl-pstat (per user; WindowServer's own temp dir).
// Cost: IOSurfaceGetSeed / GetID / size / pixel format are user-space reads (no syscall); the IOSurface name is read once
// per surface, outside the lock; one unfair lock per sampled surface bind. The 30 s report copies what it prints under
// the lock and prints from a utility queue, so the committing thread never waits on the log. The pid owning a surface is
// not readable from this process without a private IOSurfaceRoot selector, so the report names surfaces by id + IOSurface
// name + size and tools/890m/flip-census/resolve.sh maps ids to processes with /usr/bin/IOSDebug (unprivileged).

#define FLC_N 2048                       // surface slots (power of two)
#define FLC_TGT 16                       // distinct full-screen target surfaces per window
#define FLC_FS 32                        // distinct fragment shaders on full-screen passes per window
#define FLC_TOP 16                       // surfaces printed per window
#define FLC_KEEP 4                       // windows a surface is kept without being sampled (its name is not re-read)
enum { FLC_TINY, FLC_SMALL, FLC_MED, FLC_LARGE, FLC_FULL, FLC_NCLS };
static const char *const kFlcCls[FLC_NCLS] = { "tiny<64K", "small<0.5M", "medium<2M", "large<4M", "full>=4M" };

typedef struct { uint32_t sid, w, h, pf, seed; uint64_t chg_seq, smp_seq; uint32_t chg, smp; uint8_t used, own, cls, idle; char name[40]; } flc_surf;
typedef struct { uint32_t sid, passes, load[3]; uint8_t scanout; char name[32]; } flc_tgt;
typedef struct { uint32_t fs, passes, load[3]; char name[240]; } flc_fs;   // name: non-zero function constants of the variant

static int g_flc = -1, g_flc_spv;
static const void *g_flc_nz_key;            // associated-object key of a pipeline's constants summary (NVMTLObjects.m)
static os_unfair_lock g_flc_lock = OS_UNFAIR_LOCK_INIT;
static flc_surf g_flc_s[FLC_N];
static uint32_t g_flc_used;
static flc_tgt g_flc_t[FLC_TGT]; static uint32_t g_flc_tn, g_flc_t_over;
static flc_fs g_flc_f[FLC_FS]; static uint32_t g_flc_fn, g_flc_f_over, g_flc_fs_full;
static uint64_t g_flc_seq = 1;                         // current present (incremented at commitAndWaitUntilSubmitted)
static uint32_t g_flc_cur_client, g_flc_cur_own;        // changed surfaces sampled in the current present
static uint32_t g_flc_cur_big, g_flc_last_big_sid, g_flc_prev_present_sid;
static uint32_t g_flc_pres, g_flc_pres_client, g_flc_pres_ownonly, g_flc_pres_none, g_flc_pres_nobig;
static uint32_t g_flc_same, g_flc_diff, g_flc_big_switch, g_flc_cls_pres[FLC_NCLS];
static uint8_t g_flc_cur_cls;                           // bit per class: a changed client surface of that class this present

static inline int flc_knob(const char *k) { const char *e = getenv(k); return e && *e && *e != '0'; }
static inline int nvmtl_flc_on(void)
{
    if (__builtin_expect(g_flc < 0, 0)) {
        const char *me = getprogname();
        const int on = flc_knob("NVMTL_FLIP_CENSUS") && ((me && !strcmp(me, "WindowServer")) || flc_knob("NVMTL_FLIP_CENSUS_ALL"));
        g_flc_spv = on && flc_knob("NVMTL_FLIP_CENSUS_SPV");
        g_flc = on;
        if (on) nvlog("flip census: on in %s - per-present client surfaces and full-screen targets, reported with the frame census%s",
                      me ?: "?", g_flc_spv ? "; SPIR-V of hashed shaders dumped (NVMTL_FLIP_CENSUS_SPV)" : "");
    }
    return g_flc;
}
static inline uint8_t nvmtl_flc_cls(uint64_t px)
{ return px < 65536 ? FLC_TINY : px < 524288 ? FLC_SMALL : px < 2097152 ? FLC_MED : px < 4194304 ? FLC_LARGE : FLC_FULL; }
static int nvmtl_flc_own_name(const char *n)
{ return !strncmp(n, "SL.", 3) || strstr(n, "CoreDisplay") || strstr(n, "flattened") || strstr(n, "WindowServer") || strstr(n, "CaptureSurface"); }

// lock NOT held: IOSurfaceCopyValue takes the surface's own lock and allocates
static void nvmtl_flc_name(IOSurfaceRef s, char *out, size_t n)
{
    out[0] = 0;
    CFTypeRef v = IOSurfaceCopyValue(s, kIOSurfaceName);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, out, (CFIndex)n, kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    if (!out[0]) strlcpy(out, "-", n);
}
static void nvmtl_flc_init(flc_surf *e, uint32_t sid, uint32_t w, uint32_t h, uint32_t pf, const char *name)
{
    memset(e, 0, sizeof *e); e->used = 1;
    e->sid = sid; e->w = w; e->h = h; e->pf = pf;
    e->cls = nvmtl_flc_cls((uint64_t)w * h);
    strlcpy(e->name, name, sizeof e->name);
    e->own = nvmtl_flc_own_name(e->name) ? 1 : 0;
}
// lock held. A surface not in the table (or an id now naming a different surface: size or pixel format changed) needs
// its name: with name NULL that returns NULL and *need = 1, and the caller reads the name outside the lock.
static flc_surf *nvmtl_flc_find(uint32_t sid, uint32_t w, uint32_t h, uint32_t pf, const char *name, int *fresh, int *need)
{
    *fresh = 0; *need = 0;
    uint32_t i = (sid * 2654435761u) & (FLC_N - 1);
    for (uint32_t k = 0; k < FLC_N; k++, i = (i + 1) & (FLC_N - 1)) {
        flc_surf *e = &g_flc_s[i];
        if (e->used && e->sid == sid) {
            if (e->w == w && e->h == h && e->pf == pf) return e;
            if (!name) { *need = 1; return NULL; }
            nvmtl_flc_init(e, sid, w, h, pf, name); *fresh = 1; return e;   // id reused: start over in the same slot
        }
        if (!e->used) {
            if (g_flc_used >= FLC_N * 3 / 4) return NULL;      // full until the report drops idle surfaces
            if (!name) { *need = 1; return NULL; }
            nvmtl_flc_init(e, sid, w, h, pf, name); g_flc_used++; *fresh = 1; return e;
        }
    }
    return NULL;
}
// Returns with g_flc_lock HELD (entry or NULL). The name of a new surface is read between two lock holds.
static flc_surf *nvmtl_flc_get(IOSurfaceRef s, uint32_t sid, int *fresh)
{
    const uint32_t w = (uint32_t)IOSurfaceGetWidth(s), h = (uint32_t)IOSurfaceGetHeight(s), pf = (uint32_t)IOSurfaceGetPixelFormat(s);
    int need = 0;
    os_unfair_lock_lock(&g_flc_lock);
    flc_surf *e = nvmtl_flc_find(sid, w, h, pf, NULL, fresh, &need);
    if (e || !need) return e;
    os_unfair_lock_unlock(&g_flc_lock);
    char nm[40]; nvmtl_flc_name(s, nm, sizeof nm);
    os_unfair_lock_lock(&g_flc_lock);
    return nvmtl_flc_find(sid, w, h, pf, nm, fresh, &need);   // another thread may have added it meanwhile: found, not re-made
}

// A fragment shader samples this texture (setFragmentTexture).
static void nvmtl_flc_sample(NVMTLTexture *t)
{
    if (!nvmtl_flc_on()) return;
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf), seed = IOSurfaceGetSeed(r->_surf);
    int fresh = 0; flc_surf *e = nvmtl_flc_get(r->_surf, sid, &fresh);
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
    int fresh = 0; flc_surf *e = nvmtl_flc_get(sr->_surf, sid, &fresh);
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
// action la; counted once per pass (the encoder dedupes up to 8 shaders, then reports the pass once with nvmtl_flc_fs_full).
static void nvmtl_flc_fs(uint32_t fs, MTLLoadAction la, const char *fname)
{
    const int li = la == MTLLoadActionLoad ? 0 : la == MTLLoadActionClear ? 1 : 2;
    os_unfair_lock_lock(&g_flc_lock);
    uint32_t k = 0; for (; k < g_flc_fn; k++) if (g_flc_f[k].fs == fs) break;
    if (k == g_flc_fn) { if (g_flc_fn < FLC_FS) { memset(&g_flc_f[k], 0, sizeof g_flc_f[k]); g_flc_f[k].fs = fs; strlcpy(g_flc_f[k].name, fname && *fname ? fname : "?", sizeof g_flc_f[k].name); g_flc_fn++; } else { g_flc_f_over++; k = FLC_FS; } }
    if (k < FLC_FS) { g_flc_f[k].passes++; g_flc_f[k].load[li]++; }
    os_unfair_lock_unlock(&g_flc_lock);
}
static void nvmtl_flc_fs_full(void)
{ os_unfair_lock_lock(&g_flc_lock); g_flc_fs_full++; os_unfair_lock_unlock(&g_flc_lock); }

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

// One window, copied under the lock and printed from a utility queue.
typedef struct {
    double secs;
    uint32_t pres, client, ownonly, none, nobig, same, diff, bswitch, cls[FLC_NCLS], used, tn, t_over, fn, f_over, fs_full, ntop;
    flc_surf top[FLC_TOP]; flc_tgt t[FLC_TGT]; flc_fs f[FLC_FS];
} flc_snap;

static void nvmtl_flc_print(void *ctx)
{
    flc_snap *s = ctx; const double secs = s->secs;
    nvlog_report("flip census %.0f s: %u presents (%.1f/s): %u sampled a changed client surface, %u only changed surfaces of this process "
                 "(offscreen passes it drew itself), %u no changed surface at all (cursor, WindowServer animations, colour pass); "
                 "presents with a changed client surface by its size: %s %u, %s %u, %s %u, %s %u, %s %u; %u surfaces tracked",
                 secs, s->pres, s->pres / secs, s->client, s->ownonly, s->none,
                 kFlcCls[0], s->cls[0], kFlcCls[1], s->cls[1], kFlcCls[2], s->cls[2], kFlcCls[3], s->cls[3], kFlcCls[4], s->cls[4], s->used);
    for (uint32_t i = 0; i < s->ntop; i++)
        nvlog_report("flip census surface 0x%x %ux%u %s <%s>%s: changed in %u presents (%.1f/s), sampled in %u",
                     s->top[i].sid, s->top[i].w, s->top[i].h, kFlcCls[s->top[i].cls], s->top[i].name, s->top[i].own ? " own" : " client",
                     s->top[i].chg, s->top[i].chg / secs, s->top[i].smp);
    nvlog_report("flip census screen targets: presents drawing a >= 4 Mpx target: %u (%u drew none); its last full-screen target was the "
                 "same surface as the previous present's %u times, a different one %u times (process-wide, i.e. one display); "
                 "full-screen target changes between passes %u; %u distinct full-screen targets%s; passes with more than 8 fragment "
                 "shaders %u",
                 s->pres - s->nobig, s->nobig, s->same, s->diff, s->bswitch, s->tn, s->t_over ? " (table full)" : "", s->fs_full);
    for (uint32_t k = 0; k < s->tn; k++)
        nvlog_report("flip census target 0x%x <%s>%s: %u passes, load action Load %u Clear %u DontCare %u",
                     s->t[k].sid, s->t[k].name, s->t[k].scanout ? " scanout" : "", s->t[k].passes, s->t[k].load[0], s->t[k].load[1], s->t[k].load[2]);
    for (uint32_t k = 0; k < s->fn; k++)
        nvlog_report("flip census full-screen fragment shader %08x in %u passes (load Load %u Clear %u DontCare %u); constants %s%s",
                     s->f[k].fs, s->f[k].passes, s->f[k].load[0], s->f[k].load[1], s->f[k].load[2], s->f[k].name,
                     g_flc_spv ? "; SPIR-V in $TMPDIR/nvmtl-pstat" : "");
    if (s->f_over) nvlog_report("flip census: %u more full-screen fragment shaders not listed (table full)", s->f_over);
    free(s);
}

// Called from the 30 s frame census (nvmtl_vk_frame_census, inline in -commit) with the window's length in seconds.
// Under the lock: copy what is printed, reset the counts, age the surface table. The printing runs on a utility queue.
// A window with no present prints nothing.
// Roadmap 3 a1 (2026-11-27): one census window without a restart. NVMTL_FLIP_CENSUS is read once, at the first present,
// so turning it on meant restarting WindowServer and leaving it on. Now, where the knob is off, the 30 s frame census
// looks (one access(2) per 30 s, nothing per present) for a request file in this process's own per-user temp dir,
// named after the process (nvmtl-flipcensus-request.WindowServer; the path is logged once). If it is there: delete it,
// switch the census on for exactly one 30 s window (counters start clean here), print that window, switch it off again.
// WindowServer only unless NVMTL_FLIP_CENSUS_ALL=1, like the knob; compiled out of release builds. While the census is
// off its hooks stay the single predictable branch; the switch is a plain int the hooks already test (a present that
// straddles the switch is counted partly: census only).
static int g_flc_trig;                 // 1 = this window was armed by the request file
static void nvmtl_flc_trigger_poll(void)
{
#if !NVMTL_RELEASE
    static int st = -1; static char req[600];
    if (st < 0) {
        const char *me = getprogname(); char td[512] = "";
        st = 0;
        if (((me && !strcmp(me, "WindowServer")) || flc_knob("NVMTL_FLIP_CENSUS_ALL")) &&
            confstr(_CS_DARWIN_USER_TEMP_DIR, td, sizeof td) > 1 && td[0] == '/') {
            snprintf(req, sizeof req, "%s%snvmtl-flipcensus-request.%s", td, td[strlen(td) - 1] == '/' ? "" : "/", me ?: "unknown");
            nvlog("flip census: off; touch %s to record one 30 s window", req);
            st = 1;
        }
    }
    if (st != 1 || access(req, F_OK) != 0) return;
    unlink(req);
    os_unfair_lock_lock(&g_flc_lock);   // a clean window: nothing from before the switch
    g_flc_tn = g_flc_t_over = g_flc_fn = g_flc_f_over = g_flc_fs_full = 0;
    g_flc_pres = g_flc_pres_client = g_flc_pres_ownonly = g_flc_pres_none = g_flc_pres_nobig = 0;
    g_flc_same = g_flc_diff = g_flc_big_switch = 0; memset(g_flc_cls_pres, 0, sizeof g_flc_cls_pres);
    g_flc_cur_client = g_flc_cur_own = g_flc_cur_big = 0; g_flc_cur_cls = 0;
    g_flc_prev_present_sid = g_flc_last_big_sid = 0;   // no same/switch compare against a present from before the window
    os_unfair_lock_unlock(&g_flc_lock);
    g_flc_trig = 1;
    __atomic_store_n(&g_flc, 1, __ATOMIC_RELEASE);
    nvlog("flip census: request file seen: on for one 30 s window");
#endif
}
static void nvmtl_flc_report(double secs)
{
    if (__builtin_expect(g_flc < 0, 0)) (void)nvmtl_flc_on();
    if (g_flc != 1) { nvmtl_flc_trigger_poll(); return; }
    if (secs <= 0) secs = 1;
    flc_snap *s = NULL;
    os_unfair_lock_lock(&g_flc_lock);
    if (g_flc_pres) s = calloc(1, sizeof *s);
    if (s) {
        s->secs = secs; s->pres = g_flc_pres; s->client = g_flc_pres_client; s->ownonly = g_flc_pres_ownonly; s->none = g_flc_pres_none;
        s->nobig = g_flc_pres_nobig; s->same = g_flc_same; s->diff = g_flc_diff; s->bswitch = g_flc_big_switch;
        memcpy(s->cls, g_flc_cls_pres, sizeof s->cls); s->used = g_flc_used;
        s->tn = g_flc_tn; s->t_over = g_flc_t_over; memcpy(s->t, g_flc_t, g_flc_tn * sizeof g_flc_t[0]);
        s->fn = g_flc_fn; s->f_over = g_flc_f_over; memcpy(s->f, g_flc_f, g_flc_fn * sizeof g_flc_f[0]); s->fs_full = g_flc_fs_full;
        for (uint32_t i = 0; i < FLC_N; i++) {                    // the FLC_TOP most changed, kept sorted by insertion
            const flc_surf *e = &g_flc_s[i];
            if (!e->used || !e->chg) continue;
            uint32_t j;
            if (s->ntop < FLC_TOP) j = s->ntop++;
            else if (s->top[FLC_TOP - 1].chg >= e->chg) continue;
            else j = FLC_TOP - 1;
            while (j > 0 && s->top[j - 1].chg < e->chg) { s->top[j] = s->top[j - 1]; j--; }
            s->top[j] = *e;
        }
    }
    // next window: counts to zero; a surface not sampled for FLC_KEEP windows is dropped (the table is rebuilt only then,
    // so probing stays intact; otherwise it is aged in place)
    int drop = 0;
    for (uint32_t i = 0; i < FLC_N; i++) {
        flc_surf *e = &g_flc_s[i];
        if (!e->used) continue;
        e->idle = e->smp ? 0 : (uint8_t)(e->idle + 1);
        e->chg = e->smp = 0;
        if (e->idle >= FLC_KEEP) drop++;
    }
    if (drop) {
        static flc_surf keep[FLC_N]; uint32_t nk = 0;
        for (uint32_t i = 0; i < FLC_N; i++) if (g_flc_s[i].used && g_flc_s[i].idle < FLC_KEEP) keep[nk++] = g_flc_s[i];
        memset(g_flc_s, 0, sizeof g_flc_s); g_flc_used = 0;
        for (uint32_t j = 0; j < nk; j++) {
            uint32_t i = (keep[j].sid * 2654435761u) & (FLC_N - 1);
            while (g_flc_s[i].used) i = (i + 1) & (FLC_N - 1);
            g_flc_s[i] = keep[j]; g_flc_used++;
        }
    }
    g_flc_tn = g_flc_t_over = g_flc_fn = g_flc_f_over = g_flc_fs_full = 0;
    g_flc_pres = g_flc_pres_client = g_flc_pres_ownonly = g_flc_pres_none = g_flc_pres_nobig = 0;
    g_flc_same = g_flc_diff = g_flc_big_switch = 0; memset(g_flc_cls_pres, 0, sizeof g_flc_cls_pres);
    os_unfair_lock_unlock(&g_flc_lock);
    if (s) dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), s, nvmtl_flc_print);
    if (g_flc_trig) {   // a window armed by the request file: printed above, census off again
        g_flc_trig = 0;
        __atomic_store_n(&g_flc, 0, __ATOMIC_RELEASE);
        nvlog("flip census: requested window done, off again");
    }
}

// SPIR-V (and its function constants) of a hashed shader, for the flip census (NVMTL_FLIP_CENSUS_SPV=1) or the pass
// timer: written on a serial utility queue, never on the render thread.
static void nvmtl_spv_dump_async(NSData *d, uint32_t h, int frag, NVMTLFunction *f)
{
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("nvmtl.spv-dump", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    dispatch_async(q, ^{ @autoreleasepool {
        // per-user temp dir (WindowServer's home is not writable; nothing lands in ~/Library/Caches)
        static NSString *pdir; if (!pdir) { char td[512] = ""; const char *e = getenv("TMPDIR");
            if (e && *e == '/') strlcpy(td, e, sizeof td); else if (confstr(_CS_DARWIN_USER_TEMP_DIR, td, sizeof td) <= 1) strlcpy(td, "/tmp", sizeof td);
            pdir = [[NSString stringWithUTF8String:td] stringByAppendingPathComponent:@"nvmtl-pstat"]; }
        NSString *pth = [pdir stringByAppendingPathComponent:[NSString stringWithFormat:@"%08x.%s.spv", h, frag ? "frag" : "vert"]];
        if ([[NSFileManager defaultManager] fileExistsAtPath:pth]) return;
        [[NSFileManager defaultManager] createDirectoryAtPath:pdir withIntermediateDirectories:YES attributes:nil error:nil];
        [d writeToFile:pth atomically:YES];
        // the function constants that pick this variant of CA's shader (all CA fragment shaders share one name)
        NSMutableDictionary *fcn = [NSMutableDictionary dictionary];   // constant index -> name, from the AIR's reflection
        @try { NSDictionary *cd = [f functionConstantsDictionary];
            for (NSString *nm in cd) { MTLFunctionConstant *c = cd[nm]; if ([c respondsToSelector:@selector(index)]) fcn[@(c.index)] = nm; } } @catch (id x) {}
        NSDictionary *fcd = f->_fc; NSMutableString *t = [NSMutableString stringWithFormat:@"%@ %@\n", f->_fname ?: @"?", f->_stage ?: @""];
        for (NSString *k in [fcd.allKeys sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) { return [@(a.integerValue) compare:@(b.integerValue)]; }]) {
            NSArray *a = fcd[k]; [t appendFormat:@"%@ %@", k, fcn[@(k.integerValue)] ?: @"?"];
            if ([a isKindOfClass:[NSArray class]]) for (id v in a) [t appendFormat:@" %02x", [v unsignedCharValue]];
            [t appendString:@"\n"]; }
        [t writeToFile:[pth stringByAppendingString:@".fc.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    } });
}
