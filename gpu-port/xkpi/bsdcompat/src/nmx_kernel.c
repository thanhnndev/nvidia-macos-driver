/*
 * Kernel basics for the FreeBSD-on-xnu layer: per-thread records (struct thread), the current process record,
 * SYSINIT/SYSUNINIT, the tick clock, CPU identification, preemption control and the lock group.
 *
 * Per-thread records: LinuxKPI hangs Linux's task_struct off FreeBSD's struct thread, and DRM's locks record their
 * owner as a struct thread pointer. xnu has no per-thread storage for kexts, so records live in a hash table keyed by
 * thread_t, created on first use. Kernel threads the port starts drop theirs when they exit (nmx_thread_leave).
 * Records of user threads stay: xnu tells a kext about no thread or process exit, and it recycles thread_t and
 * proc_t, so a record found by its xnu pointer is checked against the owner's identity (thread_tid; pid and start
 * time) and rebuilt for the new owner when it was made for an earlier one (ga403wm Step100: a new WindowServer
 * thread on a recycled thread_t ran with the dead process's Linux task and mm, and amdgpu refused its userptr BOs).
 *
 * curthread finds its record without a lock (ga403wm Step114, Kernel-2026-11-05-111129). It runs on every kfree,
 * sched_pin, sx lock and critical section, so the registry spin lock it used to take was the hottest lock in the
 * driver, taken from all CPUs, often with interrupts off (rcu_read_lock and the FPU window are critical sections,
 * MTX_SPIN mutexes hold interrupts off), while its holder kept interrupts on. xnu's spin lock is not fair, and a
 * holder can be held up by interrupts: a CPU waiting with interrupts off hit the spin lock timeout and the Mac
 * panicked. Now:
 *   - thread records are type-stable: a dropped record goes on a free list and is reused, never freed while the
 *     layer runs, so a lock-free reader can never step onto freed memory; a reader that lands on a record that moved
 *     only misses, and a miss is always checked again under the lock;
 *   - chain links and bucket heads are published with release stores, the lock serializes the writers;
 *   - the registry lock is held with interrupts off on every path, so no holder is ever stretched by an interrupt
 *     while another CPU spins on it; full-table scans take it one bucket at a time;
 *   - in interrupt context curthread uses a per-CPU record and never allocates.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/time.h>
#include <machine/md_var.h>
#include <machine/cpufunc.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

#include <sys/module.h>
#include <sys/sysctl.h>

/* xnu calls a kext may link (checked against macOS 15 with tools/kpi_probe.sh); _disable_preemption and
 * ml_get_max_cpus are not among them */
extern boolean_t ml_at_interrupt_context(void);
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);
extern int cpu_number(void);
extern int nmx_xnu_ncpus(void);
/* exported to kexts but not declared in the SDK; the xnu proc_t is used only as an opaque key */
extern void *current_proc(void);

lck_grp_t *nmx_lck_grp;
int hz = 1000;
int tick = 1000000 / 1000;
sbintime_t tick_sbt = SBT_1S / 1000;
int cold = 0;
int rebooting = 0;
int mp_ncpus = 1;
int mp_ncores = 1;
int mp_maxid = 0;
int smp_started = 1;
cpuset_t all_cpus;
bool efi_boot = true;
struct proc *pageproc = NULL;
int (*lkpi_alloc_current)(struct thread *, int);
char osrelease[OSRELEASELEN] = "24.0.0";

/* ---- tick clock: FreeBSD 'ticks' counts hz per second since kext start ------------------------------------------- */

static uint64_t nmx_boot_abs;
static mach_timebase_info_data_t nmx_tb;

static uint64_t abs_to_ns(uint64_t abs)
{
    return abs * nmx_tb.numer / nmx_tb.denom;
}

int nmx_ticks(void)
{
    return (int)(abs_to_ns(mach_absolute_time() - nmx_boot_abs) / (1000000000ULL / (uint64_t)hz));
}

/* LinuxKPI's jiffies: the same clock, 64 bits wide so it never wraps */
unsigned long nmx_jiffies(void)
{
    return (unsigned long)(abs_to_ns(mach_absolute_time() - nmx_boot_abs) / (1000000000ULL / (uint64_t)hz));
}

/* ---- per-thread and per-process records ---------------------------------------------------------------------------- */

#define NMX_THREAD_BUCKETS 256
struct nmx_thread_rec {
    struct thread td;               /* first member: a struct thread * is a struct nmx_thread_rec * */
    struct nmx_thread_rec *next;
    int transient;                  /* created by a system-call entry; freed when that entry returns */
};
static struct nmx_thread_rec *nmx_threads[NMX_THREAD_BUCKETS];
static struct nmx_thread_rec *nmx_threads_free;     /* dropped records, reused by the next new thread */
static lck_spin_t *nmx_thread_lock;
static boolean_t nmx_thread_lock_intr;              /* the holder's interrupt state, restored at unlock */
static volatile int nmx_thread_count;
/* the records curthread hands out in interrupt context (the interrupted thread's record is not touched there) */
static struct nmx_thread_rec nmx_intr_threads[MAXCPU];
/* a lock-free walk longer than this (a chain that long, or a reader that kept landing on moving records) goes to
 * the locked lookup */
