// Model of xkpi's nmx_proc_reap vs a thread inside a device-node entry (nmx_thread_entry_enter/leave).
// A task freed while its thread is inside an entry = the exit-hang bug (wake_up_task on freed memory).
// usage: reapmodel <0=old rule (always detach) | 1=new rule> <seconds>
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
typedef struct { _Atomic unsigned entry; task *_Atomic t; } thr;
static pthread_mutex_t tl = PTHREAD_MUTEX_INITIALIZER;
static thr th[NW]; static int newrule; static _Atomic int stop;
static _Atomic long uaf, uses, detach_now, deferred, selfdetach;
static task *pend[1 << 16]; static int npend;
static void pend_push(task *t) { if (npend < (1 << 16)) pend[npend++] = t; }
static task *cur(thr *d) { task *t = atomic_load(&d->t); if (!t) { t = calloc(1, sizeof *t); atomic_store(&t->alive, 1); atomic_store(&d->t, t); } return t; }
static void enter(thr *d) { unsigned w = atomic_fetch_add(&d->entry, 1); if (w & REAP) { pthread_mutex_lock(&tl); pthread_mutex_unlock(&tl); } }
static void leave(thr *d) {
    unsigned w = atomic_fetch_sub(&d->entry, 1);
    if (!newrule || (w & DEPTH) != 1 || !(w & REAP)) return;
    pthread_mutex_lock(&tl); task *t = atomic_load(&d->t); if (t) { pend_push(t); atomic_store(&d->t, NULL); selfdetach++; } pthread_mutex_unlock(&tl);
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
            if (t) {
                if (!newrule || (w & DEPTH) == 0) { pend_push(t); atomic_store(&th[i].t, NULL); detach_now++; }
                else deferred++;
            }
            pthread_mutex_unlock(&tl);
        }
        // step 3: release what is pending (mark dead; memory kept so a late use is detected, not a crash)
        pthread_mutex_lock(&tl); int n = npend; task *p[1 << 12]; if (n > (1 << 12)) n = 1 << 12;
        for (int i = 0; i < n; i++) p[i] = pend[--npend]; pthread_mutex_unlock(&tl);
        for (int i = 0; i < n; i++) atomic_store(&p[i]->alive, 0);
        for (int i = 0; i < NW; i++) atomic_fetch_and(&th[i].entry, ~REAP);   // a new "process" per cycle
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
    printf("rule %s: %ld uses, %ld detached at reap, %ld deferred, %ld self-detached at leave, USE-AFTER-RELEASE %ld\n",
           newrule ? "new" : "old", (long)uses, (long)detach_now, (long)deferred, (long)selfdetach, (long)uaf);
    return uaf != 0;
}
