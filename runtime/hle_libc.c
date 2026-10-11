#include <errno.h>
/*
 * hle_libc.c - the C library the board provided.
 *
 * A Lindbergh game links glibc 2.3.x, and almost every one of those imports is
 * a function the host CRT already has. So these are thunks, not
 * implementations: read the arguments off the guest stack, call the host, put
 * the result back where the guest expects it.
 *
 * Two things about the guest ABI matter while reading:
 *
 *   - It is cdecl, and the lifter pushed no return address, so argument 0 is
 *     at esp+0. A handler must not move esp; the caller's own `add esp, N`
 *     does that.
 *   - A function returning float or double returns it in st(0), not in an XMM
 *     register. That is the i386 SysV convention, and it is why every math
 *     thunk here ends in RETF rather than RET.
 *
 * Pointers need no translation: the memory model is flat and the image is
 * mapped where it was linked, so a guest pointer IS a host pointer.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
/* POSIX compat for the Windows-only libc used by the lifted games */
typedef uint32_t DWORD;
#define Sleep(ms) usleep((ms)*1000)
#define _stat64 stat
#define _fstat64 fstat
#define _access access
#define _chdir chdir
#define _open open
#define _read read
#define _write write
#define _lseek lseek
#define _close close
#define _unlink unlink
#define _mkdir(p) mkdir(p,0755)
#define _strdup strdup
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _O_BINARY 0
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_APPEND O_APPEND
#define _commit(fd) fsync(fd)
#define _getcwd getcwd
static int localtime_s(struct tm *out,const time_t *t){struct tm *r=localtime_r(t,out);return r?0:1;}
/* _findfirst/_findnext/_findclose over opendir/readdir.
 * The pattern splits into dir part (before last '/' or '\') and a basename
 * wildcard that _find_match actually honors — previously every call opened
 * "." and returned all entries for ANY pattern. Single-slot global is
 * serialized by the heap lock like the rest of this file. */
struct _finddata_t{char name[260];uint32_t attrib;long size;};
#define _A_SUBDIR 0x10
#define FIND_SLOTS 8
static struct{DIR*d;char pat[512];} g_find[FIND_SLOTS];
static int _find_match(const char*n,const char*p){
    /* single-star wildcard: "*", "*.ext", "prefix*" */
    if (!p[0] || (p[0]=='*' && !p[1])) return 1;
    const char *star = strchr(p,'*');
    if (!star) return strcmp(n,p)==0;
    size_t pre = star - p;
    if (strncmp(n,p,pre)) return 0;
    const char *suf = star + 1;
    size_t nl = strlen(n), sl = strlen(suf);
    if (sl && (nl < sl || strcmp(n + nl - sl, suf))) return 0;
    return nl >= pre;
}
static int _findnext(intptr_t h,struct _finddata_t*o){
    if (h < 0 || h >= FIND_SLOTS || !g_find[h].d) return -1;
    struct dirent *de;
    while ((de = readdir(g_find[h].d))) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        if (!_find_match(de->d_name, g_find[h].pat)) continue;
        strncpy(o->name,de->d_name,259);o->name[259]=0;
        o->attrib=(de->d_type==DT_DIR)?_A_SUBDIR:0;
        return 0;
    }
    return -1;
}
static intptr_t _findfirst(const char*p,struct _finddata_t*o){
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    guest_heap_lock();
    int slot = -1;
    for (int i = 0; i < FIND_SLOTS; i++) if (!g_find[i].d) { slot = i; break; }
    if (slot < 0) { guest_heap_unlock(); return -1; }
    const char *base = strrchr(p,'/');
    const char *bs = strrchr(p,'\\');
    if (bs && (!base || bs > base)) base = bs;
    char dir[512]; size_t dl = base ? (size_t)(base - p) : 0;
    if (dl >= sizeof dir) { guest_heap_unlock(); return -1; }
    memcpy(dir, p, dl); dir[dl] = 0;
    if (!dl) strcpy(dir, ".");
    strncpy(g_find[slot].pat, base ? base + 1 : p, 511); g_find[slot].pat[511] = 0;
    g_find[slot].d = opendir(dir);
    intptr_t r = (g_find[slot].d && _findnext(slot, o) == 0) ? slot : -1;
    if (r < 0 && g_find[slot].d) { closedir(g_find[slot].d); g_find[slot].d = 0; }
    guest_heap_unlock();
    return r;
}
static int _findclose(intptr_t h){
    if (h >= 0 && h < FIND_SLOTS && g_find[h].d) { closedir(g_find[h].d); g_find[h].d = 0; }
    return 0;
}
#endif

#include "lindbergh_rt.h"

/* UDP sink to the dev host — the managed sandbox hides file writes from
 * FTP, and /dev/klog's reader wedges EBUSY after suspended-process crashes.
 * sceNet is the managed app's own network stack: always reachable.
 * On the desktop build there is no sceKernelDebugOutText and libc's headers
 * own memset/sendto declarations — plain stderr is the equivalent sink. */
int id4_dbg(int ch, const char *s)
{
#if defined(__ORBIS__) || defined(__SCE__)
    extern int sceKernelDebugOutText(int, const char *);
    (void)ch;
    if (!s) return 0;
    static int us = -2;
    if (us == -2) {
        extern int socket(int, int, int);
        us = socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
    }
    if (us >= 0) {
        /* sendto to 192.168.137.1:3334 — BSD sockaddr_in is
         * {u8 sin_len; u8 sin_family; u16 sin_port; u32 sin_addr; u8 z[8]} —
         * as one u16 'fam' field that's (family<<8)|len = 0x0210, not 0x0002
         * (family at the wrong offset EINVALs every sendto). */
        struct { unsigned short fam, port; uint32_t addr; char z[8]; } sa;
        extern void *memset(void*,int,unsigned long); memset(&sa,0,sizeof sa);
        sa.fam=0x0210; sa.port=0x060D /*htons(3334)*/; sa.addr=0x0189A8C0 /*192.168.137.1 LE bytes C0 A8 89 01*/;
        extern long sendto(int,const void*,unsigned long,int,const void*,int);
        sendto(us, s, strlen(s), 0, &sa, 16);
    }
    {   /* __asm__ label binds to the real NID — the #define in the header
         * would otherwise turn this call back into id4_dbg (recursion). */
        extern int real_dbgtext(int, const char *) __asm__("sceKernelDebugOutText");
        real_dbgtext(0, s);
    }
#else
    (void)ch;
    if (s) { fputs(s, stderr); fflush(stderr); }
#endif
    return 0;
}

#include <stdarg.h>
int id4_dbgf(const char *fmt, ...)
{
    char b[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    id4_dbg(0, b);
    return 0;
}

extern int sprintf(char *, const char *, ...);

/* ---- FILE handle table ----
 *
 * PS5's libc places FILE* objects at 64-bit addresses (~0x029B67A0... range).
 * RET() truncates those to 32 bits, producing invalid guest VAs below the
 * image base (0x08048000). Every FILE* that crosses the host/guest boundary
 * must go through this translation table instead.
 *
 * Indices 0-2 are permanently stdin/stdout/stderr; the guest uses the token
 * FHANDLE_BASE+index as its opaque FILE handle. */
#define FHANDLE_BASE 0xF3000000u
#define FHANDLE_MAX  2048   /* the asset streamer keeps ~100 files open at
                             * car-select; 64 returned NULL for real files */
static FILE *g_fhdl[FHANDLE_MAX];
static int   g_fhdl_init;

static void fhdl_setup(void)
{
    if (g_fhdl_init) return;
    g_fhdl[0] = stdin; g_fhdl[1] = stdout; g_fhdl[2] = stderr;
    g_fhdl_init = 1;
}

static uint32_t fhdl_alloc(FILE *f)
{
    if (!f) return 0;
    fhdl_setup();
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    guest_heap_lock();
    uint32_t r = 0;
    for (int i = 3; i < FHANDLE_MAX; i++)
        if (!g_fhdl[i]) { g_fhdl[i] = f; r = FHANDLE_BASE + (uint32_t)i; break; }
    guest_heap_unlock();
    if (!r) { extern void klogf(const char*, ...); klogf("[fhdl] table full (%d)\n", FHANDLE_MAX); }
    return r;
}

FILE *fhdl_resolve(uint32_t h)
{
    fhdl_setup();
    unsigned i = h - FHANDLE_BASE;
    return i < FHANDLE_MAX ? g_fhdl[i] : NULL;
}
static FILE *fhdl_get(uint32_t h)
{
    fhdl_setup();
    if (h < FHANDLE_BASE)
        return h < 3 ? g_fhdl[h] : NULL;   /* 0/1/2 = stdin/stdout/stderr */
    uint32_t i = h - FHANDLE_BASE;
    FILE *f = i < (uint32_t)FHANDLE_MAX ? g_fhdl[i] : NULL;
    /* A NULL FILE* reaching a libc FILE* fn is the segv-at-9 path — log which
     * handle resolves bad so the offender's name shows up before the crash. */
    if (!f) {
        extern void klogf(const char *, ...);
        klogf("[fhdl] NULL for h=%#x\n", h);
    }
    return f;
}

static void fhdl_free(uint32_t h)
{
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    uint32_t i = h - FHANDLE_BASE;
    if (i >= 3 && i < (uint32_t)FHANDLE_MAX) {
        guest_heap_lock();
        g_fhdl[i] = NULL;
        guest_heap_unlock();
    }
}

/* ---- memory ----
 *
 * The guest's allocator is built here on plain malloc rather than on the
 * host's aligned one, and that is the second time this seam has bitten.
 *
 * First it was free(): the game memaligns its SSE buffers and releases them
 * with plain free(), exactly as glibc allows, but _aligned_malloc memory only
 * _aligned_free understands.
 *
 * Routing everything through _aligned_malloc fixed that and introduced the
 * next one, because _aligned_realloc requires the SAME alignment the block was
 * allocated with - and a block the game got from memalign(64, n) reallocated
 * at 16 is undefined. MSVC notices and calls __fastfail, which raises
 * STATUS_STACK_BUFFER_OVERRUN past every exception handler by design, so the
 * process vanishes with no report at all.
 *
 * Owning the layout ends the whole category. A header immediately below the
 * returned pointer carries the original block and the requested size, so
 * free() and realloc() work on anything malloc, calloc, memalign or strdup
 * returned, at any alignment, with no rules to remember. The magic word is
 * also a check at a real trust boundary: a guest pointer that never came from
 * here is refused rather than passed to free().
 */
#define GUEST_ALIGN 16
#define ALLOC_MAGIC 0xA110C8EDu

/* The guest heap has to live in guest address space: the lifted code reads
 * and writes every pointer it is handed, so a host malloc() result is a
 * 64-bit address the 32-bit registers truncate into garbage. guest_brk() grows
 * the image's own heap, so carve allocations out of it instead. A first-fit
 * freelist reuses freed blocks; the AllocHdr sits at a guest address just
 * before each block. */
/* MUST stay 16 bytes: blocks are carved at 16-aligned headers and data
 * follows the header directly — a 12-byte header would make every result
 * land at addr % 16 == 12 (4-aligned), breaking guest alignment. */
typedef struct { uint32_t magic, size, next, pad; } GHdr;
extern uint32_t guest_brk(uint32_t);
extern int guest_arena_contains(uint32_t);
extern uint32_t guest_map_arena(uint32_t);
static uint32_t g_free_head;
#define G_MAGIC 0x6d616c63u
/* Tail sentinel: pad holds the *requested* size on an allocated block; a
 * 12-byte record sits at payload+pad = {cookie, ~cookie, alloc_caller_eip},
 * re-checked on free/realloc so a guest overrun is caught while the offending
 * stack is still live instead of surfacing later as a wild uc_rip. A small
 * overrun hits the cookie first and leaves alloc_caller readable; pad is
 * stale/ignored on free blocks. */
#define G_CANARY 0xC0D3CA7Eu
#define G_CAN_SZ 12u
__thread uint32_t g_heap_caller;   /* guest eip that entered the alloc/free */

/* The game's worker threads allocate and free on the same guest heap - every
 * operator new/malloc/free/memalign lands here from whichever thread ran it.
 * The freelist walk and block split/coalesce are not reentrant: two threads
 * inside gmalloc can split the same block twice or splice a freed link into a
 * live chain, and the corruption surfaces far away as a moving low-address
 * fault (segv at 0x9/0x18 inside unrelated guest code). One recursive host
 * mutex covers the heap; recursive so callers may hold it across gmalloc calls
 * (hle_cg's getProgramString keeps the last result under the same lock). */
#include <pthread.h>

/* exit() issues SYS_exit — banned on managed Orbis titles (SIGSYS). The
 * sanctioned teardown is sceSystemServiceLoadExec("exit"). */
static void orb_exit(int rc) {
    extern int sceSystemServiceLoadExec(const char *, char *const *);
    (void)rc;
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) __asm__ volatile("" ::: "memory");
}
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_heap_lock_once = PTHREAD_ONCE_INIT;
static void g_heap_lock_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_heap_lock, &a);
    pthread_mutexattr_destroy(&a);
}
void guest_heap_lock(void)   { pthread_once(&g_heap_lock_once, g_heap_lock_init); pthread_mutex_lock(&g_heap_lock); }
void guest_heap_unlock(void) { pthread_mutex_unlock(&g_heap_lock); }

extern void guest_report_state(const char *);
static void g_canary_write(GHdr *h, uint32_t usize)
{
    h->pad = usize;
    uint32_t *w = (uint32_t *)((uintptr_t)h + sizeof(GHdr) + usize);
    w[0] = G_CANARY; w[1] = ~G_CANARY; w[2] = g_heap_caller;
}
/* Verify an *allocated* block's tail sentinel. A clean block returns quietly;
 * a trampled one dumps the victim header, both eips (the free that detected
 * it and the alloc that owned it), and the bytes straddling the cookie, then
 * exits - the corrupting store already happened, so this names the victim and
 * frames the search for the writer. */
static void g_canary_check(GHdr *h)
{
    uint32_t hv = (uint32_t)(uintptr_t)h;
    extern void klogf(const char*, ...);
    if (h->pad > h->size) {
        klogf("[canary] hdr-smash hdr=%#x usize=%#x cap=%#x free_caller=%#x\n",
              hv, h->pad, h->size, g_heap_caller);
        goto corrupt;
    }
    {
        uint32_t *w = (uint32_t *)((uintptr_t)h + sizeof(GHdr) + h->pad);
        if (w[0] == G_CANARY && w[1] == ~G_CANARY) return;
        uint32_t base = hv + (uint32_t)sizeof(GHdr) + h->pad;
        klogf("[canary] OVERRUN hdr=%#x usize=%#x cap=%#x alloc_by=%#x free_by=%#x\n",
              hv, h->pad, h->size, rd32(base + 8), g_heap_caller);
        for (int i = -8; i <= 16; i += 4)
            klogf("[canary]   [%#x]=%08x\n", base + i, rd32(base + i));
        goto corrupt;
    }
corrupt:
    guest_report_state("heap canary trip");
    orb_exit(46);
}

/* sceLibcMspace: Orbis libc's bucketed allocator. Weak so the Linux host build
 * (no Orbis libc) still links; runtime check falls back to the legacy freelist
 * when either symbols or create() are unavailable. Pattern copied from
 * ProsperoEden/third_party/app_heap.c and ps5-opengl Mesa gallium screen. */
__attribute__((weak)) void *sceLibcMspaceCreate(const char *, void *, size_t, unsigned);
__attribute__((weak)) void *sceLibcMspaceMalloc(void *, size_t);
__attribute__((weak)) void  sceLibcMspaceFree(void *, void *);
__attribute__((weak)) int   sceLibcMspacePosixMemalign(void *, void **, size_t, size_t);

static void *g_mspace;
static int   g_mspace_state;             /* 0=uninit, 1=ready, -1=disabled */
#define G_MAGIC_MS 0x6d616d73u           /* 'mamS': block owned by sceLibcMspace */

/* 145K tiny allocs per boot were linear-walking a shared free list; mspace is
 * bucketed, O(1) on the dominant path. The GHdr prefix is kept so the canary
 * wrapper and foreign-pointer detection in ghdr_of still work unchanged. */
static int g_mspace_init(void)
{
    if (g_mspace_state) return g_mspace_state == 1;
    /* Permanent-disable conditions get latched; transient failures (couldn't
     * grab arena yet, mspace create returned null) stay at state=0 so a later
     * call can retry. The arena, once mapped, is retained in a static — we
     * have no guest_unmap_arena, and re-mapping on every retry would exhaust
     * the <4GB guest VA. */
    if (!sceLibcMspaceCreate || !sceLibcMspaceMalloc ||
        !sceLibcMspaceFree    || !sceLibcMspacePosixMemalign) {
        g_mspace_state = -1; return 0;
    }
    const char *off = getenv("LINDBERGH_NO_MSPACE");
    if (off && *off && *off != '0') { g_mspace_state = -1; return 0; }
    static uint32_t cached_base, cached_sz;
    if (!cached_base) {
        for (uint32_t sz = 256u << 20; sz >= 32u << 20; sz >>= 1) {
            uint32_t base = guest_map_arena(sz);
            if (base) { cached_base = base; cached_sz = sz; break; }
        }
        if (!cached_base) return 0;   /* transient — try again later */
    }
    void *msp = sceLibcMspaceCreate("guest-heap",
                                    (void *)(uintptr_t)cached_base,
                                    cached_sz, 0);
    if (!msp) return 0;               /* transient — arena stays cached */
    g_mspace = msp;
    g_mspace_state = 1;
    extern int sceKernelDebugOutText(int, const char *);
    extern int sprintf(char*,const char*,...);
    char kb[96];
    sprintf(kb, "[mspace] arena %#x size=%uMB\n", cached_base, (unsigned)(cached_sz >> 20));
    sceKernelDebugOutText(0, kb);
    return 1;
}

/* Carve: a bytes of leading pad (for align > 16) then GHdr then n user bytes
 * then G_CAN_SZ canary. Guest ptr sits a-aligned at (raw + pad), GHdr at
 * (guest - 16); for the common 16-align path pad == sizeof(GHdr) and no
 * overallocation is needed. */
static void *gmalloc_mspace(size_t n, size_t align)
{
    size_t a = align < GUEST_ALIGN ? GUEST_ALIGN : align;
    size_t lead = (a > GUEST_ALIGN) ? a : sizeof(GHdr);
    void *raw = NULL;
    if (sceLibcMspacePosixMemalign(g_mspace, &raw, a, lead + n + G_CAN_SZ) != 0 || !raw)
        return NULL;
    uint8_t *p = (uint8_t *)raw + lead;
    GHdr *h = (GHdr *)(p - sizeof(GHdr));
    h->magic = G_MAGIC_MS;
    h->size  = (uint32_t)n;
    h->next  = (uint32_t)(uintptr_t)raw;     /* mspace base - free() needs it */
    g_canary_write(h, (uint32_t)n);
    return p;
}

static void gfree_mspace(GHdr *h)
{
    g_canary_check(h);
    uint32_t raw = h->next;
    h->magic = 0;
    sceLibcMspaceFree(g_mspace, (void *)(uintptr_t)raw);
}

static void freelist_insert(uint32_t hv)
{
    /* Address-ordered insert + coalesce; hv points at a live GHdr. The walk
     * range-checks each link - a guest overrun that stomped a header would
     * otherwise send it through a wild host dereference. */
    GHdr *h = (GHdr *)(uintptr_t)hv;
    uint32_t prev = 0, cur = g_free_head;
    for (int it = 0; it < 100000 && cur && cur < hv; it++) {
        if (!guest_arena_contains(cur)) { cur = 0; break; }
        prev = cur;
        uint32_t nx = ((GHdr *)(uintptr_t)cur)->next;
        if (nx && !guest_arena_contains(nx)) { cur = 0; break; }
        cur = nx;
    }
    h->next = cur;
    if (prev) ((GHdr *)(uintptr_t)prev)->next = hv;
    else g_free_head = hv;
    /* merge with the next block if adjacent */
    if (cur && (uintptr_t)h + sizeof(GHdr) + h->size == cur) {
        GHdr *n = (GHdr *)(uintptr_t)cur;
        h->size += (uint32_t)sizeof(GHdr) + n->size;
        h->next = n->next;
    }
    /* merge with the previous block if adjacent */
    if (prev) {
        GHdr *pr = (GHdr *)(uintptr_t)prev;
        if ((uintptr_t)pr + sizeof(GHdr) + pr->size == hv) {
            pr->size += (uint32_t)sizeof(GHdr) + h->size;
            pr->next = h->next;
        }
    }
}

/* *out gets the new block's data VA when the contiguous brk arena can serve.
 * Returns 1 on that carve, 2 when a whole spill arena was pushed onto the
 * free list (caller retries the fit), 0 when there is no room anywhere. */