#define NMX_FIND_STEPS 1024

/* the registry lock, always held with interrupts off: other CPUs spin on it with interrupts off too */
static void nmx_tl_lock(void)
{
    boolean_t was = ml_set_interrupts_enabled(FALSE);
    lck_spin_lock(nmx_thread_lock);
    nmx_thread_lock_intr = was;
}

static void nmx_tl_unlock(void)
{
    boolean_t was = nmx_thread_lock_intr;
    lck_spin_unlock(nmx_thread_lock);
    ml_set_interrupts_enabled(was);
}

#define NMX_PROC_BUCKETS 64
struct nmx_proc_rec {
    struct proc p;
    void *xnu;
    struct nmx_proc_rec *next;
};
static struct nmx_proc_rec *nmx_procs[NMX_PROC_BUCKETS];
/*
 * Records of processes that are gone wait here (unlinked, under nmx_thread_lock) for nmx_proc_reap, which detaches
 * their threads' Linux tasks and drops the record's mm reference; it frees them one reap later (the graveyard), so a
 * pointer nmx_proc_find handed out just before cannot be used after it is freed.
 */
static struct nmx_proc_rec *nmx_procs_retired;
static struct nmx_proc_rec *nmx_procs_grave;
static volatile int nmx_procs_new;          /* records made since the last scan for exited processes */
static volatile int nmx_reaping;
/* Linux tasks taken from recycled thread records, released by nmx_proc_reap */
#define NMX_TASKS_PENDING 64
static struct task_struct *nmx_tasks_pending[NMX_TASKS_PENDING];
static int nmx_tasks_npending;
static unsigned nmx_tasks_leaked;           /* pending list full: the task (and its mm reference) is kept */
void (*nmx_task_release_hook)(struct task_struct *ts);
void (*nmx_proc_release_hook)(struct proc *p);
static struct proc nmx_kernproc_rec = { .p_pid = 0, .p_comm = "kernel_task" };
struct proc *nmx_kernproc = &nmx_kernproc_rec;
struct proc *proc0 = &nmx_kernproc_rec;

static unsigned bucket_of(const void *p, unsigned n)
{
    uintptr_t v = (uintptr_t)p;
    return (unsigned)((v >> 4) ^ (v >> 12)) & (n - 1);
}

/*
 * A process's start time, which with its pid would tell the owners of one recycled proc_t apart. It reads 0: the
 * pid alone tells owners apart (a recycled proc_t with the same pid needs a full pid wrap while the record lives).
 * proc_starttime is in the 26.5 SDK but 25G332 does not export it to kexts. A weak import of it does not work there:
 * kmutil bound the missing weak symbol to an address in kernel data, neither NULL nor gOSKextUnresolved, so the
 * check passed and the call jumped into data. Every boot panicked in NMXGPU::start about 7 s in (ga403wm Step128,
 * Kernel-2026-11-07-164643: Kernel NX fault at RIP == CR2, _nmx_thread_get+0x1b6 <- nmx_curthread <- lkpi_kmalloc
 * <- linux_compat_init). So no weak imports in this layer.
 */
static uint64_t nmx_proc_start(void *xnu)
{
    (void)xnu;
    return 0;
}

/* unlinks the record of a process that is gone and queues it for nmx_proc_reap; nmx_thread_lock held */
static void nmx_proc_retire_locked(struct nmx_proc_rec **pp)
{
    struct nmx_proc_rec *r = *pp;
    *pp = r->next;
    r->next = nmx_procs_retired;
    nmx_procs_retired = r;
}

/* the record of a live xnu process (current_proc() or a proc_find() reference), made on first use */
static struct proc *nmx_proc_for(void *xnu)
{
    if (xnu == NULL)
        return nmx_kernproc;
    pid_t pid = proc_pid(xnu);
    uint64_t start = nmx_proc_start(xnu);
    unsigned b = bucket_of(xnu, NMX_PROC_BUCKETS);
    nmx_tl_lock();
    for (struct nmx_proc_rec **pp = &nmx_procs[b]; *pp; pp = &(*pp)->next) {
        struct nmx_proc_rec *r = *pp;
        if (r->xnu != xnu)
            continue;
        if (r->p.p_pid == pid && r->p.nmx_start == start) {
            nmx_tl_unlock();
            return &r->p;
        }
        /* the proc_t was recycled: this record (and its mm) belongs to the process that had it before */
        nmx_proc_retire_locked(pp);
        break;
    }
    nmx_tl_unlock();

    struct nmx_proc_rec *nr = IOMallocZero(sizeof(*nr));
    if (nr == NULL)
        return nmx_kernproc;
    nr->xnu = xnu;
    nr->p.nmx_xnu = xnu;
    nr->p.p_pid = pid;
    nr->p.nmx_start = start;
    proc_name(nr->p.p_pid, nr->p.p_comm, sizeof(nr->p.p_comm));

