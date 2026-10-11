/* hle_pthread_orbis.c - the threads the board provided, for Orbis/PS5.
 *
 * The Win32 hle_pthread builds on CRITICAL_SECTION + CONDITION_VARIABLE and
 * thread handles. On Orbis the payload SDK has real POSIX pthreads, so each
 * guest call maps to the pthread function of the same name. The guest's
 * pthread objects are Linux-i386-sized opaque blobs (mutex 24, cond 48,
 * attr 36, mutexattr 4) - smaller than Orbis's structs, so we keep host-side
 * objects keyed by the guest blob's address rather than embedding.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include "lindbergh_rt.h"

/* ---- guest object -> host object indirection ----
 *
 * The guest's pthread_mutex_t/cond_t/etc are fixed-size blobs smaller than
 * the Orbis structs, so a host object lives in a table keyed by the blob's
 * address. Zeroed guest storage = the never-initialized case (PTHREAD_*_INIT
 * is zero), so a zeroed blob lazily creates the host object on first use. */

typedef struct { uint32_t ga; pthread_mutex_t m; int used; } Mtx;
typedef struct { uint32_t ga; pthread_cond_t c; int used; } Cnd;
typedef struct { uint32_t ga; pthread_key_t k; int used; } Key;
typedef struct { uint32_t ga; sem_t s; int used; int inited; } Sem;

static Mtx g_mtx[8192];   static int g_mtx_n;
static Cnd g_cnd[8192];   static int g_cnd_n;
static Key g_key[1024];   static int g_key_n;
static Sem g_sem[1024];   static int g_sem_n;

static pthread_mutex_t g_tbl_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *mtx_of(uint32_t ga)
{
    pthread_mutex_lock(&g_tbl_lock);
    for (int i = 0; i < g_mtx_n; i++) if (g_mtx[i].used && g_mtx[i].ga == ga) { pthread_mutex_unlock(&g_tbl_lock); return &g_mtx[i].m; }
    if (g_mtx_n < 8192) { int i = g_mtx_n++; g_mtx[i].ga = ga; g_mtx[i].used = 1; pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&g_mtx[i].m, &a); pthread_mutexattr_destroy(&a); pthread_mutex_unlock(&g_tbl_lock); return &g_mtx[i].m; }
    pthread_mutex_unlock(&g_tbl_lock); return NULL;
}
static pthread_cond_t *cnd_of(uint32_t ga)
{
    pthread_mutex_lock(&g_tbl_lock);
    for (int i = 0; i < g_cnd_n; i++) if (g_cnd[i].used && g_cnd[i].ga == ga) { pthread_mutex_unlock(&g_tbl_lock); return &g_cnd[i].c; }
    if (g_cnd_n < 8192) { int i = g_cnd_n++; g_cnd[i].ga = ga; g_cnd[i].used = 1; pthread_cond_init(&g_cnd[i].c, NULL); pthread_mutex_unlock(&g_tbl_lock); return &g_cnd[i].c; }
    pthread_mutex_unlock(&g_tbl_lock); return NULL;
}
static pthread_key_t *key_of(uint32_t ga)
{
    pthread_mutex_lock(&g_tbl_lock);
    for (int i = 0; i < g_key_n; i++) if (g_key[i].used && g_key[i].ga == ga) { pthread_mutex_unlock(&g_tbl_lock); return &g_key[i].k; }
    if (g_key_n < 1024) { int i = g_key_n++; g_key[i].ga = ga; g_key[i].used = 1; pthread_key_create(&g_key[i].k, NULL); pthread_mutex_unlock(&g_tbl_lock); return &g_key[i].k; }
    pthread_mutex_unlock(&g_tbl_lock); return NULL;
}
static sem_t *sem_of(uint32_t ga)
{
    pthread_mutex_lock(&g_tbl_lock);
    for (int i = 0; i < g_sem_n; i++) if (g_sem[i].used && g_sem[i].ga == ga) { pthread_mutex_unlock(&g_tbl_lock); return &g_sem[i].s; }
    if (g_sem_n < 1024) { int i = g_sem_n++; g_sem[i].ga = ga; g_sem[i].used = 1; sem_init(&g_sem[i].s, 0, 0); pthread_mutex_unlock(&g_tbl_lock); return &g_sem[i].s; }
    pthread_mutex_unlock(&g_tbl_lock); return NULL;
}

