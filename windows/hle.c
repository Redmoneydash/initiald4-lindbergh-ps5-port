/*
 * hle.c - the shared libraries the board provided.
 *
 * A Lindbergh game is dynamically linked: its calls to libc, libGL, libSDL,
 * ALSA and SEGA's own libsegaapi/libposixdrv all leave through the PLT, and
 * the lifter turns each of those into hle_call(c, HLE_<name>). There is no
 * ld.so here and no .so to load, so this is where those calls end up.
 *
 * An id with no implementation aborts naming itself. That is deliberate: a
 * silent no-op turns a missing import into a graphical glitch three hours
 * later, and the name printed here is the next thing to write.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lindbergh_rt.h"

#define HLE_NAME(id, name) name,
/* exit() issues SYS_exit — banned on managed Orbis titles (SIGSYS). The
 * sanctioned teardown is sceSystemServiceLoadExec("exit"). */
static void orb_exit(int rc) {
#ifdef _WIN32
    fflush(NULL);
    exit(rc);           /* keep the exit code visible to the caller */
#endif
    extern int sceSystemServiceLoadExec(const char *, char *const *);
    (void)rc;
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) { volatile int _z = 0; (void)_z; }
}

static const char *const g_names[] = { HLE_IMPORTS(HLE_NAME) };
#undef HLE_NAME

const char *hle_name(HleId id)
{
    return (unsigned)id < HLE_COUNT ? g_names[id] : "<bad id>";
}

HleHandler g_hle_handlers[HLE_COUNT];

int hle_bind(const char *name, HleHandler fn)
{
    for (unsigned i = 0; i < HLE_COUNT; i++)
        if (strcmp(g_names[i], name) == 0) { g_hle_handlers[i] = fn; return 1; }
    return 0;   /* this game does not import it - nothing to bind, not a fault */
}

void klogf(const char *fmt, ...)
{
    extern int sceKernelDebugOutText(int ch, const char *t);

    char b[384]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    /* The shader load spams ~250 files through the ring — the per-file
     * fopen/cg/smgr detail lines fill the 98 KB buffer before the post-init
     * scene/audio/frame markers get a chance. Cap each noisy prefix; the
     * milestones ([smgr] addShader/addvs/addps, [segapi], [host], faults) are
     * unthrottled so progress and crashes always show. */
    {
        static const char *const noisy[] = {
            "[fopen]", "[cg]", "[patch]", "[prog]", "[smgr] compileShader",
            "[smgr] vsinit", "[smgr] psinit", "[smgr] addps_cc",
            "[smgr] addvs_cc", "[smgr] getShaderByName", "[shutil]",
        };
        static unsigned seen[sizeof noisy / sizeof noisy[0]];
        for (unsigned i = 0; i < sizeof noisy / sizeof noisy[0]; i++)
            if (strncmp(b, noisy[i], strlen(noisy[i])) == 0) {
                if (++seen[i] > 60) return;
                break;
            }
    }
    sceKernelDebugOutText(0, b);
}


void hle_register_all(void)
{
    extern int sceKernelDebugOutText(int ch, const char *t);
#define KLOG(m) sceKernelDebugOutText(0, m)
    KLOG("[reg] libc\n"); hle_register_libc();
    KLOG("[reg] libc2\n"); hle_register_libc2();
    KLOG("[reg] io\n"); hle_register_io();
    KLOG("[reg] libc3\n"); hle_register_libc3();
    KLOG("[reg] pthread\n"); hle_register_pthread();
    KLOG("[reg] window\n"); hle_register_window();
    KLOG("[reg] x11\n"); { extern void hle_register_x11(void); hle_register_x11(); }
    KLOG("[reg] vidmode\n"); hle_register_vidmode();
    KLOG("[reg] matrix\n"); hle_register_matrix();
    KLOG("[reg] console\n"); hle_register_console();
    KLOG("[reg] sega\n"); hle_register_sega();
    KLOG("[reg] glut\n"); hle_register_glut();
    KLOG("[reg] cxx\n"); hle_register_cxx();
    KLOG("[reg] dir\n"); hle_register_dir();
    KLOG("[reg] scan\n"); hle_register_scan();
    KLOG("[reg] gl\n"); hle_register_gl();
    KLOG("[reg] glx\n"); { extern void hle_register_glx(void); hle_register_glx(); }
    KLOG("[reg] cg\n"); hle_register_cg();
    KLOG("[reg] all done\n");
}