    nmx_tl_lock();
    for (struct nmx_proc_rec *r = nmx_procs[b]; r; r = r->next)
        if (r->xnu == xnu && r->p.p_pid == pid && r->p.nmx_start == start) {
            nmx_tl_unlock();
            IOFree(nr, sizeof(*nr));
            return &r->p;
        }
    nr->next = nmx_procs[b];
    nmx_procs[b] = nr;
    nmx_procs_new++;
    nmx_tl_unlock();
    return &nr->p;
}

struct proc *nmx_curproc(void)
{
    return nmx_proc_for(current_proc());
}

pid_t nmx_proc_pid(struct proc *p)
{
    return p ? p->p_pid : 0;
}

/* nmx_thread_lock held */
static struct nmx_thread_rec *nmx_thread_find(thread_t xt)
{
    unsigned b = bucket_of(xt, NMX_THREAD_BUCKETS);
    for (struct nmx_thread_rec *r = nmx_threads[b]; r; r = r->next)
        if (r->td.td_xnu == xt)
            return r;
    return NULL;
}

/*
 * Without the lock: only the calling thread makes, rebuilds or drops a record keyed by its own thread_t, so a match
 * is its own record and stays put while it runs. A NULL is only a hint (the caller looks again under the lock).
 */
static struct nmx_thread_rec *nmx_thread_find_lockless(thread_t xt)
{
    unsigned b = bucket_of(xt, NMX_THREAD_BUCKETS);
    unsigned steps = 0;
    for (struct nmx_thread_rec *r = __atomic_load_n(&nmx_threads[b], __ATOMIC_ACQUIRE); r;
         r = __atomic_load_n(&r->next, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&r->td.td_xnu, __ATOMIC_RELAXED) == xt)
            return r;
        if (++steps > NMX_FIND_STEPS)
            break;
    }
    return NULL;
}

/* queues a Linux task for release by nmx_proc_reap; nmx_thread_lock held */
static void nmx_task_pending_locked(struct task_struct *ts)
{
    if (nmx_tasks_npending < NMX_TASKS_PENDING)
        nmx_tasks_pending[nmx_tasks_npending++] = ts;
    else
        nmx_tasks_leaked++;
}

/*
 * The record of a recycled thread_t, rebuilt for the thread that has it now. Only the owning thread rebuilds its
 * record; the earlier owner's Linux task (and through it that process's mm) is released later by nmx_proc_reap,
 * since this may run where nothing may sleep.
 */
static void nmx_thread_recycle(struct nmx_thread_rec *r, uint64_t tid)
{
    struct proc *p = nmx_curproc();     /* takes nmx_thread_lock itself */
    nmx_tl_lock();
    struct task_struct *ts = r->td.td_lkpi_task;
    r->td.td_lkpi_task = NULL;
    r->td.td_proc = p;
    /* ga403wm Step134: td_name is the Linux task's comm (linux_current.c); left empty, amdgpu's VM fault print named
     * no process ("in process  pid N"), which hid the 15:17 PERMISSION_FAULTS writer */
    strlcpy(r->td.td_name, p->p_comm, sizeof(r->td.td_name));
    r->td.td_tid = (int)(tid & 0x7fffffff);
    r->td.td_xnu_tid = tid;
    r->td.td_pinned = 0;
    r->td.td_critnest = 0;
    r->td.td_flags = 0;
    r->td.td_pflags = 0;
    r->td.td_fpop = NULL;
    r->td.td_nmx_open = NULL;
    r->td.td_siglist = 0;
    r->td.td_wchan = NULL;
    r->td.td_wmesg = NULL;
    if (ts != NULL)
        nmx_task_pending_locked(ts);
    nmx_tl_unlock();
}

/* the record for the calling thread, created on first use; 'transient' marks one made by a system-call entry */
static struct thread *nmx_thread_get(int transient, int *created)
{
    thread_t xt = current_thread();
    uint64_t tid = thread_tid(xt);
    *created = 0;
    struct nmx_thread_rec *r = nmx_thread_find_lockless(xt);
    if (r == NULL) {
        nmx_tl_lock();
        r = nmx_thread_find(xt);
        nmx_tl_unlock();
    }
    if (r) {
        if (__builtin_expect(r->td.td_xnu_tid != tid, 0))
            nmx_thread_recycle(r, tid);
        return &r->td;
    }