static int g_extend(size_t n, size_t align, uint32_t *out)
{
    if (align < 16) align = 16;
    extern uint32_t g_heap_max;
    uint32_t b = guest_brk(0);
    uint32_t hoff = (b + align - 1) & ~(uint32_t)(align - 1);
    uint32_t need = hoff + (uint32_t)n + sizeof(GHdr) + 16;
    if (g_heap_max && need > g_heap_max) {
        /* contiguous arena full - spill into a chunk anywhere in guest VA.
         * Orbis's contiguous wall sits near 352 MB but the process still has
         * GBs of address space; each spill arena joins the one free list. */
        uint32_t csz = 32u << 20;
        if (csz < (uint32_t)n + sizeof(GHdr) + 64)
            csz = ((uint32_t)n + sizeof(GHdr) + 64 + 0xFFFFF) & ~0xFFFFFu;
        uint32_t base = guest_map_arena(csz);
        if (!base) {
            static int said;
            if (!said) {
                said = 1;
                extern int sceKernelDebugOutText(int, const char *);

                extern int sprintf(char*,const char*,...);
                char kb[96];
                sprintf(kb,"[gmalloc] heap exhausted n=%u\n",(unsigned)n);
                sceKernelDebugOutText(0, kb);
            }
            return 0;
        }
        GHdr *h = (GHdr *)(uintptr_t)base;
        h->magic = G_MAGIC; h->size = csz - (uint32_t)sizeof(GHdr); h->next = 0;
        freelist_insert(base);
        return 2;
    }
    guest_brk(need);
    GHdr *h = (GHdr *)(uintptr_t)hoff;
    h->magic = G_MAGIC; h->size = (uint32_t)n; h->next = 0;
    *out = hoff + sizeof(GHdr);
    return 1;
}

void *gmalloc(size_t n, size_t align)
{
    if (n == 0) n = 1;
    void *result = NULL;
    guest_heap_lock();
    if (g_mspace_init()) {
        result = gmalloc_mspace(n, align);
        if (result) { guest_heap_unlock(); return result; }
        /* mspace exhausted — fall through to the legacy arena so a late large
         * carve can still land in spill memory. */
    }
    /* first-fit over the free list; a corrupted link means a guest overrun
     * stomped the headers - drop the list rather than chase it into a wild
     * host dereference. The freelist only guarantees the GHdr carve
     * alignment (16) - anything stricter goes through g_extend's aligned
     * carve instead of returning a misaligned block from the list. */
    if (align > GUEST_ALIGN) {
        uint32_t got = 0;
        if (g_extend(n + G_CAN_SZ, align, &got)) {
            result = (void *)(uintptr_t)got;
            g_canary_write((GHdr *)((uintptr_t)got - sizeof(GHdr)), (uint32_t)n);
        }
        guest_heap_unlock();
        return result;
    }
    size_t cn = n + G_CAN_SZ;               /* carve covers the tail canary */
    for (;;) {
        uint32_t prev = 0, cur = g_free_head;
        while (cur) {
            if (!guest_arena_contains(cur)) {
                extern int sceKernelDebugOutText(int, const char *);
                extern int sprintf(char*,const char*,...);
                char b[96]; sprintf(b, "[gc] corrupt link cur=%#x prev=%#x n=%u\n",
                                    cur, prev, (unsigned)n);
                sceKernelDebugOutText(0, b);
                g_free_head = 0;
                break;
            }
            GHdr *h = (GHdr *)(uintptr_t)cur;
            if (h->size >= cn) {
                /* split when the leftover can still hold a useful block */
                if (h->size >= cn + sizeof(GHdr) + 32) {
                    GHdr *r = (GHdr *)(uintptr_t)(cur + sizeof(GHdr) + cn);
                    r->magic = G_MAGIC;
                    r->size = h->size - (uint32_t)sizeof(GHdr) - (uint32_t)cn;
                    r->next = h->next;
                    h->size = (uint32_t)cn;
                    if (prev) ((GHdr *)(uintptr_t)prev)->next = (uint32_t)(uintptr_t)r;
                    else g_free_head = (uint32_t)(uintptr_t)r;
                } else {
                    if (prev) ((GHdr *)(uintptr_t)prev)->next = h->next;
                    else g_free_head = h->next;
                }
                h->magic = G_MAGIC;
                g_canary_write(h, (uint32_t)n);
                uint32_t r = cur + sizeof(GHdr);
                result = (void *)(uintptr_t)r;
                goto out;
            }
            prev = cur; cur = h->next;
        }
        uint32_t got = 0;
        int how = g_extend(cn, align, &got);
        if (how == 0) { result = NULL; goto out; }   /* genuinely out of memory */
        if (how == 1) {
            result = (void *)(uintptr_t)got;
            g_canary_write((GHdr *)((uintptr_t)got - sizeof(GHdr)), (uint32_t)n);
            goto out;
        }
        /* how == 2: a spill arena just joined the free list - loop and fit */
    }
out:
    guest_heap_unlock();
    return result;
}

static GHdr *ghdr_of(void *p)
{
    uint32_t pv = (uint32_t)(uintptr_t)p;
    if (!p || pv < 0x8000000u || !guest_arena_contains(pv)) return NULL;
    GHdr *h = (GHdr *)((uintptr_t)p - sizeof(GHdr));
    return (h->magic == G_MAGIC || h->magic == G_MAGIC_MS) ? h : NULL;
}

void gfree(void *p)
{
    static long n_bad;
    guest_heap_lock();
    GHdr *h = ghdr_of(p);
    if (!h) {
        if (p && ++n_bad <= 8) {
            extern int sceKernelDebugOutText(int, const char *);
            extern int sprintf(char*,const char*,...);
            char b[80]; sprintf(b, "[gfree] foreign ptr %#x\n", (unsigned)(uintptr_t)p);
            sceKernelDebugOutText(0, b);
        }
        guest_heap_unlock();
        return;
    }
    if (h->magic == G_MAGIC_MS) {
        gfree_mspace(h);
        guest_heap_unlock();
        return;
    }
    /* Tail sentinel: a clean block reads back both cookies; a trampled one
     * means a guest overrun - name the victim + caller eip, then exit. */
    g_canary_check(h);
    /* A free block keeps its magic, so a second free of the same block reads
     * as a live GHdr and inserts a cycle into the list - the next alloc walks
     * the ring or splits a block that is already handed out. Tag freed blocks
     * so the double-free lands on the foreign-pointer log instead. */
    h->magic = 0;
    /* Address-ordered insert + coalesce: without merging, hundreds of same-size
     * shader compiles fragment the heap into unusable splinters. */
    freelist_insert((uint32_t)((uintptr_t)h));
    /* magic STAYS 0 for free blocks - it is the in-use bit; restoring G_MAGIC
     * here would let a double-free re-insert this live free node and hand the
     * same memory out twice. freelist_insert only reads h->size/h->next. */
    guest_heap_unlock();
}

void *grealloc(void *p, size_t n)
{
    if (!p) return gmalloc(n, GUEST_ALIGN);
    /* Heap lock is recursive — hold it across gmalloc/memcpy/gfree so another
     * thread cannot free p mid-copy, insert it into the freelist, and then
     * have gmalloc hand that same block back as q (which would make the
     * memcpy read from — and write over — overlapping storage). */
    guest_heap_lock();
    GHdr *h = ghdr_of(p);
    if (!h) { guest_heap_unlock();
        extern void klogf(const char*, ...);
        static int _fr; if (_fr++ < 12)
            klogf("[heap] grealloc foreign p=%p n=%u -> fresh alloc (old data dropped)\n", p, (unsigned)n);
        return gmalloc(n, GUEST_ALIGN); }
    g_canary_check(h);
    /* grow/shrink in place only if the canary still fits inside the carve */
    if (h->size >= (uint32_t)n + G_CAN_SZ) {
        g_canary_write(h, (uint32_t)n);
        guest_heap_unlock(); return p;
    }
    size_t old = h->pad;   /* user bytes, not the carve capacity */
    void *q = gmalloc(n, GUEST_ALIGN);
    if (q) { memcpy(q, p, old < n ? old : n); gfree(p); }
    guest_heap_unlock();
    return q;
}

static void alloc_log(const char *who, uint32_t n, uint32_t got)
{
    (void)who; (void)n; (void)got;
}

static void h_malloc(CPU *c)
{
    g_heap_caller = c->eip;
    uint32_t n = A32(0), got = (uint32_t)(uintptr_t)gmalloc(n, GUEST_ALIGN);
    /* Per-call fprintf used to fire here — 737K allocs per boot × UDP klog =
     * minutes of pure I/O wait. Watch-gated klogf below is the diagnostic. */
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if (got >= g_watch_lo && got < g_watch_hi)
        klogf("[heap] malloc n=%#x -> %#x\n", n, got);
    RET(got);
}
static void h_realloc (CPU *c) { g_heap_caller = c->eip; RET(grealloc(APTR(0), A32(1))); }
static void h_free    (CPU *c) {
    uint32_t p = A32(0);
    g_heap_caller = c->eip;
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if (p >= g_watch_lo && p < g_watch_hi)
        klogf("[heap] free %#x\n", p);
    gfree(APTR(0));
}

/* ---- C++ allocation ----
 *
 * operator new and its friends, which a C++ game reaches for constantly and
 * which are not libc. Let's Go Jungle imports none of these - its engine
 * allocates through its own pools - so nothing here needed them until Ghost
 * Squad Evolution, which imports all four. An unbound operator new returns
 * zero under permissive mode, every construction writes through a null, and
 * the game dies before it reaches its own main loop with nothing to say why.
 *
 * new never returns null in C++ - it throws - so a failure here is not
 * something the game will check for, and there is nothing useful to do about
 * it other than say so. */
static void h_op_new(CPU *c)
{
    g_heap_caller = c->eip;
    void *p = gmalloc(A32(0), GUEST_ALIGN);
    if (!p) fprintf(stderr, "[libc] operator new(%u) failed\n", A32(0));
    RET(p);
}

static void h_op_delete(CPU *c) {
    uint32_t p = A32(0);
    g_heap_caller = c->eip;
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if (p >= g_watch_lo && p < g_watch_hi)
        klogf("[heap] delete %#x\n", p);
    gfree(APTR(0)); RET(0);
}

/* isprint and friends get asked about bytes above 127, where the host's
 * locale-aware version is free to disagree with the guest's. The C locale
 * answer is the one a 2007 arcade binary was built against. */
/* The game's own assertions. Unbound, one is a silent exit; bound, it names
 * the file, the line and the expression that failed, which is the game
 * telling you exactly what it does not like about its environment. */
static void h_assert_fail(CPU *c)
{
    fprintf(stderr, "[assert] %s failed at %s:%u in %s\n",
            A32(0) ? ASTR(0) : "(null)",
            A32(1) ? ASTR(1) : "(null)",
            A32(2),
            A32(3) ? ASTR(3) : "(null)");
    fflush(stderr);
    guest_report_state("assertion failed");
    orb_exit(44);
}

/* Defined with the maths handlers further down; these need them earlier. */
static double guest_arg_d(CPU *c, unsigned i);
static float  guest_arg_f(CPU *c, unsigned i);

static void h_atan2(CPU *c)  { RETF(atan2(guest_arg_d(c, 0), guest_arg_d(c, 2))); }
static void h_atan2f(CPU *c) { RETF(atan2f(guest_arg_f(c, 0), guest_arg_f(c, 1))); }
static void h_fmodf(CPU *c)  { RETF(fmodf(guest_arg_f(c, 0), guest_arg_f(c, 1))); }
static void h_sqrt(CPU *c)   { RETF(sqrt(guest_arg_d(c, 0))); }
static void h_sqrtf(CPU *c)  { RETF(sqrtf(guest_arg_f(c, 0))); }
static void h_srand(CPU *c)  { srand(A32(0)); RET(0); }
/* Guest pointers are checked before they reach a CRT function. A null here
 * is not a crash in the guest's world - it is on Windows, and a silent one. */
static void h_clearerr(CPU *c) { FILE *f = fhdl_get(A32(0)); if (f) clearerr(f); RET(0); }
static void h_sysconf(CPU *c)  { RET(A32(0) == 30 ? 4096u : 1u); }  /* _SC_PAGESIZE */
static void h_sleep(CPU *c)    { Sleep(A32(0) * 1000u); RET(0); }
static void h_strcasecmp(CPU *c)
{
    const char *a = A32(0) ? ASTR(0) : "", *b = A32(1) ? ASTR(1) : "";
    RET((uint32_t)_stricmp(a, b));
}
static void h_strncasecmp(CPU *c)
{
    const char *a = A32(0) ? ASTR(0) : "", *b = A32(1) ? ASTR(1) : "";
    RET((uint32_t)_strnicmp(a, b, A32(2)));
}

/* qsort, with the comparison living in lifted code.
 *
 * The callback is the whole difficulty: it is a guest function, so every
 * comparison is a call back across the seam. An insertion sort keeps that
 * simple and needs no scratch beyond one element - these are short arrays
 * that a game sorts once, not a hot path, and a subtle quicksort that calls
 * into the guest from inside its own recursion is not worth the risk. */
static void h_qsort(CPU *c)
{
    uint32_t base = A32(0), n = A32(1), sz = A32(2), cmp = A32(3);
    /* Every one of these comes from the guest, and a wrong one does not fail
     * politely: the sort walks off the end of what it was given and writes
     * over the host's own memory, which ends as a fail-fast with no handler
     * and nothing printed. Bound them all. */
    if (getenv("LINDBERGH_ALLOC"))
        fprintf(stderr, "[libc] qsort base 0x%08X n %u size %u cmp 0x%08X\n",
                base, n, sz, cmp);
    if (!base || !cmp || sz == 0 || n < 2) { RET(0); return; }
    if (n > (1u << 20) || (unsigned long long)n * sz > (64ull << 20)) {
        fprintf(stderr, "[libc] qsort refused: %u elements of %u bytes\n", n, sz);
        RET(0);
        return;
    }
    if (sz > 4096) { fprintf(stderr, "[libc] qsort element %u bytes, refusing\n", sz);
                     RET(0); return; }

    /* The comparator reads args[1] as a guest pointer — malloc() can land
     * above 4GB and truncate to garbage; gmalloc stays in guest space. */
    unsigned char *tmp = (unsigned char *)gmalloc(sz, GUEST_ALIGN);
    if (!tmp) { RET(0); return; }
    unsigned char *a = (unsigned char *)(uintptr_t)base;

    for (uint32_t i = 1; i < n; i++) {
        memcpy(tmp, a + (size_t)i * sz, sz);
        uint32_t j = i;
        while (j > 0) {
            uint32_t args[2];
            args[0] = base + (uint32_t)((j - 1) * sz);
            args[1] = (uint32_t)(uintptr_t)tmp;
            if ((int32_t)guest_call(c, cmp, args, 2) <= 0) break;
            memcpy(a + (size_t)j * sz, a + (size_t)(j - 1) * sz, sz);
            j--;
        }
        memcpy(a + (size_t)j * sz, tmp, sz);
    }
    gfree(tmp);
    RET(0);
}

static void h_atanf(CPU *c) { RETF(atanf(guest_arg_f(c, 0))); }
static void h_powf(CPU *c)  { RETF(powf(guest_arg_f(c, 0), guest_arg_f(c, 1))); }
static void h_rewind(CPU *c) { FILE *f = fhdl_get(A32(0)); if (f) rewind(f); RET(0); }
static void h_isdigit(CPU *c){ uint32_t ch = A32(0); RET(ch >= '0' && ch <= '9'); }

static void h_isprint(CPU *c) { uint32_t ch = A32(0); RET(ch >= 0x20 && ch < 0x7F); }

/* nanosleep takes a timespec; the guest lays it out as two longs. */
static void h_nanosleep(CPU *c)
{
    uint32_t req = A32(0);
    if (req) {
        uint32_t sec = rd32(req), nsec = rd32(req + 4);
        DWORD ms = sec * 1000u + nsec / 1000000u;
        Sleep(ms ? ms : 1);
    }
    RET(0);
}
static void h_memalign(CPU *c)
{
    g_heap_caller = c->eip;
    uint32_t al = A32(0), n = A32(1);
    /* POSIX requires alignment to be a power of two and a multiple of
     * sizeof(void*). gmalloc trusts al and uses it in masks — a zero or
     * non-pow2 value corrupts the carve. Round up to GUEST_ALIGN rather
     * than failing, matching how most libcs tolerate sloppy callers. */
    if (al < GUEST_ALIGN) al = GUEST_ALIGN;
    if (al & (al - 1)) {
        uint32_t p2 = GUEST_ALIGN;
        while (p2 < al) p2 <<= 1;
        al = p2;
    }
    void *p = gmalloc(n, al);
    /* Per-call fprintf used to fire here — 737K memaligns per boot × UDP klog
     * dominated boot time. Watch-gated klogf below matches h_malloc's pattern. */
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    uint32_t got = (uint32_t)(uintptr_t)p;
    if (got >= g_watch_lo && got < g_watch_hi)
        klogf("[heap] memalign al=%u n=%#x -> %#x\n", al, n, got);
    RET(p);
}

static void h_calloc(CPU *c)
{
    if (A32(1) && (uint32_t)A32(0) > (UINT32_MAX / (uint32_t)A32(1))) { RET(0); return; }
    size_t n = (size_t)A32(0) * A32(1);
    g_heap_caller = c->eip;
    void *p = gmalloc(n, GUEST_ALIGN);
    if (p) memset(p, 0, n);
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if ((uintptr_t)p >= g_watch_lo && (uintptr_t)p < g_watch_hi)
        klogf("[heap] calloc n=%#x -> %#x\n", (unsigned)n, (unsigned)(uintptr_t)p);
    RET(p);
}

/* A pointer the guest supplied, checked before the host writes through it.
 *
 * This is a trust boundary: every address here was computed by recompiled
 * code, and a single mis-lifted instruction upstream turns memcpy into a tool
 * for scribbling over the host's own stacks and heap. The failure then lands
 * somewhere else entirely, long afterwards, as a corrupted cookie or a
 * corrupted arena, with nothing left to say where it came from.
 *
 * The guest's allocations come from the host heap and so have no tidy range to
 * check against. What is checkable is the shape of a plainly broken pointer:
 * the bottom of the address space is never mapped, and no call in this game
 * moves a quarter of a gigabyte. */
#define GUEST_LOW  0x10000u
#define GUEST_HUGE (256u << 20)

static int sane(const char *who, uint32_t va, uint32_t len)
{
    if (va < GUEST_LOW || len > GUEST_HUGE) {
        fprintf(stderr, "[libc] %s(%#010x, %u bytes) refused - bad pointer or size\n",
                who, va, len);
        fflush(stderr);
        return 0;
    }
    return 1;
}

static void h_memcpy(CPU *c)
{
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if (A32(0) < g_watch_hi && A32(0) + A32(2) > g_watch_lo)
        klogf("[heap] memcpy %#x <- %#x n=%#x\n", A32(0), A32(1), A32(2));
    if (!sane("memcpy dst", A32(0), A32(2)) || !sane("memcpy src", A32(1), A32(2))) { RET(A32(0)); return; }
    RET(memcpy(APTR(0), APTR(1), A32(2)));
}
static void h_memmove(CPU *c)
{
    if (!sane("memmove dst", A32(0), A32(2)) || !sane("memmove src", A32(1), A32(2))) { RET(A32(0)); return; }
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if (A32(0) < g_watch_hi && A32(0) + A32(2) > g_watch_lo)
        klogf("[heap] memmove %#x <- %#x n=%#x\n", A32(0), A32(1), A32(2));
    RET(memmove(APTR(0), APTR(1), A32(2)));
}
static void h_memset(CPU *c)
{
    uint32_t d = A32(0), n = A32(2);
    if (!sane("memset", d, n)) { RET(d); return; }
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if (d < g_watch_hi && d + n > g_watch_lo)
        klogf("[heap] memset %#x n=%#x v=%#x\n", d, n, A32(1));
    RET(memset(APTR(0), AI32(1), n));
}
static void h_memchr (CPU *c) { RET(memchr(APTR(0), AI32(1), A32(2))); }

/* ---- strings ---- */
static void h_strlen  (CPU *c) { RET(strlen(ASTR(0))); }
static void h_strcpy  (CPU *c) { RET(strcpy(ASTR(0), ASTR(1))); }
static void h_strncpy (CPU *c) {
    extern void klogf(const char*, ...);
    extern uint32_t g_watch_lo, g_watch_hi;
    if (A32(0) < g_watch_hi && A32(0) + A32(2) > g_watch_lo)
        klogf("[heap] strncpy %#x n=%#x\n", A32(0), A32(2));
    RET(strncpy(ASTR(0), ASTR(1), A32(2)));
}
static void h_strcat  (CPU *c) { RET(strcat(ASTR(0), ASTR(1))); }
static void h_strncat (CPU *c) { RET(strncat(ASTR(0), ASTR(1), A32(2))); }
static void h_strcmp  (CPU *c) {
    RET(strcmp(ASTR(0), ASTR(1)));
}
static void h_strncmp (CPU *c) { RET(strncmp(ASTR(0), ASTR(1), A32(2))); }
static void h_strchr  (CPU *c) { RET(strchr(ASTR(0), AI32(1))); }
static void h_strrchr (CPU *c) { RET(strrchr(ASTR(0), AI32(1))); }
static void h_strstr  (CPU *c) { RET(strstr(ASTR(0), ASTR(1))); }
static void h_strerror(CPU *c)
{
    const char *s = strerror(AI32(0));
    static uint32_t g_err_str;
    if (!g_err_str) { extern uint32_t guest_brk(uint32_t); uint32_t b=(guest_brk(0)+0xf)&~0xf; guest_brk(b+128); g_err_str=b; }
    memset((void*)(uintptr_t)g_err_str, 0, 128);
    strncpy((char*)(uintptr_t)g_err_str, s, 127);
    RET(g_err_str);
}

