// Userspace stress test of the xkpi thread registry (run.sh extracts region.c from nmx_kernel.c).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <os/lock.h>
typedef void *thread_t; typedef int pid_t_; typedef int lwpid_t; typedef unsigned sigset_t_; typedef int boolean_t;
#define FALSE 0
#define MAXCPU 64
#define PZERO 80
struct task_struct; struct file; struct nmx_open; struct ucred;
struct proc { int p_pid; char p_comm[20]; void *nmx_xnu; uint64_t nmx_start; int p_siglist; };
struct thread { thread_t td_xnu; struct proc *td_proc; struct task_struct *td_lkpi_task; int td_tid; unsigned char td_priority, td_base_pri;
  int td_pinned, td_critnest, td_intr_nesting_level, td_flags, td_owepreempt, td_inhibitors; struct { int code; long args[8]; } td_sa;
  struct file *td_fpop; struct nmx_open *td_nmx_open; struct ucred *td_ucred; unsigned td_siglist, td_sigmask; int td_pflags; char td_name[32];
  void *td_wchan; const char *td_wmesg; int td_crit_saved; uint64_t td_xnu_tid; };
typedef os_unfair_lock lck_spin_t;
static void lck_spin_lock(lck_spin_t *l) { os_unfair_lock_lock(l); }
static void lck_spin_unlock(lck_spin_t *l) { os_unfair_lock_unlock(l); }
static boolean_t ml_set_interrupts_enabled(boolean_t e) { return 1; }
static boolean_t ml_at_interrupt_context(void) { return 0; }
static int cpu_number(void) { return 0; }
static void *IOMallocZero(size_t n) { return calloc(1, n); }
static void IOFree(void *p, size_t n) { memset(p, 0xa5, n); free(p); }   // poison: a reader on freed memory would crash
static void panic(const char *m, ...) { fprintf(stderr, "panic %s\n", m); abort(); }
static void printf_(const char *m, ...) {}
#define printf(...) 0
// fake xnu: a small pool of thread_t values reused by successive "threads" (recycling), each incarnation a new tid
static char pool[24][64];
static __thread thread_t cur_xt; static __thread uint64_t cur_tid;
static thread_t current_thread(void) { return cur_xt; }
static uint64_t thread_tid(thread_t t) { return cur_tid; }
static char theproc[64];
static void *current_proc(void) { return theproc; }
static int proc_pid(void *p) { return 42; }
static void proc_name(int pid, char *b, int n) { strncpy(b, "test", n); }
static struct proc nmx_kernproc_rec_ = { 0 };
#define pid_t int
#define bzero(p,n) memset(p,0,n)
#include "region.c"
#undef printf
static _Atomic uint64_t next_tid = 1; static _Atomic long lookups, mismatches;
static _Atomic int slot_busy[24];
static void *worker(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    for (int life = 0; life < 4000; life++) {
        int s; do { s = rand_r(&seed) % 24; int z = 0; if (atomic_compare_exchange_strong(&slot_busy[s], &z, 1)) break; } while (1);
        cur_xt = pool[s]; cur_tid = atomic_fetch_add(&next_tid, 1);
        int created = nmx_thread_enter();
        for (int i = 0; i < 200; i++) {
            struct thread *td = nmx_curthread();
            atomic_fetch_add(&lookups, 1);
            if (td->td_xnu != cur_xt || td->td_xnu_tid != cur_tid) atomic_fetch_add(&mismatches, 1);
            td->td_pinned++; td->td_pinned--;
        }
        if (rand_r(&seed) & 1) nmx_thread_leave(created ? created : 1);   // dropped (kthread exit / syscall leave)
        // else: the thread dies with its record kept (user thread); the next owner of the slot recycles it
        atomic_store(&slot_busy[s], 0);
    }
    return NULL;
}
int main(void)
{
    static os_unfair_lock l = OS_UNFAIR_LOCK_INIT; nmx_thread_lock = &l;
    pthread_t t[16];
    for (int i = 0; i < 16; i++) pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < 16; i++) pthread_join(t[i], NULL);
    int nfree = 0, nlive = 0;
    for (struct nmx_thread_rec *r = nmx_threads_free; r; r = r->next) nfree++;
    for (int b = 0; b < NMX_THREAD_BUCKETS; b++) for (struct nmx_thread_rec *r = nmx_threads[b]; r; r = r->next) nlive++;
    printf("lookups %ld mismatches %ld live %d (count %d) free %d\n", (long)lookups, (long)mismatches, nlive, nmx_thread_count, nfree);
    return mismatches != 0 || nlive != nmx_thread_count || nlive > 24;
}