    /* a dropped record if there is one, else a new one */
    nmx_tl_lock();
    struct nmx_thread_rec *nr = nmx_threads_free;
    if (nr != NULL)
        nmx_threads_free = nr->next;
    nmx_tl_unlock();
    if (nr != NULL) {
        /* a stale lock-free reader may still look at it: the link goes first and in one store, and the key is
         * NULL (cleared when the record was dropped) until the record is complete */
        __atomic_store_n(&nr->next, NULL, __ATOMIC_RELAXED);
        bzero(&nr->td, sizeof(nr->td));
    } else {
        nr = IOMallocZero(sizeof(*nr));
        if (nr == NULL)
            panic("NullMoth xkpi: no memory for a thread record");
    }
    nr->td.td_proc = nmx_curproc();
    strlcpy(nr->td.td_name, nr->td.td_proc->p_comm, sizeof(nr->td.td_name));   /* Step134: the task's comm */
    nr->td.td_tid = (int)(tid & 0x7fffffff);
    nr->td.td_xnu_tid = tid;
    nr->td.td_priority = PZERO;
    nr->td.td_base_pri = PZERO;
    nr->transient = transient;
    __atomic_store_n(&nr->td.td_xnu, xt, __ATOMIC_RELAXED);

    nmx_tl_lock();
    r = nmx_thread_find(xt);
    if (r == NULL) {
        unsigned b = bucket_of(xt, NMX_THREAD_BUCKETS);
        __atomic_store_n(&nr->next, nmx_threads[b], __ATOMIC_RELAXED);
        __atomic_store_n(&nmx_threads[b], nr, __ATOMIC_RELEASE);
        nmx_thread_count++;
        r = nr;
        *created = 1;
    } else {
        /* cannot happen (only this thread makes its record); keep the spare */
        __atomic_store_n(&nr->td.td_xnu, NULL, __ATOMIC_RELAXED);
        nr->next = nmx_threads_free;
        nmx_threads_free = nr;
    }
    nmx_tl_unlock();
    return &r->td;
}

struct thread *nmx_curthread(void)
{
    if (__builtin_expect(ml_at_interrupt_context(), 0)) {
        /* interrupts are off here, so this CPU's record cannot change hands; nothing is allocated */
        struct thread *td = &nmx_intr_threads[cpu_number() % MAXCPU].td;
        td->td_proc = nmx_kernproc;
        td->td_intr_nesting_level = 1;
        return td;
    }
    int created;
    struct thread *td = nmx_thread_get(0, &created);
    if (td->td_intr_nesting_level != 0)
        td->td_intr_nesting_level = 0;
    return td;
}

/* unlinks the calling thread's record onto the free list; LinuxKPI's task_struct must already be released */
static void nmx_thread_drop_current(void)
{
    thread_t xt = current_thread();
    unsigned b = bucket_of(xt, NMX_THREAD_BUCKETS);
    nmx_tl_lock();
    for (struct nmx_thread_rec **pp = &nmx_threads[b]; *pp; pp = &(*pp)->next) {
        struct nmx_thread_rec *found = *pp;
        if (found->td.td_xnu == xt) {
            __atomic_store_n(pp, found->next, __ATOMIC_RELEASE);
            nmx_thread_count--;
            /* the record is not freed: a lock-free reader may be on it; it keeps its link until reuse */
            __atomic_store_n(&found->td.td_xnu, NULL, __ATOMIC_RELAXED);
            found->transient = 0;
            /* free-list link: a reader that follows it only meets NULL keys */
            __atomic_store_n(&found->next, nmx_threads_free, __ATOMIC_RELEASE);
            nmx_threads_free = found;
            break;
        }
    }
    nmx_tl_unlock();
}

/*
 * System-call entry and exit for the device-node glue: enter returns non-zero when it created the record, and leave
 * frees it again (after LinuxKPI's per-thread state is released through the hook), unless the thread is one of ours.
 */
void (*nmx_thread_release_hook)(struct thread *td);
int nmx_thread_enter(void)
{
    int created;
    (void)nmx_thread_get(1, &created);
    return created;
}

void nmx_thread_leave(int created)
{
    if (!created)
        return;
    int c2;
    struct thread *td = nmx_thread_get(1, &c2);
    if (nmx_thread_release_hook && td->td_lkpi_task)
        nmx_thread_release_hook(td);
    nmx_thread_drop_current();
}

/*
 * Called by the device-node glue as an entry point returns: a critical section the driver left open must not go back
 * to user space with interrupts off (Step33c) - log it and close it.
 */
void nmx_thread_crit_check(const char *where)
{
    if (ml_at_interrupt_context())
        return;
    struct thread *td = nmx_curthread();
    if (td->td_critnest != 0) {
        printf("NullMoth xkpi: %s returned with critical nesting %d - closed\n", where, td->td_critnest);
        td->td_critnest = 0;
        ml_set_interrupts_enabled((boolean_t)td->td_crit_saved);
    }
    if (td->td_pinned != 0) {
        printf("NullMoth xkpi: %s returned with sched_pin nesting %d - closed\n", where, td->td_pinned);
        td->td_pinned = 0;
    }
}

void nmx_thread_exit_cleanup(void)
{
    nmx_thread_leave(1);
}