static void h_strdup(CPU *c)
{
    /* Not the host strdup: the guest hands the result to its own free(), which
     * is this file's free(), so it must come from this file's malloc(). */
    const char *s = ASTR(0);
    size_t n = strlen(s) + 1;
    char *p = (char *)gmalloc(n, GUEST_ALIGN);
    if (p) memcpy(p, s, n);
    RET(p);
}

/* glibc exports plain index/rindex as aliases of strchr/strrchr. */
static void h_index(CPU *c) { RET(strchr(ASTR(0), AI32(1))); }

/* ---- formatted output ----
 *
 * printf cannot be forwarded to the host, because the arguments are on the
 * GUEST stack and there is no portable way to build a host va_list from them.
 * So the format string is walked here and each directive handed to the host
 * snprintf on its own, with the one argument it consumes pulled off the guest
 * stack by hand. That keeps the host's formatting for the fiddly parts - field
 * width, precision, %g - without ever needing a real va_list.
 */
static int guest_format(char *out, size_t cap, const char *fmt, uint32_t argbase)
{
    unsigned argi = 0;      /* index into the argument list at argbase */
    size_t n = 0;
    char spec[64];

#define PUT(ch)     do { if (n + 1 < cap) out[n] = (char)(ch); n++; } while (0)
#define APPEND(str) do { const char *_p = (str); while (*_p) PUT(*_p++); } while (0)

    while (*fmt) {
        if (*fmt != 37) { PUT(*fmt++); continue; }        /* 37 is '%' */

        const char *start = fmt++;
        if (*fmt == 37) { PUT(37); fmt++; continue; }

        int stars = 0, longlong = 0;
        while (*fmt && strchr("-+ #0", *fmt)) fmt++;                 /* flags */
        while (*fmt == 42 || isdigit((unsigned char)*fmt)) {         /* width */
            if (*fmt == 42) stars++;
            fmt++;
        }
        if (*fmt == 46) {                                            /* precision */
            fmt++;
            while (*fmt == 42 || isdigit((unsigned char)*fmt)) {
                if (*fmt == 42) stars++;
                fmt++;
            }
        }
        while (*fmt && strchr("hlLqjzt", *fmt)) {                    /* length */
            if ((*fmt == 108 && fmt[1] == 108) || *fmt == 113 || *fmt == 76)
                longlong = 1;                                        /* ll, q, L */
            fmt++;
        }
        char conv = *fmt ? *fmt++ : 0;

        size_t len = (size_t)(fmt - start);
        if (!conv || len >= sizeof spec - 4) { APPEND("<badfmt>"); continue; }
        memcpy(spec, start, len);
        spec[len] = 0;

        /* A star takes its width from the argument list, ahead of the value.
         * Rather than reproduce that plumbing, fold each one into the spec as
         * a literal so the host only ever sees a fixed width. */
        for (int i = 0; i < stars; i++) {
            char fixed[96];
            char *st = strchr(spec, 42);
            if (!st) break;
            snprintf(fixed, sizeof fixed, "%.*s%d%s",
                     (int)(st - spec), spec, (int)rd32(argbase + 4u * argi++), st + 1);
            snprintf(spec, sizeof spec, "%s", fixed);
        }

        char piece[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c':
            if (longlong) {
                unsigned long long v = (unsigned long long)rd32(argbase + 4u * argi) |
                                       ((unsigned long long)rd32(argbase + 4u * (argi + 1)) << 32);
                argi += 2;
                char *q = strchr(spec, 113);     /* glibc %q -> %l, MSVC knows %ll */
                if (q) *q = 108;
                snprintf(piece, sizeof piece, spec, v);
            } else {
                snprintf(piece, sizeof piece, spec, (int)rd32(argbase + 4u * argi++));
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            /* A double is 8 bytes on the guest stack whether the call site
             * wrote a float or not - C promotes it in a variadic call. */
            uint64_t bits = (uint64_t)rd32(argbase + 4u * argi) | ((uint64_t)rd32(argbase + 4u * (argi + 1)) << 32);
            double d;
            argi += 2;
            memcpy(&d, &bits, 8);
            snprintf(piece, sizeof piece, spec, d);
            break;
        }
        case 's': {
            const char *sv = (const char *)(uintptr_t)rd32(argbase + 4u * argi++);
            snprintf(piece, sizeof piece, spec, sv ? sv : "(null)");
            break;
        }
        case 'p':
            snprintf(piece, sizeof piece, "0x%08X", rd32(argbase + 4u * argi++));
            break;
        case 'n':
            argi++;                          /* refuse, but still eat the pointer */
            piece[0] = 0;
            break;
        default:
            snprintf(piece, sizeof piece, "<bad %c>", conv);
            break;
        }
        APPEND(piece);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
#undef APPEND
    return (int)n;
}

/* One scratch buffer. The guest is single-threaded through these, and a 16 KB
 * automatic in a deep mechanically-translated call chain is a stack overflow
 * waiting to happen.
 * ponytail: make it thread-local if two guest threads ever format at once. */
/* Per-thread: CRI's file server (lxCiReqRd/lxCiExecServer) sprintf()s on a
 * worker while the main thread formats too; a shared buffer corrupts paths. */
static __thread char g_fmtbuf[16384];

static void h_printf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(0), c->esp + 4u);
    fputs(g_fmtbuf, stdout);
    RET(n);
}
/* fprintf/vfprintf to an untracked FILE* (the game's stdout/stderr globals
 * resolved at link time, strPutConsole's console stream) used to be dropped;
 * route them to stderr -> klog so the game's own narration is visible,
 * which is how lindbergh-loader surfaces its errors. */
static void h_fprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), c->esp + 8u);
    FILE *f = fhdl_get(A32(0));
    if (!f) { fputs("[game] ", stderr); f = stderr; }
    fputs(g_fmtbuf, f);
    RET(n);
}
static void h_sprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), c->esp + 8u);
    /* guest_format reports the length the format WOULD have produced, which is
     * what sprintf returns - but only what fit is in the buffer, so copying n
     * bytes would read past it. */
    size_t have = strlen(g_fmtbuf);
    memcpy(ASTR(0), g_fmtbuf, have + 1);
    RET(n);
}
static void h_snprintf(CPU *c)
{
    uint32_t cap = A32(1);
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(2), c->esp + 12u);
    if (cap) {
        size_t take = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        memcpy(ASTR(0), g_fmtbuf, take);
        ASTR(0)[take] = 0;
    }
    RET(n);                              /* the length it WOULD have been */
}

/* Game paths are relative to the title dir; the managed sandbox does not
 * chdir us there. Resolve a non-absolute guest path under /app0. */
const char *app0_root(void)
{
    /* LINDBERGH_APP0 points the desktop build at an unpacked game tree;
     * on PS5 the default /app0 mount is always right. */
    const char *e = getenv("LINDBERGH_APP0");
    return (e && *e) ? e : "/app0";
}

static const char *guest_path(const char *p)
{
    static __thread char ap[512];   /* fopen races across threads otherwise */
    if (!p || !*p) return p;
    /* /mnt/… — the cabinet's deliverable/media mount; /var/tmp/… is its
     * scratch dir — remap both to /app0/… so they resolve instead of ENOENT. */
    if (p[0] == '/' && (!strncmp(p, "/mnt/", 5) || !strncmp(p, "/var/tmp/", 9))) {
        snprintf(ap, sizeof ap, "%s%s", app0_root(), p);
        return ap;
    }
    if (p[0] == '/') {
        /* Absolute guest path: on the cabinet these live under its rootfs —
         * /shader, /data, /initialD. On desktop (and only there) the host fs
         * has no such dirs, so when a root override is set, try it. */
        if (getenv("LINDBERGH_APP0")) {
            snprintf(ap, sizeof ap, "%s%s", app0_root(), p);
            return ap;
        }
        return p;
    }
    if (p[0]=='.' && p[1]=='/') p += 2;
    snprintf(ap, sizeof ap, "%s/%s", app0_root(), p);
    return ap;
}

/* ---- stdio ---- */
static void h_fopen (CPU *c)
{
    const char *raw = A32(0) ? ASTR(0) : "(null)";
    const char *path = guest_path(raw);
    const char *mode = A32(1) ? ASTR(1) : "r";
    /* /proc/bus/pci — the game probes Linux's PCI config files for its GPU;
     * /proc/bus/pci/01/00.0 is the 256-byte raw config of the VGA device at
     * bus 1 slot 0 (the NVIDIA 7800GTX on Lindbergh). Serve a fake config
     * with NVIDIA's vendor/device so the probe reads real bytes. */
    if (!strcmp(path, "/proc/bus/pci/01/00.0") ||
        !strncmp(path, "/proc/bus/pci/", 14)) {
        FILE *t = fopen("/app0/pci0100.0", "rb");
        if (t) { RET(fhdl_alloc(t)); return; }
    }
    FILE *f = fopen(path, mode);
    if (!f && path != raw) f = fopen(raw, mode);
    /* /app0's root is a signed-package mount — loose files there EINVAL.
     * Deliverable files (timestamp.dat etc.) live under data/ — retry there
     * when the root lookup fails. */
    {   const char *_r = app0_root(); size_t _rl = strlen(_r);
        if (!f && path && !strncmp(path, _r, _rl) && path[_rl] == '/') {
            char alt[512];
            snprintf(alt, sizeof alt, "%s/data/%s", _r, path + _rl + 1);
            f = fopen(alt, mode);
        } }
    /* The JP asset tree is incomplete in this data set: the locale flag
     * points at ACROBATA/JP/<name> but many files only exist as
     * ACROBATA/EN/E_<name>. ACRO_GAMEPATH already falls back EN->JP through
     * __xstat — this is the same idea in the other direction, at the fopen
     * layer so every locale consumer gets it. */
    if (!f) {
        /* Either spelling can be the live one — /app0 on the board, ./ on
         * Linux — so derive the EN path from each in turn. */
        const char *cands[2] = { path, raw };
        for (int i = 0; i < 2 && !f; i++) {
            const char *jp = strstr(cands[i], "/ACROBATA/JP/");
            if (!jp) continue;
            char alt[512];
            snprintf(alt, sizeof alt, "%.*s/ACROBATA/EN/E_%s",
                     (int)(jp - cands[i]), cands[i],
                     jp + strlen("/ACROBATA/JP/"));
            f = fopen(alt, mode);
            if (f) klogf("[fopen] JP miss -> EN fallback %s\n", alt);
        }
    }
    uint32_t h = fhdl_alloc(f);
    klogf("[fopen] %s -> %#x\n", path, h);
    if (!f) {
        extern int sceKernelAvailableFlexibleMemorySize(size_t *);
        size_t flex = 0; sceKernelAvailableFlexibleMemorySize(&flex);
        klogf("[fopen-FAIL] %s (raw %s, mode %s) errno=%d flex=%zu\n",
              path, raw, mode, errno, flex);
        /* fopen's FILE comes from the default mspace — ENOMEM means that
         * arena is full. We used to mass-close every open FILE* in the fhdl
         * table here, but the guest holds those handles and the next fread
         * would UAF. If the streamer truly leaks FILEs we need a refcount or
         * LRU scheme; until then, surface the error. Opt in to the old
         * behaviour with LINDBERGH_FOPEN_SCAVENGE=1 for A/B diagnosis — it
         * can crash the guest. */
        if (errno == ENOMEM) {
            const char *scav = getenv("LINDBERGH_FOPEN_SCAVENGE");
            if (scav && *scav && *scav != '0') {
                extern void guest_heap_lock(void), guest_heap_unlock(void);
                int freed = 0;
                FILE *stale[64];
                guest_heap_lock();
                for (int i = 3; i < FHANDLE_MAX && freed < 64; i++)
                    if (g_fhdl[i]) { stale[freed++] = g_fhdl[i]; g_fhdl[i] = NULL; }
                guest_heap_unlock();
                for (int i = 0; i < freed; i++) fclose(stale[i]);
                if (freed) {
                    klogf("[fopen] ENOMEM — freed %d stale FILEs (UNSAFE), retry\n", freed);
                    f = fopen(path, mode);
                    if (!f && path != raw) f = fopen(raw, mode);
                    if (!f && path && !strncmp(path, "/app0/", 6)) {
                        char alt[512];
                        snprintf(alt, sizeof alt, "/app0/data/%s", path + 6);
                        f = fopen(alt, mode);
                    }
                }
            } else {
                klogf("[fopen] ENOMEM — scavenge disabled (set LINDBERGH_FOPEN_SCAVENGE=1 to enable, unsafe)\n");
            }
        }
        if (!f) {
            extern int sceLibcMspaceMallocStats(void *, void *);
            uint64_t ms[8]; memset(ms, 0, sizeof ms);
            int r = sceLibcMspaceMallocStats ? sceLibcMspaceMallocStats(NULL, ms) : -1;
            klogf("[heap] mspace stats r=%d sz=%llu alloc=%llu free=%llu peak=%llu\n",
                  r, (unsigned long long)ms[0], (unsigned long long)ms[1],
                  (unsigned long long)ms[2], (unsigned long long)ms[3]);
        }
    }
    /* opendir is sandbox-blocked, so the Cg index learns variant include
     * files from the game's own opens instead. */
    if (f) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        size_t bn = strlen(base);
        if (bn > 7 && strcmp(base + bn - 7, "_inc.fx") == 0) {
            extern void cg_note_inc(const char *, const char *, size_t);
            FILE *g = fopen(path, "rb");
            if (g) {
                fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET);
                char *buf = malloc((size_t)sz + 1);
                if (buf && fread(buf, 1, (size_t)sz, g) == (size_t)sz) {
                    buf[sz] = 0;
                    char stem[128];
                    snprintf(stem, sizeof stem, "%.*s", (int)(bn - 7), base);
                    cg_note_inc(stem, buf, (size_t)sz);
                }
                free(buf);
                fclose(g);
            }
        } else if (bn > 3 && strcmp(base + bn - 3, ".fx") == 0) {
            /* Non-inc .fx — combiner/post-process shaders the define-prefix
             * match can't name. Index the body so the tail-match can bind it. */
            extern void cg_note_fx(const char *, const char *, size_t);
            FILE *g = fopen(path, "rb");
            if (g) {
                fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET);
                char *buf = malloc((size_t)sz + 1);
                if (buf && fread(buf, 1, (size_t)sz, g) == (size_t)sz) {
                    buf[sz] = 0;
                    cg_note_fx(path, buf, (size_t)sz);
                }
                free(buf);
                fclose(g);
            }
        }
    }
    RET(h);
}
static void h_fclose(CPU *c)
{
    uint32_t h = A32(0);
    /* Take the FILE out of the slot under the lock so a racing fhdl_get on
     * another thread can't hand out a soon-to-be-closed stream. */
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    FILE *f = NULL;
    uint32_t i = h - FHANDLE_BASE;
    guest_heap_lock();
    if (h < FHANDLE_BASE) f = h < 3 ? g_fhdl[h] : NULL;
    else if (i < (uint32_t)FHANDLE_MAX) { f = g_fhdl[i]; g_fhdl[i] = NULL; }
    guest_heap_unlock();
    int r = f ? fclose(f) : EOF;
    RET(r);
}
static void h_fread (CPU *c) {
    FILE *f = fhdl_get(A32(3));
    size_t r = f ? fread(APTR(0), A32(1), A32(2), f) : 0;
    if (r != A32(2) || A32(2) >= 0x8000) { extern void klogf(const char*, ...); klogf("[fread] buf=%#x n=%#x h=%#x f=%p -> %#x pos=%ld%s err=%d errno=%d\n", A32(0), A32(2), A32(3), (void *)f, (unsigned)r, f ? ftell(f) : -1L, r < A32(2) ? " SHORT" : "", f ? ferror(f) : -9, errno); }
    /* a stream in error state returns 0 on every later fread — one transient
     * short read would otherwise wedge the file for good. clear it so the
     * next call retries the fd. */
    if (f && ferror(f)) clearerr(f);
    RET(r);
}
static FILE *fhdl_or_err(uint32_t h) { FILE *f = fhdl_get(h); return f ? f : stderr; }
static void h_fwrite(CPU *c) { FILE *f = fhdl_get(A32(3)); RET(f ? fwrite(APTR(0), A32(1), A32(2), f) : 0); }
static void h_fseek (CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? fseek(f, AI32(1), AI32(2)) : -1); }
static void h_ftell (CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? ftell(f) : -1); }
static void h_fflush(CPU *c) { uint32_t h=A32(0); RET(fflush(h ? fhdl_get(h) : NULL)); }
static void h_feof  (CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? feof(f) : 1); }
static void h_ferror(CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? ferror(f) : 0); }
static void h_fgets (CPU *c) { FILE *f = fhdl_get(A32(2)); char *r = f ? fgets(ASTR(0),AI32(1),f) : NULL; RET(r?A32(0):0); }
static void h_fputs (CPU *c) { RET(fputs(ASTR(0), fhdl_or_err(A32(1)))); }
static void h_fputc (CPU *c) { RET(fputc(AI32(0), fhdl_or_err(A32(1)))); }
static void h_puts  (CPU *c) { RET(puts(ASTR(0))); }

/* ---- process and environment ---- */
static void h_exit(CPU *c)
{
    /* Worth announcing. A game that decides to leave looks exactly like a
     * game that crashed, unless it says so. */
    extern int sceKernelDebugOutText(int, const char *);
    extern int sceSystemServiceLoadExec(const char *, char *const *);
    extern void guest_report_state(const char *);
    char b[96]; sprintf(b, "[guest] exit(%d)\n", AI32(0));
    sceKernelDebugOutText(0, b);
    { char w[64]; sprintf(w, "guest exit(%d)", AI32(0)); guest_report_state(w); }
    guest_backtrace(c);   /* which guest fn decided to leave */
    /* exit() issues SYS_exit — banned on managed titles (SIGSYS). The
     * sanctioned teardown is sceSystemServiceLoadExec("exit"). */
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) __asm__ volatile("" ::: "memory");
}
static void h_getenv(CPU *c)
{
    const char *k = ASTR(0);
    const char *v = getenv(k);
    fprintf(stderr, "[env] %s = %s\n", k, v ? v : "(unset)");
    if (!v) { RET(0); return; }
    size_t n = strlen(v) + 1;
    char *p = (char *)gmalloc(n, GUEST_ALIGN);
    if (p) memcpy(p, v, n);
    RET(p);
}
static void h_getpid(CPU *c) { (void)c; RET(1); }
static void h_rand  (CPU *c) { (void)c; RET(rand()); }

static void h_abort(CPU *c)
{
    (void)c;
    fprintf(stderr, "[guest] called abort()\n");
    abort();
}
static void h_time(CPU *c)
{
    uint32_t t = (uint32_t)time(NULL);
    if (A32(0)) wr32(A32(0), t);
    RET(t);
}
static void h_gettimeofday(CPU *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (A32(0)) {
        wr32(A32(0), (uint32_t)ts.tv_sec);
        wr32(A32(0) + 4, (uint32_t)(ts.tv_nsec / 1000));
    }
    RET(0);
}
static void h_usleep(CPU *c)
{
#ifdef _WIN32
    Sleep(A32(0) / 1000);
#else
    usleep(A32(0));
#endif
    RET(0);
}

/* ---- math ----
 *
 * Every one of these returns in st(0). The single-precision variants compute
 * in float and widen only at the end: the guest called sinf and expects a
 * float's worth of precision, and a double-precision sine differs from it. */
