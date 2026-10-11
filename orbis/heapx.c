#include <stdio.h>
/* heapx.c — complete malloc replacement.
 *
 * The PS5's libc heap (jemalloc arena) is a fixed reservation that the
 * driver + shader compiler exhaust — fopen() then fails ENOMEM on a few-KB
 * FILE even with flexible memory free. Providing strong malloc/calloc/free/
 * realloc/memalign/… symbols makes our arena the ONLY allocator, so libc's
 * internal allocs (fopen's FILE) and Mesa's big objects all come from the
 * same pool — no foreign-pointer mixing, no mspace starvation.
 *
 * Arena grows via direct memory (12GB free) for big chunks and mmap for the
 * rest. Free-list, 32B granularity. Foreign pointers (init-time libc statics)
 * are detected by the magic header and ignored safely — never OOB-copied.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>

int id4_dbg(int ch, const char *s);
#define sceKernelDebugOutText id4_dbg

#define HX_MAGIC   0x48584d41u   /* "HXMA" */
#define HX_CHUNK   (512u << 10)  /* grow in 512KB chunks — flex is ~7MB free */
#define HX_ALIGN   32

typedef struct HxBlk {
    uint32_t magic;
    uint32_t size;          /* usable bytes */
    struct HxBlk *next;     /* free-list link (only meaningful when free) */
    uint32_t align;         /* requested alignment (for memalign/usable_size) */
    uint32_t _pad[3];
} HxBlk;                     /* 32B: every user ptr = aligned_base + 32 */

static pthread_mutex_t hx_lock = PTHREAD_MUTEX_INITIALIZER;
static HxBlk *hx_free = NULL;            /* singly-linked, address-ordered */
static unsigned long hx_mapped_total;

static int hx_is_ours(void *p)
{
    if (!p) return 0;
    HxBlk *b = (HxBlk *)p - 1;
    return b->magic == HX_MAGIC;
}

static int hx_grow(size_t need)
{
    size_t chunk = need + sizeof(HxBlk) + HX_ALIGN;
    if (chunk < HX_CHUNK) chunk = HX_CHUNK;
    chunk = (chunk + 0xFFFF) & ~0xFFFFull;
    void *p;
    /* Direct pool for chunks ≥64KB — leaves the ~7MB flex for the tiny
     * FILE/struct allocs that fopen needs. */
    if (chunk >= (64u << 10)) {
        extern int sceKernelAllocateDirectMemory(int64_t, int64_t, int64_t,
                                                 int64_t, int32_t, int64_t *);
        extern int sceKernelMapDirectMemory(void **, int64_t, int, int,
                                            int64_t, int64_t);
        int64_t phys = -1; void *va = NULL; int ok = 0, rc = -1;
        extern int64_t sceKernelGetDirectMemorySize(void);
        static int64_t dlim;
        if (!dlim) dlim = sceKernelGetDirectMemorySize();
        static const int32_t mtypes[] = {12, 0, 3};
        for (unsigned t = 0; t < 3; t++) {
            rc = sceKernelAllocateDirectMemory(0, dlim, (int64_t)chunk, 0x4000,
                                              mtypes[t], &phys);
            if (rc == 0) { ok = 1; break; }
        }
        int mrc = -1;
        if (ok) mrc = sceKernelMapDirectMemory(&va, (int64_t)chunk, 0x33, 0x0,
                                           phys, 0x4000);
        if (ok && mrc == 0 && va) {
            p = va;
            goto mapped;
        }
        { extern int sceKernelDebugOutText(int, const char*);
          char _b[96]; snprintf(_b, sizeof _b,
              "[hx] dmem FAIL chunk=%x arc=%x mrc=%x va=%p",
              (unsigned)chunk, rc, mrc, va);
          sceKernelDebugOutText(0, _b); }
    }
    p = mmap(NULL, chunk, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        extern int sceKernelDebugOutText(int, const char*);
        char _b[96]; snprintf(_b, sizeof _b,
            "[hx] grow FAIL chunk=%x err=%d total=%lu MB",
            (unsigned)chunk, errno, hx_mapped_total >> 20);
        sceKernelDebugOutText(0, _b);
        return -1;
    }
mapped:
    hx_mapped_total += (unsigned long)chunk;
    HxBlk *b = (HxBlk *)p;
    b->magic = HX_MAGIC;
    b->size  = (uint32_t)(chunk - sizeof(HxBlk));
    b->align = 0;
    { extern int sceKernelDebugOutText(int, const char*);
      char _b[96]; snprintf(_b, sizeof _b, "[hx] grow p=%p chunk=%x total=%luM",
                            p, (unsigned)chunk, hx_mapped_total >> 20);
      sceKernelDebugOutText(0, _b); }
    pthread_mutex_lock(&hx_lock);
    HxBlk **pp = &hx_free;
    while (*pp && (char *)*pp < (char *)b) pp = &(*pp)->next;
    b->next = *pp; *pp = b;
    pthread_mutex_unlock(&hx_lock);
    return 0;
}