struct thread *tdfind(lwpid_t tid, pid_t pid)
{
    struct thread *found = NULL;
    /* one bucket per lock hold: interrupts are off while it is held */
    for (unsigned b = 0; b < NMX_THREAD_BUCKETS && !found; b++) {
        nmx_tl_lock();
        for (struct nmx_thread_rec *r = nmx_threads[b]; r; r = r->next)
            if (r->td.td_tid == tid && (pid == -1 || nmx_proc_pid(r->td.td_proc) == pid)) { found = &r->td; break; }
        nmx_tl_unlock();
    }
    return found;
}

/* ---- preemption and CPU ------------------------------------------------------------------------------------------- */

/*
 * Critical sections: xnu does not let kexts disable preemption, so a critical section runs with interrupts off on
 * this CPU, which also rules out preemption and migration. The interrupt state from the outermost entry is restored
 * at the outermost exit.
 *
 * Nesting is counted per thread (ga403wm, docs/890M-TRACE.md Step33c). Every LinuxKPI spin_lock enters one
 * (local_bh_disable -> sched_pin), and a spin_lock taken inside another can block in lck_mtx_lock and resume on
 * another CPU: with a per-CPU count the exit then decremented the new CPU's count, the old CPU's stayed raised, and
 * the next thread to leave a critical section there never got its interrupts back - a process exiting with
 * interrupts off panicked in workq_exit ("unsafe thread_call_cancel_wait"). The thread record is looked up before
 * interrupts go off (the outermost entry may create it).
 */
void nmx_critical_enter(void)
{
    struct thread *td = nmx_curthread();
    boolean_t was = ml_set_interrupts_enabled(FALSE);
    if (td->td_critnest++ == 0)
        td->td_crit_saved = was;
}

void nmx_critical_exit(void)
{
    struct thread *td = nmx_curthread();
    if (td->td_critnest <= 0)
        panic("NullMoth xkpi: critical_exit without critical_enter");
    if (--td->td_critnest == 0)
        ml_set_interrupts_enabled((boolean_t)td->td_crit_saved);
}

int nmx_in_critical(void)
{
    struct thread *td;
    if (ml_at_interrupt_context())
        return 1;
    td = nmx_curthread();
    return (td->td_critnest > 0 || td->td_pinned > 0) ? 1 : 0;
}

int nmx_curcpu(void)
{
    return cpu_number();
}

/*
 * sched_pin only counts. It used to be a critical section (interrupts off), and every LinuxKPI spin_lock pins, so a
 * lock held across a long wait kept this CPU's interrupts off for as long: amdgpu's MES queue reset polls a fence under
 * its ring lock for up to two seconds, another CPU's TLB shootdown then timed out and the Mac panicked ("NMIPI for
 * unresponsive processor: TLB flush timeout", Kernel-2026-10-18-153556). A lock is a mutex here, which excludes
 * other threads without needing the CPU to itself; the pin is only a marker for in_atomic-style checks. Code that
 * really needs this CPU to itself (the FPU window, seqc) calls critical_enter.
 */
void sched_pin(void)
{
    nmx_curthread()->td_pinned++;
}

void sched_unpin(void)
{
    struct thread *td = nmx_curthread();
    if (td->td_pinned <= 0)
        panic("NullMoth xkpi: sched_unpin without sched_pin");
    td->td_pinned--;
}

void sched_prio(struct thread *td, u_char prio) { if (td) td->td_priority = prio; }
void sched_add(struct thread *td, int flags) { }
int sched_is_bound(struct thread *td) { return 0; }
void sched_bind(struct thread *td, int cpu) { }
void sched_unbind(struct thread *td) { }

void sched_relinquish(struct thread *td)
{
    mi_switch(SW_VOL);
}

void kern_yield(int prio)
{
    mi_switch(0);
}

/* yield: a one-microsecond timed wait always gives the CPU to another runnable thread */
void mi_switch(int flags)
{
    uint64_t deadline;
    clock_interval_to_deadline(1, NSEC_PER_USEC, &deadline);
    assert_wait_deadline((event_t)&deadline, THREAD_UNINT, deadline);
    thread_block(THREAD_CONTINUE_NULL);
}

void maybe_yield(void)
{
    mi_switch(SW_VOL);
}

int rtp_to_pri(struct rtprio *rtp, struct thread *td)
{
    /* Linux's sched_set_fifo: the request is recorded; xnu schedules kernel threads at kernel priority already */
    if (td)
        td->td_priority = (u_char)(PRI_MIN_REALTIME + (rtp ? rtp->prio : 0));
    return 0;
}

unsigned cpu_feature, cpu_feature2, cpu_stdext_feature, cpu_stdext_feature2, cpu_high, cpu_id, cpu_vendor_id;
unsigned amd_feature, amd_feature2;
char cpu_vendor[20];
int cpu_clflush_line_size = 64;

