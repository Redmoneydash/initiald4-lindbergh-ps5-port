/*
 * guest.c - put a Linux/i386 process where the recompiled code expects one.
 *
 * Lifted code holds real addresses in its registers, so the game's segments
 * have to live at the virtual addresses it was linked for. A Lindbergh ELF is
 * ET_EXEC linked at 0x08048000, which is free in a 32-bit Windows process, so
 * this reserves it outright rather than relocating anything.
 *
 * Only PT_LOAD is honoured. The real kernel also honours PT_INTERP and hands
 * off to ld.so; here there is no ld.so, because every library the game would
 * have loaded is answered by hle_call() instead. Nothing needs relocating.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "lindbergh_rt.h"

#define PT_LOAD   1
#define PAGE      0x1000u
/* Windows reserves address space in 64 KB regions whatever the page size is. */
#define GRAN      0x10000u
#define STACK_TOP 0xBFFF0000u     /* about where Linux/i386 puts it, 64 KB aligned */
#define STACK_SZ  (8u << 20)

static uint32_t g_entry;
uint32_t g_brk; uint32_t g_heap_max;
uint32_t g_seqmgr;                      /* last cSequenceManager `this` from h_regist_log */
static uint32_t g_stack_pointer;
uint32_t g_heap_base;                   /* == hi: start of the contiguous arena */
uint32_t g_stack_base, g_stack_top;     /* reserved guest stack range for ptr checks */

/* Spill arenas: once the contiguous heap hits g_heap_max, extra guest-VA
 * chunks mapped wherever the kernel has low space left, all feeding the one
 * gmalloc free list. Orbis's wall stopped contiguous growth at ~352 MB while
 * the process still owns GBs of VA elsewhere. */
#define MAX_ARENAS 32
static uint32_t g_arena_lo[MAX_ARENAS], g_arena_hi[MAX_ARENAS];
static int g_arena_n;

uint32_t g_image_delta = 0;       /* cpu.h's GVA(): we map where it asked */

