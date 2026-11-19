// Model of xkpi's nmx_proc_reap vs a thread inside a device-node entry (nmx_thread_entry_enter/leave).
// A task freed while its thread is inside an entry = the exit-hang bug (wake_up_task on freed memory).
// usage: reapmodel <0=old rule (always detach) | 1=fdffd32 (defer while inside) | 2=37bac65 (and clear REAP)> <seconds>
// Rule 2 clears REAP where the kernel does: after an immediate detach or for a thread with no task (reaper), and after
// the hand-over at the outermost leave. Each reaper visit retires the thread's process once; REPEAT counts hand-overs
// at leave after that retirement was already settled (fdffd32's sticky REAP: each later entry made a task, handed it
// over and printed). Rule 2 must show 0 and no use after release.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#define REAP 0x80000000u
#define DEPTH 0x7fffffffu
#define NW 8
typedef struct { _Atomic int alive; } task;
typedef struct { _Atomic unsigned entry; task *_Atomic t; int handed; } thr;   // handed: under tl
static pthread_mutex_t tl = PTHREAD_MUTEX_INITIALIZER;
static thr th[NW]; static int newrule; static _Atomic int stop;
static _Atomic long uaf, uses, detach_now, deferred, selfdetach, repeat;
static task *pend[1 << 16]; static int npend;
static void pend_push(task *t) { if (npend < (1 << 16)) pend[npend++] = t; }
// rule 2 stores a new task under tl, as linux_alloc_current does since 37bac65 (nmx_thread_proc_locked); without
// that, a task made between the reaper's fetch_or and its clear is handed over again at leave (REPEAT)
static task *cur(thr *d) {
    task *t = atomic_load(&d->t);
    if (!t) {
        t = calloc(1, sizeof *t); atomic_store(&t->alive, 1);
        if (newrule >= 2) pthread_mutex_lock(&tl);
        atomic_store(&d->t, t);
        if (newrule >= 2) pthread_mutex_unlock(&tl);
    }
    return t;
}
static void enter(thr *d) { unsigned w = atomic_fetch_add(&d->entry, 1); if (w & REAP) { pthread_mutex_lock(&tl); pthread_mutex_unlock(&tl); } }
static void leave(thr *d) {
    unsigned w = atomic_fetch_sub(&d->entry, 1);
    if (!newrule || (w & DEPTH) != 1 || !(w & REAP)) return;
    pthread_mutex_lock(&tl);
    task *t = atomic_load(&d->t);
    if (t) { pend_push(t); atomic_store(&d->t, NULL); selfdetach++; if (d->handed) repeat++; d->handed = 1; }
    if (newrule >= 2) atomic_fetch_and(&d->entry, ~REAP);
    pthread_mutex_unlock(&tl);
}
static void *worker(void *a) {
    thr *d = a; unsigned r = (unsigned)(uintptr_t)a;
    while (!atomic_load(&stop)) {
        enter(d);
        task *t = cur(d);                       // cb.task = current
        for (int i = 0; i < (int)(r % 64); i++) { if (!atomic_load(&t->alive)) { uaf++; break; } }  // "sleeping" on t
        if (!atomic_load(&t->alive)) uaf++;     // wake_up_task(cb.task) reads its state
        uses++;
        leave(d); r = r * 1103515245 + 12345;
    }
    return NULL;
}
static void *reaper(void *a) {
    while (!atomic_load(&stop)) {
        for (int i = 0; i < NW; i++) {
            pthread_mutex_lock(&tl);
            unsigned w = atomic_fetch_or(&th[i].entry, REAP);
            task *t = atomic_load(&th[i].t);
            if (newrule >= 2) {
                if (t && (w & DEPTH) != 0) deferred++;
                else {
                    if (t) { pend_push(t); atomic_store(&th[i].t, NULL); detach_now++; }
                    atomic_fetch_and(&th[i].entry, ~REAP);
                }
            } else if (t) {
                if (!newrule || (w & DEPTH) == 0) { pend_push(t); atomic_store(&th[i].t, NULL); detach_now++; }
                else deferred++;
            }
            th[i].handed = t != NULL && newrule && (w & DEPTH) != 0 ? 0 : 1;   // 0: its leave still owes the hand-over
            pthread_mutex_unlock(&tl);
        }
        // step 3: release what is pending (mark dead; memory kept so a late use is detected, not a crash)
        pthread_mutex_lock(&tl); int n = npend; task *p[1 << 12]; if (n > (1 << 12)) n = 1 << 12;
        for (int i = 0; i < n; i++) p[i] = pend[--npend]; pthread_mutex_unlock(&tl);
        for (int i = 0; i < n; i++) atomic_store(&p[i]->alive, 0);
        // a new "process" per cycle: rules 0/1 clear REAP only here; rule 2 has cleared it already (or the thread is
        // still inside with its task, and clears it at its leave)
        if (newrule < 2) for (int i = 0; i < NW; i++) atomic_fetch_and(&th[i].entry, ~REAP);
    }
    return NULL;
}
int main(int c, char **v) {
    newrule = c > 1 ? atoi(v[1]) : 1; int secs = c > 2 ? atoi(v[2]) : 3;
    pthread_t w[NW], r;
    for (int i = 0; i < NW; i++) pthread_create(&w[i], NULL, worker, &th[i]);
    pthread_create(&r, NULL, reaper, NULL);
    struct timespec ts = { secs, 0 }; while (nanosleep(&ts, &ts)) ;
    atomic_store(&stop, 1);
    for (int i = 0; i < NW; i++) pthread_join(w[i], NULL); pthread_join(r, NULL);
    printf("rule %d: %ld uses, %ld detached at reap, %ld deferred, %ld self-detached at leave, REPEAT %ld, USE-AFTER-RELEASE %ld\n",
           newrule, (long)uses, (long)detach_now, (long)deferred, (long)selfdetach, (long)repeat, (long)uaf);
    return uaf != 0 || (newrule >= 2 && repeat != 0);
}