void nmx_cpu_identify(void)
{
    unsigned r[4];
    do_cpuid(0, r);
    cpu_high = r[0];
    memcpy(cpu_vendor, &r[1], 4);
    memcpy(cpu_vendor + 4, &r[3], 4);
    memcpy(cpu_vendor + 8, &r[2], 4);
    cpu_vendor[12] = 0;
    cpu_vendor_id = strcmp(cpu_vendor, "GenuineIntel") == 0 ? CPU_VENDOR_INTEL :
                    strcmp(cpu_vendor, "AuthenticAMD") == 0 ? CPU_VENDOR_AMD :
                    strcmp(cpu_vendor, "HygonGenuine") == 0 ? CPU_VENDOR_HYGON : 0;
    do_cpuid(1, r);
    cpu_id = r[0];
    cpu_feature = r[3];
    cpu_feature2 = r[2];
    cpu_clflush_line_size = (int)(((r[1] >> 8) & 0xff) * 8);
    if (cpu_high >= 7) {
        cpuid_count(7, 0, r);
        cpu_stdext_feature = r[1];
        cpu_stdext_feature2 = r[2];
    }
    do_cpuid(0x80000000, r);
    if (r[0] >= 0x80000001) {
        do_cpuid(0x80000001, r);
        amd_feature = r[3];
        amd_feature2 = r[2];
    }
}

/* ---- SYSINIT / SYSUNINIT ------------------------------------------------------------------------------------------ */

extern const struct sysinit *const nmx_sysinit_start __asm("section$start$__DATA$nmx_sysinit");
extern const struct sysinit *const nmx_sysinit_end __asm("section$end$__DATA$nmx_sysinit");

static int sysinit_count(void)
{
    return (int)(&nmx_sysinit_end - &nmx_sysinit_start);
}

static void sysinit_sorted(const struct sysinit **out, int n, int uninit)
{
    int k = 0;
    for (const struct sysinit *const *p = &nmx_sysinit_start; p < &nmx_sysinit_end; p++)
        if ((*p)->uninit == uninit)
            out[k++] = *p;
    /* insertion sort by (subsystem, order): a few hundred entries at most, run once */
    for (int i = 1; i < k; i++) {
        const struct sysinit *v = out[i];
        int j = i - 1;
        while (j >= 0 && (out[j]->subsystem > v->subsystem ||
                          (out[j]->subsystem == v->subsystem && out[j]->order > v->order))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = v;
    }
    for (int i = k; i < n; i++)
        out[i] = NULL;
}

int nmx_sysinit_ran;

int nmx_sysinit_run(void)
{
    int n = sysinit_count();
    if (n <= 0) {
        /* a missing set means every module_init and static lock silently did nothing: refuse to start */
        printf("NullMoth xkpi: SYSINIT set is empty (%d) - the build lost its initialisers; not starting\n", n);
        return ENOEXEC;
    }
    const struct sysinit **v = IOMallocZero(sizeof(*v) * (size_t)n);
    if (v == NULL)
        return ENOMEM;
    sysinit_sorted(v, n, 0);
    for (int i = 0; i < n && v[i]; i++) {
        v[i]->func(v[i]->udata);
        nmx_sysinit_ran++;
    }
    IOFree(v, sizeof(*v) * (size_t)n);
    printf("NullMoth xkpi: ran %d SYSINITs\n", nmx_sysinit_ran);
    return 0;
}

void nmx_sysuninit_run(void)
{
    int n = sysinit_count();
    if (n <= 0)
        return;
    const struct sysinit **v = IOMallocZero(sizeof(*v) * (size_t)n);
    if (v == NULL)
        return;
    sysinit_sorted(v, n, 1);
    int k = 0;
    while (k < n && v[k])
        k++;
    for (int i = k - 1; i >= 0; i--)
        v[i]->func(v[i]->udata);
    IOFree(v, sizeof(*v) * (size_t)n);
}

void nmx_module_event(void *arg)
{
    moduledata_t *md = arg;
    if (md && md->evhand) {
        int err = md->evhand(NULL, MOD_LOAD, md->priv);
        if (err)
            printf("NullMoth xkpi: module %s failed to load: %d\n", md->name ? md->name : "?", err);
    }
}

/* ---- start/stop of the whole layer -------------------------------------------------------------------------------- */

/* FreeBSD's tsc_freq (machine/clock.h): drivers scale clocks by it */
uint64_t tsc_freq;
extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);

int nmx_kernel_init(void)
{
    clock_timebase_info(&nmx_tb);
    nmx_boot_abs = mach_absolute_time();
    nmx_lck_grp = lck_grp_alloc_init("com.nullmoth.xkpi", LCK_GRP_ATTR_NULL);
    if (nmx_lck_grp == NULL)
        return ENOMEM;
    nmx_thread_lock = lck_spin_alloc_init(nmx_lck_grp, LCK_ATTR_NULL);
    if (nmx_thread_lock == NULL)
        return ENOMEM;
    mp_ncpus = nmx_xnu_ncpus();
    mp_ncores = mp_ncpus;
    mp_maxid = mp_ncpus - 1;
    CPU_ZERO(&all_cpus);
    for (int i = 0; i < mp_ncpus && i < CPU_SETSIZE; i++)
        CPU_SET(i, &all_cpus);
    nmx_cpu_identify();
    size_t len = sizeof(tsc_freq);
    if (sysctlbyname("machdep.tsc.frequency", &tsc_freq, &len, NULL, 0) != 0)
        tsc_freq = 0;
    return 0;
}