static double guest_arg_d(CPU *c, unsigned i)
{
    uint64_t bits = (uint64_t)A32(i) | ((uint64_t)A32(i + 1) << 32);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
static float guest_arg_f(CPU *c, unsigned i)
{
    uint32_t bits = A32(i);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

#define MATH1(nm, fn)  static void h_##nm(CPU *c) { RETF(fn(guest_arg_d(c, 0))); }
#define MATH1F(nm, fn) static void h_##nm(CPU *c) { RETF(fn(guest_arg_f(c, 0))); }

MATH1(acos, acos)    MATH1F(acosf, acosf)
MATH1(asin, asin)    MATH1F(asinf, asinf)
MATH1(cos,  cos)     MATH1F(cosf,  cosf)
MATH1(sin,  sin)     MATH1F(sinf,  sinf)
MATH1(tan,  tan)     MATH1F(tanf,  tanf)
MATH1(cosh, cosh)    MATH1(sinh, sinh)    MATH1(tanh, tanh)
MATH1(exp,  exp)     MATH1F(expf,  expf)
MATH1(log,  log)     MATH1F(logf,  logf)
MATH1(log10, log10)

static void h_pow(CPU *c) { RETF(pow(guest_arg_d(c, 0), guest_arg_d(c, 2))); }
static void h_finitef(CPU *c) { RET(isfinite(guest_arg_f(c, 0)) ? 1 : 0); }

static void h_frexp(CPU *c)
{
    int e = 0;
    double m = frexp(guest_arg_d(c, 0), &e);
    if (A32(2)) wr32(A32(2), (uint32_t)e);
    RETF(m);
}
static void h_modf(CPU *c)
{
    double ip = 0.0;
    double fp = modf(guest_arg_d(c, 0), &ip);
    if (A32(2)) {
        uint64_t b;
        memcpy(&b, &ip, 8);
        wr32(A32(2), (uint32_t)b);
        wr32(A32(2) + 4, (uint32_t)(b >> 32));
    }
    RETF(fp);
}

/* ---- glibc internals a compiled binary calls directly ----
 *
 * __ctype_b_loc() returns a pointer TO a pointer into the middle of a
 * 384-entry table indexable from -128 to 255. isalpha() and friends are macros
 * in the game's own headers that index straight out of it, so the double
 * indirection and the negative half both have to be there or the game reads
 * garbage and every character classification is wrong. */
static unsigned short g_ctype_b[384];
static int32_t g_ctype_lower[384], g_ctype_upper[384];
/* On Orbis the host pointers for the ctype indirection can't live in guest
 * registers: rd32() would deref a 64-bit host address. So the tables and the
 * pointer-to-pointer slots both live in guest memory, carved off the heap. */
static uint32_t g_ga_b_p, g_ga_lower_p, g_ga_upper_p, g_ga_errno;
static uint32_t g_ga_environ;

static uint32_t guest_calloc_words(unsigned bytes)
{
    extern uint32_t guest_brk(uint32_t);
    extern int sceKernelDebugOutText(int ch, const char *t);
    sceKernelDebugOutText(0, "[gcw] brk(0)\n");
    uint32_t b = guest_brk(0);
    sceKernelDebugOutText(0, "[gcw] brk(b)\n");
    b = (b + 0xfu) & ~0xfu;
    guest_brk(b + bytes);
    { char bb[96]; sprintf(bb,"[gcw] memset b=%#x bytes=%u\n", b, bytes);
      sceKernelDebugOutText(0, bb); }
    /* write-probe before the real memset */ 
    *(volatile unsigned char *)(uintptr_t)b = 0xAA;
    sceKernelDebugOutText(0, "[gcw] store ok\n");
    memset((void *)(uintptr_t)b, 0, bytes);
    sceKernelDebugOutText(0, "[gcw] done\n");
    return b;
}

static void ctype_init(void)
{
    /* glibc's own bit assignments, from its <ctype.h>. */
    enum { ISupper = 1 << 8, ISlower = 1 << 9, ISalpha = 1 << 10,
           ISdigit = 1 << 11, ISxdigit = 1 << 12, ISspace = 1 << 13,
           ISprint = 1 << 14, ISgraph = 1 << 15, ISblank = 1 << 0,
           IScntrl = 1 << 1, ISpunct = 1 << 2, ISalnum = 1 << 3 };
    for (int i = -128; i < 256; i++) {
        int ch = i & 0xFF;
        unsigned short f = 0;
        if (isupper(ch))  f |= ISupper;
        if (islower(ch))  f |= ISlower;
        if (isalpha(ch))  f |= ISalpha;
        if (isdigit(ch))  f |= ISdigit;
        if (isxdigit(ch)) f |= ISxdigit;
        if (isspace(ch))  f |= ISspace;
        if (isprint(ch))  f |= ISprint;
        if (isgraph(ch))  f |= ISgraph;
        if (iscntrl(ch))  f |= IScntrl;
        if (ispunct(ch))  f |= ISpunct;
        if (isalnum(ch))  f |= ISalnum;
        if (ch == 32 || ch == 9) f |= ISblank;
        g_ctype_b[i + 128]     = (i < 0) ? 0 : f;
        g_ctype_lower[i + 128] = (i < 0) ? i : tolower(ch);
        g_ctype_upper[i + 128] = (i < 0) ? i : toupper(ch);
    }

    extern int sceKernelDebugOutText(int ch, const char *t);
    sceKernelDebugOutText(0, "[ctype] table loop done\n");
    /* copy the tables into guest memory, then a page of guest pointer slots. */
    uint32_t tb = guest_calloc_words(384 * 2 + 384 * 4 + 384 * 4 + 0x40);
    sceKernelDebugOutText(0, "[ctype] calloc done\n");
    uint32_t b_tab = tb, l_tab = tb + 384 * 2, u_tab = tb + 384 * 2 + 384 * 4;
    memcpy((void *)(uintptr_t)b_tab, g_ctype_b, sizeof g_ctype_b);
    memcpy((void *)(uintptr_t)l_tab, g_ctype_lower, sizeof g_ctype_lower);
    memcpy((void *)(uintptr_t)u_tab, g_ctype_upper, sizeof g_ctype_upper);
    uint32_t pp = tb + 384 * 2 + 384 * 4 + 384 * 4;
    g_ga_b_p = pp;        wr32(pp, b_tab + 128 * 2);  /* ptr to table+128 */
    g_ga_lower_p = pp+4;  wr32(pp+4, l_tab + 128 * 4);
    g_ga_upper_p = pp+8;  wr32(pp+8, u_tab + 128 * 4);
    g_ga_errno   = pp+12; wr32(pp+12, 0);
    g_ga_environ = pp+16; wr32(pp+16, 0);
}

static void h_ctype_b_loc      (CPU *c) { RET(g_ga_b_p); }
static void h_ctype_tolower_loc(CPU *c) { RET(g_ga_lower_p); }
static void h_ctype_toupper_loc(CPU *c) { RET(g_ga_upper_p); }

static void h_errno_location(CPU *c) { RET(g_ga_errno); }
static void h_environ(CPU *c)        { RET(g_ga_environ); }

/* libgcc's 64-bit division helpers. A 32-bit compiler emits calls to these
 * for any long long divide, so they turn up in ordinary game code. */
static void h_divdi3(CPU *c)
{
    int64_t a = (int64_t)((uint64_t)A32(0) | ((uint64_t)A32(1) << 32));
    int64_t b = (int64_t)((uint64_t)A32(2) | ((uint64_t)A32(3) << 32));
    int64_t r = b ? a / b : 0;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)((uint64_t)r >> 32);
}
static void h_udivdi3(CPU *c)
{
    uint64_t a = (uint64_t)A32(0) | ((uint64_t)A32(1) << 32);
    uint64_t b = (uint64_t)A32(2) | ((uint64_t)A32(3) << 32);
    uint64_t r = b ? a / b : 0;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
}
static void h_umoddi3(CPU *c)
{
    uint64_t a = (uint64_t)A32(0) | ((uint64_t)A32(1) << 32);
    uint64_t b = (uint64_t)A32(2) | ((uint64_t)A32(3) << 32);
    uint64_t r = b ? a % b : 0;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
}

/* ---- the CRT entry point ----
 *
 * glibc's i386 _start pushes seven arguments and calls this:
 *
 *   __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
 *
 * and it never returns - the process ends by this calling exit(). `init` is
 * __libc_csu_init, which walks .init_array and runs every C++ static
 * constructor in the binary; for a game built on Xerces and Cg that is where
 * most of the work before main() happens, and getting it wrong means main()
 * runs against uninitialised globals rather than crashing honestly.
 */
static void h_libc_start_main(CPU *c)
{
    uint32_t main_fn = A32(0), argc = A32(1), argv = A32(2);
    uint32_t init = A32(3), fini = A32(4);

    /* envp follows argv's NULL terminator on the stack the kernel built. */
    uint32_t envp = argv + (argc + 1) * 4;
    uint32_t args[3] = { argc, argv, envp };

    /* The guest's glibc BSS globals stderr/stdout/stdin start zeroed (real
     * glibc fills them in its own init, which we skip). Give each the stdio
     * handle token so guest fwrite(..., stderr) reaches the real streams.
     * These are the guest binary's data symbols (not in the function table). */
    {
        static const uint32_t stdio_va[] = { 0x089a8170u /*stdout*/,
                                             0x089a8174u /*stderr*/,
                                             0x089a8208u /*stdin*/ };
        static const uint32_t stdio_tok[] = { FHANDLE_BASE + 1u,
                                              FHANDLE_BASE + 2u,
                                              FHANDLE_BASE + 0u };
        for (int i = 0; i < 3; i++) {
            uint32_t a = stdio_va[i];
            if (a >= 0x08048000u && a < 0x09000000u && !rd32(a))
                wr32(a, stdio_tok[i]);
        }
    }

    if (init) {
        fprintf(stderr, "[crt] __libc_csu_init at %#010x\n", init);
        guest_call(c, init, args, 3);
    }

    fprintf(stderr, "[crt] main at %#010x (argc=%u)\n", main_fn, argc);
    uint32_t rc = guest_call(c, main_fn, args, 3);
    fprintf(stderr, "[crt] main returned %d\n", (int)rc);
    {   extern int sceKernelDebugOutText(int, const char *);
        char kb[96]; sprintf(kb, "[crt] main returned %d\n", (int)rc);
        sceKernelDebugOutText(0, kb); }

    if (fini)
        guest_call(c, fini, NULL, 0);

    orb_exit((int)rc);
}

/* ---- libgcc's exception-frame registry ----
 *
 * __register_frame_info_bases(begin, ob, tbase, dbase) hands libgcc the
 * .eh_frame section so a later throw can walk it. Nothing here unwinds DWARF -
 * the _Unwind_* family is unimplemented on purpose - so recording the pointer
 * would serve nobody, and a no-op is the honest translation rather than a
 * shortcut. If this game ever throws, it will stop in _Unwind_RaiseException
 * with its own name on it, which is the right place to find out.
 *
 * The deregister form returns the object it was given; the CRT stores that
 * result, so returning 0 would be a lie it might notice. */
static void h_register_frame_info_bases(CPU *c) { RET(0); }
static void h_deregister_frame_info_bases(CPU *c) { RET(A32(0)); }

/* Called from exit() to release glibc's own caches. There are none. */
static void h_libc_freeres(CPU *c) { (void)c; }

/* The game only ever asks for the C locale, and that is the one we are in. */
static void h_setlocale(CPU *c)
{
    /* The caller stores the return then strdup()s it, so it has to live in
     * guest memory rather than the host's "C" literal. */
    static uint32_t g_c_str;
    if (!g_c_str) { extern uint32_t guest_brk(uint32_t); uint32_t b=(guest_brk(0)+0xf)&~0xf; guest_brk(b+16); memcpy((void*)(uintptr_t)b,"C",2); g_c_str=b; }
    RET(g_c_str);
}

/* ---- where the game thinks it is ----
 *
 * The game calls these to work out its own install directory and then builds
 * every data path from the result, so a wrong answer here is a wrong answer
 * for every file it opens afterwards. Both return forward-slash-free Windows
 * paths, which the guest never inspects - it only concatenates - so no
 * translation is needed. */
static void h_getcwd(CPU *c)
{
#ifdef _WIN32
    RET(_getcwd(ASTR(0), AI32(1)));
#else
    /* Orbis: host getcwd reaches into kernel/TLS state that is not reliable
     * in the managed sandbox - the answer is always /app0 anyway. */
    uint32_t buf = A32(0), sz = A32(1);
    const char *cwd = "/app0";
    extern int sceKernelDebugOutText(int, const char *);
    sceKernelDebugOutText(0, "[call] getcwd -> /app0\n");
    if (buf && sz > strlen(cwd)) {
        memcpy((void *)(uintptr_t)buf, cwd, strlen(cwd) + 1);
        RET(buf);
    } else RET(0);
#endif
}

/* Copy a host string into guest memory and return its guest VA. */
static uint32_t g_strdup_h(const char *s)
{
    extern void *gmalloc(size_t, size_t);
    if (!s) return 0;
    void *p = gmalloc(strlen(s) + 1, 8);
    if (!p) return 0;
    strcpy((char *)p, s);
    return (uint32_t)(uintptr_t)p;
}

static void h_realpath(CPU *c)
{
#ifdef _WIN32
    RET(_fullpath(A32(1) ? ASTR(1) : NULL, ASTR(0), 260));
#else
    char *r = realpath(ASTR(0), A32(1) ? ASTR(1) : NULL);
    if (!r) { RET(0); return; }
    if (A32(1)) RET(A32(1));                  /* caller's guest buffer - done */
    else { RET(g_strdup_h(r)); free(r); }     /* realpath malloc'd a host buffer */
#endif
}

extern int sceKernelDebugOutText(int ch, const char *t);
void hle_register_libc(void)
{
    sceKernelDebugOutText(0, "[reg] libc: before ctype_init\n");
    ctype_init();
    sceKernelDebugOutText(0, "[reg] libc: after ctype_init\n");

    hle_bind("malloc", h_malloc);        hle_bind("calloc", h_calloc);
    hle_bind("realloc", h_realloc);      hle_bind("free", h_free);

    /* C++ allocation. Mangled, but plain C-linkage symbols as far as the PLT
     * is concerned, so they bind by name like anything else. */
    hle_bind("_Znwj", h_op_new);         hle_bind("_Znaj", h_op_new);
    hle_bind("_ZdlPv", h_op_delete);     hle_bind("_ZdaPv", h_op_delete);
    hle_bind("isprint", h_isprint);      hle_bind("nanosleep", h_nanosleep);
    hle_bind("__assert_fail", h_assert_fail);
    hle_bind("atanf", h_atanf);          hle_bind("powf", h_powf);
    hle_bind("rewind", h_rewind);        hle_bind("isdigit", h_isdigit);
    hle_bind("atan2", h_atan2);          hle_bind("atan2f", h_atan2f);
    hle_bind("fmodf", h_fmodf);          hle_bind("qsort", h_qsort);
    hle_bind("sqrt", h_sqrt);            hle_bind("sqrtf", h_sqrtf);
    hle_bind("srand", h_srand);          hle_bind("clearerr", h_clearerr);
    hle_bind("sysconf", h_sysconf);      hle_bind("sleep", h_sleep);
    hle_bind("strcasecmp", h_strcasecmp);
    hle_bind("strncasecmp", h_strncasecmp);
    hle_bind("memcpy", h_memcpy);        hle_bind("memmove", h_memmove);
    hle_bind("memset", h_memset);        hle_bind("memchr", h_memchr);
    hle_bind("memalign", h_memalign);

    hle_bind("strlen", h_strlen);        hle_bind("strcpy", h_strcpy);
    hle_bind("strncpy", h_strncpy);      hle_bind("strcat", h_strcat);
    hle_bind("strncat", h_strncat);      hle_bind("strcmp", h_strcmp);
    hle_bind("strncmp", h_strncmp);      hle_bind("strchr", h_strchr);
    hle_bind("strrchr", h_strrchr);      hle_bind("strstr", h_strstr);
    hle_bind("strdup", h_strdup);        hle_bind("strerror", h_strerror);
    hle_bind("index", h_index);

    hle_bind("printf", h_printf);        hle_bind("fprintf", h_fprintf);
    hle_bind("sprintf", h_sprintf);      hle_bind("snprintf", h_snprintf);

    hle_bind("fopen", h_fopen);          hle_bind("fclose", h_fclose);
    hle_bind("fread", h_fread);          hle_bind("fwrite", h_fwrite);
    hle_bind("fseek", h_fseek);          hle_bind("ftell", h_ftell);
    hle_bind("fflush", h_fflush);        hle_bind("feof", h_feof);
    hle_bind("ferror", h_ferror);        hle_bind("fgets", h_fgets);
    hle_bind("fputs", h_fputs);          hle_bind("fputc", h_fputc);
    hle_bind("puts", h_puts);

    hle_bind("exit", h_exit);            hle_bind("_exit", h_exit);
    hle_bind("abort", h_abort);          hle_bind("getenv", h_getenv);
    hle_bind("getpid", h_getpid);        hle_bind("rand", h_rand);
    hle_bind("time", h_time);            hle_bind("usleep", h_usleep);
    hle_bind("gettimeofday", h_gettimeofday);

    hle_bind("acos", h_acos);    hle_bind("acosf", h_acosf);
    hle_bind("asin", h_asin);    hle_bind("asinf", h_asinf);
    hle_bind("cos", h_cos);      hle_bind("cosf", h_cosf);
    hle_bind("sin", h_sin);      hle_bind("sinf", h_sinf);
    hle_bind("tan", h_tan);      hle_bind("tanf", h_tanf);
    hle_bind("cosh", h_cosh);    hle_bind("sinh", h_sinh);
    hle_bind("tanh", h_tanh);    hle_bind("exp", h_exp);
    hle_bind("expf", h_expf);    hle_bind("log", h_log);
    hle_bind("logf", h_logf);    hle_bind("log10", h_log10);
    hle_bind("pow", h_pow);      hle_bind("finitef", h_finitef);
    hle_bind("frexp", h_frexp);  hle_bind("modf", h_modf);

    hle_bind("__ctype_b_loc", h_ctype_b_loc);
    hle_bind("__ctype_tolower_loc", h_ctype_tolower_loc);
    hle_bind("__ctype_toupper_loc", h_ctype_toupper_loc);
    hle_bind("__errno_location", h_errno_location); hle_bind("__environ", h_environ); hle_bind("environ", h_environ);
    hle_bind("__libc_start_main", h_libc_start_main);
    hle_bind("__register_frame_info_bases", h_register_frame_info_bases);
    hle_bind("__deregister_frame_info_bases", h_deregister_frame_info_bases);
    hle_bind("__libc_freeres", h_libc_freeres);
    hle_bind("setlocale", h_setlocale);
    hle_bind("realpath", h_realpath);
    hle_bind("__divdi3", h_divdi3);
    hle_bind("__udivdi3", h_udivdi3);
    hle_bind("__umoddi3", h_umoddi3);
}

/* ---- files, wide characters, and glibc's versioned internals ----
 *
 * The game reaches these once it starts loading data: stat to find files,
 * strtol to parse its configuration, and the wide-character set because its
 * text handling is locale-aware. */

/* glibc never exported `stat` itself - the header turns it into __xstat with a
 * struct-version argument, so that is what a binary imports. The struct is the
 * kernel's stat64 layout, and the game reads st_mode and st_size from it. */
#define GST_mode 16
#define GST_size 44

static void xstat_common(CPU *c, const char *path, uint32_t out)
{
    struct stat st;
    const char *rp = path ? guest_path(path) : NULL;
    if (!rp || _stat64(rp, &st) != 0) { RET(-1); return; }
    if (out) {
        memset((void *)(uintptr_t)out, 0, 88);          /* sizeof(struct stat64) */
        wr32(out + GST_mode, (uint32_t)st.st_mode);
        wr32(out + GST_size, (uint32_t)st.st_size);
    }
    RET(0);
}

/* __xstat(version, path, buf) - the version is glibc's, and nothing here
 * varies by it. */
static void h_xstat(CPU *c)  { xstat_common(c, ASTR(1), A32(2)); }
static void h_lxstat(CPU *c) { xstat_common(c, ASTR(1), A32(2)); }

static void h_access(CPU *c) { int r=_access(guest_path(ASTR(0)), AI32(1)); if(r<0) r=_access(ASTR(0),AI32(1)); RET(r); }

/* strtol and strtoul are exported under these names with a `group` argument
 * that only affects locale-specific digit grouping, which no game uses. */
static void h_strtol_internal(CPU *c)
{
    RET(strtol(ASTR(0), (char **)(uintptr_t)A32(1), AI32(2)));
}
static void h_strtoul_internal(CPU *c)
{
    RET(strtoul(ASTR(0), (char **)(uintptr_t)A32(1), AI32(2)));
}
static void h_strtod_internal(CPU *c)
{
    RETF(strtod(ASTR(0), (char **)(uintptr_t)A32(1)));
}

/* Wide characters. The game is in the C locale, where the conversions are the
 * identity over ASCII and undefined above it - so these are as complete as
 * they need to be. */
static void h_wcslen(CPU *c)
{
    const uint32_t *w = (const uint32_t *)APTR(0);
    size_t n = 0;
    while (w && w[n]) n++;
    RET(n);
}
static void h_btowc(CPU *c) { int ch = AI32(0); RET(ch < 0 || ch > 0x7F ? 0xFFFFFFFFu : (uint32_t)ch); }
static void h_wctob(CPU *c) { uint32_t w = A32(0); RET(w > 0x7F ? 0xFFFFFFFFu : w); }

/* wctype("alpha") and friends return an opaque handle that iswctype then
 * interprets. The handle only has to be distinct and non-zero per class. */
static void h_wctype(CPU *c)
{
    static const char *const classes[] = {
        "alnum","alpha","blank","cntrl","digit","graph",
        "lower","print","punct","space","upper","xdigit"
    };
    const char *want = ASTR(0);
    for (unsigned i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (strcmp(want, classes[i]) == 0) { RET(i + 1); return; }
    RET(0);
}

static void h_iswctype(CPU *c)
{
    int ch = (int)A32(0);
    unsigned cls = A32(1);
    if (ch > 0x7F) { RET(0); return; }
    switch (cls) {
    case 1:  RET(isalnum(ch)); return;   case 2:  RET(isalpha(ch)); return;
    case 3:  RET(ch == ' ' || ch == '\t'); return;
    case 4:  RET(iscntrl(ch)); return;   case 5:  RET(isdigit(ch)); return;
    case 6:  RET(isgraph(ch)); return;   case 7:  RET(islower(ch)); return;
    case 8:  RET(isprint(ch)); return;   case 9:  RET(ispunct(ch)); return;
    case 10: RET(isspace(ch)); return;   case 11: RET(isupper(ch)); return;
    case 12: RET(isxdigit(ch)); return;
    default: RET(0); return;
    }
}

static void h_towlower(CPU *c) { uint32_t w = A32(0); RET(w <= 0x7F ? (uint32_t)tolower((int)w) : w); }
static void h_towupper(CPU *c) { uint32_t w = A32(0); RET(w <= 0x7F ? (uint32_t)toupper((int)w) : w); }

void hle_register_libc2(void)
{
    hle_bind("__xstat", h_xstat);
    hle_bind("__lxstat", h_lxstat);
    hle_bind("access", h_access);
    hle_bind("__strtol_internal", h_strtol_internal);
    hle_bind("__strtoul_internal", h_strtoul_internal);
    hle_bind("__strtod_internal", h_strtod_internal);
    hle_bind("wcslen", h_wcslen);
    hle_bind("btowc", h_btowc);
    hle_bind("wctob", h_wctob);
    hle_bind("wctype", h_wctype);
    hle_bind("iswctype", h_iswctype);
    hle_bind("towlower", h_towlower);
    hle_bind("towupper", h_towupper);
}

/* ---- descriptors and devices ----
 *
 * The game opens the JVS I/O board as a device and drives it with ioctl. There
 * is no such device here, so open fails and the game is told so - which is the
 * truth, and better than handing back a descriptor that answers nothing.
 */
/* The guest O_* constants are Linux's. Only the access mode and a few flags
 * ever appear, and passing them through unchanged opens the wrong mode. */
static int guest_open_flags(uint32_t f)
{
    int out = (int)(f & 3) | _O_BINARY;
    if (f & 0x40)  out |= _O_CREAT;
    if (f & 0x200) out |= _O_TRUNC;
    if (f & 0x400) out |= _O_APPEND;
    return out;
}

/* ---- fake serial/baseboard devices -------------------------------------
 * The game opens /dev/tts/N (serial boards), /dev/lbb (baseboard),
 * /dev/i2c/0 (eeprom) — all -1 on the host, and its init then waits on a
 * board handshake that can never happen. Lindbergh hooks the same paths to
 * a fake fd and answers the protocol on read/write. Give the game a fake
 * fd, accept writes (logged — the bytes name the board it expects), report
 * reads/selects as no-data. */
#define FAKE_FD_BASE 0x7000
static char g_serial_path[8][64];
static int is_serial_fake(int fd) { return fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + 8; }

/* Drive-board protocol state per fake serial fd (linuxloader
 * hardware/lindbergh/driveBoard.c): the game writes a 4-byte command, polls
 * ioctl for "data ready", then reads one response byte. */
static uint8_t g_db_resp[8];
static int     g_db_pend[8];
static uint8_t g_db_acc[8][8];
static int     g_db_n[8];

static void db_process(int slot, const uint8_t *buf)
{
    switch (buf[0]) {
    case 0xFF: g_db_resp[slot] = 0x00; break;          /* reset        -> READY */
    case 0x81: g_db_resp[slot] = 0x11; break;          /* reset2       -> NOT_INIT */
    case 0xFC: g_db_resp[slot] = 0x00; break;          /* bounds test  -> READY */
    case 0x80: g_db_resp[slot] = 0x00; break;          /* wheel init   -> READY */
    case 0x83:
    case 0x86: g_db_resp[slot] = 0x00; break;          /*              -> READY */
    case 0x9e: g_db_resp[slot] = 0x00; break;
    case 0x84: g_db_resp[slot] = 0x00; break;          /* centre/FFB   -> READY */
    case 0xF1: g_db_resp[slot] = 0x00; break;
    case 0xFA:
    case 0xFD: g_db_resp[slot] = 0x00; break;
    default:   g_db_resp[slot] = 0x00; break;
    }
    g_db_pend[slot] = 1;
}

static void h_open(CPU *c)
{
    const char *p = ASTR(0);
    if (p && (strncmp(p, "/dev/tts/", 9) == 0 || strncmp(p, "/dev/ttyS", 9) == 0 ||
              strcmp(p, "/dev/lbb") == 0 || strcmp(p, "/dev/i2c/0") == 0)) {
        /* The game can re-open the same board after close(). Reuse the slot
         * whose recorded path matches, so a reopen gets the same fake fd and
         * the slot table never collapses on hot-path devices like /dev/lbb. */
        int slot = -1;
        for (int i = 0; i < 8; i++) {
            if (g_serial_path[i][0] && strcmp(g_serial_path[i], p) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            for (int i = 0; i < 8; i++) {
                if (!g_serial_path[i][0]) { slot = i; break; }
            }
        }
        if (slot < 0) {
            klogf("[open] fake serial %s: table full, failing\n", p);
            RET(-1); return;
        }
        int fd = FAKE_FD_BASE + slot;
        snprintf(g_serial_path[slot], 64, "%s", p);
        g_db_resp[slot] = 0x11;               /* NOT_INIT until a command lands */
        g_db_pend[slot] = 0;
        g_db_n[slot] = 0;
        klogf("[open] fake serial %s -> %d\n", p, fd);
        RET(fd); return;
    }
    int fd = _open(guest_path(p), guest_open_flags(A32(1)), 0666);
    if (fd < 0) fd = _open(p, guest_open_flags(A32(1)), 0666);
    /* The cabinet's devices are the ones that matter here: a JVS I/O board
     * hangs off a serial port, and a game that cannot open it stops at an
     * error screen instead of running its attract mode. */
    if (getenv("LINDBERGH_OPENS"))
        klogf("[open] %s -> %d\n", p, fd);
    RET(fd);
}
static void h_close(CPU *c) {
    int fd = AI32(0);
    if (is_serial_fake(fd)) {
        int slot = fd - FAKE_FD_BASE;
        if (slot >= 0 && slot < 8) g_serial_path[slot][0] = 0;
        RET(0); return;
    }
    RET(_close(fd));
}
static void h_read(CPU *c)
{
    int fd = AI32(0);
    if (is_serial_fake(fd)) {
        int slot = fd - FAKE_FD_BASE;
        /* Drive board: one pending response byte per command.  Before any
         * command the board reads back NOT_INIT, which is what starts the
         * game's init sequence. */
        uint8_t *buf = (uint8_t *)(uintptr_t)APTR(1);
        buf[0] = g_db_resp[slot];
        g_db_pend[slot] = 0;
        static int s_serread;
        if (s_serread < 40 || (s_serread % 300) == 0)
            klogf("[serread] %s -> resp=%02x\n",
                  g_serial_path[slot], g_db_resp[slot]);
        s_serread++;
        RET(1); return;
    }
    RET(_read(fd, APTR(1), A32(2)));
}
static void h_write(CPU *c)
{
    int fd = AI32(0);
    if (is_serial_fake(fd)) {
        int slot = fd - FAKE_FD_BASE;
        const unsigned char *b = (const unsigned char *)(uintptr_t)APTR(1);
        unsigned n = A32(2), show = n < 24 ? n : 24;
        char buf[128]; int o = snprintf(buf, sizeof buf, "[serwr] %s n=%u:",
                                        g_serial_path[slot], n);
        for (unsigned i = 0; i < show && o < (int)sizeof buf - 4; i++)
            o += snprintf(buf + o, sizeof buf - o, " %02x", b[i]);
        klogf("%s\n", buf);
        /* Accumulate command bytes; a packet is 4 bytes (7 for the dual
         * player variant — second half is the P2 packet). */
        for (unsigned i = 0; i < n; i++) {
            g_db_acc[slot][g_db_n[slot]++] = b[i];
            if (g_db_n[slot] == 4) {
                db_process(slot, g_db_acc[slot]);
                if (n != 7) g_db_n[slot] = 0;
            } else if (g_db_n[slot] == 7) {
                uint8_t p2[4] = { g_db_acc[slot][0], g_db_acc[slot][4],
                                  g_db_acc[slot][5], g_db_acc[slot][6] };
                db_process(slot, p2);
                g_db_n[slot] = 0;
            }
        }
        RET((uint32_t)n); return;
    }
    RET(_write(fd, APTR(1), A32(2)));
}
static void h_lseek(CPU *c) { RET(_lseek(AI32(0), (long)AI32(1), AI32(2))); }
static void h_ioctl(CPU *c)
{
    int fd = AI32(0);
    if (is_serial_fake(fd)) {
        int slot = fd - FAKE_FD_BASE;
        uint32_t req = A32(1);
        void *argp = (void *)(uintptr_t)APTR(2);
        static int s_ioctllog;
        if (s_ioctllog < 40 || (s_ioctllog % 500) == 0)
            klogf("[serioctl] %s req=%08x argp=%p pend=%d\n",
                  g_serial_path[slot], req, argp, g_db_pend[slot]);
        s_ioctllog++;
        /* Data-ready poll — the game reads the flag out of argp.  A 4-byte
         * int 1 covers both byte-flag and int-count conventions. */
        if (argp) *(uint32_t *)argp = g_db_pend[slot] ? 1 : 0;
        RET(0); return;
    }
    RET(-1);
}
static void h_unlink(CPU *c) { int r=_unlink(guest_path(ASTR(0))); if(r<0) r=_unlink(ASTR(0)); RET(r); }
static void h_mkdir(CPU *c)  { int r=_mkdir(guest_path(ASTR(0))); if(r<0) r=_mkdir(ASTR(0)); RET(r); }

/* iopl(3) asks the kernel for permission to execute IN and OUT directly.
 * Granting it would be a lie with consequences: the game would then run port
 * I/O instructions, which do not lift and cannot mean anything on a host that
 * is not the cabinet. Refusing is both true and the answer that sends it down
 * whatever path it has for not being on real hardware. */
static void h_iopl(CPU *c) { RET(-1); }

/* ---- SysV shared memory ----
 *
 * zmSharedMem is how the game talks to picodaemon over shm segments. On the
 * cabinet these are kernel objects shared between processes; here it is one
 * process, so each key just names a guest-memory block. shmat hands back its
 * guest address and every later access is a plain lifted load/store. */
#define SHM_MAX 16
static struct { uint32_t key, va, size, id; } g_shm[SHM_MAX];
static int g_shm_n;
static void h_shmget(CPU *c)
{
    uint32_t key = A32(0), size = A32(1);
    for (int i = 0; i < g_shm_n; i++)
        if (g_shm[i].key == key) { RET(g_shm[i].id); return; }
    if (g_shm_n >= SHM_MAX) { RET(-1); return; }
    extern uint32_t guest_brk(uint32_t);
    uint32_t va = (guest_brk(0) + 0xfffu) & ~0xfffu;
    guest_brk(va + (size + 0xfffu & ~0xfffu));
    memset((void *)(uintptr_t)va, 0, size);
    int i = g_shm_n++;
    g_shm[i].key = key; g_shm[i].va = va; g_shm[i].size = size;
    g_shm[i].id = 0x8000 + i;
    RET(g_shm[i].id);
}
static void h_shmat(CPU *c)
{
    uint32_t id = A32(0);
    for (int i = 0; i < g_shm_n; i++)
        if (g_shm[i].id == id) { RET(g_shm[i].va); return; }
    RET(0xFFFFFFFFu); /* (void*)-1 */
}
static void h_shmdt(CPU *c) { RET(0); }
static void h_shmctl(CPU *c) { RET(0); }

/* ---- small libc pieces the game reaches on normal paths ---- */
static void h_tolower(CPU *c) { int ch=AI32(0); RET(ch>='A'&&ch<='Z'?ch+32:ch); }
static void h_toupper(CPU *c) { int ch=AI32(0); RET(ch>='a'&&ch<='z'?ch-32:ch); }
static void h_atoi(CPU *c)
{
    const char *s = A32(0) ? ASTR(0) : "";
    while (*s==' '||*s=='\t'||*s=='\n') s++;
    int neg=0; if (*s=='-'||*s=='+') neg = *s++=='-';
    long v=0; while (*s>='0'&&*s<='9') v = v*10 + (*s++-'0');
    RET(neg?-v:v);
}
static void h_isxdigit(CPU *c){int ch=AI32(0);RET((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')||(ch>='A'&&ch<='F'));}
static void h_getc(CPU *c)  { FILE *f=fhdl_get(A32(0)); RET(f?fgetc(f):EOF); }
static void h_fgetc(CPU *c) { FILE *f=fhdl_get(A32(0)); RET(f?fgetc(f):EOF); }
static void h_getc_unlocked(CPU *c) { FILE *f=fhdl_get(A32(0)); RET(f?fgetc(f):EOF); }
static void h_fgets_unlocked(CPU *c) { FILE *f=fhdl_get(A32(2)); char *r=f?fgets(ASTR(0),AI32(1),f):NULL; RET(r?A32(0):0); }
static void h_fgetpos(CPU *c) { FILE *f=fhdl_get(A32(0)); RET(f?fgetpos(f,APTR(1)):-1); }
static void h_fsetpos(CPU *c) { FILE *f=fhdl_get(A32(0)); RET(f?fsetpos(f,APTR(1)):-1); }
static void h_fseeko(CPU *c)  { FILE *f=fhdl_get(A32(0)); RET(f?fseeko(f,(off_t)AI32(1),AI32(2)):-1); }
static void h_ftello(CPU *c)  { FILE *f=fhdl_get(A32(0)); RET(f?(uint32_t)ftello(f):(uint32_t)-1); }
static void h_freopen(CPU *c) { FILE *f=fhdl_get(A32(2)); RET(f?fhdl_alloc(freopen(ASTR(0),A32(1)?ASTR(1):"r",f)):0); }
static void h_putc(CPU *c)  { FILE *f=fhdl_get(A32(1)); RET(f?fputc(AI32(0),f):EOF); }
static void h_ungetc(CPU *c){ FILE *f=fhdl_get(A32(1)); RET(f?ungetc(AI32(0),f):EOF); }
static void h_putchar(CPU *c){ RET(fputc(AI32(0), stdout)); }
static void h_fileno(CPU *c){ FILE *f=fhdl_get(A32(0)); RET(f?fileno(f):-1); }
static void h_fdopen(CPU *c){ RET(fhdl_alloc(fdopen(AI32(0), A32(1)?ASTR(1):"r"))); }
static void h_setvbuf(CPU *c){ FILE *f=fhdl_get(A32(0)); RET(f?setvbuf(f,APTR(1),AI32(2),A32(3)):0); }
static void h_writev(CPU *c)
{
    int fd=AI32(0); uint32_t iov=A32(1); int n=AI32(2); long tot=0;
    for (int i=0;i<n;i++) {
        uint32_t base=rd32(iov+i*8), len=rd32(iov+i*8+4);
        tot += _write(fd,(void*)(uintptr_t)base,len);
    }
    RET(tot);
}
static void h_strtok_r(CPU *c)
{
    static uint32_t save;
    if (A32(2)) { uint32_t *sp=(uint32_t*)(uintptr_t)A32(2); save=*sp; }
    char *p = A32(0) ? ASTR(0) : (char*)(uintptr_t)save;
    if (!p) { RET(0); return; }
    const char *d = A32(1) ? ASTR(1) : " ";
    while (*p && strchr(d,*p)) p++;
    if (!*p) { save=0; RET(0); return; }
    char *tok=p; while (*p && !strchr(d,*p)) p++;
    if (*p) *p++=0;
    save=(uint32_t)(uintptr_t)p;
    if (A32(2)) wr32(A32(2), save);
    RET((uint32_t)(uintptr_t)tok);
}
static void h_strcoll(CPU *c) { RET(strcmp(ASTR(0),ASTR(1))); }
static void h_strxfrm(CPU *c) { RET(strxfrm(ASTR(0),ASTR(1),A32(2))); }
static void h_mktime(CPU *c)
{
    /* struct tm is laid out the same in i386 glibc */
    RET(mktime((struct tm *)APTR(0)));
}
static void h_strftime(CPU *c){ RET(strftime(ASTR(0),A32(1),ASTR(2),APTR(3))); }
static void h_utime(CPU *c) { extern int utime(const char*,const void*); RET(utime(guest_path(ASTR(0)),APTR(1))); }
static void h_lseek64(CPU *c){ RET(_lseek(AI32(0), (int64_t)(((uint64_t)A32(2) << 32) | A32(1)), AI32(3))); }
static void h_fseeko64(CPU *c)
{
    FILE *f=fhdl_get(A32(0));
    if (!f) { RET(-1); return; }
    RET(fseek(f, (long)(int64_t)(((uint64_t)A32(2) << 32) | A32(1)), AI32(3)));
}
static void h_ftello64(CPU *c)
{
    FILE *f=fhdl_get(A32(0));
    RET(f?ftell(f):-1);
}
static void h_fopen64(CPU *c)
{
    FILE *f=fopen(guest_path(ASTR(0)), ASTR(1));
    if (!f) f=fopen(ASTR(0), ASTR(1));
    RET(fhdl_alloc(f));
}
static void h___fxstat64(CPU *c)
{
    /* arg2 is a 32-bit struct stat64 buffer in guest space; write a compact
     * fake — st_mode/st_size are what the game actually reads. */
    struct stat b;
    memset(&b,0,sizeof b);
    int r = fstat(AI32(1), &b);
    uint32_t out = A32(2);
    if (out) {
        memset((void*)(uintptr_t)out, 0, 128);
        memcpy((void*)(uintptr_t)out, &b, sizeof b > 128 ? 128 : sizeof b);
    }
    RET(r);
}
/* select(nfds, rfds, wfds, efds, tv) — report a fake serial fd readable while
 * a drive-board response byte is pending. */
static void h_select(CPU *c)
{
    int nfds = AI32(0);
    uint8_t *rfds = (uint8_t *)(uintptr_t)APTR(1);
    int ready = 0;
    if (rfds && nfds > FAKE_FD_BASE) {
        for (int i = 0; i < 8; i++) {
            int fd = FAKE_FD_BASE + i;
            if (fd < nfds && g_db_pend[i] &&
                (rfds[fd >> 3] & (1 << (fd & 7)))) {
                ready++;
            }
        }
    }
    RET(ready);
}
/* poll(struct pollfd *fds, nfds, timeout) — pollfd = {fd, events, revents} */
static void h_poll(CPU *c)
{
    uint8_t *fds = (uint8_t *)(uintptr_t)APTR(0);
    unsigned nfds = A32(1);
    int ready = 0;
    if (fds) {
        for (unsigned i = 0; i < nfds; i++) {
            int32_t fd = *(int32_t *)(fds + i * 8);
            if (is_serial_fake(fd) && g_db_pend[fd - FAKE_FD_BASE]) {
                int16_t *rev = (int16_t *)(fds + i * 8 + 6);
                *rev |= 0x0001;                    /* POLLIN */
                ready++;
            }
        }
    }
    RET(ready);
}
static void h_fcntl(CPU *c)  { RET(0); }

/* ---- system identity probes — plausible values, never NULL deref ---- */
static void h_uname(CPU *c)
{
    uint32_t o = A32(0);
    if (!o) { RET(-1); return; }
    char *u = (char *)(uintptr_t)o;
    memset(u, 0, 390);          /* sizeof(struct utsname) = 6*65 */
    strcpy(u + 0,   "Linux");
    strcpy(u + 65,  "lindbergh");
    strcpy(u + 130, "2.6.32-lindbergh");
    strcpy(u + 195, "#1 SMP");
    strcpy(u + 260, "i686");
    RET(0);
}
static void h_sysinfo(CPU *c)
{
    uint32_t o = A32(0);
    if (!o) { RET(-1); return; }
    uint32_t *u = (uint32_t *)(uintptr_t)o;
    memset(u, 0, 96);           /* struct sysinfo ~96 bytes on 32-bit */
    u[0] = 60;                  /* uptime */
    u[4] = 0; u[5] = 0; u[6] = 0;              /* loads */
    u[8] = 0x8000000 / 4096;    /* totalram 128MB */
    u[9] = 0x4000000 / 4096;    /* freeram */
    RET(0);
}
static void h_getrlimit(CPU *c)
{
    uint32_t o = A32(1);
    if (!o) { RET(0); return; }
    uint32_t *u = (uint32_t *)(uintptr_t)o;
    u[0] = 0xFFFFFFFF; u[1] = 0xFFFFFFFF;   /* RLIM_INFINITY */
    RET(0);
}
static void h_setrlimit(CPU *c) { RET(0); }
static void h_getpriority(CPU *c) { RET(0); }
static void h_setpriority(CPU *c) { RET(0); }
static void h_getuid(CPU *c) { RET(0); }
static void h_geteuid(CPU *c) { RET(0); }
static void h_getgid(CPU *c) { RET(0); }
static void h_getegid(CPU *c) { RET(0); }
static void h_getppid(CPU *c) { RET(1); }
static void h_getlogin(CPU *c) { RET(0); }
static void h_gethostname(CPU *c)
{
    uint32_t o = A32(0);
    if (o) { memset((void *)(uintptr_t)o, 0, A32(1)); strncpy((char *)(uintptr_t)o, "lindbergh", A32(1) - 1); }
    RET(0);
}
/* signals — install nothing, report success */
static void h_signal(CPU *c) { RET(0); }
static void h_sigaction(CPU *c) { RET(0); }
static void h_sigprocmask(CPU *c) { RET(0); }
static void h_sigemptyset(CPU *c) { if (A32(0)) memset(APTR(0), 0, 128); RET(0); }
static void h_sigfillset(CPU *c) { if (A32(0)) memset(APTR(0), 0xFF, 128); RET(0); }
static void h_sigaddset(CPU *c) { RET(0); }
static void h_sigdelset(CPU *c) { RET(0); }
static void h_sigismember(CPU *c) { RET(0); }
static void h_pause(CPU *c) { RET(0); }
static void h_alarm(CPU *c) { RET(0); }
static void h_atexit(CPU *c) { RET(0); }
static void h_on_exit(CPU *c) { RET(0); }
/* fstat-family via the shared helper */
static void h_stat(CPU *c)  { xstat_common(c, ASTR(0), A32(1)); }
static void h_lstat(CPU *c) { xstat_common(c, ASTR(0), A32(1)); }
static void h_fstat(CPU *c)
{
    struct stat b; memset(&b, 0, sizeof b);
    int r = fstat(AI32(0), &b);
    uint32_t o = A32(1);
    if (o) { memset((void *)(uintptr_t)o, 0, 88); wr32(o + GST_mode, (uint32_t)b.st_mode); wr32(o + GST_size, (uint32_t)b.st_size); }
    RET(r);
}
static void h_stat64(CPU *c)  { xstat_common(c, ASTR(0), A32(1)); }
static void h_fstat64(CPU *c) { h_fstat(c); }
static void h_lstat64(CPU *c) { xstat_common(c, ASTR(0), A32(1)); }
static void h_fstatat(CPU *c) { xstat_common(c, ASTR(1), A32(3)); }
static void h_statfs(CPU *c)
{
    uint32_t o = A32(1);
    if (o) { memset((void *)(uintptr_t)o, 0, 120); }
    RET(0);
}
static void h_statvfs(CPU *c)
{
    uint32_t o = A32(1);
    if (o) { memset((void *)(uintptr_t)o, 0, 112); }
    RET(0);
}
/* getline/getdelim/gets/getw/putw/getchar aren't in the Orbis libc — the game
 * uses fgets/fscanf/read, not these; leave them unbound. */

/* The remaining FILE-fn surface — NULL-f guarded so a NULL fopen result never
 * reaches libc's FILE* deref. */
static void h_vfscanf(CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? -1 : -1); }
static void h_vsfscanf(CPU *c) { RET(0); }
static void h_fwscanf(CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? 0 : -1); }
static void h_swscanf(CPU *c) { RET(0); }
static void h_vwscanf(CPU *c) { RET(0); }
static void h_fgets_s(CPU *c) { FILE *f = fhdl_get(A32(2)); char *r = f ? fgets(ASTR(0), AI32(1), f) : NULL; RET(r ? A32(0) : 0); }
static void h_fscanf_s(CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? -1 : -1); }
static void h_sscanf_s(CPU *c) { RET(0); }
static void h_fgetws(CPU *c) { FILE *f = fhdl_get(A32(2)); RET(f ? 0 : 0); }
static void h_fputws(CPU *c) { FILE *f = fhdl_get(A32(1)); RET(f ? 0 : -1); }
static void h_fgetwc(CPU *c) { FILE *f = fhdl_get(A32(0)); RET(f ? 0 : -1); }
static void h_fputwc(CPU *c) { FILE *f = fhdl_get(A32(1)); RET(f ? 0 : -1); }
static void h_fwide(CPU *c) { (void)c; RET(0); }
static void h_wprintf(CPU *c) { RET(0); }
static void h_fwprintf(CPU *c) { RET(0); }
static void h_swprintf(CPU *c) { RET(0); }
static void h_fgetln(CPU *c) { (void)c; RET(0); }
static void h_fgetline(CPU *c) { (void)c; RET(0); }
static void h_fpurge(CPU *c) { (void)c; RET(0); }
static void h_freadable(CPU *c) { (void)c; RET(1); }
static void h_fwritable(CPU *c) { (void)c; RET(0); }
static void h_flbf(CPU *c) { (void)c; RET(0); }
static void h_fbufsize(CPU *c) { (void)c; RET(0); }
static void h_fcloseall(CPU *c) { (void)c; RET(0); }
static void h_fpending(CPU *c) { (void)c; RET(0); }
static void h_setbuf(CPU *c) { (void)c; }
static void h_setbuffer(CPU *c) { (void)c; }
static void h_setlinebuf(CPU *c) { (void)c; }
static void h_fmemopen(CPU *c) { (void)c; RET(0); }
static void h_open_memstream(CPU *c) { (void)c; RET(0); }
static void h_fopencookie(CPU *c) { (void)c; RET(0); }
static void h_funopen(CPU *c) { (void)c; RET(0); }
static void h_fdopendir(CPU *c) { (void)c; RET(0); }
static void h_rewinddir(CPU *c) { (void)c; }
static void h_seekdir(CPU *c) { (void)c; }
static void h_telldir(CPU *c) { (void)c; RET(0); }
static void h_scandir(CPU *c) { (void)c; RET(-1); }
static void h_alphasort(CPU *c) { (void)c; RET(0); }
static void h_dirfd(CPU *c) { (void)c; RET(-1); }
static void h_mkdtemp(CPU *c) { (void)c; RET(0); }
static void h_mkstemp(CPU *c) { (void)c; RET(-1); }
static void h_tempnam(CPU *c) { (void)c; RET(0); }
static void h_tmpnam(CPU *c) { (void)c; RET(0); }
static void h_ftw(CPU *c) { (void)c; RET(-1); }
static void h_nftw(CPU *c) { (void)c; RET(-1); }
static void h_mb_cur_max(CPU *c) { RET(1); }
static void h_mbrtowc(CPU *c)
{
    uint32_t pwc=A32(0), s=A32(1), n=A32(2);
    if (!s) { RET(0); return; }
    unsigned char b=*(unsigned char*)(uintptr_t)s;
    if (!b) { if(pwc) wr32(pwc,0); RET(0); return; }
    if (pwc) wr32(pwc, b);   /* latin-1-ish: enough for ini parsing */
    RET(1);
}
static void h_wcrtomb(CPU *c)
{
    if (!A32(0)) { RET(0); return; }
    *(char*)(uintptr_t)A32(0) = (char)A32(1);
    RET(1);
}
static void h_getwc(CPU *c)  { RET((uint32_t)fgetc(stdin)); }
static void h_putwc(CPU *c)  { RET(0); }
static void h_ungetwc(CPU *c){ RET(0); }
static void h_wmemchr(CPU *c)
{
    uint32_t *s=(uint32_t*)(uintptr_t)A32(0); uint32_t ch=A32(1); uint32_t n=A32(2);
    for (uint32_t i=0;i<n;i++) if (s[i]==ch) { RET((uint32_t)(uintptr_t)(s+i)); return; }
    RET(0);
}
static void h_wmemcmp(CPU *c)
{
    uint32_t *a=(uint32_t*)(uintptr_t)A32(0),*b=(uint32_t*)(uintptr_t)A32(1);
    uint32_t n=A32(2);
    for (uint32_t i=0;i<n;i++) if (a[i]!=b[i]) { RET(a[i]<b[i]?-1:1); return; }
    RET(0);
}
static void h_wmemcpy(CPU *c){ memmove(APTR(0),APTR(1),A32(2)*4); RET(A32(0)); }
static void h_wmemmove(CPU *c){ memmove(APTR(0),APTR(1),A32(2)*4); RET(A32(0)); }
static void h_wmemset(CPU *c)
{
    uint32_t *d=(uint32_t*)(uintptr_t)A32(0); for(uint32_t i=0;i<A32(2);i++) d[i]=A32(1);
    RET(A32(0));
}
static void h_wcscoll(CPU *c)  { RET(0); }
static void h_wcsxfrm(CPU *c)  { RET(0); }
static void h_wcsftime(CPU *c) { RET(0); }
static void h___fixunsdfdi(CPU *c) { RET(0); } /* fp helpers: handled elsewhere */
static void h___fixunssfdi(CPU *c) { RET(0); }
static void h_cfgetispeed(CPU *c){ RET(0); }
static void h_cfgetospeed(CPU *c){ RET(0); }
static void h_cfsetspeed(CPU *c){ RET(0); }
static void h_tcdrain(CPU *c){ RET(0); }
static void h_tcflush(CPU *c){ RET(0); }
static void h_tcgetattr(CPU *c){ RET(is_serial_fake(AI32(0)) ? 0 : -1); }
static void h_tcsetattr(CPU *c){ RET(is_serial_fake(AI32(0)) ? 0 : -1); }

/* ---- SEGAAPI — the cabinet sound library ----
 *
 * criau_Init opens the SEGA audio device through these PLT imports. The PS5
 * port has no SEGAAPI device yet; every entry answers SEGA_SUCCESS(0) with a
 * plausible value so the mixer comes up silent instead of dead. Buffer
 * handles are faked per-request; GetPlaybackStatus says STOP so waits on a
 * finished cue complete immediately rather than hang on a playing sound. */
/* ---- SEGAAPI bridge to libsegaapi_ps5.c --------------------------------
 * The real SEGAAPI lives in libsegaapi_ps5.c (software mixer + sceAudioOut).
 * These handlers unmarshal the 32-bit guest structs (host pointers are
 * 8 bytes where the guest's are 4) and pass a small 0x1C0000-handle token
 * back to the game instead of the raw host Buffer*. */
typedef struct {
    unsigned int dwPriority, dwSampleRate, dwSampleFormat, byNumChans, dwReserved;
    void *hUserData;
    struct { unsigned int dwSize, dwOffset; void *hBufferHdr; } mapData;
} HostBufferConfig;
typedef void (*HostBufferCallback)(void *hHandle, int message);
typedef struct { int param; int lPARWValue; } HostSynthParamSet;
typedef struct { unsigned int dwChannel, dwSend; int dwDest; }  HostSendRouteParamSet;
typedef struct { unsigned int dwChannel, dwSend, dwLevel; }     HostSendLevelParamSet;
typedef struct { int VoiceIoctl; unsigned int dwParam1, dwParam2; } HostVoiceParamSet;

extern int      SEGAAPI_Init(void);
extern int      SEGAAPI_Exit(void);
extern int      SEGAAPI_Reset(void);
extern int      SEGAAPI_GetLastStatus(void);
extern int      SEGAAPI_CreateBuffer(HostBufferConfig*, HostBufferCallback, unsigned, void**);
extern int      SEGAAPI_DestroyBuffer(void*);
extern int      SEGAAPI_Play(void*);
extern int      SEGAAPI_PlayWithSetup(void*, unsigned, HostSendRouteParamSet*, unsigned,
                    HostSendLevelParamSet*, unsigned, HostVoiceParamSet*, unsigned, HostSynthParamSet*);
extern int      SEGAAPI_Pause(void*);
extern int      SEGAAPI_Stop(void*);
extern int      SEGAAPI_UpdateBuffer(void*, unsigned, unsigned);
extern int      SEGAAPI_GetPlaybackStatus(void*);
extern unsigned SEGAAPI_GetPlaybackPosition(void*);
extern int      SEGAAPI_SetPlaybackPosition(void*, unsigned);
extern int      SEGAAPI_GetSynthParam(void*, int);
extern int      SEGAAPI_SetSynthParam(void*, int, int);
extern int      SEGAAPI_SetSynthParamMultiple(void*, unsigned, HostSynthParamSet*);
extern void*    SEGAAPI_GetUserData(void*);
extern int      SEGAAPI_SetUserData(void*, void*);
extern int      SEGAAPI_SetChannelVolume(void*, unsigned, unsigned);
extern int      SEGAAPI_SetIOVolume(int, unsigned);
extern int      SEGAAPI_SetSampleRate(void*, unsigned);
extern int      SEGAAPI_SetLoopState(void*, int);
extern int      SEGAAPI_SetReleaseState(void*, int);
extern int      SEGAAPI_SetStartLoopOffset(void*, unsigned);
extern int      SEGAAPI_SetEndLoopOffset(void*, unsigned);
extern int      SEGAAPI_SetEndOffset(void*, unsigned);
extern int      SEGAAPI_SetSendRouting(void*, unsigned, unsigned, int);
extern int      SEGAAPI_SetSendLevel(void*, unsigned, unsigned, unsigned);

#define SEG_HDL_MAX 256
static void    *s_seg_hdl[SEG_HDL_MAX];
static unsigned s_seg_cnt = 0;

static pthread_mutex_t s_seg_lock = PTHREAD_MUTEX_INITIALIZER;
static void *seg_lookup(uint32_t token) {
    if ((token >> 16) != 0x1C) return NULL;
    unsigned idx = token & 0xFFFF;
    pthread_mutex_lock(&s_seg_lock);
    void *h = idx < SEG_HDL_MAX ? s_seg_hdl[idx] : NULL;
    pthread_mutex_unlock(&s_seg_lock);
    return h;
}
static uint32_t seg_alloc(void *handle) {
    pthread_mutex_lock(&s_seg_lock);
    unsigned idx = s_seg_cnt++ % SEG_HDL_MAX;
    s_seg_hdl[idx] = handle;
    pthread_mutex_unlock(&s_seg_lock);
    return 0x1C0000u | idx;
}

/* The real mixer + the CRI worker threads it wakes have a host-side crash we
 * have not isolated (rip=0x8 at the audio pump's first dispatch). Audio is
 * opt-in until that's fixed: default to the silent-stub behaviour that runs
 * stable, enable the real SEGAAPI with LINDBERGH_AUDIO=1. */
static int s_audio_on = -1;
static int audio_on(void)
{
    if (s_audio_on < 0) s_audio_on = getenv("LINDBERGH_AUDIO") ? 1 : 0;
    return s_audio_on;
}

static void h_segapi_init(CPU *c)
{
    if (!audio_on()) { RET(0); return; }
    int r = SEGAAPI_Init();
    extern void klogf(const char*, ...); klogf("[segapi] Init -> %d\n", r);
    RET(r);
}
static void h_segapi_exit(CPU *c)          { SEGAAPI_Exit(); RET(0); }
static void h_segapi_reset(CPU *c)         { RET(SEGAAPI_Reset()); }
static void h_segapi_getlaststatus(CPU *c) { RET(SEGAAPI_GetLastStatus()); }

/* Fake-DAC state for muted mode: the renderer's ring buffer still has to be
 * real memory (lbrna_trans mixes straight into it), and GetPlaybackPosition
 * must advance like a sound card draining the buffer or the CRI fill logic
 * deadlocks the stream threads. */
typedef struct { uint32_t base, size, rate, chans; uint64_t t0_us; int playing; } SegFake;
static SegFake s_seg_fake[SEG_HDL_MAX];
static uint64_t seg_now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint32_t seg_fake_pos(SegFake *f)
{
    if (!f || !f->size || !f->playing) return 0;
    uint64_t bps = (uint64_t)f->rate * (f->chans ? f->chans : 2) * 2ull;
    uint64_t el  = seg_now_us() - f->t0_us;
    return (uint32_t)((el * bps / 1000000ull) % f->size);
}

static uint32_t g_fake_seg_hdl;
static void h_segapi_createbuffer(CPU *c)
{
    if (!audio_on()) {
        uint32_t gcfg = A32(0), ph = A32(3);
        uint32_t size = gcfg ? rd32(gcfg + 24) : 0;
        uint32_t gbuf = gcfg ? rd32(gcfg + 32) : 0;
        if (!gbuf && size) {
            extern uint32_t guest_brk(uint32_t);
            extern void guest_heap_lock(void), guest_heap_unlock(void);
            guest_heap_lock();
            uint32_t base = (guest_brk(0) + 0xfu) & ~0xfu;
            guest_brk(base + size);
            guest_heap_unlock();
            memset((void *)(uintptr_t)base, 0, size);
            gbuf = base;
        }
        if (gcfg && gbuf) wr32(gcfg + 32, gbuf);
        uint32_t tok = 0x1C0000u | (++g_fake_seg_hdl & 0xFFFF);
        SegFake *f = &s_seg_fake[tok & 0xFFFF];
        f->base = gbuf; f->size = size;
        f->rate = gcfg ? rd32(gcfg + 4) : 44100;
        f->chans = gcfg ? rd32(gcfg + 12) : 2;
        f->t0_us = seg_now_us(); f->playing = 1;
        if (ph) wr32(ph, tok);
        RET(0); return;
    }
    uint32_t gcfg = A32(0);
    HostBufferConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.dwPriority     = rd32(gcfg + 0);
    cfg.dwSampleRate   = rd32(gcfg + 4);
    cfg.dwSampleFormat = rd32(gcfg + 8);
    cfg.byNumChans     = rd32(gcfg + 12);
    cfg.dwReserved     = rd32(gcfg + 16);
    cfg.hUserData      = (void*)(uintptr_t)rd32(gcfg + 20);
    cfg.mapData.dwSize   = rd32(gcfg + 24);
    cfg.mapData.dwOffset = rd32(gcfg + 28);
    uint32_t hdrbuf_gva  = rd32(gcfg + 32);
    cfg.mapData.hBufferHdr = hdrbuf_gva ? (void*)(uintptr_t)hdrbuf_gva : NULL;

    unsigned int flags = A32(2);
    if (hdrbuf_gva) flags |= 0x00000004u; /* USE_MAPPED_MEM */

    /* If the guest did not pre-allocate a PCM buffer, allocate one from
     * guest_brk so the pointer stays 32-bit. Letting SEGAAPI calloc() would
     * return a 64-bit host address whose wr32 writeback truncates to a small
     * value (e.g. 0x1_0000_0008 -> 8), which the game later calls/dereferences
     * -> host rip=0x8. */
    uint32_t gbuf_gva = 0;
    if (!hdrbuf_gva && cfg.mapData.dwSize > 0) {
        extern uint32_t guest_brk(uint32_t);
        extern void guest_heap_lock(void), guest_heap_unlock(void);
        guest_heap_lock();
        uint32_t base = (guest_brk(0) + 0xfu) & ~0xfu;
        guest_brk(base + cfg.mapData.dwSize);
        guest_heap_unlock();
        memset((void*)(uintptr_t)base, 0, cfg.mapData.dwSize);
        cfg.mapData.hBufferHdr = (void*)(uintptr_t)base;
        gbuf_gva = base;
        flags |= 0x00000004u; /* USE_MAPPED_MEM */
    }

    void *handle = NULL;
    int r = SEGAAPI_CreateBuffer(&cfg, NULL, flags, &handle);
    if (r == 0 && handle) {
        uint32_t token = seg_alloc(handle);
        if (A32(3)) wr32(A32(3), token);
        if (gbuf_gva)
            wr32(gcfg + 32, gbuf_gva);   /* already a 32-bit guest addr */
        extern void klogf(const char*, ...);
        klogf("[segapi] CreateBuffer rate=%u fmt=%u ch=%u sz=%u hdr=%#x gbuf=%#x cb=%#x -> %#x\n",
              cfg.dwSampleRate, cfg.dwSampleFormat, cfg.byNumChans,
              cfg.mapData.dwSize, hdrbuf_gva, gbuf_gva, A32(1), token);
    }
    RET(r);
}

/* Rate-limited klog for the play path — criAu voices start/stream through
 * these and they were invisible. First 16 calls each, then every 256th. */
static int segrl(const char *tag, uint32_t tok)
{
    static struct { const char *t; int n; } s[16];
    for (int i = 0; i < 16; i++) {
        if (!s[i].t) { s[i].t = tag; s[i].n = 1;
            klogf("[segapi] %s tok=%#x (#1)\n", tag, tok); return 1; }
        if (s[i].t == tag) { int n = ++s[i].n;
            if (n <= 16 || (n & 255) == 0) {
                klogf("[segapi] %s tok=%#x (#%d)\n", tag, tok, n); return 1; }
            return 0; }
    }
    return 0;
}
static void h_segapi_destroybuffer(CPU *c) { if (audio_on()) segrl("DestroyBuffer", A32(0)); RET(audio_on() ? SEGAAPI_DestroyBuffer(seg_lookup(A32(0))) : 0); }
static SegFake *seg_fake_lookup(uint32_t tok)
{
    return ((tok >> 16) == 0x1C) ? &s_seg_fake[tok & 0xFFFF] : NULL;
}
static void h_segapi_play(CPU *c)          { if (audio_on()) segrl("Play", A32(0)); else { SegFake *f = seg_fake_lookup(A32(0)); if (f) { f->playing = 1; f->t0_us = seg_now_us(); } } RET(audio_on() ? SEGAAPI_Play(seg_lookup(A32(0))) : 0); }
static void h_segapi_pause(CPU *c)         { if (audio_on()) segrl("Pause", A32(0)); else { SegFake *f = seg_fake_lookup(A32(0)); if (f) f->playing = 0; } RET(audio_on() ? SEGAAPI_Pause(seg_lookup(A32(0))) : 0); }
static void h_segapi_stop(CPU *c)          { if (audio_on()) segrl("Stop", A32(0)); else { SegFake *f = seg_fake_lookup(A32(0)); if (f) { f->playing = 0; f->t0_us = seg_now_us(); } } RET(audio_on() ? SEGAAPI_Stop(seg_lookup(A32(0))) : 0); }
static void h_segapi_updatebuffer(CPU *c)  { if (audio_on()) { static int ub; if (ub < 24 || (ub & 63) == 0) klogf("[segapi] UpdateBuffer h=%#x off=%#x len=%#x\n", A32(0), A32(1), A32(2)); ub++; } RET(audio_on() ? SEGAAPI_UpdateBuffer(seg_lookup(A32(0)), A32(1), A32(2)) : 0); }
static void h_segapi_getstatus(CPU *c)     { SegFake *f = audio_on() ? NULL : seg_fake_lookup(A32(0)); RET(audio_on() ? SEGAAPI_GetPlaybackStatus(seg_lookup(A32(0))) : (f && f->base) ? 1 : 0); }
static void h_segapi_getpos(CPU *c)        { if (audio_on()) { static int gp; if ((gp & 255) == 0) klogf("[segapi] GetPos h=%#x\n", A32(0)); gp++; } RET(audio_on() ? (int)SEGAAPI_GetPlaybackPosition(seg_lookup(A32(0))) : (int)seg_fake_pos(seg_fake_lookup(A32(0)))); }
static void h_segapi_setpos(CPU *c)        { if (!audio_on()) { SegFake *f = seg_fake_lookup(A32(0)); if (f && f->size) { uint64_t bps = (uint64_t)f->rate * (f->chans ? f->chans : 2) * 2ull; f->t0_us = bps ? seg_now_us() - (uint64_t)A32(1) * 1000000ull / bps : seg_now_us(); f->playing = 1; } } RET(audio_on() ? SEGAAPI_SetPlaybackPosition(seg_lookup(A32(0)), A32(1)) : 0); }
static void h_segapi_getsynthparam(CPU *c) { RET(audio_on() ? SEGAAPI_GetSynthParam(seg_lookup(A32(0)), AI32(1)) : 0); }
static void h_segapi_setsynthparam(CPU *c) { if (audio_on()) segrl("SetSynthParam", A32(0)); RET(audio_on() ? SEGAAPI_SetSynthParam(seg_lookup(A32(0)), AI32(1), AI32(2)) : 0); }
static void h_segapi_getuserdata(CPU *c)   { RET(audio_on() ? (uint32_t)(uintptr_t)SEGAAPI_GetUserData(seg_lookup(A32(0))) : 0); }
static void h_segapi_setuserdata(CPU *c)   { RET(audio_on() ? SEGAAPI_SetUserData(seg_lookup(A32(0)), (void*)(uintptr_t)A32(1)) : 0); }
static void h_segapi_setchanvol(CPU *c)    { RET(audio_on() ? SEGAAPI_SetChannelVolume(seg_lookup(A32(0)), A32(1), A32(2)) : 0); }
static void h_segapi_setiovol(CPU *c)      { RET(audio_on() ? SEGAAPI_SetIOVolume(AI32(0), A32(1)) : 0); }
static void h_segapi_setsamplerate(CPU *c) { RET(audio_on() ? SEGAAPI_SetSampleRate(seg_lookup(A32(0)), A32(1)) : 0); }
static void h_segapi_setloopstate(CPU *c)  { RET(audio_on() ? SEGAAPI_SetLoopState(seg_lookup(A32(0)), AI32(1)) : 0); }
static void h_segapi_setreleasestate(CPU *c){ RET(audio_on() ? SEGAAPI_SetReleaseState(seg_lookup(A32(0)), AI32(1)) : 0); }
static void h_segapi_setstartloop(CPU *c)  { if (audio_on()) { static int sl; if (sl < 32 || (sl & 63) == 0) klogf("[segapi] SetStartLoop h=%#x off=%#x\n", A32(0), A32(1)); sl++; } RET(audio_on() ? SEGAAPI_SetStartLoopOffset(seg_lookup(A32(0)), A32(1)) : 0); }
static void h_segapi_setendloop(CPU *c)    { if (audio_on()) { static int el; if (el < 32 || (el & 63) == 0) klogf("[segapi] SetEndLoop h=%#x off=%#x\n", A32(0), A32(1)); el++; } RET(audio_on() ? SEGAAPI_SetEndLoopOffset(seg_lookup(A32(0)), A32(1)) : 0); }
static void h_segapi_setendoffset(CPU *c)  { RET(audio_on() ? SEGAAPI_SetEndOffset(seg_lookup(A32(0)), A32(1)) : 0); }
static void h_segapi_setsendrouting(CPU *c){ RET(audio_on() ? SEGAAPI_SetSendRouting(seg_lookup(A32(0)), A32(1), A32(2), AI32(3)) : 0); }
static void h_segapi_setsendlevel(CPU *c)  { RET(audio_on() ? SEGAAPI_SetSendLevel(seg_lookup(A32(0)), A32(1), A32(2), A32(3)) : 0); }

static void h_segapi_setsynthparamm(CPU *c)
{
    if (!audio_on()) { RET(0); return; }
    void *h = seg_lookup(A32(0)); unsigned n = A32(1); uint32_t gp = A32(2);
    if (!h || !gp) { RET(0); return; }
    RET(SEGAAPI_SetSynthParamMultiple(h, n, (HostSynthParamSet*)(uintptr_t)gp));
}
static void h_segapi_playwithsetup(CPU *c)
{
    if (!audio_on()) { RET(0); return; }
    void *h = seg_lookup(A32(0));
    if (!h) { RET(-1); return; }
    segrl("PlayWithSetup", A32(0));
    RET(SEGAAPI_PlayWithSetup(h, A32(1), (HostSendRouteParamSet*)(uintptr_t)A32(2),
        A32(3), (HostSendLevelParamSet*)(uintptr_t)A32(4),
        A32(5), (HostVoiceParamSet*)(uintptr_t)A32(6),
        A32(7), (HostSynthParamSet*)(uintptr_t)A32(8)));
}
static void h_segapi_ok(CPU *c) { RET(0); }
static uint32_t g_sega_hdl;

/* ---- sockets — the cabinet link link ----
 *
 * ID4 opens a UDP socket to probe eth0 for the cabinet-link network. There
 * is no ethernet here: socket() failing is the honest answer, the caller
 * does not check, the ifreq stays zero, and the game logs "0.0.0.0" and runs
 * offline. Returning a real fd would invite ioctl/SIOCGIFADDR answers we
 * cannot honestly give, so the whole family refuses. inet_* string helpers
 * still work — they are pure formatting and the game uses them regardless. */
static void h_socket_retneg1(CPU *c) { RET(-1); }
static void h_gethost_null(CPU *c) { RET(0); }

/* parse "a.b.c.d" → packed in_addr (low byte = first octet, network order) */
static uint32_t parse_ipv4(const char *s)
{
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        unsigned v = 0; int nd = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; nd++; }
        if (!nd || v > 255 || (i < 3 && *s++ != '.')) return 0xFFFFFFFFu;
        parts[i] = v;
    }
    return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}
static void h_inet_addr(CPU *c)
{
    RET(A32(0) ? parse_ipv4(ASTR(0)) : 0xFFFFFFFFu); /* INADDR_NONE on bad input */
}
static void h_inet_aton(CPU *c)
{
    uint32_t v = A32(0) ? parse_ipv4(ASTR(0)) : 0xFFFFFFFFu;
    if (v == 0xFFFFFFFFu) { RET(0); return; }
    if (A32(1)) wr32(A32(1), v);
    RET(1);
}
static void h_inet_ntoa(CPU *c)
{
    /* returns a static guest string */
    static uint32_t s_buf;
    if (!s_buf) {
        extern uint32_t guest_brk(uint32_t);
        extern void guest_heap_lock(void), guest_heap_unlock(void);
        guest_heap_lock();
        if (!s_buf) {   /* double-check under the lock */
            uint32_t b = (guest_brk(0) + 0xf) & ~0xfu;
            guest_brk(b + 32);
            s_buf = b;
        }
        guest_heap_unlock();
    }
    uint32_t a = A32(0);
    char tmp[32];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = (a >> (8 * i)) & 255;
        if (v >= 100) tmp[n++] = '0' + v / 100, v %= 100;
        if (v >= 10)  tmp[n++] = '0' + v / 10;
        tmp[n++] = '0' + v % 10;
        if (i < 3) tmp[n++] = '.';
    }
    tmp[n] = 0;
    memcpy((void *)(uintptr_t)s_buf, tmp, n + 1);
    RET(s_buf);
}

