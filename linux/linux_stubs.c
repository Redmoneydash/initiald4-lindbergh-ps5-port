/*
 * linux_stubs.c — stubs for symbols that only exist in Windows/PS5 builds.
 *
 * The runtime's hle_window.c, hle_gl.c, and hle.c reference a set of
 * platform-specific entry points that are provided by ps5-opengl / Win32 on
 * those targets.  On Linux we only need to get past the linker and into the
 * deserialization paths we want to test; nothing here needs to do real work.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lindbergh_rt.h"

/* ---- PS5 kernel memory queries ---- */

int64_t sceKernelAvailableDirectMemorySize(void) { return 0; }
int64_t sceKernelGetDirectMemorySize(void) { return 0; }
int sceKernelAvailableFlexibleMemorySize(size_t *out) { if (out) *out = 0; return 0; }
int sceKernelVirtualQuery(const void *a, int f, void *out, uint64_t sz)
    { (void)a; (void)f; (void)out; (void)sz; return -1; }
int _sceLibcGetMallocParam(void *out) { if (out) memset(out, 0, 64); return 0; }

/* ---- PS5 kernel log ----
 * sceKernelDebugOutText resolves to hle_libc's id4_dbg via the lindbergh_rt.h
 * macro — a separate stub here would duplicate it. */

/* ---- Orbis-only kernel/libc calls shared code can reach ---- */

int sceSystemServiceLoadExec(const char *path, char *const argv[])
{
    (void)path; (void)argv;
    fputs("[stub] sceSystemServiceLoadExec -> exit(0)\n", stderr);
    _exit(0);
}

/* Signatures match the extern declarations at the shared-code call sites
 * (guest.c, hle_libc.c, hle_pthread_orbis.c) — i386 cdecl arg widths. */
int sceLibcMspaceMallocStats(void *a, void *b)
{
    (void)a; (void)b;
    return 0;
}

int sceKernelAllocateDirectMemory(int64_t a, int64_t b, int64_t c,
                                  int64_t d, int32_t e, int64_t *phys)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (phys) *phys = -1;
    return -1;
}

int sceKernelMapDirectMemory(void **va, int64_t bytes, int prot, int flags,
                             int64_t phys, int64_t align)
{
    (void)va; (void)bytes; (void)prot; (void)flags; (void)phys; (void)align;
    return -1;
}

int sceKernelReleaseDirectMemory(int64_t phys, int64_t bytes)
{
    (void)phys; (void)bytes;
    return 0;
}

/* ---- SEGAAPI audio — real implementation lives in libsegaapi_linux.c ----
 * (ALSA backend, ported from the PS5 sceAudioOut mixer. Still gated behind
 * LINDBERGH_AUDIO=1 at the hle_libc call sites; default path is silent.) */