static void *reserve(uint32_t addr, uint32_t size)
{
#ifdef _WIN32
    return VirtualAlloc((LPVOID)(uintptr_t)addr, size,
                        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static uint32_t rd32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int guest_load(const char *elf_path, int argc, char **argv)
{
    extern int sceKernelDebugOutText(int, const char*);
    FILE *f = fopen(elf_path, "rb");
    if (!f) { sceKernelDebugOutText(0, "[load] fopen failed\n"); perror(elf_path); return -1; }
    sceKernelDebugOutText(0, "[load] fopen ok\n");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *img = (unsigned char *)malloc((size_t)len);
    { char b[80]; extern int sprintf(char*,const char*,...); sprintf(b,"[load] len=%ld\n",len); sceKernelDebugOutText(0,b); }
    if (!img || fread(img, 1, (size_t)len, f) != (size_t)len) {
        sceKernelDebugOutText(0, "[load] short read\n"); fclose(f); return -1;
    }
    sceKernelDebugOutText(0, "[load] read ok\n");
    fclose(f);
    sceKernelDebugOutText(0, "[load] closed\n");

    if (memcmp(img, "\x7f" "ELF\x01\x01", 6) != 0) {
        sceKernelDebugOutText(0, "[load] not elf\n"); free(img); return -1;
    }
    sceKernelDebugOutText(0, "[load] elf ok\n");
    g_entry = rd32le(img + 24);
    { char b[80]; extern int sprintf(char*,const char*,...); sprintf(b,"[load] entry=%#x\n",g_entry); sceKernelDebugOutText(0,b); }
    uint32_t phoff = rd32le(img + 28);
    unsigned phentsize = img[42] | (img[43] << 8);
    unsigned phnum     = img[44] | (img[45] << 8);
    /* Reserve the whole image span in ONE call, then copy the segments into
     * it. Reserving each PT_LOAD separately looks right and is not: Windows
     * rounds a reservation's REGION up to the 64 KB allocation granularity,
     * not to pages. This game's two segments are page-adjacent at 0x08681000,
     * so reserving the first claims everything up to 0x08690000 and the second
     * reserve then fails on address space that is already taken - which is
     * exactly what it did. */
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = img + phoff + (size_t)i * phentsize;
        if (rd32le(ph) != PT_LOAD) continue;
        uint32_t vaddr = rd32le(ph + 8), memsz = rd32le(ph + 20);
        if ((vaddr & ~(PAGE - 1)) < lo) lo = vaddr & ~(PAGE - 1);
        if (vaddr + memsz > hi) hi = vaddr + memsz;
    }
    if (lo > hi) {
        fprintf(stderr, "%s: no PT_LOAD segments\n", elf_path);
        free(img); return -1;
    }
    lo &= ~(GRAN - 1);
    hi = (hi + GRAN - 1) & ~(GRAN - 1);

    { char b[96]; extern int sprintf(char*,const char*,...); sprintf(b,"[load] span %#x..%#x\n", lo, hi); sceKernelDebugOutText(0,b); }
    if (!reserve(lo, hi - lo)) {
        extern int sprintf(char*,const char*,...);
        char b[96]; sprintf(b, "[load] map image %#x..%#x fail\n", lo, hi);
        sceKernelDebugOutText(0, b); free(img); return -1;
    }
    sceKernelDebugOutText(0, "[load] image mapped\n");
    memset((void *)(uintptr_t)lo, 0, hi - lo);        /* .bss, and the gaps */
    sceKernelDebugOutText(0, "[load] bss cleared\n");

    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = img + phoff + (size_t)i * phentsize;
        if (rd32le(ph) != PT_LOAD) continue;
        memcpy((void *)(uintptr_t)rd32le(ph + 8),     /* p_vaddr  */
               img + rd32le(ph + 4),                  /* p_offset */
               rd32le(ph + 16));                      /* p_filesz */
    }
    sceKernelDebugOutText(0, "[load] segments copied\n");
    g_brk = hi;                                       /* the heap starts past the image */
    g_heap_base = hi;
    free(img);

    /* Reserve the guest stack BEFORE heap growth: the heap takes every byte
     * the system will give, and on Orbis the flexible-memory budget is small
     * enough that a big heap left no room for the stack at all - a real
     * failure observed on console. Stack first, then the heap fills the gap
     * and stops short of the stack's base. */
    static const uint32_t stack_cands[] = {
        (STACK_TOP - STACK_SZ) & ~(GRAN-1),  /* 0xBFF70000 - Linux default */
        0x60000000u,   /* 1.5 GB - clear of heap and typical PS5 mappings  */
        0x40000000u,   /* 1.0 GB */
        0x30000000u,   /* 768 MB */
        0u
    };
    uint32_t stack_base = 0;
    for (int si = 0; stack_cands[si]; si++) {
        if (reserve(stack_cands[si], STACK_SZ)) { stack_base = stack_cands[si]; break; }
    }
    if (!stack_base) {
        sceKernelDebugOutText(0, "[load] ALL stack reserves FAILED\n"); return -1;
    }
    { extern int sprintf(char*,const char*,...);
      char sb[80]; sprintf(sb,"[load] stack at %#x\n", stack_base);
      sceKernelDebugOutText(0, sb); }
    uint32_t stack_top = stack_base + STACK_SZ;
    g_stack_base = stack_base; g_stack_top = stack_top;
    memset((void *)(uintptr_t)stack_base, 0, STACK_SZ);

    /* Orbis mmap() MAP_FIXED at a fresh low address works for the big initial
     * reservations but fails on later per-alloc growth, so commit the whole
     * guest heap up front - brk growth then lands in already-mapped pages.
     * A single big map can lose to fragmentation where several smaller ones
     * win, so grow in chunks and keep whatever contiguous run lands. On Orbis
     * the target is smaller: flexible memory is a thin budget shared with
     * stacks and threads, so the guest heap grows on through direct-memory
     * spill arenas instead (see guest_map_arena). */
    { extern int sprintf(char*,const char*,...);
      char kb[96];
      uint32_t heap_end = hi +
#ifdef __linux__
          0x20000000u;                 /* 512MB of guest heap */
#else
          /* 128MB — 256MB starved the libc heap (mspace arena ~270MB) so the
           * compiler now has its own 128MB mspace; the guest needs >64MB for
           * seq/object allocations (_Znwj failing returned -1 -> segv). */
          0x8000000u;
#endif
      if (stack_base > hi && stack_base < heap_end) heap_end = stack_base;
      void *hp = reserve(hi, heap_end - hi);
      if (hp) g_heap_max = heap_end;
      else {
          /* chunked growth: the largest adjacent run the system will give */
          uint32_t cur = hi;
          for (int i = 0; i < 64 && cur < heap_end; i++) {
              uint32_t mb;
              for (mb = 256; mb >= 1; mb >>= 1)
                  if (cur + (mb << 20) <= heap_end && reserve(cur, mb << 20)) break;
              if (mb < 1) break;
              cur += mb << 20;
          }
          if (cur > hi) g_heap_max = cur;
      }
      sprintf(kb,"[brk] heap %#x..%#x (%uMB)\n", hi, g_heap_max,
              (unsigned)(g_heap_max - hi) >> 20);
      sceKernelDebugOutText(0, kb);
      if (!g_heap_max) { fprintf(stderr, "cannot reserve guest heap\n"); return -1; } }

    /* The initial stack, exactly as the kernel lays it out for _start:
     *   esp -> argc, argv[0..argc-1], NULL, envp[0..], NULL, auxv, AT_NULL
     * The strings go above it. The game's CRT walks this to find its own
     * argv and to read AT_PAGESZ etc, so a wrong shape crashes before main. */
    char *strp = (char *)(uintptr_t)(stack_top - 4096);
    uint32_t *sp = (uint32_t *)(uintptr_t)(stack_top - 8192);
    uint32_t *base = sp;
    *sp++ = (uint32_t)argc;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(strp, argv[i], n);
        *sp++ = (uint32_t)(uintptr_t)strp;
        strp += n;
    }
    *sp++ = 0;            /* argv terminator */
    *sp++ = 0;            /* empty envp */
    *sp++ = 6; *sp++ = PAGE;   /* AT_PAGESZ */
    *sp++ = 0; *sp++ = 0;      /* AT_NULL */

    g_stack_pointer = (uint32_t)(uintptr_t)base;
    return 0;
}

