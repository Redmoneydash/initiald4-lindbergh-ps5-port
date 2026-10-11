/* __emutls_get_address — re-entrant-safe for the Orbis managed app.
 *
 * The stock implementation allocates the per-thread slot array with
 * malloc/realloc and stores it under a pthread key. On this stack the weak,
 * default-visibility __emutls_get_address is also what libc's own
 * thread_local accesses resolve to, so emutls -> malloc -> (libc TLS) ->
 * emutls recurses until the stack guard page dies (the repeating
 * eboot->trampoline->libc backtrace seen in klog).
 *
 * This version uses no libc calls at all: a hashed tid table plus per-thread
 * bump arenas carved from static memory. Slots are never freed and
 * destructors are never run — the process outlives them.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

typedef struct __emutls_control {
  uintptr_t size;
  uintptr_t align;
  union {
    uintptr_t index;
    void *address;
  } object;
  void *value;
} __emutls_control;

#define EMUTLS_NTHREADS   64   /* ID4 spins CRI + GL + JVS + seq threads past 32;
                                overflow aliased thread 0's TLS (emutls_tid_slot
                                returned 0 on full table). Pool cost: 64×256KB. */
#define EMUTLS_NSLOTS     128
#define EMUTLS_POOL_BYTES (256u * 1024u)

static uintptr_t   emutls_tids[EMUTLS_NTHREADS];
static void       *emutls_objs[EMUTLS_NTHREADS][EMUTLS_NSLOTS];
static uintptr_t   emutls_off[EMUTLS_NTHREADS];
static unsigned char emutls_pool[EMUTLS_NTHREADS][EMUTLS_POOL_BYTES]
    __attribute__((aligned(16)));
static volatile int    emutls_glock;
static volatile int    emutls_tlock[EMUTLS_NTHREADS];
static uintptr_t       emutls_num_object;

static int emutls_tid_slot(void)
{
   uintptr_t t = (uintptr_t)pthread_self();
   if (!t) t = 1;
   for (int i = 0; i < EMUTLS_NTHREADS; i++) {
      uintptr_t cur = __atomic_load_n(&emutls_tids[i], __ATOMIC_ACQUIRE);
      if (cur == t)
         return i;
      if (cur == 0) {
         uintptr_t want = 0;
         if (__atomic_compare_exchange_n(&emutls_tids[i], &want, t, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return i;
      }
   }
   return 0; /* table full: degenerate but alive */
}

static void *emutls_alloc(int tid, uintptr_t sz, uintptr_t align)
{
   while (__atomic_exchange_n(&emutls_tlock[tid], 1, __ATOMIC_ACQUIRE))
      ;
   uintptr_t a = (emutls_off[tid] + align - 1) & ~(align - 1);
   void *p = NULL;
   if (a + sz <= EMUTLS_POOL_BYTES) {
      p = &emutls_pool[tid][a];
      emutls_off[tid] = a + sz;
   }
   __atomic_store_n(&emutls_tlock[tid], 0, __ATOMIC_RELEASE);
   return p;
}

__attribute__((visibility("default"), weak))
void *__emutls_get_address(__emutls_control *control)
{
   uintptr_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
   if (!index) {
      while (__atomic_exchange_n(&emutls_glock, 1, __ATOMIC_ACQUIRE))
         ;
      index = control->object.index;
      if (!index) {
         index = ++emutls_num_object;
         __atomic_store_n(&control->object.index, index, __ATOMIC_RELEASE);
      }
      __atomic_store_n(&emutls_glock, 0, __ATOMIC_RELEASE);
   }
   if (!index || index > EMUTLS_NSLOTS)
      abort();
   index--;

   int t = emutls_tid_slot();
   void *p = __atomic_load_n(&emutls_objs[t][index], __ATOMIC_ACQUIRE);
   if (!p) {
      uintptr_t align = control->align < sizeof(void *) ? sizeof(void *)
                                                      : control->align;
      p = emutls_alloc(t, control->size, align);
      if (!p)
         abort();
      if (control->value)
         memcpy(p, control->value, control->size);
      else
         memset(p, 0, control->size);
      __atomic_store_n(&emutls_objs[t][index], p, __ATOMIC_RELEASE);
   }
   return p;
}
