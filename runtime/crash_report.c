/*
 * crash_report.c - what the simulated machine was doing when it faulted.
 *
 * A debugger on a recompiled program shows you a C function named L_08411FF0
 * a million lines into a generated file, which is true and useless. What you
 * need is the guest's state: which lifted function was running, how it got
 * there, and what the registers held.
 *
 * pcrecomp has the equivalent for its global-register model in
 * runtime/recomp32/crash_report.c. That one reads g_eax and friends directly,
 * so it cannot serve the CPU-struct model - this is the same idea against a
 * `CPU *`. If a second CPU-struct target ever wants it, it should move up into
 * pcrecomp/runtime/recomp32_cpu/ beside cpu.h rather than be copied again.
 *
 * The handler reports and then declines the exception, so the process still
 * dies and a debugger still gets its turn. Nothing here allocates.
 */

#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "lindbergh_rt.h"   /* before the extern below so the alias applies */

/* fault.log — durable crash copy in the title state dir (Simpsons-style
 * fault.log). The FILE is opened at handler install, before libc objects
 * can be torn down by a fault mid-constructor. */
static FILE *g_fault_f;
static void fault_tee(const char *s)
{
    if (!g_fault_f)
        g_fault_f = fopen("/data/homebrew/PPSA99406/logs/fault.log", "ab");
    if (g_fault_f) {
        fwrite(s, 1, strlen(s), g_fault_f);
        fflush(g_fault_f);
    }
}
/* klog + fault.log for the sections that print through sceKernelDebugOutText
 * directly (mcontext dump, memory census) rather than kf_/id4_dbg. */
static void segv_out(const char *s)
{
    extern int sceKernelDebugOutText(int, const char *);
    sceKernelDebugOutText(0, s);
    fault_tee(s);
}

static int kf_(const char *fmt, ...){ char b[512]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); id4_dbg(0,b); fault_tee(b); return 0; }

#ifdef _WIN32
#include <windows.h>
#endif

#include "lindbergh_rt.h"

/* The CPU currently executing guest code. Thread-local: every guest thread
 * runs on its own CPU struct, so a single global would report whichever thread
 * happened to start last rather than the one that faulted. */
#ifdef _MSC_VER
static __declspec(thread) CPU *g_current;
#else
static __thread CPU *g_current;
#endif

static CPU *g_shared_cpu;
/* Every CPU that has ever been registered (main + guest threads). Lets the
 * watchdog report all threads, not just the last one to start. */
#define CPU_REG_N 65
static CPU *g_cpu_reg[CPU_REG_N];
static int g_cpu_reg_n;
#ifndef _WIN32
#include <signal.h>
#include <sys/time.h>
static void eip_sig(int s)
{
    (void)s;
    for (int i = 0; i < g_cpu_reg_n; i++) {
        CPU *c = g_cpu_reg[i];
        if (c)
            fprintf(stderr, "[eip] cpu%d %#010x esp=%#x edi=%#x esi=%#x\n",
                    i, c->eip, c->esp, c->edi, c->esi);
        if (c) {
            extern int guest_mapped(uint32_t);
            if (guest_mapped(c->esp)) {
                fprintf(stderr, "  [esp] %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        rd32(c->esp), rd32(c->esp+4), rd32(c->esp+8), rd32(c->esp+12),
                        rd32(c->esp+16), rd32(c->esp+20), rd32(c->esp+24), rd32(c->esp+28));
                fprintf(stderr, "  [esp+] %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        rd32(c->esp+32), rd32(c->esp+36), rd32(c->esp+40), rd32(c->esp+44),
                        rd32(c->esp+48), rd32(c->esp+52), rd32(c->esp+56), rd32(c->esp+60));
            }
            fprintf(stderr, "  ebp=%#x", c->ebp);
            uint32_t f = c->ebp;
            for (int k = 0; k < 10 && guest_mapped(f) && guest_mapped(f + 4); k++) {
                uint32_t ret = rd32(f + 4);
                if (ret < 0x08048000 || ret > 0x0a000000) break;
                fprintf(stderr, " -> %#x", ret);
                f = rd32(f);
            }
            fprintf(stderr, "\n");
        }
    }
}
#endif
/* Poll-safe version of the SIGPROF dump for hosts where a periodic signal
 * is fatal (managed Orbis filters the sigreturn stub). A host thread calls
 * this on a timer instead. Same content, no signal machinery. */