void nmx_kernel_fini(void)
{
    if (nmx_thread_lock) {
        for (unsigned b = 0; b < NMX_THREAD_BUCKETS; b++) {
            struct nmx_thread_rec *r = nmx_threads[b];
            while (r) { struct nmx_thread_rec *n = r->next; IOFree(r, sizeof(*r)); r = n; }
            nmx_threads[b] = NULL;
        }
        for (struct nmx_thread_rec *r = nmx_threads_free, *n; r; r = n) { n = r->next; IOFree(r, sizeof(*r)); }
        nmx_threads_free = NULL;
        for (unsigned b = 0; b < NMX_PROC_BUCKETS; b++) {
            struct nmx_proc_rec *r = nmx_procs[b];
            while (r) { struct nmx_proc_rec *n = r->next; IOFree(r, sizeof(*r)); r = n; }
            nmx_procs[b] = NULL;
        }
        struct nmx_proc_rec *lists[2] = { nmx_procs_retired, nmx_procs_grave };
        for (int i = 0; i < 2; i++)
            for (struct nmx_proc_rec *r = lists[i], *n; r; r = n) { n = r->next; IOFree(r, sizeof(*r)); }
        nmx_procs_retired = nmx_procs_grave = NULL;
        lck_spin_free(nmx_thread_lock, nmx_lck_grp);
        nmx_thread_lock = NULL;
    }
    if (nmx_lck_grp) {
        lck_grp_free(nmx_lck_grp);
        nmx_lck_grp = NULL;
    }
}

/* CPU count from the kernel's own sysctl (kexts may call sysctlbyname) */
int nmx_xnu_ncpus(void)
{
    extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);
    int n = 0;
    size_t len = sizeof(n);
    if (sysctlbyname("hw.logicalcpu_max", &n, &len, NULL, 0) != 0 || n <= 0)
        n = 1;
    return n > MAXCPU ? MAXCPU : n;
}

int nmx_thread_records(void)
{
    return nmx_thread_count;
}

/* ---- signals: LinuxKPI reads pending signals right after PROC_LOCK, which refreshes them from xnu ------------------ */

extern int proc_issignal(int pid, sigset_t mask);
extern void proc_signal(int pid, int signum);

void nmx_proc_refresh_signals(struct proc *p)
{
    if (p == NULL || p->nmx_xnu == NULL) {
        if (p)
            p->p_siglist = 0;
        return;
    }
    sigset_t pending = 0;
    if (proc_issignal(p->p_pid, sigmask(SIGKILL)))
        pending |= sigmask(SIGKILL);
    /* any other deliverable signal: one representative bit is all signal_pending() needs */
    if (proc_issignal(p->p_pid, ~sigmask(SIGKILL)))
        pending |= sigmask(SIGINT);
    p->p_siglist = pending;
}

void tdsignal(struct thread *td, int sig)
{
    if (td && td->td_proc && td->td_proc->nmx_xnu)
        proc_signal(td->td_proc->p_pid, sig);
}

/* ---- helpers for LinuxKPI's per-thread state (the linux_current fork) ---------------------------------------------- */

/* xnu's proc_find/proc_exiting/proc_rele (declared by xnu with its own proc_t); the pointer is only a key here */

int linux_alloc_current_noop(struct thread *td, int flags)
{
    return 0;
}

/* the port's record for a running process, or NULL; looked up through xnu so a stale pid finds nothing */
struct proc *nmx_proc_find(pid_t pid)
{
    void *xp = (void *)proc_find(pid);
    if (xp == NULL)
        return NULL;
    struct proc *p = nmx_proc_for(xp);
    proc_rele((proc_t)xp);
    return p;
}

bool nmx_proc_exiting(struct proc *p)
{
    if (p == NULL || p->nmx_xnu == NULL)
        return false;
    void *xp = (void *)proc_find(p->p_pid);
    if (xp == NULL)
        return true;            /* already gone */
    bool exiting = proc_exiting((proc_t)xp) != 0;
    proc_rele((proc_t)xp);
    return exiting;
}

/* any thread of the process that has Linux state; with 'get' its task_struct gains a reference under the lock */
struct task_struct *nmx_proc_any_task(struct proc *p, bool get)
{
    extern void nmx_task_get(struct task_struct *ts);
    struct task_struct *ts = NULL;
    for (unsigned b = 0; b < NMX_THREAD_BUCKETS && ts == NULL; b++) {
        nmx_tl_lock();
        for (struct nmx_thread_rec *r = nmx_threads[b]; r; r = r->next)
            if (r->td.td_proc == p && r->td.td_lkpi_task != NULL) {
                ts = r->td.td_lkpi_task;
                if (get)
                    nmx_task_get(ts);
                break;
            }
        nmx_tl_unlock();
    }
    return ts;
}