/* The engine's own console.
 *
 * _sDebug::putConsole is where this game narrates what it is doing - which
 * hardware it looked for, what answered, and why it gave up. On the cabinet
 * it goes to a debug console that is not here, so every one of those messages
 * has been thrown away. It is a printf, and a static one, so the format
 * string is the first argument and the rest follow it on the stack.
 *
 * LINDBERGH_CONSOLE=1 turns it on. Reading what the game says beats guessing
 * what it wants. */
static void h_putConsole(CPU *c)
{
    static __thread char buf[4096];
    guest_format(buf, sizeof buf, ASTR(0), c->esp + 4u);
    klogf("[game] ");
    klogf("%s", buf);
    if (!*buf || buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
    RET(0);
}

void hle_register_console(void)
{
    if (!getenv("LINDBERGH_CONSOLE")) return;
    if (guest_override("_ZN7_sDebug10putConsoleEPKcz", h_putConsole))
        fprintf(stderr, "[console] engine console messages enabled\n");
}

void hle_register_io(void)
{
    hle_bind("open", h_open);       hle_bind("close", h_close);
    hle_bind("read", h_read);       hle_bind("write", h_write);
    hle_bind("lseek", h_lseek);     hle_bind("ioctl", h_ioctl);
    hle_bind("unlink", h_unlink);   hle_bind("mkdir", h_mkdir);
    hle_bind("iopl", h_iopl);
    hle_bind("shmget", h_shmget);   hle_bind("shmat", h_shmat);
    hle_bind("shmdt", h_shmdt);     hle_bind("shmctl", h_shmctl);

    hle_bind("tolower", h_tolower); hle_bind("toupper", h_toupper);
    hle_bind("atoi", h_atoi);       hle_bind("isxdigit", h_isxdigit);
    hle_bind("getc", h_getc);       hle_bind("putc", h_putc);
    hle_bind("fgetc", h_fgetc);     hle_bind("getc_unlocked", h_getc_unlocked);
    hle_bind("fgets_unlocked", h_fgets_unlocked);
    hle_bind("fgetpos", h_fgetpos); hle_bind("fsetpos", h_fsetpos);
    hle_bind("fseeko", h_fseeko);   hle_bind("ftello", h_ftello);
    hle_bind("freopen", h_freopen);
    hle_bind("ungetc", h_ungetc);   hle_bind("putchar", h_putchar);
    hle_bind("fileno", h_fileno);   hle_bind("fdopen", h_fdopen);
    hle_bind("setvbuf", h_setvbuf); hle_bind("writev", h_writev);
    hle_bind("strtok_r", h_strtok_r);
    hle_bind("strcoll", h_strcoll); hle_bind("strxfrm", h_strxfrm);
    hle_bind("mktime", h_mktime);   hle_bind("strftime", h_strftime);
    hle_bind("utime", h_utime);
    hle_bind("lseek64", h_lseek64);
    hle_bind("fseeko64", h_fseeko64); hle_bind("ftello64", h_ftello64);

    /* identity / resource / signal probes — plausible values, never crash */
    hle_bind("uname", h_uname);       hle_bind("sysinfo", h_sysinfo);
    hle_bind("getrlimit", h_getrlimit); hle_bind("setrlimit", h_setrlimit);
    hle_bind("getpriority", h_getpriority); hle_bind("setpriority", h_setpriority);
    hle_bind("getuid", h_getuid);     hle_bind("geteuid", h_geteuid);
    hle_bind("getgid", h_getgid);     hle_bind("getegid", h_getegid);
    hle_bind("getppid", h_getppid);   hle_bind("getlogin", h_getlogin);
    hle_bind("gethostname", h_gethostname);
    hle_bind("signal", h_signal);     hle_bind("sigaction", h_sigaction);
    hle_bind("sigprocmask", h_sigprocmask);
    hle_bind("sigemptyset", h_sigemptyset); hle_bind("sigfillset", h_sigfillset);
    hle_bind("sigaddset", h_sigaddset); hle_bind("sigdelset", h_sigdelset);
    hle_bind("sigismember", h_sigismember);
    hle_bind("pause", h_pause);       hle_bind("alarm", h_alarm);
    hle_bind("atexit", h_atexit);     hle_bind("on_exit", h_on_exit);
    /* stat-family — the game calls stat/fstat/lstat/stat64 directly too */
    hle_bind("stat", h_stat);         hle_bind("lstat", h_lstat);
    hle_bind("fstat", h_fstat);
    hle_bind("stat64", h_stat64);     hle_bind("fstat64", h_fstat64);
    hle_bind("lstat64", h_lstat64);   hle_bind("fstatat", h_fstatat);
    hle_bind("statfs", h_statfs);     hle_bind("statvfs", h_statvfs);
    /* FILE-fn surface — NULL-f guarded */
    hle_bind("vfscanf", h_vfscanf);   hle_bind("vsfscanf", h_vsfscanf);
    hle_bind("fwscanf", h_fwscanf);   hle_bind("swscanf", h_swscanf);
    hle_bind("vwscanf", h_vwscanf);
    hle_bind("fgets_s", h_fgets_s);   hle_bind("fscanf_s", h_fscanf_s);
    hle_bind("sscanf_s", h_sscanf_s);
    hle_bind("fgetws", h_fgetws);     hle_bind("fputws", h_fputws);
    hle_bind("fgetwc", h_fgetwc);     hle_bind("fputwc", h_fputwc);
    hle_bind("fwide", h_fwide);
    hle_bind("wprintf", h_wprintf);   hle_bind("fwprintf", h_fwprintf);
    hle_bind("swprintf", h_swprintf);
    hle_bind("fgetln", h_fgetln);     hle_bind("fgetline", h_fgetline);
    hle_bind("fpurge", h_fpurge);     hle_bind("freadable", h_freadable);
    hle_bind("fwritable", h_fwritable); hle_bind("flbf", h_flbf);
    hle_bind("fbufsize", h_fbufsize); hle_bind("fcloseall", h_fcloseall);
    hle_bind("fpending", h_fpending);
    hle_bind("setbuf", h_setbuf);     hle_bind("setbuffer", h_setbuffer);
    hle_bind("setlinebuf", h_setlinebuf);
    hle_bind("fmemopen", h_fmemopen); hle_bind("open_memstream", h_open_memstream);
    hle_bind("fopencookie", h_fopencookie); hle_bind("funopen", h_funopen);
    hle_bind("rewinddir", h_rewinddir); hle_bind("seekdir", h_seekdir);
    hle_bind("telldir", h_telldir);
    hle_bind("scandir", h_scandir);   hle_bind("alphasort", h_alphasort);
    hle_bind("dirfd", h_dirfd);
    hle_bind("mkdtemp", h_mkdtemp);   hle_bind("mkstemp", h_mkstemp);
    hle_bind("tempnam", h_tempnam);   hle_bind("tmpnam", h_tmpnam);
    hle_bind("ftw", h_ftw);           hle_bind("nftw", h_nftw);
    hle_bind("fopen64", h_fopen64); hle_bind("__fxstat64", h___fxstat64);
    hle_bind("select", h_select);   hle_bind("poll", h_poll);
    hle_bind("fcntl", h_fcntl);
    hle_bind("__ctype_get_mb_cur_max", h_mb_cur_max);
    hle_bind("mbrtowc", h_mbrtowc); hle_bind("wcrtomb", h_wcrtomb);
    hle_bind("getwc", h_getwc);     hle_bind("putwc", h_putwc);
    hle_bind("ungetwc", h_ungetwc);
    hle_bind("wmemchr", h_wmemchr); hle_bind("wmemcmp", h_wmemcmp);
    hle_bind("wmemcpy", h_wmemcpy); hle_bind("wmemmove", h_wmemmove);
    hle_bind("wmemset", h_wmemset);
    hle_bind("wcscoll", h_wcscoll); hle_bind("wcsxfrm", h_wcsxfrm);
    hle_bind("wcsftime", h_wcsftime);
    hle_bind("__fixunsdfdi", h___fixunsdfdi);
    hle_bind("__fixunssfdi", h___fixunssfdi);
    hle_bind("cfgetispeed", h_cfgetispeed);
    hle_bind("cfgetospeed", h_cfgetospeed);
    hle_bind("cfsetspeed", h_cfsetspeed);
    hle_bind("tcdrain", h_tcdrain); hle_bind("tcflush", h_tcflush);
    hle_bind("tcgetattr", h_tcgetattr); hle_bind("tcsetattr", h_tcsetattr);

    /* SEGAAPI — wired to libsegaapi_ps5.c (real mixer + sceAudioOut) */
    hle_bind("SEGAAPI_Init", h_segapi_init);
    hle_bind("SEGAAPI_Exit", h_segapi_exit);
    hle_bind("SEGAAPI_Reset", h_segapi_reset);
    hle_bind("SEGAAPI_GetLastStatus", h_segapi_getlaststatus);
    hle_bind("SEGAAPI_CreateBuffer", h_segapi_createbuffer);
    hle_bind("SEGAAPI_DestroyBuffer", h_segapi_destroybuffer);
    hle_bind("SEGAAPI_Play", h_segapi_play);
    hle_bind("SEGAAPI_PlayWithSetup", h_segapi_playwithsetup);
    hle_bind("SEGAAPI_Pause", h_segapi_pause);
    hle_bind("SEGAAPI_Stop", h_segapi_stop);
    hle_bind("SEGAAPI_UpdateBuffer", h_segapi_updatebuffer);
    hle_bind("SEGAAPI_GetPlaybackStatus", h_segapi_getstatus);
    hle_bind("SEGAAPI_GetPlaybackPosition", h_segapi_getpos);
    hle_bind("SEGAAPI_SetPlaybackPosition", h_segapi_setpos);
    hle_bind("SEGAAPI_GetSynthParam", h_segapi_getsynthparam);
    hle_bind("SEGAAPI_SetSynthParam", h_segapi_setsynthparam);
    hle_bind("SEGAAPI_SetSynthParamMultiple", h_segapi_setsynthparamm);
    hle_bind("SEGAAPI_GetUserData", h_segapi_getuserdata);
    hle_bind("SEGAAPI_SetUserData", h_segapi_setuserdata);
    hle_bind("SEGAAPI_SetChannelVolume", h_segapi_setchanvol);
    hle_bind("SEGAAPI_SetIOVolume", h_segapi_setiovol);
    hle_bind("SEGAAPI_SetSampleRate", h_segapi_setsamplerate);
    hle_bind("SEGAAPI_SetLoopState", h_segapi_setloopstate);
    hle_bind("SEGAAPI_SetReleaseState", h_segapi_setreleasestate);
    hle_bind("SEGAAPI_SetEndLoopOffset", h_segapi_setendloop);
    hle_bind("SEGAAPI_SetEndOffset", h_segapi_setendoffset);
    hle_bind("SEGAAPI_SetStartLoopOffset", h_segapi_setstartloop);
    hle_bind("SEGAAPI_SetSendLevel", h_segapi_setsendlevel);
    hle_bind("SEGAAPI_SetSendRouting", h_segapi_setsendrouting);
    hle_bind("SEGAAPI_SetGlobalEAXProperty", h_segapi_ok);

    /* Cabinet-link sockets — absent here; every entry refuses cleanly so the
     * game settles for 0.0.0.0 and runs offline. */
    hle_bind("socket", h_socket_retneg1);
    hle_bind("bind", h_socket_retneg1);
    hle_bind("connect", h_socket_retneg1);
    hle_bind("listen", h_socket_retneg1);
    hle_bind("accept", h_socket_retneg1);
    hle_bind("sendto", h_socket_retneg1);
    hle_bind("recvfrom", h_socket_retneg1);
    hle_bind("send", h_socket_retneg1);
    hle_bind("recv", h_socket_retneg1);
    hle_bind("sendmsg", h_socket_retneg1);
    hle_bind("recvmsg", h_socket_retneg1);
    hle_bind("setsockopt", h_socket_retneg1);
    hle_bind("getsockopt", h_socket_retneg1);
    hle_bind("shutdown", h_socket_retneg1);
    hle_bind("socketpair", h_socket_retneg1);
    hle_bind("gethostbyname", h_gethost_null);
    hle_bind("gethostbyname_r", h_socket_retneg1);
    hle_bind("gethostbyaddr", h_gethost_null);
    hle_bind("gethostbyaddr_r", h_socket_retneg1);
    hle_bind("getservbyname", h_gethost_null);
    hle_bind("getservbyport", h_gethost_null);
    hle_bind("getprotobyname", h_gethost_null);
    hle_bind("inet_addr", h_inet_addr);
    hle_bind("inet_aton", h_inet_aton);
    hle_bind("inet_ntoa", h_inet_ntoa);
    hle_bind("inet_network", h_socket_retneg1);
    hle_bind("inet_makeaddr", h_socket_retneg1);
    hle_bind("inet_lnaof", h_socket_retneg1);
    hle_bind("inet_netof", h_socket_retneg1);

    /* Process spawns must never report success: fork()==0 reads as "I am the
     * child" and reruns main; popen/system/execl success lets the game think
     * picodaemon or zmd came up when nothing did. -1 sends it to the real
     * "cannot start helper" path instead. */
    hle_bind("fork", h_socket_retneg1);
    hle_bind("vfork", h_socket_retneg1);
    hle_bind("system", h_socket_retneg1);
    hle_bind("popen", h_gethost_null);      /* NULL stream */
    hle_bind("pclose", h_socket_retneg1);
    hle_bind("execlp", h_socket_retneg1);
    hle_bind("execl", h_socket_retneg1);
    hle_bind("execv", h_socket_retneg1);
    hle_bind("execvp", h_socket_retneg1);
    hle_bind("execlpe", h_socket_retneg1);
    hle_bind("daemon", h_socket_retneg1);
    hle_bind("wait", h_socket_retneg1);
    hle_bind("waitpid", h_socket_retneg1);
    hle_bind("kill", h_socket_retneg1);
    hle_bind("raise", h_gethost_null);      /* 0 = "signal raised" — harmless */
    hle_bind("creat", h_socket_retneg1);
}

/* ---- the v* formatters ----
 *
 * A va_list on i386 is nothing but a pointer into the caller's stack, so these
 * are the same formatter with the argument base taken from the guest rather
 * than computed from esp. That is the whole reason guest_format takes an
 * address. */
static void h_vsprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), A32(2));
    size_t have = strlen(g_fmtbuf);
    memcpy(ASTR(0), g_fmtbuf, have + 1);
    RET(n);
}
static void h_vsnprintf(CPU *c)
{
    uint32_t cap = A32(1);
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(2), A32(3));
    if (cap) {
        size_t have = strlen(g_fmtbuf);
        size_t take = have < cap - 1 ? have : cap - 1;
        memcpy(ASTR(0), g_fmtbuf, take);
        ASTR(0)[take] = 0;
    }
    RET(n);
}
static void h_vfprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), A32(2));
    FILE *f = fhdl_get(A32(0));
    if (!f) { fputs("[game] ", stderr); f = stderr; }
    fputs(g_fmtbuf, f);
    RET(n);
}