/* True when p lands inside the contiguous heap arena or a spill arena. The
 * freelist walker and gfree's foreign-pointer check use this instead of a
 * single [base, max) range now that arenas are scattered. */
int guest_arena_contains(uint32_t p)
{
    if (p >= g_heap_base && p < g_heap_max) return 1;
    for (int i = 0; i < g_arena_n; i++)
        if (p >= g_arena_lo[i] && p < g_arena_hi[i]) return 1;
    return 0;
}

/* True when va is inside ANY guest mapping a data pointer can legitimately
 * hold: the heap arenas, the reserved main stack, or the image. The stack
 * walkers use it instead of a raw range check because they run against live
 * CPUs - a torn ebp must end the walk, not take the dumper down with it. */
int guest_mapped(uint32_t va)
{
    if (g_stack_top && va >= g_stack_base && va < g_stack_top) return 1;
    if (g_entry && va >= 0x08040000u && va < g_heap_base) return 1;
    return guest_arena_contains(va);
}

/* Map a fresh heap chunk anywhere in guest VA and return its base, or 0.
 *
 * The safe primitive is mmap WITHOUT MAP_FIXED: the kernel only takes a hint
 * when the range is free, so it can never clobber the image, the stack, the
 * contiguous arena or anything the managed loader already owns. When the hint
 * is ignored (or the slot is busy) the result is rejected if it lands outside
 * the low 4 GB the guest can address.
 *
 * On Linux, MAP_FIXED_NOREPLACE is the follow-up probe - it fails rather than
 * replace an occupied range. Orbis gets plain MAP_FIXED: the 352 MB wall that
 * motivated this was reserve() failing where occupied space began, i.e. its
 * MAP_FIXED refuses used VA instead of replacing it. */