/* Survey mode. Aborting on the first unimplemented import is the right default
 * - it stops at the first place the game went somewhere we cannot follow - but
 * during bring-up it means one rebuild per import, and a rebuild here is 80
 * translation units. With LINDBERGH_HLE_PERMISSIVE=1 an unbound import instead
 * reports itself once, returns 0, and lets the guest carry on, so a single run
 * enumerates everything the startup path reaches. The game is wrong from the
 * first such call - this is for finding the work, not for playing. */
static int permissive(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_HLE_PERMISSIVE");
        cached = (v && *v && *v != '0') ? 1
                : access("/app0/permissive", F_OK) == 0 ? 1 : 0;
    }
    return cached;
}

static unsigned char *g_seen;

/* Name every import as it is entered. Heavier than the survey - one line per
 * call, not per distinct import - but it is the only way to see what was
 * running when the process dies in a way no handler can catch, which
 * __fastfail (STATUS_STACK_BUFFER_OVERRUN) does by design. */
static int tracing(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_HLE_TRACE");
        cached = (v && *v && *v != '0') ? 1
                : access("/app0/hletrace", F_OK) == 0 ? 1 : 0;
    }
    return cached;
}

/* Stop deliberately at the Nth import call and report, because the failure
 * this was written for kills the process with __fastfail - which bypasses
 * every exception handler by design, so the crash reporter never runs. When a
 * failure is deterministic, arriving just before it is as good as catching it.
 *   LINDBERGH_HLE_BREAK=46211 */
static void maybe_break(CPU *c, HleId id)
{
    static long counter, at = -1;
    if (at < 0) {
        const char *v = getenv("LINDBERGH_HLE_BREAK");
        at = (v && *v) ? atol(v) : 0;
    }
    if (!at) return;
    if (++counter >= at) {
        fprintf(stderr, "[break] import call #%ld is %s\n", counter, hle_name(id));
        guest_report_state("deliberate break");
        orb_exit(42);
    }
}

/* Last import invoked — the crash dump prints it, since guest eip is only
 * function-granular and a fault inside an HLE handler otherwise looks like a
 * fault in the caller. Per-thread so audio/net worker threads don't clobber
 * what the main thread set, and nested handler calls restore the outer id. */
__thread int g_hle_current = -1;

void hle_call(CPU *c, HleId id)
{
    /* [hle] flood disabled — slows boot ~10x */
    maybe_break(c, id);
    /* [call] tracing disabled on console — the flood overran the klog ring and
     * hid the crash markers. Re-enable for local debugging only. */
    int prev = g_hle_current;
    g_hle_current = id;
    if ((unsigned)id < HLE_COUNT && g_hle_handlers[id]) {
        g_hle_handlers[id](c);
        g_hle_current = prev;
        return;
    }
    if (permissive()) {
        if (!g_seen) g_seen = (unsigned char *)calloc(HLE_COUNT, 1);
        if (g_seen && !g_seen[id]) {
            g_seen[id] = 1;
            fprintf(stderr, "[hle] %s\n", hle_name(id));
            fflush(stderr);
        }
        c->eax = 0;
        return;
    }
    fprintf(stderr,
            "[hle] %s() is not implemented.\n"
            "      Bind it:  hle_bind(\"%s\", my_%s);\n"
            "      Or set LINDBERGH_HLE_PERMISSIVE=1 to list everything the\n"
            "      startup path wants in one run.\n",
            hle_name(id), hle_name(id), hle_name(id));
    abort();
}