/* ---- dynamic loading ----
 *
 * The game dlopens the Sega sound library and looks its entry points up by
 * name. It also imports those same names through its PLT, so the lookup can be
 * answered the same way glXGetProcAddressARB is: hand back the stub, which is
 * already an address dispatch() routes to a handler. The handle itself only
 * has to be non-null and recognisable. */
#define DL_HANDLE 0x444C0001u

static void h_dlopen(CPU *c)
{
    fprintf(stderr, "[dl] dlopen(%s)\n", A32(0) ? ASTR(0) : "(self)");
    RET(DL_HANDLE);
}
/* ---- symbols that live in a shared object we do not have ----
 *
 * Ghost Squad Evolution dlopens its audio library and dlsyms every ADX entry
 * point out of it. Those are not PLT imports of the game binary, so there is
 * nothing to hand back - and a game that gets a null from dlsym does not
 * check it. It calls it, and dispatch lands on address zero.
 *
 * A stub address is a better answer than a null one. These tokens live above
 * the image where nothing else can be, dispatch routes them here, and calling
 * one does nothing and returns zero - which for an audio library means a game
 * that runs in silence rather than one that does not run.
 */
#define DL_TOKEN_BASE 0xF1000000u
#define DL_TOKEN_MAX  256

static struct { const char *name; int moaned; } g_dl_tok[DL_TOKEN_MAX];
static int g_dl_tok_n;