void guest_dump_eips(void)
{
    for (int i = 0; i < g_cpu_reg_n; i++) {
        CPU *c = g_cpu_reg[i];
        if (c)
            fprintf(stderr, "[eip] cpu%d %#010x esp=%#x edi=%#x esi=%#x\n",
                    i, c->eip, c->esp, c->edi, c->esi);
        /* Main thread gets a return-address walk — that's the one whose
         * caller identifies the wait/loop the game is parked in. */
        if (c && i == 0) {
            extern int guest_mapped(uint32_t);
            uint32_t f = c->ebp;
            fprintf(stderr, "  [bt]");
            for (int k = 0; k < 8 && guest_mapped(f) && guest_mapped(f + 4); k++) {
                uint32_t ret = rd32(f + 4);
                if (ret < 0x08048000 || ret > 0x0a000000) break;
                fprintf(stderr, " %#x", ret);
                f = rd32(f);
            }
            fprintf(stderr, "\n");
        }
    }
    /* Counters that answer "is the guest actually asking for this" without
     * enabling the flood-prone dumps. */
    { extern unsigned long g_jvs_sends, g_jvs_recvs;
      fprintf(stderr, "  [jvs] sends=%lu recvs=%lu\n", g_jvs_sends, g_jvs_recvs); }
}

CPU *const *crash_cpu_list(void) { return g_cpu_reg; }

void guest_set_current_cpu(CPU *c) {
    g_current = c; g_shared_cpu = c;
    int seen = 0;
    for (int i = 0; i < g_cpu_reg_n; i++) if (g_cpu_reg[i] == c) seen = 1;
    if (!seen) {
        /* Bound-check THEN store must be atomic — two racing threads could
         * both pass g_cpu_reg_n < CPU_REG_N and write past the array. */
        extern void guest_heap_lock(void), guest_heap_unlock(void);
        guest_heap_lock();
        for (int i = 0; i < g_cpu_reg_n; i++) if (g_cpu_reg[i] == c) seen = 1;
        if (!seen && g_cpu_reg_n < CPU_REG_N) g_cpu_reg[g_cpu_reg_n++] = c;
        guest_heap_unlock();
    }
#ifndef _WIN32
    static int _started;
    if (!_started && !getenv("LINDBERGH_NO_WATCHDOG")) {
        _started = 1;
        /* On managed Orbis every delivered signal returns through a
         * sigreturn stub the sandbox filters, so a periodic SIGPROF is
         * fatal there. Environment-gated, off by default on Orbis. */
        struct itimerval it; it.it_interval.tv_sec=0; it.it_interval.tv_usec=400000;
        it.it_value.tv_sec=0; it.it_value.tv_usec=400000;
        signal(SIGPROF, eip_sig);
        setitimer(ITIMER_PROF, &it, 0);
    }
#endif
}

/* Recent dispatches, newest last. Written on the hot path, so it is one store
 * and one masked increment and nothing else - no timestamps, no names. */
#define TRACE_N 64
static uint32_t g_trace[TRACE_N];
static unsigned g_trace_i;

/* Persist the ring to a file, because the failure being chased kills the
 * process with __fastfail and no handler runs - so an in-memory ring is lost
 * exactly when it is wanted. Written every RING_FLUSH dispatches, which costs
 * one small write per few thousand calls and nothing at all unless
 * LINDBERGH_TRACE_FILE names somewhere to put it. */
#define RING_FLUSH 2048

static FILE *g_ringf;
static unsigned g_since_flush;

static void ring_persist(void)
{
    if (!g_ringf) {
        const char *path = getenv("LINDBERGH_TRACE_FILE");
        if (!path || !*path) { g_since_flush = 0; return; }
        g_ringf = fopen(path, "wb");
        if (!g_ringf) return;
    }
    fseek(g_ringf, 0, SEEK_SET);
    fwrite(&g_trace_i, 4, 1, g_ringf);
    fwrite(g_trace, 4, TRACE_N, g_ringf);
    fflush(g_ringf);
}