void *malloc(size_t size)
{
    if (!size) size = 1;
    size = (size + HX_ALIGN - 1) & ~(size_t)(HX_ALIGN - 1);
    pthread_mutex_lock(&hx_lock);
    HxBlk **pp = &hx_free;
    while (*pp) {
        HxBlk *b = *pp;
        if (b->size >= size) {
            if (b->size >= size + sizeof(HxBlk) + HX_ALIGN) {
                HxBlk *r = (HxBlk *)((char *)(b + 1) + size);
                r->magic = HX_MAGIC;
                r->size  = b->size - (uint32_t)(size + sizeof(HxBlk));
                r->next  = b->next;
                r->align = 0;
                b->size  = (uint32_t)size;
                *pp = r;
            } else {
                *pp = b->next;
            }
            b->next = (HxBlk *)0x1;
            pthread_mutex_unlock(&hx_lock);
            if (size > (512u << 10)) {
                extern int sceKernelDebugOutText(int, const char*);
                char _b[96]; snprintf(_b, sizeof _b,
                    "[hx] big alloc size=%x p=%p b=%p bsz=%x",
                    (unsigned)size, (void *)(b + 1), (void *)b, b->size);
                sceKernelDebugOutText(0, _b);
            }
            return (void *)(b + 1);
        }
        pp = &b->next;
    }
    pthread_mutex_unlock(&hx_lock);
    if (hx_grow(size) != 0) {
        extern int sceKernelDebugOutText(int, const char*);
        void *ra = __builtin_return_address(0);
        char _b[96]; snprintf(_b, sizeof _b,
            "[hx] OOM size=%x ra=%p — aborting", (unsigned)size, ra);
        sceKernelDebugOutText(0, _b);
        __builtin_trap();
    }
    return malloc(size);
}

void free(void *p)
{
    if (!p) return;
    HxBlk *b = (HxBlk *)p - 1;
    if (b->magic != HX_MAGIC)
        return;                 /* foreign — ignore */
    if (b->align)               /* memalign'd — b->next holds the real blk */
        b = (HxBlk *)b->next;
    if (b->next != (HxBlk *)0x1)
        return;                 /* double-free or stale — ignore */
    pthread_mutex_lock(&hx_lock);
    HxBlk **pp = &hx_free;
    while (*pp && (char *)*pp < (char *)b) pp = &(*pp)->next;
    b->next = *pp; *pp = b;
    b->magic = HX_MAGIC;
    pthread_mutex_unlock(&hx_lock);
}

void *calloc(size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n) return NULL;
    size_t total = n * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t size)
{
    if (!p) return malloc(size);
    HxBlk *b = (HxBlk *)p - 1;
    if (b->magic != HX_MAGIC || b->next != (HxBlk *)0x1) {
        /* Foreign pointer — return NULL rather than memcpy `size` bytes from
         * a region we don't know the real length of (the old code OOB-read
         * and crashed Mesa's glthread). */
        return NULL;
    }
    if (size <= b->size) return p;
    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, p, b->size);
    free(p);
    return np;
}

void *reallocf(void *p, size_t size)
{
    void *np = realloc(p, size);
    if (!np && p) free(p);
    return np;
}

void *reallocarray(void *p, size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n) { errno = ENOMEM; return NULL; }
    return realloc(p, n * size);
}

size_t malloc_usable_size(void *p)
{
    if (!p) return 0;
    HxBlk *b = (HxBlk *)p - 1;
    return b->magic == HX_MAGIC ? b->size : 0;
}

