/* win_stubs.c - Windows equivalents of the Orbis/Linux-only services the
 * runtime references. Audio is silent (SEGAAPI stubbed), the guest heap uses
 * the CRT allocator, and exit paths map to the host exit. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

/* ---- process exit ---- */
int sceSystemServiceLoadExec(const char *path, char *const argv[])
{
    (void)path; (void)argv;
    exit(0);
    for (;;) { volatile int z = 0; (void)z; }
}

void orb_exit(int rc)
{
    exit(rc);
    for (;;) { volatile int z = 0; (void)z; }
}

/* ---- flexible memory query ---- */
int sceKernelAvailableFlexibleMemorySize(size_t *out)
{
    if (out) *out = (size_t)512u << 20;
    return 0;
}

/* ---- mspace heap: thin CRT-backed shim ----
 * sceLibcMspaceCreate returns an opaque arena handle; all traffic funnels
 * through MspaceMalloc/Free/PosixMemalign, so a sentinel + CRT malloc is a
 * correct (if unfragmented-tolerant) implementation. */
void *sceLibcMspaceCreate(const char *name, void *base, size_t size, unsigned flags)
{
    (void)name; (void)base; (void)size; (void)flags;
    return (void *)1;
}
void *sceLibcMspaceMalloc(void *msp, size_t size) { (void)msp; return malloc(size); }
void  sceLibcMspaceFree(void *msp, void *p)       { (void)msp; free(p); }
int   sceLibcMspacePosixMemalign(void *msp, void **pp, size_t align, size_t size)
{
    (void)msp;
    if (align < sizeof(void *)) align = sizeof(void *);
    void *p = _aligned_malloc(size, align);
    if (!p) return 12 /*ENOMEM*/;
    *pp = p;
    return 0;
}
void *sceLibcMspaceCalloc(void *msp, size_t n, size_t size)
{
    (void)msp;
    if (n && size > (size_t)-1 / n) return NULL;
    void *p = calloc(n, size);
    return p;
}
void *sceLibcMspaceRealloc(void *msp, void *p, size_t size)
{
    (void)msp;
    return realloc(p, size);
}
size_t sceLibcMspaceMallocUsableSize(void *msp, void *p)
{
    (void)msp; (void)p;
    return 0;
}
int sceLibcMspaceMallocStats(void *a, void *b)
{
    (void)a; (void)b;
    return 0;
}
void sceLibcMspaceDestroy(void *msp) { (void)msp; }

/* ---- SEGAAPI audio: silent stubs ----
 * The game creates buffers and streams ADX through this API. Returning
 * success with no audio lets every non-audio code path run; the mixer is a
 * later port (winmm waveOut or SDL). */
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

int      SEGAAPI_Init(void) { return 0; }
int      SEGAAPI_Exit(void) { return 0; }
int      SEGAAPI_Reset(void) { return 0; }
int      SEGAAPI_GetLastStatus(void) { return 0; }
int      SEGAAPI_CreateBuffer(HostBufferConfig *cfg, HostBufferCallback cb, unsigned flags, void **out)
{ (void)cfg; (void)cb; (void)flags; if (out) *out = calloc(1, 64); return 0; }
int      SEGAAPI_DestroyBuffer(void *h) { (void)h; return 0; }
int      SEGAAPI_Play(void *h) { (void)h; return 0; }
int      SEGAAPI_PlayWithSetup(void *h, unsigned a, HostSendRouteParamSet *b, unsigned c,
    HostSendLevelParamSet *d, unsigned e, HostVoiceParamSet *f, unsigned g, HostSynthParamSet *s)
{ (void)h;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)s; return 0; }
int      SEGAAPI_Pause(void *h) { (void)h; return 0; }
int      SEGAAPI_Stop(void *h) { (void)h; return 0; }
int      SEGAAPI_UpdateBuffer(void *h, unsigned off, unsigned len)
{ (void)h;(void)off;(void)len; return 0; }
int      SEGAAPI_GetPlaybackStatus(void *h) { (void)h; return 0; }
unsigned SEGAAPI_GetPlaybackPosition(void *h) { (void)h; return 0; }
int      SEGAAPI_SetPlaybackPosition(void *h, unsigned pos) { (void)h;(void)pos; return 0; }
int      SEGAAPI_GetSynthParam(void *h, int p) { (void)h;(void)p; return 0; }
int      SEGAAPI_SetSynthParam(void *h, int p, int v) { (void)h;(void)p;(void)v; return 0; }
int      SEGAAPI_SetSynthParamMultiple(void *h, unsigned n, HostSynthParamSet *set)
{ (void)h;(void)n;(void)set; return 0; }
void    *SEGAAPI_GetUserData(void *h) { (void)h; return NULL; }
int      SEGAAPI_SetUserData(void *h, void *d) { (void)h;(void)d; return 0; }
int      SEGAAPI_SetChannelVolume(void *h, unsigned ch, unsigned vol)
{ (void)h;(void)ch;(void)vol; return 0; }
int      SEGAAPI_SetIOVolume(int ch, unsigned vol) { (void)ch;(void)vol; return 0; }
int      SEGAAPI_SetSampleRate(void *h, unsigned rate) { (void)h;(void)rate; return 0; }
int      SEGAAPI_SetLoopState(void *h, int s) { (void)h;(void)s; return 0; }
int      SEGAAPI_SetReleaseState(void *h, int s) { (void)h;(void)s; return 0; }
int      SEGAAPI_SetStartLoopOffset(void *h, unsigned o) { (void)h;(void)o; return 0; }
int      SEGAAPI_SetEndLoopOffset(void *h, unsigned o) { (void)h;(void)o; return 0; }
int      SEGAAPI_SetEndOffset(void *h, unsigned o) { (void)h;(void)o; return 0; }
int      SEGAAPI_SetSendRouting(void *h, unsigned a, unsigned b, int c)
{ (void)h;(void)a;(void)b;(void)c; return 0; }
int      SEGAAPI_SetSendLevel(void *h, unsigned a, unsigned b, unsigned c)
{ (void)h;(void)a;(void)b;(void)c; return 0; }

/* ---- misc ---- */
int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }

int fseeko(FILE *f, long long off, int whence)
{
    return _fseeki64(f, off, whence);
}
long long ftello(FILE *f)
{
    return _ftelli64(f);
}

void hle_register_x11(void) { }
void hle_register_glx(void) { }

const char *g_gl_last_call = 0;