void guest_trace_dispatch(uint32_t va)
{
    /* How often the ring reaches the disc. Every few thousand calls is free
     * and enough to see roughly where a run died; chasing an exact
     * instruction wants LINDBERGH_TRACE_EVERY=1, which is slow and exact. */
    static unsigned every;
    if (!every) {
        const char *v = getenv("LINDBERGH_TRACE_EVERY");
        every = (v && *v) ? (unsigned)atoi(v) : RING_FLUSH;
        if (!every) every = RING_FLUSH;
    }
    g_trace[g_trace_i++ & (TRACE_N - 1)] = va;
    if (++g_since_flush >= every) {
        g_since_flush = 0;
        ring_persist();
    }
}

static const char *region(uint32_t va)
{
    if (va >= 0x08048000u && va < 0x08d00000u) return "image";
    if (va >= 0xbf000000u)                     return "stack";
    if (!va)                                   return "NULL";
    if (va < 0x1000u)                          return "near NULL";
    return "";
}

/* Walk the guest's frame pointers.
 *
 * The dispatch ring says what ran; it does not say who called whom, and for a
 * fault inside a leaf like _Rb_tree::find the caller is the entire question.
 * Lifted code keeps real frames - every function opens with push ebp / mov
 * ebp, esp - so the chain is the ordinary one: saved ebp at [ebp], return
 * address at [ebp+4].
 *
 * Addresses only, deliberately. The names live in the guest ELF's symbol
 * table, which this runtime has no reason to carry; tools/elf turns a column
 * of addresses into names in one pass, and that keeps the fault path free of
 * anything that could itself fail. */
static int readable(uint32_t va)
{
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi)) {
        fprintf(stderr, "  (cannot query %#010x)\n", va);
        return 0;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        fprintf(stderr, "  (%#010x: state %#lx protect %#lx)\n",
                va, (unsigned long)mbi.State, (unsigned long)mbi.Protect);
        return 0;
    }
    return 1;
#else
    /* Mapped-range check, not just va>0x1000: a torn live-CPU frame pointer
     * must end the walk instead of faulting the dumper (the "rip"=0x3f800000
     * crashes were exactly this - rd32(f+4) on ASCII/float garbage). */
    extern int guest_mapped(uint32_t);
    return guest_mapped(va);
#endif
}

void guest_backtrace(CPU *c)
{
    if (!c) return;
    fprintf(stderr, "guest backtrace (ebp chain):\n");

    uint32_t ebp = c->ebp;
    for (int depth = 0; depth < 32; depth++) {
        if (!readable(ebp) || !readable(ebp + 4)) break;
        uint32_t saved = rd32(ebp);
        uint32_t ret   = rd32(ebp + 4);
        if (!ret) break;
        fprintf(stderr, "  %2d  %#010x\n", depth, ret);
        if (saved <= ebp) break;          /* frames grow upward; anything else is junk */
        ebp = saved;
    }
    fflush(stderr);
}