uint32_t guest_map_arena(uint32_t size)
{
#ifdef _WIN32
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    guest_heap_lock();
    size = (size + GRAN - 1) & ~(GRAN - 1);
    /* Same candidate list as the POSIX path. VirtualAlloc without MEM_FIXED
     * honours the hint when the range is free and picks another low address
     * when it is not - a 32-bit process can never be handed VA above 4 GB. */
    uint32_t cand[20]; int nc = 0;
    for (uint32_t b = g_heap_max; b < 0x40000000u && nc < 4; b += 0x04000000u)
        cand[nc++] = b;
    static const uint32_t extra[] = {
        0x40000000u, 0x48000000u, 0x50000000u, 0x58000000u,
        0x60000000u, 0x68000000u, 0x70000000u, 0x78000000u,
        0x04000000u,
    };
    for (unsigned i = 0; i < sizeof extra / sizeof *extra && nc < 20; i++)
        cand[nc++] = extra[i];
    for (int i = 0; i < nc; i++) {
        if (g_arena_n >= MAX_ARENAS) break;
        uint32_t b = cand[i];
        if (b < 0x02000000u || b + size < b || b + size > 0xC0000000u) continue;
        void *p = VirtualAlloc((LPVOID)(uintptr_t)b, size,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) continue;
        uint32_t base = (uint32_t)(uintptr_t)p;
        g_arena_lo[g_arena_n] = base;
        g_arena_hi[g_arena_n] = base + size;
        g_arena_n++;
        { extern int sceKernelDebugOutText(int, const char*);
          extern int sprintf(char*,const char*,...);
          char kb[96];
          sprintf(kb, "[brk] spill arena %#x..%#x (%uMB)\n",
                  base, base + size, size >> 20);
          sceKernelDebugOutText(0, kb); }
        guest_heap_unlock();
        return base;
    }
    guest_heap_unlock();
    return 0;
#else
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    guest_heap_lock();
    size = (size + GRAN - 1) & ~(GRAN - 1);
    uint32_t cand[20]; int nc = 0;
    /* keep extending past the contiguous arena first - the wall that stopped
     * the chunked growth may be narrow enough to leapfrog */
    for (uint32_t b = g_heap_max; b < 0x40000000u && nc < 4; b += 0x04000000u)
        cand[nc++] = b;
    static const uint32_t extra[] = {
        0x40000000u, 0x48000000u, 0x50000000u, 0x58000000u,
        0x60000000u, 0x68000000u, 0x70000000u, 0x78000000u,
        0x04000000u,                      /* below the image */
    };
    for (unsigned i = 0; i < sizeof extra / sizeof *extra && nc < 20; i++)
        cand[nc++] = extra[i];
    for (int i = 0; i < nc; i++) {
        if (g_arena_n >= MAX_ARENAS) break;
        uint32_t b = cand[i];
        if (b < 0x02000000u || b + size < b || b + size > 0xC0000000u) continue;
        void *p = mmap((void *)(uintptr_t)b, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            if ((uintptr_t)p >= 0x100000000ull ||
                (uintptr_t)p + size > 0x100000000ull) {
                munmap(p, size);
                p = MAP_FAILED;
            }
        }
        if (p == MAP_FAILED) {
#ifdef __linux__
            p = mmap((void *)(uintptr_t)b, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
#else
            /* Orbis: flexible memory is a small per-process budget (the guest
             * heap alone already starved an 8 MB stack), so fall through to
             * the direct pool - managed apps own ~3 GB of it, completely
             * separate from flexible. */
            p = mmap((void *)(uintptr_t)b, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (p == MAP_FAILED)
            { extern int sceKernelAllocateDirectMemory(int64_t,int64_t,int64_t,
                                                       int64_t,int32_t,int64_t*);
              extern int sceKernelMapDirectMemory(void**,int64_t,int,int,
                                                  int64_t,int64_t);
              extern int sceKernelReleaseDirectMemory(int64_t,int64_t);
              static const int32_t mtypes[] = {12,0,3};
              extern int64_t sceKernelGetDirectMemorySize(void);
              static int64_t dlim; if (!dlim) dlim = sceKernelGetDirectMemorySize();
              int64_t phys = -1; int ok = 0;
              for (unsigned t = 0; t < 3; t++)
                  if (sceKernelAllocateDirectMemory(0, dlim, size, GRAN,
                                                    mtypes[t], &phys) == 0) { ok = 1; break; }
              if (ok) {
                  void *va = (void *)(uintptr_t)b;
                  /* flags 0x10|0x80 = MAP_FIXED|NO_OVERWRITE: lands at b or
                   * fails - without 0x80 a fixed map silently replaces a
                   * mapping already there (a prior arena, the image), which
                   * is a silent-corruption source (AnyPS5 GuestMapNoOverwrite). */
                  if (sceKernelMapDirectMemory(&va, size, 0x33, 0x90,
                                               phys, GRAN) == 0 &&
                      (uintptr_t)va < 0x100000000ull) {
                      p = va;
                  } else {
                      sceKernelReleaseDirectMemory(phys, size);
                  }
              }
            }
            if (p == MAP_FAILED) continue;
#endif
        }
        uint32_t base = (uint32_t)(uintptr_t)p;
        g_arena_lo[g_arena_n] = base;
        g_arena_hi[g_arena_n] = base + size;
        g_arena_n++;
        { extern int sceKernelDebugOutText(int, const char*);
          extern int sprintf(char*,const char*,...);
          char kb[96];
          sprintf(kb, "[brk] spill arena %#x..%#x (%uMB)\n",
                  base, base + size, size >> 20);
          sceKernelDebugOutText(0, kb); }
        guest_heap_unlock();
        return base;
    }
    guest_heap_unlock();
    return 0;
#endif
}

uint32_t guest_entry(void) { return g_entry; }

void guest_init_cpu(CPU *c)
{
    memset(c, 0, sizeof *c);
    guest_set_current_cpu(c);
    c->esp = g_stack_pointer;
    c->eip = g_entry;
    c->fpu_cw = 0x027Fu;
}

uint32_t guest_brk(uint32_t newbrk)
{
    /* brk(0) queries; a grow commits more pages. The recompiled CRT uses this
     * before it ever reaches mmap, so it has to work from the first call. */
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    guest_heap_lock();
    if (newbrk > g_brk) {
        if (g_heap_max && newbrk > g_heap_max) {
            static int said;
            if (!said) {
                said = 1;
                extern int sceKernelDebugOutText(int, const char*);
                extern int sprintf(char*,const char*,...);
                char kb[112];
                sprintf(kb,"[brk] OVERFLOW: brk %#x past heap_max %#x\n",
                        newbrk, g_heap_max);
                sceKernelDebugOutText(0, kb);
            }
            guest_heap_unlock();
            return g_brk;   /* refuse the grow — caller sees no space */
        }
        g_brk = newbrk;
    }
    uint32_t r = g_brk;
    guest_heap_unlock();
    return r;
}
