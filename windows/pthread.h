/* pthread.h - minimal host-side pthread API over Win32.
 * Only what the runtime's host code actually calls: mutexes (recursive via
 * CRITICAL_SECTION), pthread_once (native INIT_ONCE), pthread_self.
 * Guest-side pthread_* is implemented by hle_pthread.c separately. */
#ifndef LIND_PTHREAD_SHIM_H
#define LIND_PTHREAD_SHIM_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    CRITICAL_SECTION cs;
    volatile LONG    init;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { { 0 }, 0 }
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_NORMAL 0

typedef struct { void *ptr; } pthread_mutexattr_t;
static int pthread_mutexattr_init(pthread_mutexattr_t *a) { (void)a; return 0; }
static int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) { (void)a; (void)t; return 0; }
static int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }

extern void lind_pthread_global_init(void);
static int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    (void)a;
    m->init = 0;
    InitializeCriticalSection(&m->cs);   /* CS is valid immediately; init flag
                                         * only guards PTHREAD_MUTEX_INITIALIZER
                                         * statics, which start zeroed. */
    InterlockedExchange(&m->init, 2);    /* 2 = ready (1 = being initialised) */
    return 0;
}
static int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (m->init != 2) {
        /* Static-initializer path: zeroed memory is not a valid CS. Claim the
         * object 0 -> 1, initialise, publish 2; losers wait for 2. The old
         * global flag was published BEFORE its own CS was initialised, so a
         * second thread could enter an uninitialised CRITICAL_SECTION. */
        if (InterlockedCompareExchange(&m->init, 1, 0) == 0) {
            InitializeCriticalSection(&m->cs);
            InterlockedExchange(&m->init, 2);
        } else {
            while (m->init != 2) Sleep(0);
        }
    }
    EnterCriticalSection(&m->cs);
    return 0;
}
static int pthread_mutex_unlock(pthread_mutex_t *m) { LeaveCriticalSection(&m->cs); return 0; }
static int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static int pthread_mutex_trylock(pthread_mutex_t *m)
{
    if (m->init != 2) return pthread_mutex_lock(m), 0;
    return TryEnterCriticalSection(&m->cs) ? 0 : 16 /*EBUSY*/;
}

typedef struct { INIT_ONCE once; } pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

typedef void (*pthread_once_fn)(void);
static BOOL WINAPI lind_once_thunk(PINIT_ONCE io, PVOID param, PVOID *ctx)
{
    (void)io; (void)ctx;
    ((pthread_once_fn)param)();
    return TRUE;
}
static int pthread_once(pthread_once_t *o, pthread_once_fn fn)
{
    BOOL ok = InitOnceExecuteOnce(&o->once, lind_once_thunk, (PVOID)fn, NULL);
    return ok ? 0 : -1;
}

typedef void *pthread_t;
typedef unsigned long pthread_key_t;
static pthread_t pthread_self(void) { return (pthread_t)(uintptr_t)GetCurrentThreadId(); }

/* Unused on this target; declared so stray references link. */
typedef struct { int _unused; } pthread_cond_t;
static int pthread_cond_init(pthread_cond_t *c, const void *a) { (void)c; (void)a; return 0; }
static int pthread_cond_signal(pthread_cond_t *c) { (void)c; return 0; }
static int pthread_cond_broadcast(pthread_cond_t *c) { (void)c; return 0; }
static int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void)c; (void)m; return 0; }
static int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

#ifdef __cplusplus
}
#endif
#endif