void guest_report_state(const char *why)
{
    fprintf(stderr, "\n==== %s ====\n", why);

    if (g_current) {
        CPU *c = g_current;
        fprintf(stderr, "eax %08X  ecx %08X  edx %08X  ebx %08X\n",
                c->eax, c->ecx, c->edx, c->ebx);
        fprintf(stderr, "esp %08X  ebp %08X  esi %08X  edi %08X\n",
                c->esp, c->ebp, c->esi, c->edi);
        fprintf(stderr, "eip %08X\n", c->eip);
        { extern __thread int g_hle_current;
          if (g_hle_current >= 0)
            fprintf(stderr, "inside hle: %s\n", hle_name(g_hle_current)); }
        { extern __thread uint32_t g_last_dispatch_va;
          if (g_last_dispatch_va)
            fprintf(stderr, "inside dispatch va=%#x\n", g_last_dispatch_va); }
        { extern const char *g_gl_last_call;
          if (g_gl_last_call)
            fprintf(stderr, "inside gl: %s\n", g_gl_last_call); }
        fprintf(stderr, "flags cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                c->cf, c->zf, c->sf, c->of, c->pf, c->af);
    } else {
        fprintf(stderr, "(no CPU registered)\n");
    }

    if (g_current) {
        extern int guest_mapped(uint32_t);
        fprintf(stderr, "stack words at ebp:\n");
        uint32_t *p = (uint32_t*)(uintptr_t)(g_current->ebp);
        for (int i = 0; i < 10 && guest_mapped(g_current->ebp + (uint32_t)(i * 4)); i++)
            fprintf(stderr, "  [ebp+%02d] %#010x\n", i*4, p[i]);
        /* esp+0 is the return address for a fault inside a callee, and the
         * surrounding words often carry the caller's frame when ebp has been
         * repurposed - print them through the same readability guard. */
        fprintf(stderr, "stack words at esp:\n");
        for (int i = 0; i < 12; i++) {
            uint32_t a = g_current->esp + (uint32_t)(i * 4);
            fprintf(stderr, "  [esp+%02d] %#010x\n", i*4, readable(a) ? rd32(a) : 0xDEADDEADu);
        }
    }
    guest_backtrace(g_current);
    fprintf(stderr, "last %d dispatches, oldest first:\n", TRACE_N);
    unsigned n = g_trace_i < TRACE_N ? g_trace_i : TRACE_N;
    for (unsigned k = n; k > 0; k--) {
        uint32_t va = g_trace[(g_trace_i - k) & (TRACE_N - 1)];
        fprintf(stderr, "  %2u  %#010x %s\n", n - k, va, region(va));
    }
    fflush(stderr);
}

#ifdef _WIN32
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char why[160];

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uint32_t addr = (uint32_t)er->ExceptionInformation[1];
        snprintf(why, sizeof why, "access violation: %s %#010x %s",
                 er->ExceptionInformation[0] ? "write to" : "read from",
                 addr, region(addr));
    } else {
        snprintf(why, sizeof why, "exception %08lX", (unsigned long)er->ExceptionCode);
    }
    guest_report_state(why);
    return EXCEPTION_CONTINUE_SEARCH;      /* still crash; let a debugger see it */
}

/* The CRT's invalid parameter handler.
 *
 * Hand a Microsoft CRT function something it refuses - a null string, a
 * closed FILE - and it does not return an error. It calls __fastfail, which
 * raises 0xC0000409 and takes the process down past every vectored exception
 * handler and every atexit hook. Nothing is printed, nothing is flushed, and
 * from the outside it is indistinguishable from a silent crash.
 *
 * Which is exactly what it looked like, for several builds. Taking the
 * handler means the guest state gets reported like any other fault. */
static void on_invalid_parameter(const wchar_t *expr, const wchar_t *func,
                                 const wchar_t *file, unsigned line,
                                 uintptr_t reserved)
{
    (void)reserved;
    fprintf(stderr, "\n[crt] invalid parameter in %ls (%ls:%u): %ls\n",
            func ? func : L"?", file ? file : L"?", line,
            expr ? expr : L"(no expression)");
    fflush(stderr);
    guest_report_state("invalid parameter passed to a host library function");
    orb_exit(45);
}

static void on_abort_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[abort] something called abort()\n");
    fflush(stderr);
    guest_report_state("abort");
    orb_exit(49);
}

static void on_exit_note(void)
{
    fprintf(stderr, "[exit] process is exiting\n");
    fflush(stderr);
}

/* Per thread, because the CRT looks there first.
 *
 * _set_invalid_parameter_handler is process wide, but the UCRT consults a
 * thread-local handler before it, and a thread that never sets one gets the
 * default - which calls __fastfail. A game with a render thread and an audio
 * thread therefore dies silently on two of its three threads no matter what
 * the main thread installed. */
void guest_install_thread_handlers(void)
{
    _set_thread_local_invalid_parameter_handler(on_invalid_parameter);
    signal(SIGABRT, on_abort_signal);
}

void guest_install_crash_handler(void)
{
    /* Unbuffered, before anything else can be lost.
     *
     * stderr is block buffered the moment it is redirected to a file, so a
     * process that dies without unwinding takes its last few thousand
     * characters with it - including, most of the time, the one line that
     * said what went wrong. Every report here ends in fflush for that reason,
     * but the lines that matter are often the ordinary ones just before. */
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Who ends the process. A clean exit with nothing said looks exactly like
     * a crash from the outside, and it took a Windows exit code of zero to
     * notice that Ghost Squad was leaving rather than falling over. */
    atexit(on_exit_note);
    _set_invalid_parameter_handler(on_invalid_parameter);
    guest_install_thread_handlers();

    /* Stop abort() from becoming a fail-fast.
     *
     * By default the Microsoft CRT turns abort() into __fastfail, which the
     * kernel handles directly: no vectored handler, no atexit, no output, and
     * an exit code of 0xC0000409 that says only "something gave up". Clearing
     * _CALL_REPORTFAULT makes it an ordinary abort again, which SIGABRT can
     * catch and report like anything else. */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, on_abort_signal);
    AddVectoredExceptionHandler(1, on_exception);
}
#else
#include <sys/ucontext.h>