/*
 * Releases what earlier owners of recycled records left, and the records of processes that exited; called by
 * LinuxKPI where it may sleep (linux_alloc_current with M_WAITOK). One caller at a time; cheap when there is nothing
 * to do. A process that exits is noticed when a new process record appears (the next process to use the driver).
 */
void nmx_proc_reap(void)
{
    if (nmx_procs_new == 0 && nmx_procs_retired == NULL && nmx_procs_grave == NULL && nmx_tasks_npending == 0)
        return;
    if (__atomic_exchange_n(&nmx_reaping, 1, __ATOMIC_ACQUIRE))
        return;

    /* 1. records whose process is gone (its proc_t not recycled yet) */
    if (__atomic_exchange_n(&nmx_procs_new, 0, __ATOMIC_ACQ_REL) != 0) {
        for (unsigned b = 0; b < NMX_PROC_BUCKETS; b++) {
            enum { MAXB = 16 };
            struct { struct nmx_proc_rec *r; void *xnu; pid_t pid; uint64_t start; } c[MAXB];
            int n = 0;
            nmx_tl_lock();
            for (struct nmx_proc_rec *r = nmx_procs[b]; r && n < MAXB; r = r->next) {
                c[n].r = r; c[n].xnu = r->xnu; c[n].pid = r->p.p_pid; c[n].start = r->p.nmx_start;
                n++;
            }
            if (n == MAXB)
                nmx_procs_new = 1;      /* a long chain: the rest at the next reap */
            nmx_tl_unlock();
            for (int i = 0; i < n; i++) {
                void *xp = (void *)proc_find(c[i].pid);
                bool alive = xp == c[i].xnu && nmx_proc_start(xp) == c[i].start;
                if (xp != NULL)
                    proc_rele((proc_t)xp);
                if (alive)
                    continue;
                nmx_tl_lock();
                for (struct nmx_proc_rec **pp = &nmx_procs[b]; *pp; pp = &(*pp)->next)
                    if (*pp == c[i].r) { nmx_proc_retire_locked(pp); break; }
                nmx_tl_unlock();
            }
        }
    }

    /* 2. free the graveyard (retired one reap ago), then retire: detach the threads, drop the mm */
    nmx_tl_lock();
    struct nmx_proc_rec *grave = nmx_procs_grave;
    struct nmx_proc_rec *retired = nmx_procs_retired;
    nmx_procs_grave = NULL;
    nmx_procs_retired = NULL;
    nmx_tl_unlock();
    /* 'retired' is ours now; the thread table is walked one bucket per lock hold (interrupts are off meanwhile) */
    if (retired != NULL)
        for (unsigned b = 0; b < NMX_THREAD_BUCKETS; b++) {
            nmx_tl_lock();
            for (struct nmx_thread_rec *t = nmx_threads[b]; t; t = t->next)
                for (struct nmx_proc_rec *r = retired; r; r = r->next)
                    if (t->td.td_proc == &r->p) {
                        /* a thread of the dead process: its record waits for its thread_t to be recycled */
                        t->td.td_proc = nmx_kernproc;
                        if (t->td.td_lkpi_task != NULL) {
                            nmx_task_pending_locked(t->td.td_lkpi_task);
                            t->td.td_lkpi_task = NULL;
                        }
                        break;
                    }
            nmx_tl_unlock();
        }
    for (struct nmx_proc_rec *r = grave, *n; r; r = n) {
        n = r->next;
        IOFree(r, sizeof(*r));
    }
    struct nmx_proc_rec *last = NULL;
    for (struct nmx_proc_rec *r = retired; r; r = r->next) {
        if (nmx_proc_release_hook)
            nmx_proc_release_hook(&r->p);
        last = r;
    }
    if (last != NULL) {
        nmx_tl_lock();
        last->next = nmx_procs_grave;
        nmx_procs_grave = retired;
        nmx_tl_unlock();
    }

    /* 3. the Linux tasks of earlier owners */
    for (;;) {
        struct task_struct *ts = NULL;
        nmx_tl_lock();
        if (nmx_tasks_npending > 0)
            ts = nmx_tasks_pending[--nmx_tasks_npending];
        nmx_tl_unlock();
        if (ts == NULL)
            break;
        if (nmx_task_release_hook)
            nmx_task_release_hook(ts);
    }
    __atomic_store_n(&nmx_reaping, 0, __ATOMIC_RELEASE);
}

/* detaches every thread's Linux state and releases it outside the registry lock (unload) */
void nmx_threads_release_all(void (*release)(struct thread *))
{
    for (;;) {
        struct thread *td = NULL;
        for (unsigned b = 0; b < NMX_THREAD_BUCKETS && td == NULL; b++) {
            nmx_tl_lock();
            for (struct nmx_thread_rec *r = nmx_threads[b]; r; r = r->next)
                if (r->td.td_lkpi_task != NULL) { td = &r->td; break; }
            nmx_tl_unlock();
        }
        if (td == NULL)
            return;
        release(td);            /* clears td_lkpi_task */
    }
}