static uint32_t dl_stub_for(const char *name)
{
    for (int i = 0; i < g_dl_tok_n; i++)
        if (strcmp(g_dl_tok[i].name, name) == 0)
            return DL_TOKEN_BASE + (uint32_t)i;      /* same name, same address */
    if (g_dl_tok_n >= DL_TOKEN_MAX) return 0;
    g_dl_tok[g_dl_tok_n].name = _strdup(name);
    return DL_TOKEN_BASE + (uint32_t)(g_dl_tok_n++);
}

/* What a stubbed library function should hand back.
 *
 * Zero is the wrong answer for the half of an API that creates things. Ghost
 * Squad Evolution asks its sound library for an ADXT handle, gets a null, and
 * does not survive using it - and a null is not what the real library would
 * ever return. A pointer to a page of zeroes is: reads see nothing set,
 * writes go somewhere harmless, and a handle check passes.
 *
 * Every stub shares the one page, so two objects from a stubbed library are
 * the same object. That is fine for a library doing nothing, and would not be
 * for one doing something - which is the point at which the answer is to lift
 * the library rather than to stub it. */
/* One of these per stubbed library function, not one shared by all of them,
 * and each big enough to be a context object rather than a page.
 *
 * A game that asks a stubbed library to create something writes into what it
 * gets back. Handing every caller the same four kilobytes means two objects
 * are one object, and a context larger than the buffer runs off the end into
 * whatever the runtime keeps next door - which is host memory, so it ends as
 * a fail-fast with no handler and nothing printed. */