/* exit() issues SYS_exit — banned on managed Orbis titles (SIGSYS). The
 * sanctioned teardown is sceSystemServiceLoadExec("exit"). */
static void orb_exit(int rc) {
    extern int sceSystemServiceLoadExec(const char *, char *const *);
    (void)rc;
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) __asm__ volatile("" ::: "memory");
}
static long uc_rip(void *uc)
{
#if defined(__linux__) && defined(REG_RIP)
    return (long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#elif defined(__FreeBSD__) || defined(__ORBIS__) || defined(__SCE__)
    /* FreeBSD/Orbis amd64 mcontext: mc_rip field. */
    return (long)((ucontext_t *)uc)->uc_mcontext.mc_rip;
#else
    (void)uc; return 0;
#endif
}
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    extern int sceKernelDebugOutText(int, const char*);
    /* Re-entrant faults (the report path itself segfaulting) used to spam the
     * kernel log until the early boot lines scrolled out. Log the first fault
     * with full context, then die on any nested one. */
    static __thread int _depth;
    if (_depth++) {
        char b[80]; snprintf(b, sizeof b, "nested segv rip=%#lx\n", uc_rip(uc));
        segv_out(b);
        __builtin_trap();
    }
    char why[200];
    snprintf(why, sizeof why, "access violation: %s at %p rip=%#lx",
             sig == SIGBUS ? "bus" : "segv", si ? si->si_addr : 0, uc_rip(uc));
    segv_out(why); segv_out("\n");
    /* Faulting cpu: eip, regs, and the guest ebp return chain — the call
     * stack that produced the bad dispatch/host pointer. */
    if (g_current) {
        CPU *c = g_current;
        char b[200];
        snprintf(b, sizeof b, "  FAULT cpu eip=%#010x eax=%#x ebx=%#x ecx=%#x edx=%#x\n",
                 c->eip, c->eax, c->ebx, c->ecx, c->edx);
        segv_out(b);
        snprintf(b, sizeof b, "  esp=%#x ebp=%#x esi=%#x edi=%#x\n",
                 c->esp, c->ebp, c->esi, c->edi);
        segv_out(b);
        segv_out("  callchain:");
        extern int guest_mapped(uint32_t);
        uint32_t f = c->ebp;
        for (int k = 0; k < 16 && guest_mapped(f) && guest_mapped(f + 4); k++) {
            uint32_t ret = rd32(f + 4);
            if (ret < 0x08048000 || ret > 0x0a000000) break;
            snprintf(b, sizeof b, " %#x", ret);
            segv_out(b);
            f = rd32(f);
        }
        segv_out("\n");
        { extern __thread int g_hle_current;
          if (g_hle_current >= 0) {
              char b[80]; snprintf(b, sizeof b, "inside hle: %s\n", hle_name(g_hle_current));
              segv_out(b); } }
        { extern __thread uint32_t g_last_dispatch_va;
          if (g_last_dispatch_va) {
              char b[80]; snprintf(b, sizeof b, "inside dispatch va=%#x\n", g_last_dispatch_va);
              segv_out(b); } }
        { extern const char *g_gl_last_call;
          if (g_gl_last_call) {
              char b[80]; snprintf(b, sizeof b, "inside gl: %s\n", g_gl_last_call);
              segv_out(b); } }
    }
#if defined(__FreeBSD__) || defined(__ORBIS__) || defined(__SCE__)
    /* Raw mcontext dump FIRST — every other section has faulted on garbage
     * pointers before, and losing this loses the real fault PC. The kernel's
     * frame layout differs from the SDK's mcontext_t, so we print the whole
     * register block and pick out the real values by range offline. */
    {
        const uint64_t *m = (const uint64_t *)&((ucontext_t *)uc)->uc_mcontext;
        char b[96];
        for (int i = 0; i < 26; i++) {
            snprintf(b, sizeof b, "  mc[%02d]=%#lx\n", i, (unsigned long)m[i]);
            segv_out(b);
        }
    }
#endif
    /* Restore the default action BEFORE any more reporting: if anything below
     * faults, the kernel still prints its own coredump. On return the
     * faulting instruction re-executes, refaults, and the kernel dumps the
     * REAL segv — true rip/regs/backtrace — instead of our SIGILL trap. */
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);   /* nested SIGBUS still enters on_segv otherwise */
    {
        /* How much direct memory is left when it dies — the vertex-buffer
         * NULL writes smell like dmem exhaustion under ps5-gallium. */
        extern int sceKernelAvailableDirectMemorySize(int64_t, int64_t,
                                                      size_t, int64_t *,
                                                      size_t *);
        extern int64_t sceKernelGetDirectMemorySize(void);
        int64_t phys = -1; size_t avail = 0;
        int64_t total = sceKernelGetDirectMemorySize();
        int rc = sceKernelAvailableDirectMemorySize(0, total, 0x4000,
                                                  &phys, &avail);
        /* Flexible memory is the SEPARATE pool host malloc() draws from —
         * a malloc→NULL inside Mesa/ACO writes to NULL+small = our segv. */
        extern int sceKernelAvailableFlexibleMemorySize(size_t *);
        size_t flex = 0;
        int rcf = sceKernelAvailableFlexibleMemorySize(&flex);
        char b[128];
        snprintf(b, sizeof b, "  dmem avail=%zu of %lld rc=%d flex=%zu rc=%d\n",
                 avail, (long long)total, rc, flex, rcf);
        segv_out(b);

        /* VA census — is the process out of *address space* (not bytes)? */
        struct VQI {
            uintptr_t start, end; uint64_t offset;
            int protection, memory_type;
            uint32_t is_flexible:1,is_direct:1,is_stack:1,is_pooled:1,
                     is_committed:1,is_gpu_prt:1,amm_usage:1,reserved:1;
            char name[32]; uint8_t gpu_mask_id, reserved2;
        };
        extern int sceKernelVirtualQuery(const void *, int, struct VQI *, uint64_t);
        unsigned long long va_total = 0; unsigned regions = 0;
        uintptr_t a = 0x1000;
        while (a < (1ull<<46)) {
            struct VQI qi; int vq = sceKernelVirtualQuery((const void *)a, 1, &qi, sizeof(qi));
            if (vq || qi.end <= qi.start) break;
            regions++;
            va_total += qi.end - qi.start;
            a = qi.end;
            if (a < qi.start) break;
        }
        snprintf(b, sizeof b, "  va: %u regions %lluMB mapped\n",
                 regions, va_total >> 20);
        segv_out(b);
        /* regions >= 8MB — who owns the VA */
        a = 0x1000;
        while (a < (1ull<<46)) {
            struct VQI qi; int vq = sceKernelVirtualQuery((const void *)a, 1, &qi, sizeof(qi));
            if (vq || qi.end <= qi.start) break;
            if (qi.end - qi.start >= (8ull<<20)) {
                snprintf(b, sizeof b,
                    "  va %08lx..%08lx %lluMB %.28s\n",
                    (unsigned long)qi.start, (unsigned long)qi.end,
                    (unsigned long long)(qi.end - qi.start) >> 20,
                    qi.name[0] ? qi.name : "?");
                segv_out(b);
            }
            a = qi.end;
            if (a < qi.start) break;
        }
        /* Sony's mspace params — heap size/limits */
        {
            struct SceLibcMallocParam {
                unsigned long long reserved, committed, free_in_arena,
                                   free_in_pool, _pad[8];
            };
            extern int _sceLibcGetMallocParam(void *);
            struct SceLibcMallocParam mp; memset(&mp, 0, sizeof mp);
            int mrc = _sceLibcGetMallocParam(&mp);
            snprintf(b, sizeof b,
                "  malloc_param rc=%d %016llx %016llx %016llx %016llx\n",
                mrc, mp.reserved, mp.committed, mp.free_in_arena,
                mp.free_in_pool);
            segv_out(b);
        }
    }
    if (g_current) {
        /* Dump the guest stack near esp — return addresses identify who
         * called the faulting lifted function when dispatch didn't record it. */
        CPU *c = g_current;
        extern int sceKernelDebugOutText(int, const char *);
        extern int guest_mapped(uint32_t);
        char b[160];
        for (int i = 0; i < 16 && guest_mapped(c->esp + i * 4); i++) {
            uint32_t a = c->esp + i * 4;
            uint32_t v;
            __builtin_memcpy(&v, (const void *)(uintptr_t)a, 4);
            if (v >= 0x08040000u && v < 0x09000000u) {
                snprintf(b, sizeof b, "  stk[esp+%02x]=%08x\n", i * 4, v);
                segv_out(b);
            }
        }
    }
    guest_report_state(why);
    return;
}
void _hx_wcb(void *opaque, const char *s)
{
    (void)opaque;
    extern int sceKernelDebugOutText(int, const char *);
    segv_out(s);
}
/* syscall-free SIGSYS handler — sceKernelDebugOutText resolves to
 * sys_mdbg_service which is itself banned inside a signal frame on managed
 * titles (calling it re-faults → SIGILL → the real faulting context is lost).
 * Instead: snapshot the raw ucontext into a stable global the ps5debug
 * debugger can read out of the suspended process. */