void *memalign(size_t align, size_t size)
{
    if (align <= HX_ALIGN) return malloc(size);
    /* over-allocate and align within */
    size_t total = size + align + sizeof(HxBlk);
    void *raw = malloc(total);
    if (!raw) return NULL;
    uintptr_t a = ((uintptr_t)raw + sizeof(HxBlk) + align - 1) & ~(uintptr_t)(align - 1);
    /* Alternate header just before the aligned ptr — free() reads it at
     * p-1, sees ->align set, and frees the real block via ->next. */
    HxBlk *b = (HxBlk *)a - 1;
    b->magic = HX_MAGIC; b->size = (uint32_t)size;
    b->next  = (HxBlk *)raw - 1;
    b->align = (uint32_t)align;
    return (void *)a;
}

void *valloc(size_t size) { return memalign(0x1000, size); }
void *pvalloc(size_t size) { return memalign(0x1000, (size + 0xfff) & ~0xfffull); }
void *aligned_alloc(size_t align, size_t size) { return memalign(align, size); }

int posix_memalign(void **pp, size_t align, size_t size)
{
    if (!pp) return EINVAL;
    if (align & (align - 1)) return EINVAL;
    void *p = memalign(align, size);
    if (!p) return ENOMEM;
    *pp = p;
    return 0;
}

void cfree(void *p) { free(p); }
void free_memalign(void *p) { free(p); }

char *strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t l = strnlen(s, n) + 1;
    char *p = malloc(l);
    if (p) { memcpy(p, s, l - 1); p[l - 1] = 0; }
    return p;
}

void *malloc_get_malloc_state(void) { return NULL; }
void malloc_stats(void) { }
void malloc_stats_fast(void) { }
int  mallopt(int p, int v) { (void)p; (void)v; return 0; }

/* sbrk/brk — some code paths probe the break; report a stable end. */
void *sbrk(intptr_t d) { (void)d; return (void *)-1; }
int   brk(void *p) { (void)p; return -1; }

/* Strong abort() — every in-image caller (Mesa asserts, libc++ terminate,
 * libc internal __assert) binds here instead of the libc stub. Log the
 * caller's return address and a few stack words so we can name the caller,
 * then let the real abort semantics run (SIGABRT -> on_abrt -> exit). */
__attribute__((visibility("default"), noreturn))
void abort(void)
{
    /* Raw syscall path — id4_dbg can block on the UDP socket under abort */
    extern int real_dbgtext(int, const char *) __asm__("sceKernelDebugOutText");
    char b[160];
    volatile unsigned long *fp = __builtin_frame_address(0);
    unsigned long ra = fp ? fp[1] : 0;
    __builtin_snprintf(b, sizeof b, "[abort] caller=%#lx\n", ra);
    real_dbgtext(0, b);
    for (int i = 0; i < 40; i++) {
        unsigned long v = fp[i];
        if (v > 0x400000UL && v < 0x3450000UL) {
            __builtin_snprintf(b, sizeof b, "[abort] fp+%#x=%#lx\n", i*8, v);
            real_dbgtext(0, b);
        }
    }
    extern int raise(int);
    raise(6 /*SIGABRT*/);
    for (;;) __asm__ volatile("" ::: "memory");
}

/* raise interposer — abort() never fires, so the SIGABRT comes from
 * somewhere else. Log the caller (return address) and signal number, then
 * forward to the real raise via kill(). */
__attribute__((visibility("default")))
int raise(int sig)
{
    extern int real_dbgtext(int, const char *) __asm__("sceKernelDebugOutText");
    char b[160];
    volatile unsigned long *fp = __builtin_frame_address(0);
    unsigned long ra = fp ? fp[1] : 0;
    __builtin_snprintf(b, sizeof b, "[raise] sig=%d caller=%#lx\n", sig, ra);
    real_dbgtext(0, b);
    for (int i = 0; i < 24; i++) {
        unsigned long v = fp[i];
        if (v > 0x400000UL && v < 0x3450000UL) {
            __builtin_snprintf(b, sizeof b, "[raise] fp+%#x=%#lx\n", i*8, v);
            real_dbgtext(0, b);
        }
    }
    extern int kill(int, int);
    extern int getpid(void);
    return kill(getpid(), sig);
}