/* ---- mutex ---- */
static void h_mutex_init(CPU *c)   { (void)mtx_of(A32(0)); RET(0); }
static void h_mutex_lock(CPU *c)
{
    /* Deadlock hunt: trylock-spin for ~5s; if the mutex never releases log
     * the guest address + our cpu index so we can find the holder. */
    pthread_mutex_t *m = mtx_of(A32(0));
    if (!m) { RET(EINVAL); return; }
    for (int i = 0; i < 5000; i++) {
        if (pthread_mutex_trylock(m) == 0) { RET(0); return; }
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
    extern int sceKernelDebugOutText(int, const char *);
    char b[96]; snprintf(b, sizeof b,
        "[mtx] deadlock? ga=%#x thr=%lx\n", (unsigned)A32(0),
        (unsigned long)pthread_self());
    sceKernelDebugOutText(0, b);
    RET(pthread_mutex_lock(m));
}
static void h_mutex_trylock(CPU *c){ pthread_mutex_t *m=mtx_of(A32(0)); RET(m?pthread_mutex_trylock(m):EINVAL); }
static void h_mutex_unlock(CPU *c) { pthread_mutex_t *m=mtx_of(A32(0)); RET(m?pthread_mutex_unlock(m):EINVAL); }
static void h_mutex_destroy(CPU *c){ RET(0); }
static void h_mutexattr(CPU *c)    { RET(0); }

/* ---- cond ---- */
static void h_cond_init(CPU *c)    { (void)cnd_of(A32(0)); RET(0); }
static void h_cond_signal(CPU *c)  { pthread_cond_t *p=cnd_of(A32(0)); RET(p?pthread_cond_signal(p):EINVAL); }
static void h_cond_broadcast(CPU *c){ pthread_cond_t *p=cnd_of(A32(0)); RET(p?pthread_cond_broadcast(p):EINVAL); }
static void h_cond_wait(CPU *c)    { pthread_cond_t *p=cnd_of(A32(0)); pthread_mutex_t *m=mtx_of(A32(1)); RET(p&&m?pthread_cond_wait(p,m):EINVAL); }
static void h_cond_timedwait(CPU *c)
{
    uint32_t ts = A32(2);
    struct timespec t;
    if (ts) { t.tv_sec = rd32(ts); t.tv_nsec = rd32(ts + 4); }
    else    if (clock_gettime(CLOCK_REALTIME, &t) != 0) { t.tv_sec = time(NULL); t.tv_nsec = 0; }
    pthread_cond_t *p=cnd_of(A32(0)); pthread_mutex_t *m=mtx_of(A32(1));
    RET(p&&m?pthread_cond_timedwait(p,m,&t):EINVAL);
}
static void h_cond_destroy(CPU *c) { RET(0); }

/* ---- threads ----
 * The guest gives us a guest-code entry address + arg. We run it through
 * dispatch on a real pthread. The record keeps the CPU the thread runs on. */
typedef struct { uint32_t start, arg; CPU cpu; int live; pthread_t tid; void *stk; int64_t dphys; } Thrd;
static Thrd g_thrd[64]; static int g_thrd_n;

static void *thrd_run(void *p)
{
    Thrd *t = p;
    extern uint32_t guest_call(CPU *, uint32_t, const uint32_t *, int);
    extern void guest_set_current_cpu(CPU *);
    extern void klogf(const char*, ...);
    /* Alt-signal-stack per thread — a stack-overflow SIGSEGV can't run the
     * handler on the dead stack; give this thread's handler its own stack. */
    static __thread char sigstk[64 << 10];
    stack_t ss; ss.ss_sp = sigstk; ss.ss_size = sizeof sigstk; ss.ss_flags = 0;
    sigaltstack(&ss, 0);
    klogf("[thrd] enter start=%#x arg=%#x esp=%#x ps=%p\n", t->start, t->arg, t->cpu.esp, (void*)pthread_self());
    guest_set_current_cpu(&t->cpu);
    uint32_t arg = t->arg;
    guest_call(&t->cpu, t->start, &arg, 1);
    klogf("[thrd] exit start=%#x\n", t->start);
    t->live = 0;
    if (t->stk) {
        extern int sceKernelReleaseDirectMemory(int64_t, int64_t);
        extern int munmap(void *, unsigned long);
        munmap(t->stk, 16ull << 20);
        sceKernelReleaseDirectMemory(t->dphys, 16ull << 20);
        t->stk = NULL;
    }
    return NULL;
}

/* Return the 1-based slot index for a guest thread ID, or NULL. */
static Thrd *thrd_for(uint32_t id)
{
    if (id == 0 || id > (uint32_t)g_thrd_n) return NULL;
    return &g_thrd[id - 1];
}

static void h_pthread_create(CPU *c)
{
    uint32_t attr = A32(1); (void)attr;
    Thrd *t = NULL;
    uint32_t idx = 0;
    pthread_mutex_lock(&g_tbl_lock);
    if (g_thrd_n < 64) { idx = (uint32_t)(g_thrd_n + 1); t = &g_thrd[g_thrd_n++]; }
    pthread_mutex_unlock(&g_tbl_lock);
    if (!t) { RET((uint32_t)-1); return; }
    memset(t, 0, sizeof *t);
    t->start = A32(2); t->arg = A32(3); t->live = 1;
    klogf("[pcreate] start=%#x arg=%#x tid_out=%#x idx=%u\n",
            t->start, t->arg, A32(0), idx);
    memcpy(&t->cpu, c, sizeof *c);
    extern uint32_t guest_brk(uint32_t);
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    guest_heap_lock();
    uint32_t st = guest_brk(0);
    st = (st + 0xfff) & ~0xfffu;
    guest_brk(st + 0x40000);
    t->cpu.esp = st + 0x40000 - 16;
    guest_heap_unlock();
    /* The shader-compile worker (vtn) recurses deep — 512KB/768KB/1MB/4MB all
     * overflowed. Give it a 16MB stack mapped from direct memory instead of
     * flex so size doesn't cost the arena. */
    extern int sceKernelAllocateDirectMemory(int64_t, int64_t, int64_t, int64_t, int32_t, int64_t *);
    extern int sceKernelMapDirectMemory(void **, int64_t, int, int, int64_t, int64_t);
    void *stk = NULL;
    int64_t dphys = -1;
    { static const int32_t mtypes[] = {12, 0, 3};
      extern int64_t sceKernelGetDirectMemorySize(void);
      static int64_t dlim; if (!dlim) dlim = sceKernelGetDirectMemorySize();
      int ok = 0;
      for (unsigned i = 0; i < 3; i++)
          if (sceKernelAllocateDirectMemory(0, dlim, 16ull << 20, 0x4000,
                                            mtypes[i], &dphys) == 0) { ok = 1; break; }
      if (!(ok && sceKernelMapDirectMemory(&stk, 16ull << 20, 0x33, 0,
                                           dphys, 0) == 0 && stk))
          stk = NULL; }
    t->stk = stk; t->dphys = dphys;
    pthread_attr_t at; pthread_attr_init(&at);
    int rc;
    if (stk) {
        pthread_attr_setstack(&at, stk, 16u << 20);
        rc = pthread_create(&t->tid, &at, thrd_run, t);
        pthread_attr_destroy(&at);
    } else rc = EAGAIN;
    if (rc) {
        pthread_attr_t at2; pthread_attr_init(&at2);
        pthread_attr_setstacksize(&at2, 768 << 10);
        rc = pthread_create(&t->tid, &at2, thrd_run, t);
        pthread_attr_destroy(&at2);
    }
    if (rc) {
        extern int sceKernelDebugOutText(int, const char *);
        char b[96]; snprintf(b, sizeof b,
            "[pcreate] FAIL rc=%d start=%#x idx=%u\n", rc, t->start, idx);
        sceKernelDebugOutText(0, b);
        /* don't hand the guest a live handle for a thread that never ran */
        pthread_mutex_lock(&g_tbl_lock); t->live = 0; g_thrd_n--;
        pthread_mutex_unlock(&g_tbl_lock);
        RET((uint32_t)-1); return;
    }
    wr32(A32(0), idx);   /* store 1-based index; safe 32-bit guest value */
    RET(0);
}

static void h_pthread_join(CPU *c)
{
    Thrd *t = thrd_for(A32(0));
    if (t) pthread_join(t->tid, NULL);
    RET(0);
}
static void h_pthread_detach(CPU *c) { Thrd *t=thrd_for(A32(0)); if(t)pthread_detach(t->tid); RET(0); }
static void h_pthread_self(CPU *c)
{
    /* Return the 1-based index for the calling thread, or 0 for the main thread. */
    pthread_t self = pthread_self();
    for (int i = 0; i < g_thrd_n; i++)
        if (g_thrd[i].live && pthread_equal(g_thrd[i].tid, self)) { RET((uint32_t)(i + 1)); return; }
    RET(0);   /* main thread or unregistered — no slot */
}
static void h_pthread_equal(CPU *c)  { RET(A32(0) == A32(1)); }
static void h_pthread_cancel(CPU *c) { (void)c; RET(0); /* real pthread_cancel
    is a banned thr_kill on managed Orbis — pretend the cancel posted; the
    thread's own exit path cleans up */ }
static void h_sched(CPU *c)          { RET(0); }

/* ---- once ---- */
static void h_pthread_once(CPU *c)
{
    /* pthread_once_t is a guest int, zero until done. The routine takes no
     * args; run it through dispatch once per flag. Serialise the check+init
     * — two threads can otherwise both read flag==0 and both run it. */
    static pthread_mutex_t s_once_mu = PTHREAD_MUTEX_INITIALIZER;
    uint32_t flag = A32(0), fn = A32(1);
    if (!flag) { RET(0); return; }
    pthread_mutex_lock(&s_once_mu);
    int run = (rd32(flag) == 0);
    if (run) wr32(flag, 1);             /* claim it under the lock */
    pthread_mutex_unlock(&s_once_mu);   /* release before dispatch — fn may
                                         * recurse into pthread_once or block */
    if (run) {
        extern void dispatch(CPU *, uint32_t);
        dispatch(c, fn);
    }
    RET(0);
}

/* ---- keys ----
 * The guest stores a 1-based key index in its pthread_key_t (an int-sized
 * guest blob).  h_key_create writes that index; get/set look it up. */
static void h_key_create(CPU *c)
{
    uint32_t out = A32(0);
    if (!out) { RET(EINVAL); return; }
    int idx = -1;
    for (int i = 0; i < g_key_n; i++) if (!g_key[i].used) { idx = i; break; }
    if (idx < 0) { if (g_key_n < 1024) idx = g_key_n++; else { RET(EAGAIN); return; } }
    g_key[idx].used = 1;
    pthread_key_create(&g_key[idx].k, NULL);
    wr32(out, (uint32_t)(idx + 1));   /* 1-based, fits in 32 bits */
    RET(0);
}
static void h_key_delete(CPU *c)   { RET(0); }
static void h_getspecific(CPU *c)
{
    uint32_t idx = A32(0);
    if (!idx || idx > (uint32_t)g_key_n) { RET(0); return; }
    void *v = pthread_getspecific(g_key[idx - 1].k);
    RET((uint32_t)(uintptr_t)v);
}
static void h_setspecific(CPU *c)
{
    uint32_t idx = A32(0);
    if (!idx || idx > (uint32_t)g_key_n) { RET(EINVAL); return; }
    pthread_setspecific(g_key[idx - 1].k, (void *)(uintptr_t)A32(1));
    RET(0);
}

/* ---- sems ---- */
static void h_sem_init(CPU *c)
{
    /* The game runs the same sem_init under several pthread_once flags — each
     * fires once, so h_sem_init sees the same guest address repeatedly.
     * sem_init() on a live semaphore corrupts its futex/waiter list → SIGSEGV.
     * Initialise only the first time a guest address is seen. */
    extern void klogf(const char*, ...);
    uint32_t ga = A32(0); int val = AI32(2);
    pthread_mutex_lock(&g_tbl_lock);
    Sem *e = NULL;
    for (int i = 0; i < g_sem_n; i++)
        if (g_sem[i].used && g_sem[i].ga == ga) { e = &g_sem[i]; break; }
    if (!e && g_sem_n < 1024) { e = &g_sem[g_sem_n++]; e->ga = ga; e->used = 1; }
    int first = (e != NULL) && !e->inited;
    if (first) e->inited = 1;
    pthread_mutex_unlock(&g_tbl_lock);
    klogf("[sem] init ga=%#x val=%d %s\n", ga, val, first ? "ok" : "skip");
    if (first) sem_init(&e->s, 0, val);
    RET(0);
}
static void h_sem_post(CPU *c)     { sem_t *s=sem_of(A32(0)); RET(s?sem_post(s):-1); }
static void h_sem_wait(CPU *c)     { sem_t *s=sem_of(A32(0)); int r=s?sem_wait(s):-1; RET(r); }
static void h_sem_trywait(CPU *c)  { sem_t *s=sem_of(A32(0)); RET(s?sem_trywait(s):-1); }
static void h_sem_timedwait(CPU *c)
{
    sem_t *s = sem_of(A32(0));
    if (!s) { RET(-1); return; }
    uint32_t tsp = A32(1);
    if (!tsp) { RET(sem_wait(s)); return; }
    struct timespec ts = { (time_t)rd32(tsp), (long)rd32(tsp + 4) };
    RET(sem_timedwait(s, &ts));
}
static void h_sem_destroy(CPU *c)  { RET(0); }
static void h_sem_getvalue(CPU *c) { sem_t *s=sem_of(A32(0)); int v=0; if(s)sem_getvalue(s,&v); wr32(A32(1),(uint32_t)v); RET(0); }

/* ---- sched ---- */
static void h_sched_get_priority_max(CPU *c) { RET(0); }
static void h_sched_get_priority_min(CPU *c) { RET(0); }
static void h_sched_yield(CPU *c)            { RET(0); /* sched_yield is a
    real syscall — banned on managed Orbis (the SVM lock loop calls it when
    contended → SIGSYS). A no-op is safe: the lock retries anyway. */ }
static void h_sched_getaffinity(CPU *c)      { RET(0); }
static void h_sched_setaffinity(CPU *c)      { RET(0); }

void hle_register_pthread(void)
{
    hle_bind("pthread_mutex_init", h_mutex_init);
    hle_bind("pthread_mutex_lock", h_mutex_lock);
    hle_bind("pthread_mutex_trylock", h_mutex_trylock);
    hle_bind("pthread_mutex_unlock", h_mutex_unlock);
    hle_bind("pthread_mutex_destroy", h_mutex_destroy);
    hle_bind("pthread_mutexattr_init", h_mutexattr);
    hle_bind("pthread_mutexattr_settype", h_mutexattr);
    hle_bind("pthread_mutexattr_setpshared", h_mutexattr);
    hle_bind("pthread_mutexattr_destroy", h_mutexattr);
    hle_bind("pthread_cond_init", h_cond_init);
    hle_bind("pthread_cond_signal", h_cond_signal);
    hle_bind("pthread_cond_broadcast", h_cond_broadcast);
    hle_bind("pthread_cond_wait", h_cond_wait);
    hle_bind("pthread_cond_timedwait", h_cond_timedwait);
    hle_bind("pthread_cond_destroy", h_cond_destroy);
    hle_bind("pthread_condattr_init", h_mutexattr);
    hle_bind("pthread_condattr_destroy", h_mutexattr);
    hle_bind("pthread_attr_init", h_mutexattr);
    hle_bind("pthread_attr_destroy", h_mutexattr);
    hle_bind("pthread_attr_setstacksize", h_mutexattr);
    hle_bind("pthread_attr_setschedparam", h_mutexattr);
    hle_bind("pthread_attr_setschedpolicy", h_mutexattr);
    hle_bind("pthread_create", h_pthread_create);
    hle_bind("pthread_join", h_pthread_join);
    hle_bind("pthread_detach", h_pthread_detach);
    hle_bind("pthread_self", h_pthread_self);
    hle_bind("pthread_equal", h_pthread_equal);
    hle_bind("pthread_cancel", h_pthread_cancel);
    hle_bind("pthread_once", h_pthread_once);
    hle_bind("pthread_getschedparam", h_sched);
    hle_bind("pthread_setschedparam", h_sched);
    hle_bind("pthread_key_create", h_key_create);
    hle_bind("pthread_key_delete", h_key_delete);
    hle_bind("pthread_getspecific", h_getspecific);
    hle_bind("pthread_setspecific", h_setspecific);
    hle_bind("sem_init", h_sem_init);
    hle_bind("sem_post", h_sem_post);
    hle_bind("sem_wait", h_sem_wait);
    hle_bind("sem_trywait", h_sem_trywait);
    hle_bind("sem_timedwait", h_sem_timedwait);
    hle_bind("sem_destroy", h_sem_destroy);
    hle_bind("sem_getvalue", h_sem_getvalue);
    hle_bind("sched_get_priority_max", h_sched_get_priority_max);
    hle_bind("sched_get_priority_min", h_sched_get_priority_min);
    hle_bind("sched_yield", h_sched_yield);
    hle_bind("sched_getaffinity", h_sched_getaffinity);
    hle_bind("sched_setaffinity", h_sched_setaffinity);
}