unsigned long g_sigsys_dump[64];
unsigned long g_sigsys_magic;
static void on_sigsys(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    g_sigsys_dump[0] = (unsigned long)si;
    g_sigsys_dump[1] = si ? (unsigned long)si->si_signo : 0;
    g_sigsys_dump[2] = si ? (unsigned long)si->si_code : 0;
    g_sigsys_dump[3] = si ? (unsigned long)si->si_errno : 0;
    g_sigsys_dump[4] = si ? (unsigned long)si->si_addr : 0;
    g_sigsys_dump[5] = (unsigned long)uc;
    /* copy the first 55 qwords of the ucontext wholesale — whatever its
     * Orbis layout, the faulting rip/rax/rsp live somewhere inside */
    if (uc) {
        const unsigned long *p = (const unsigned long *)uc;
        for (int i = 0; i < 55; i++) g_sigsys_dump[6 + i] = p[i];
    }
    g_sigsys_magic = 0x51595345474e4953UL;   /* "SIGSYGS" — debugger beacon */
    __builtin_trap();
}
static void on_abrt_impl(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    /* si_code tells us who raised it: SI_USER/SI_TKILL = abort()/raise(),
     * kernel-sent, or a hardware-trap alias for SIGABRT (rare). si_addr is
     * only meaningful for hardware faults; for abort() it's the killer pid. */
    {
        char b[96];
        __builtin_snprintf(b, sizeof b, "[abrt] code=%d addr=%p pid=%d\n",
                           si ? si->si_code : -999, si ? si->si_addr : 0,
                           si ? si->si_pid : 0);
        id4_dbg(0, b);
    }
    {   extern void *pthread_self(void);
        char tb[64];
        __builtin_snprintf(tb, sizeof tb, "[abrt] thread=%p\n", pthread_self());
        id4_dbg(0, tb);
    }
    /* Dump the faulting thread's ucontext — rip/rsp of the real abort site. */
    {
        char ln[96];
        const uint64_t *m = (const uint64_t *)&((ucontext_t *)uc)->uc_mcontext;
        uint64_t rsp = 0;
        /* FreeBSD amd64 mcontext64 layout — mc[16]=rip, mc[19]=rsp per
         * the ps5-payload-sdk x86/ucontext.h. Print them explicitly. */
        {
            /* amd64 mcontext: mc[16]=trapno|fs|gs  mc[17]=addr  mc[18]=flags|es|ds
             * mc[19]=err  mc[20]=rip  mc[21]=cs  mc[22]=rflags  mc[23]=rsp  mc[24]=ss */
            __builtin_snprintf(ln, sizeof ln,
                "[uctx] rip=%#lx rsp=%#lx rflags=%#lx cs=%#lx err=%#lx addr=%#lx\n",
                (unsigned long)m[20], (unsigned long)m[23],
                (unsigned long)m[22], (unsigned long)m[21],
                (unsigned long)m[19], (unsigned long)m[17]);
            id4_dbg(0, ln);
        }
        for (int i = 0; i < 26; i++) {
            __builtin_snprintf(ln, sizeof ln, "[uctx] mc[%02d]=%#lx\n", i, (unsigned long)m[i]);
            id4_dbg(0, ln);
            /* the host stack pointer is the mc[] entry in the 0x7ee.. range */
            if (m[i] > 0x7e0000000ULL && m[i] < 0x800000000ULL) rsp = m[i];  /* host stacks are 0x7ee..; 0x800.. is dmem */
        }
        /* walk the faulting stack for return addresses (any module) */
        if (rsp) {
            volatile uint64_t *sp = (volatile uint64_t *)(uintptr_t)rsp;
            for (int i = 0; i < 200; i++) {
                uint64_t v = sp[i];
                if (v > 0x400000ULL && v < 0x800000000000ULL && !(v & 3) && v != 0x201c95830ULL) {
                    __builtin_snprintf(ln, sizeof ln, "[nctx] sp+%#x = %#lx\n", i*8, (unsigned long)v);
                    id4_dbg(0, ln);
                }
            }
        }
    }
    guest_report_state("abort");
    /* DIAG: if the SIGABRT is a soft-fail path (Mesa compile-fail cleanup,
     * library assert, C++ terminate substitute) the caller can continue
     * once the handler returns. Resume instead of exiting — the game either
     * proceeds or re-aborts at a deeper site, which is diagnostic either way. */
    id4_dbg(0, "[abrt] resuming\n");
    return;
}
/* on_abrt is a plain handler alias kept for the non-SIGINFO fallback path */
static void on_abrt(int sig) { (void)sig; on_abrt_impl(sig, NULL, NULL); }
void guest_install_crash_handler(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    /* SA_ONSTACK: a stack-overflow SIGSEGV can't run the handler on the dead
     * stack — give the handler its own 64KB stack so the report still fires. */
    static char sigstk[64 << 10];
    stack_t ss;
    ss.ss_sp = sigstk; ss.ss_size = sizeof sigstk; ss.ss_flags = 0;
    sigaltstack(&ss, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sa.sa_sigaction = on_sigsys;
    sigaction(SIGSYS, &sa, 0);
    sa.sa_sigaction = on_abrt_impl;
    sigaction(SIGABRT, &sa, 0);
    /* Retain the crash destination now — a fault later may leave libc or
     * the heap unable to open files. */
    g_fault_f = fopen("/data/homebrew/PPSA99406/logs/fault.log", "ab");
}
#endif

/* An instruction the lifter could not translate, reached at run time.
 *
 * The lifter used to emit a bare abort() here, and that cost four wrong
 * diagnoses: MSVC's abort() calls __fastfail(FAST_FAIL_FATAL_APP_EXIT), which
 * raises 0xC0000409 - a status named STATUS_STACK_BUFFER_OVERRUN, past every
 * exception handler, with no message. So an unimplemented instruction looked
 * exactly like memory corruption, and the one thing it could not look like was
 * itself.
 *
 * Saying what it was, where it was, and how the program got there costs four
 * lines and removes the whole confusion. */
void lindbergh_todo(uint32_t va, const char *text)
{
    /* Implemented-on-demand x87 ops the lifter left as TODO. The FPU stack
     * lives on the CPU struct; the current guest CPU is g_current. */
    CPU *c = g_current;
    (void)c;
    /* A/B4: fyl2x/fyl2xp1/f2xm1 arm removed — the sites are lifted inline
     * (zero unimplemented hits), so this fallback is dead code. If a VA ever
     * does reach lindbergh_todo it now exits 44 instead of computing — that
     * makes a hidden lift gap visible instead of silently papering it. */
#if 0
    if (c) switch (va) {
    case 0x0877DC9Du: /* fyl2xp1: ST1 = ST1*log2(ST0+1); pop */
        { double x = fpop(c); *fst(c, 0) = *fst(c, 0) * log2(x + 1.0); return; }
    case 0x0877DCA1u: /* fyl2x:  ST1 = ST1*log2(ST0);   pop */
        { double x = fpop(c); *fst(c, 0) = *fst(c, 0) * log2(x);       return; }
    case 0x0877DCADu: /* f2xm1:  ST0 = 2^ST0 - 1 */
        *fst(c, 0) = exp2(*fst(c, 0)) - 1.0;                            return;
    }
#endif
    fprintf(stderr, "\n[recomp] unimplemented instruction at %#010x: %s\n", va, text);
    guest_report_state("unimplemented instruction");
    orb_exit(44);
}