#define STUB_OBJECT_SIZE (256u * 1024u)
static unsigned char *g_stub_objects[DL_TOKEN_MAX];

int hle_stub_call(CPU *c, uint32_t va)
{
    if (va < DL_TOKEN_BASE || va >= DL_TOKEN_BASE + (uint32_t)g_dl_tok_n) return 0;
    int i = (int)(va - DL_TOKEN_BASE);
    if (!g_dl_tok[i].moaned) {
        g_dl_tok[i].moaned = 1;
        fprintf(stderr, "[dl] %s() is a stub - that library is not loaded\n",
                g_dl_tok[i].name);
    }
    if (!g_stub_objects[i]) {
        /* the game stores + reads this, so it has to be in guest memory. */
        extern uint32_t guest_brk(uint32_t);
        uint32_t b = guest_brk(0);
        b = (b + 0xfffu) & ~0xfffu;
        guest_brk(b + STUB_OBJECT_SIZE);
        memset((void *)(uintptr_t)b, 0, STUB_OBJECT_SIZE);
        g_stub_objects[i] = (unsigned char *)(uintptr_t)b;
    }
    RET((uint32_t)(uintptr_t)g_stub_objects[i]);
    return 1;
}

static void h_dlsym(CPU *c)
{
    const char *name = ASTR(1);
    uint32_t va = hle_plt_address(name);
    if (!va) {
        va = dl_stub_for(name);
        static int shown;
        if (shown < 12) {
            shown++;
            fprintf(stderr, "[dl] dlsym(%s) -> stub at %#010x\n", name, va);
        }
    }
    RET(va);
}
static void h_dlclose(CPU *c) { RET(0); }
static void h_dlerror(CPU *c) { (void)c; RET(0); }

/* ---- time ----
 *
 * struct tm on i386 glibc: nine ints, then tm_gmtoff and tm_zone. The game
 * reads the date fields to stamp its logs.
 * g_tm_gva lives in GUEST memory so the VA survives RET()'s 32-bit truncation. */
static uint32_t g_tm_gva;

static void h_localtime(CPU *c)
{
    if (!g_tm_gva) {
        uint32_t b = (guest_brk(0) + 0xfu) & ~0xfu;
        guest_brk(b + 44u);
        g_tm_gva = b;
    }
    time_t t = (time_t)(A32(0) ? rd32(A32(0)) : 0);
    struct tm lt;
    if (localtime_s(&lt, &t) != 0) { RET(0); return; }
    wr32(g_tm_gva +  0, (uint32_t)lt.tm_sec);
    wr32(g_tm_gva +  4, (uint32_t)lt.tm_min);
    wr32(g_tm_gva +  8, (uint32_t)lt.tm_hour);
    wr32(g_tm_gva + 12, (uint32_t)lt.tm_mday);
    wr32(g_tm_gva + 16, (uint32_t)lt.tm_mon);
    wr32(g_tm_gva + 20, (uint32_t)lt.tm_year);
    wr32(g_tm_gva + 24, (uint32_t)lt.tm_wday);
    wr32(g_tm_gva + 28, (uint32_t)lt.tm_yday);
    wr32(g_tm_gva + 32, (uint32_t)lt.tm_isdst);
    wr32(g_tm_gva + 36, 0);
    wr32(g_tm_gva + 40, 0);
    RET(g_tm_gva);
}

static void h_fscanf(CPU *c);   /* defined below, bound here */

void hle_register_libc3(void)
{
    hle_bind("vsprintf", h_vsprintf);
    hle_bind("vsnprintf", h_vsnprintf);
    hle_bind("vfprintf", h_vfprintf);
    hle_bind("dlopen", h_dlopen);
    hle_bind("dlsym", h_dlsym);
    hle_bind("dlclose", h_dlclose);
    hle_bind("dlerror", h_dlerror);
    hle_bind("localtime", h_localtime);
    hle_bind("fscanf", h_fscanf);
}

/* ---- scanf ----
 *
 * The same problem as printf and a worse failure mode. A guest va_list cannot
 * be handed to the host, so the format is walked here and each directive given
 * to the host's own scanner on its own, with the single guest pointer it
 * fills. Literal and whitespace runs are passed through too, because they
 * consume input the conversions rely on.
 *
 * Leaving this unbound is not a no-op: the game reads its configuration with
 * `while (!feof(f)) fscanf(...)`, and a scanf that returns without consuming
 * anything turns that into an infinite loop. It span a hundred thousand times
 * a second and drew nothing.
 */
static int guest_scan(FILE *f, const char *fmt, CPU *c, uint32_t argbase)
{
    unsigned argi = 0;
    int filled = 0;
    char spec[64];

    while (*fmt) {
        if (*fmt != 37) {                      /* not '%': literal or space */
            const char *start = fmt;
            while (*fmt && *fmt != 37) fmt++;
            size_t len = (size_t)(fmt - start);
            if (len < sizeof spec - 1) {
                memcpy(spec, start, len);
                spec[len] = 0;
                fscanf(f, spec);               /* consumes, assigns nothing */
            }
            continue;
        }

        const char *start = fmt++;
        if (*fmt == 37) { fscanf(f, "%%"); fmt++; continue; }

        int suppress = 0;
        if (*fmt == 42) { suppress = 1; fmt++; }            /* %* */
        while (isdigit((unsigned char)*fmt)) fmt++;         /* width */
        while (*fmt && strchr("hlLqjzt", *fmt)) fmt++;      /* length */
        char conv = *fmt ? *fmt++ : 0;
        if (!conv) break;

        size_t len = (size_t)(fmt - start);
        if (len >= sizeof spec - 1) break;
        memcpy(spec, start, len);
        spec[len] = 0;

        if (suppress) { fscanf(f, spec); continue; }

        void *dst = (void *)(uintptr_t)rd32(argbase + 4u * argi++);
        if (!dst) break;
        if (fscanf(f, spec, dst) == 1) filled++;
        else break;                            /* a failed conversion ends it */
    }
    /* C89: EOF when an input failure hits before the first conversion —
     * callers loop `while (fscanf(...) != -1)` (cColorChipEditor::load reads
     * CAR_COLOR_DATA.txt this way); returning 0 at EOF spins push_back until
     * the heap bursts. */
    if (filled == 0 && feof(f)) return -1;
    return filled;
}

static void h_fscanf(CPU *c)
{
    FILE *f = fhdl_get(A32(0));
    RET(f ? guest_scan(f, ASTR(1), c, c->esp + 8u) : -1);
}

/* ---- reading a directory ----
 *
 * OutRun 2 SP SDX scans for its saved ghost car data at start up, and a null
 * from opendir is not something it recovers from quietly.
 *
 * The guest's struct dirent is glibc's 32-bit one, and the game reads d_name
 * out of it, so the offsets have to be right:
 *
 *      +0   d_ino      +4  d_off     +8  d_reclen
 *      +10  d_type     +11 d_name, 256 bytes
 */
#define DIRENT_NAME 11
#define DIRENT_SIZE (DIRENT_NAME + 256)

/* DIR handle table — opendir/readdir return host DIR* objects that truncate
 * fatally through RET().  Use token indices instead: DHANDLE_BASE+i is the
 * opaque handle the guest stores; the matching slot holds the real state.
 * The dirent buffer for readdir is in GUEST memory so its address is safe. */
#define DHANDLE_BASE 0xF4000000u
#define DHANDLE_MAX  16

typedef struct {
    intptr_t            find;
    int                 used;
    int                 first; /* _findfirst already loaded data; return before advancing */
    struct _finddata_t  data;
    uint32_t            ent_gva; /* guest VA of the DIRENT_SIZE byte buffer */
} DirSlot;

static DirSlot g_dslot[DHANDLE_MAX];

static int dslot_alloc(void)
{
    for (int i = 0; i < DHANDLE_MAX; i++)
        if (!g_dslot[i].used) return i;
    return -1;
}

static DirSlot *dslot_get(uint32_t h)
{
    uint32_t i = h - DHANDLE_BASE;
    if (i >= DHANDLE_MAX || !g_dslot[i].used) return NULL;
    return &g_dslot[i];
}

static void h_opendir(CPU *c)
{
    const char *path = A32(0) ? ASTR(0) : NULL;
    if (!path) { RET(0); return; }

    int idx = dslot_alloc();
    if (idx < 0) { RET(0); return; }

    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*", path);

    DirSlot *d = &g_dslot[idx];
    d->find = _findfirst(pattern, &d->data);
    if (d->find == -1) { RET(0); return; }
    if (!d->ent_gva) {
        void *p = gmalloc(DIRENT_SIZE, 16);
        d->ent_gva = (uint32_t)(uintptr_t)p;
    }
    d->used = 1;
    d->first = 1; /* _findfirst already loaded first entry into data */
    RET(DHANDLE_BASE + (uint32_t)idx);
}

static void h_readdir(CPU *c)
{
    DirSlot *d = dslot_get(A32(0));
    if (!d) { RET(0); return; }

    if (d->first) {
        d->first = 0; /* consume the pre-loaded entry from _findfirst */
    } else if (_findnext(d->find, &d->data) != 0) {
        RET(0); return;
    }

    unsigned char *ent = (unsigned char *)(uintptr_t)d->ent_gva;
    memset(ent, 0, DIRENT_SIZE);
    ent[8] = (unsigned char)DIRENT_SIZE;
    ent[10] = (d->data.attrib & _A_SUBDIR) ? 4 : 8;
    snprintf((char *)ent + DIRENT_NAME, 256, "%s", d->data.name);
    RET(d->ent_gva);
}

static void h_closedir(CPU *c)
{
    DirSlot *d = dslot_get(A32(0));
    if (d) { if (d->find != -1) _findclose(d->find); d->used = 0; }
    RET(0);
}

/* The game changes into its own data directory and then opens files by
 * relative path, so this has to actually happen rather than be waved through. */
static void h_chdir(CPU *c)
{
    const char *path = A32(0) ? ASTR(0) : NULL;
    int rc = path ? _chdir(path) : -1;
    fprintf(stderr, "[io] chdir(%s) -> %d\n", path ? path : "(null)", rc);
    RET((uint32_t)rc);
}

static void h_perror(CPU *c)
{
    fprintf(stderr, "[guest] %s\n", A32(0) ? ASTR(0) : "error");
    RET(0);
}

void hle_register_dir(void)
{
    hle_bind("opendir", h_opendir);
    hle_bind("readdir", h_readdir);
    hle_bind("closedir", h_closedir);
    hle_bind("perror", h_perror);
    hle_bind("chdir", h_chdir);
    hle_bind("getcwd", h_getcwd);
}

/* ---- sscanf ----
 *
 * OutRun 2 SP SDX parses its shader descriptions with this, so it has to work
 * rather than merely return.
 *
 * Rather than write another format parser, each directive is handed to the
 * host's own sscanf one at a time with a %n on the end, which says how much
 * of the input it consumed so the next directive starts in the right place.
 * The host parser is then the one deciding what a %f accepts, which is the
 * part that is easy to get subtly wrong by hand.
 */
static int guest_sscanf(const char *in, const char *fmt, CPU *c, uint32_t argbase)
{
    int filled = 0;
    uint32_t arg = argbase;
    size_t pos = 0;

    for (const char *f = fmt; *f; ) {
        /* Literal text must match the input: whitespace in the format skips
         * any run of input whitespace (possibly none), anything else has to
         * match byte for byte. Dropping literals loses anchors - with the
         * "[ %s ]" category syntax every ini line read as "[" and no
         * category was ever found. */
        if (*f != '%') {
            if (isspace((unsigned char)*f)) {
                while (isspace((unsigned char)in[pos])) pos++;
            } else {
                if (in[pos] != *f) return filled;
                pos++;
            }
            f++;
            continue;
        }
        f++;
        if (*f == '%') {                     /* %% - a literal '%' */
            if (in[pos] != '%') return filled;
            pos++; f++;
            continue;
        }

        /* Copy this one directive out, flags and width and all. A %[...]
         * scanset runs to its closing ']'; a '*' suppresses assignment so
         * it consumes input but takes no pointer argument. */
        const char *start = f - 1;   /* include the '%' itself */
        int suppress = 0;
        while (*f && !strchr("diouxXeEfgGaAcspn[", *f)) {
            if (*f == '*') suppress = 1;
            f++;
        }
        if (!*f) break;
        char conv = *f++;
        if (conv == '[') {
            if (*f == '^') f++;
            if (*f == ']') f++;
            while (*f && *f != ']') f++;
            if (*f != ']') break;
            f++;
        }

        char one[64];
        size_t len = (size_t)(f - start);
        if (len > sizeof one - 4) break;
        memcpy(one, start, len);
        strcpy(one + len, "%n");

        int consumed = -1;
        int got;
        if (suppress) {
            static char scratch[65536]; int iv; float fl;
            if (conv == 'c' || conv == 's' || conv == '[')
                got = sscanf(in + pos, one, scratch, &consumed);
            else if (conv == 'e' || conv == 'E' || conv == 'f' ||
                     conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A')
                got = sscanf(in + pos, one, &fl, &consumed);
            else
                got = sscanf(in + pos, one, &iv, &consumed);
            (void)got;
            if (consumed < 0) break;         /* the conversion never matched */
            pos += (size_t)consumed;
            continue;
        }

        uint32_t target = rd32(arg);
        if (!target) break;          /* a null destination is not ours to write */
        if (conv == 'c' || conv == 's' || conv == '[')
            got = sscanf(in + pos, one, (char *)(uintptr_t)target, &consumed);
        else if (conv == 'e' || conv == 'E' || conv == 'f' ||
                 conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A')
            got = sscanf(in + pos, one, (float *)(uintptr_t)target, &consumed);
        else
            got = sscanf(in + pos, one, (int *)(uintptr_t)target, &consumed);

        if (got != 1 || consumed < 0) break;
        pos += (size_t)consumed;
        arg += 4;
        filled++;
    }
    return filled;
}

static void h_sscanf(CPU *c)
{
    const char *in  = A32(0) ? ASTR(0) : "";
    const char *fmt = A32(1) ? ASTR(1) : "";
    /* A32(0) is at esp+0, so the variable arguments begin at esp+8 - two
     * slots along, not three. */
    RET((uint32_t)guest_sscanf(in, fmt, c, c->esp + 8u));
}

void hle_register_scan(void) { hle_bind("sscanf", h_sscanf); }
