/*
 * libsegaapi_ps5.c
 *
 * SEGAAPI implementation for PS5 using sceAudioOut.
 * Replaces the FAudio-backed libsegaapi.c from lindbergh-loader.
 *
 * Architecture:
 *   - One mixer thread that wakes at the sceAudioOut grain rate (~256 frames)
 *   - All active Buffers are software-mixed into a stereo float accumulator
 *   - Simple linear resampling handles source rates != 48000 Hz
 *   - Routing matrix is simplified to left/right gain per input channel
 *
 * sceAudioOut output: 48000 Hz, stereo, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>

int id4_dbg(int ch, const char *s);
#define sceKernelDebugOutText id4_dbg

/* PS5 sceAudioOut declarations (no header shipped with payload SDK) */
int sceAudioOutInit(void);
int sceAudioOutDeviceIdOpen(int user, int type, int idx, int grain, int freq, int param);
int sceAudioOutOpen(int user, int type, int idx, uint32_t grain, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutClose(int handle);
int sceAudioOutSetVolume(int handle, int flag, int *vol);

#define SCE_AUDIO_OUT_FORMAT_S16_STEREO 0x0001

/* -------------------------------------------------------------------------- */
/* SEGAAPI public types (from libsegaapi.h)                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    unsigned long  Data1;
    unsigned short Data2, Data3;
    unsigned char  Data4[8];
} GUID;

#define SEGARESULT_FAILURE(_x)  ((1u << 31) | 0xA000u | (_x))
#define SEGA_SUCCESS             0
#define SEGA_ERROR_FAIL          SEGARESULT_FAILURE(0)
#define SEGA_ERROR_BAD_POINTER   SEGARESULT_FAILURE(3)
#define SEGA_ERROR_BAD_PARAM     SEGARESULT_FAILURE(9)
#define SEGA_ERROR_INVALID_CHANNEL SEGARESULT_FAILURE(10)
#define SEGA_ERROR_INVALID_SEND  SEGARESULT_FAILURE(11)
#define SEGA_ERROR_BAD_CONFIG    SEGARESULT_FAILURE(14)
#define SEGA_ERROR_BAD_HANDLE    SEGARESULT_FAILURE(18)
#define SEGA_ERROR_OUT_OF_MEMORY SEGARESULT_FAILURE(31)

#define SYNTH_BUFFER     0x00000001u
#define ALLOC_USER_MEM   0x00000002u
#define USE_MAPPED_MEM   0x00000004u
#define UNSIGNED_8PCM    0x0004u
#define SIGNED_16PCM     0x0020u
#define VOL_MAX          0xFFFFFFFFu
#define P_MAXIMUM        0xFFFFFFFFu
#define UNUSED_SEND      0xFFFF0001u

typedef enum {
    UNUSED_PORT       = (int)UNUSED_SEND,
    FRONT_LEFT_PORT   = 0,
    FRONT_RIGHT_PORT  = 1,
    FRONT_CENTER_PORT = 2,
    LFE_PORT          = 3,
    REAR_LEFT_PORT    = 4,
    REAR_RIGHT_PORT   = 5,
    FXSLOT0_PORT      = 10,
    FXSLOT1_PORT      = 11,
    FXSLOT2_PORT      = 12,
    FXSLOT3_PORT      = 13,
} Routing;

typedef enum {
    PLAYBACK_STATUS_STOP    = 0,
    PLAYBACK_STATUS_ACTIVE  = 1,
    PLAYBACK_STATUS_PAUSE   = 2,
    PLAYBACK_STATUS_INVALID = -1,
} PlaybackStatus;

typedef struct {
    unsigned int dwSampleRate;
    unsigned int dwSampleFormat;
    unsigned int byNumChans;
} OutputFormat;

typedef struct {
    unsigned int dwSize;
    unsigned int dwOffset;
    void        *hBufferHdr;
} MapData;

typedef struct {
    unsigned int dwPriority;
    unsigned int dwSampleRate;
    unsigned int dwSampleFormat;
    unsigned int byNumChans;
    unsigned int dwReserved;
    void        *hUserData;
    MapData      mapData;
} BufferConfig;

typedef enum {
    RESOURCE_STOLEN = 0,
    NOTIFY          = 2,
} CallbackMessage;

typedef void (*BufferCallback)(void *hHandle, CallbackMessage message);

typedef enum {
    ATTENUATION = 0,
    PITCH,
    FILTER_CUTOFF,
    FILTER_Q,
    /* … rest unused by ID4, keep count correct */
    MOD_ENV_TO_FILTER_CUTOFF = 25,
} SynthParams;

typedef struct { SynthParams param; int lPARWValue; } SynthParamSet;
typedef struct { unsigned int dwChannel, dwSend; Routing dwDest; }  SendRouteParamSet;
typedef struct { unsigned int dwChannel, dwSend, dwLevel; }         SendLevelParamSet;
typedef enum {
    SET_START_LOOP_OFFSET   = 0x100,
    SET_END_LOOP_OFFSET,
    SET_END_OFFSET,
    SET_PLAY_POSITION,
    SET_LOOP_STATE,
    SET_NOTIFICATION_POINT,
    CLEAR_NOTIFICATION_POINT,
    SET_NOTIFICATION_FREQUENCY,
} VoiceIOControls;
typedef struct { VoiceIOControls VoiceIoctl; unsigned int dwParam1, dwParam2; } VoiceParamSet;

typedef enum {
    SPDIFOUT_44_1KHZ = 0,
    SPDIFOUT_48KHZ,
    SPDIFOUT_96KHZ,
} SPDIFOutputSampleRate;

typedef enum {
    OUT_FRONT_LEFT = 0,
    OUT_FRONT_RIGHT,
    OUT_FRONT_CENTER,
    OUT_LFE_PORT,
    OUT_REAR_LEFT,
    OUT_REAR_RIGHT,
    OUT_OPTICAL_LEFT = 10,
    OUT_OPTICAL_RIGHT,
    IN_LINEIN_LEFT  = 20,
    IN_LINEIN_RIGHT,
} SoundBoardIO;

/* EAX GUIDs — zeroed is fine, we only export the symbols */
GUID EAX_NULL_GUID                = {0};
GUID EAX_REVERB_EFFECT            = {0};
GUID EAX_AGCCOMPRESSOR_EFFECT     = {0};
GUID EAX_AUTOWAH_EFFECT           = {0};
GUID EAX_CHORUS_EFFECT            = {0};
GUID EAX_DISTORTION_EFFECT        = {0};
GUID EAX_ECHO_EFFECT              = {0};
GUID EAX_EQUALIZER_EFFECT         = {0};
GUID EAX_FLANGER_EFFECT           = {0};
GUID EAX_FREQUENCYSHIFTER_EFFECT  = {0};
GUID EAX_PITCHSHIFTER_EFFECT      = {0};
GUID EAX_RINGMODULATOR_EFFECT     = {0};
GUID EAX_VOCALMORPHER_EFFECT      = {0};
GUID EAXPROPERTYID_EAX40_FXSlot0  = {0};
GUID EAXPROPERTYID_EAX40_FXSlot1  = {0};
GUID EAXPROPERTYID_EAX40_FXSlot2  = {0};
GUID EAXPROPERTYID_EAX40_FXSlot3  = {0};

/* -------------------------------------------------------------------------- */
/* Internal Buffer                                                             */
/* -------------------------------------------------------------------------- */

#define MAX_CHANNELS 6
#define MAX_SENDS    13
#define OUT_RATE     48000
#define GRAIN        256   /* frames per sceAudioOut call */

typedef struct Buffer {
    /* audio data */
    uint8_t     *data;
    size_t       size;

    /* format */
    unsigned int sampleRate;
    unsigned int sampleFormat;  /* SIGNED_16PCM | UNSIGNED_8PCM */
    unsigned int numChans;

    /* playback state */
    PlaybackStatus playbackStatus;
    double       pos;           /* read position in source samples (float for resampling) */
    unsigned int startLoop;     /* bytes */
    unsigned int endLoop;       /* bytes */
    unsigned int endOffset;     /* bytes */
    int          bLooping;

    /* gain: computed from routing table */
    float        gainL;
    float        gainR;
    float        masterGain;    /* from ATTENUATION synth param */
    float        freqRatio;     /* from PITCH synth param */

    /* full routing tables (kept for Get* accessors) */
    Routing      routing[MAX_CHANNELS][MAX_SENDS];
    unsigned int sendLevels[MAX_CHANNELS][MAX_SENDS];
    unsigned int channelVolumes[MAX_CHANNELS];

    /* synth params */
    int          synthParams[MOD_ENV_TO_FILTER_CUTOFF + 1];

    /* user data / callback */
    void        *userData;
    BufferCallback callback;
    int          user_mem;      /* 1 if `data` is caller-supplied — do not free */

    /* linked list for mixer */
    struct Buffer *next;
} Buffer;

/* -------------------------------------------------------------------------- */
/* Globals                                                                     */
/* -------------------------------------------------------------------------- */

static int            s_audioHandle   = -1;
static pthread_t      s_mixThread;
static pthread_mutex_t s_mutex        = PTHREAD_MUTEX_INITIALIZER;
static Buffer        *s_bufList       = NULL;   /* head of all live buffers */
static volatile int   s_running       = 0;
static unsigned int   s_lastStatus    = 0;
static float          s_masterVolume  = 1.0f;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static inline unsigned int bytes_to_src_samples(Buffer *b, unsigned int bytes)
{
    unsigned int bps = (b->sampleFormat == SIGNED_16PCM) ? 2 : 1;
    return bytes / (b->numChans * bps);
}

static inline unsigned int src_samples_to_bytes(Buffer *b, unsigned int samples)
{
    unsigned int bps = (b->sampleFormat == SIGNED_16PCM) ? 2 : 1;
    return samples * b->numChans * bps;
}

/* Read one source sample from channel ch at integer index i, return as float */
static inline float read_sample(Buffer *b, unsigned int i, unsigned int ch)
{
    if (b->sampleFormat == SIGNED_16PCM) {
        const int16_t *p = (const int16_t *)b->data;
        return p[i * b->numChans + ch] * (1.0f / 32768.0f);
    } else {
        /* 8-bit unsigned PCM: 128 = silence */
        const uint8_t *p = b->data;
        return (p[i * b->numChans + ch] - 128) * (1.0f / 128.0f);
    }
}

/* Recompute gainL/gainR from routing tables */
static void recompute_gains(Buffer *b)
{
    float l = 0.0f, r = 0.0f;
    for (unsigned int ch = 0; ch < b->numChans; ch++) {
        float vol = b->channelVolumes[ch] / (float)VOL_MAX;
        for (int s = 0; s < MAX_SENDS; s++) {
            Routing dst = b->routing[ch][s];
            float lvl = b->sendLevels[ch][s] / (float)VOL_MAX;
            float v = vol * lvl;
            if (dst == FRONT_LEFT_PORT || dst == REAR_LEFT_PORT || dst == FRONT_CENTER_PORT)
                l += v;
            if (dst == FRONT_RIGHT_PORT || dst == REAR_RIGHT_PORT || dst == FRONT_CENTER_PORT)
                r += v;
            if (dst == LFE_PORT) { l += v * 0.5f; r += v * 0.5f; }
        }
    }
    b->gainL = l;
    b->gainR = r;
}

/* -------------------------------------------------------------------------- */
/* Mixer thread                                                                */
/* -------------------------------------------------------------------------- */

static void *mixer_thread(void *arg)
{
    (void)arg;

    /* Output buffer: GRAIN stereo S16 frames */
    int16_t out[GRAIN * 2];

    while (s_running) {
        float accL[GRAIN], accR[GRAIN];
        memset(accL, 0, sizeof(accL));
        memset(accR, 0, sizeof(accR));

        pthread_mutex_lock(&s_mutex);

        for (Buffer *b = s_bufList; b; b = b->next) {
            if (b->playbackStatus != PLAYBACK_STATUS_ACTIVE)
                continue;
            if (!b->data || b->size == 0)
                continue;

            unsigned int endSmp  = bytes_to_src_samples(b, b->endOffset ? b->endOffset : (unsigned)b->size);
            unsigned int loopEnd = bytes_to_src_samples(b, b->endLoop   ? b->endLoop   : (unsigned)b->size);
            unsigned int loopBeg = bytes_to_src_samples(b, b->startLoop);

            /* effective playback rate relative to output rate, adjusted by pitch */
            double step = (b->sampleRate * (double)b->freqRatio) / (double)OUT_RATE;

            for (int f = 0; f < GRAIN; f++) {
                unsigned int i0 = (unsigned int)b->pos;
                double frac     = b->pos - i0;

                /* bounds */
                if (i0 >= endSmp) {
                    if (b->bLooping && loopEnd > loopBeg) {
                        b->pos = loopBeg + fmod(b->pos - loopBeg, (double)(loopEnd - loopBeg));
                        i0   = (unsigned int)b->pos;
                        frac = b->pos - i0;
                    } else {
                        b->playbackStatus = PLAYBACK_STATUS_STOP;
                        b->pos = 0.0;
                        if (b->callback)
                            b->callback(b, NOTIFY);
                        break;
                    }
                }

                unsigned int i1 = i0 + 1;
                if (i1 >= endSmp)
                    i1 = i0;

                /* Mix channels to stereo with linear interp */
                for (unsigned int ch = 0; ch < b->numChans; ch++) {
                    float s0 = read_sample(b, i0, ch);
                    float s1 = read_sample(b, i1, ch);
                    float s  = s0 + (float)frac * (s1 - s0);
                    s *= b->masterGain;

                    if (b->numChans == 1) {
                        accL[f] += s * b->gainL;
                        accR[f] += s * b->gainR;
                    } else {
                        if (ch == 0) accL[f] += s * b->gainL;
                        else         accR[f] += s * b->gainR;
                    }
                }

                b->pos += step;

                /* loop wrap */
                if (b->bLooping && loopEnd > loopBeg && (unsigned int)b->pos >= loopEnd)
                    b->pos = loopBeg + fmod(b->pos - loopBeg, (double)(loopEnd - loopBeg));
            }
        }

        pthread_mutex_unlock(&s_mutex);

        /* Convert to S16 and apply master volume */
        int peak = 0;
        for (int f = 0; f < GRAIN; f++) {
            float l = accL[f] * s_masterVolume;
            float r = accR[f] * s_masterVolume;
            if (l >  1.0f) l =  1.0f;
            if (l < -1.0f) l = -1.0f;
            if (r >  1.0f) r =  1.0f;
            if (r < -1.0f) r = -1.0f;
            out[f * 2 + 0] = (int16_t)(l * 32767.0f);
            out[f * 2 + 1] = (int16_t)(r * 32767.0f);
            int a = out[f * 2] < 0 ? -out[f * 2] : out[f * 2];
            if (a > peak) peak = a;
            a = out[f * 2 + 1] < 0 ? -out[f * 2 + 1] : out[f * 2 + 1];
            if (a > peak) peak = a;
        }

        /* Amplitude heartbeat — every ~4s report live-buffer count, how many
         * are ACTIVE, and the peak sample pushed to sceAudioOut. Distinguishes
         * "game never plays" from "plays silence" from "real audio out". */
        {
            extern int sceKernelDebugOutText(int, const char*);
            static int beat, ever;
            int nbuf = 0, nact = 0;
            Buffer *first_act = NULL;
            pthread_mutex_lock(&s_mutex);
            for (Buffer *b = s_bufList; b; b = b->next) {
                nbuf++;
                if (b->playbackStatus == PLAYBACK_STATUS_ACTIVE) { nact++; if (!first_act) first_act = b; }
            }
            /* Sample the first active buffer's live content mid-play: nonzero
             * ratio + first samples at current pos. Distinguishes "decode
             * never writes" (zeros) from "decode writes garbage" (noise) from
             * "data is fine, mixer wrong" (smooth PCM). */
            int nz = -1; long dmax = -1; unsigned pos = 0;
            static double lstep, lfreq;
            static unsigned lsrate;
            unsigned short w[8] = {0};
            static unsigned short fw[4]; static unsigned fbeat;
            if (first_act && first_act->data && first_act->size >= 0x100) {
                const int16_t *s = (const int16_t *)first_act->data;
                int n = (int)(first_act->size / 2);
                nz = 0; dmax = 0;
                for (int i = 1; i < n && i < 8192; i++) {
                    long d = (long)s[i] - s[i-1]; if (d < 0) d = -d;
                    if (d > dmax) dmax = d;
                    if (s[i]) nz++;
                }
                pos = (unsigned)first_act->pos;
                lsrate = first_act->sampleRate;
                lfreq = first_act->freqRatio;
                lstep = (first_act->sampleRate * first_act->freqRatio) / (double)OUT_RATE;
                int i0 = (int)pos * first_act->numChans;
                if (i0 + 8 <= n)
                    for (int k = 0; k < 8; k++) w[k] = (unsigned short)s[i0 + k];
                /* fixed-offset liveness probe: s[0x3000..0x3003]. If these
                 * change between beats the guest writes the ring directly;
                 * if static, ring content only moves on UpdateBuffer. */
                if (n > 0x3004) {
                    unsigned nf = 0;
                    for (int k = 0; k < 4; k++)
                        if ((unsigned short)s[0x3000 + k] != fw[k]) nf = 1;
                    for (int k = 0; k < 4; k++) fw[k] = (unsigned short)s[0x3000 + k];
                    if (nf) fbeat++;
                }
            }
            pthread_mutex_unlock(&s_mutex);
            if (peak) ever = 1;
            if (++beat >= 750) {
                char mb[224];
                snprintf(mb, sizeof mb,
                    "[segaapi] mix: bufs=%d active=%d peak=%d ever=%d nz=%d dmax=%ld pos=%u fb=%u sr=%u fr=%.3f step=%.4f\n  w=%04x %04x %04x %04x %04x %04x %04x %04x f=%04x %04x %04x %04x\n",
                    nbuf, nact, peak, ever, nz, dmax, pos, fbeat, lsrate, lfreq, lstep,
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                    fw[0], fw[1], fw[2], fw[3]);
                sceKernelDebugOutText(0, mb);
                beat = 0;
            }
        }

        sceAudioOutOutput(s_audioHandle, out);
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* SEGAAPI_Init / Exit                                                         */
/* -------------------------------------------------------------------------- */

int SEGAAPI_Init(void)
{
    extern int sceKernelDebugOutText(int, const char*);
    sceKernelDebugOutText(0, "[segaapi] Init: sceAudioOutInit\n");
    int r0 = sceAudioOutInit();
    { char b[64]; snprintf(b,sizeof b,"[segaapi] Init: outinit=%d, devopen\n", r0); sceKernelDebugOutText(0,b); }

    /* user=0xff (system), type=0 (main), idx=0 — sceAudioOutOpen, the call
     * prospero-win proves works. sceAudioOutDeviceIdOpen takes a device id we
     * do not have and returns a device handle, not a playable port. */
    s_audioHandle = sceAudioOutOpen(
        0xff, 0, 0, GRAIN, OUT_RATE, SCE_AUDIO_OUT_FORMAT_S16_STEREO);
    { char b[64]; snprintf(b,sizeof b,"[segaapi] Init: handle=%d\n", s_audioHandle); sceKernelDebugOutText(0,b); }

    if (s_audioHandle < 0)
        return SEGA_ERROR_FAIL;

    /* 0 dB on all channels */
    { int vol[8]; for (int i = 0; i < 8; i++) vol[i] = 0x8000;
      sceAudioOutSetVolume(s_audioHandle, 3, vol); }

    s_running = 1;
    int pr = pthread_create(&s_mixThread, NULL, mixer_thread, NULL);
    { struct sched_param sp = { .sched_priority = 700 };
      /* sched_setscheduler is banned under the managed-app seccomp →
       * SIGSYS. Normal-priority mixing works; skip the RT bump. */ }
    { char b[64]; snprintf(b,sizeof b,"[segaapi] Init: pcreate=%d\n", pr); sceKernelDebugOutText(0,b); }

    return SEGA_SUCCESS;
}

int SEGAAPI_Reset(void) { return SEGA_SUCCESS; }

int SEGAAPI_Exit(void)
{
    s_running = 0;
    pthread_join(s_mixThread, NULL);

    if (s_audioHandle >= 0) {
        sceAudioOutClose(s_audioHandle);
        s_audioHandle = -1;
    }
    return SEGA_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Buffer lifecycle                                                            */
/* -------------------------------------------------------------------------- */

int SEGAAPI_CreateBuffer(BufferConfig *pConfig, BufferCallback pCallback,
                         unsigned int dwFlags, void **phHandle)
{
    if (!pConfig || !phHandle)
        return SEGA_ERROR_BAD_CONFIG;

    Buffer *b = (Buffer *)calloc(1, sizeof(Buffer));
    if (!b) return SEGA_ERROR_OUT_OF_MEMORY;

    b->sampleRate   = pConfig->dwSampleRate;
    b->sampleFormat = pConfig->dwSampleFormat;
    b->numChans     = pConfig->byNumChans;
    b->size         = pConfig->mapData.dwSize;
    b->userData     = pConfig->hUserData;
    b->callback     = pCallback;
    b->masterGain   = 1.0f;
    b->freqRatio    = 1.0f;
    b->endOffset    = (unsigned int)b->size;
    b->endLoop      = (unsigned int)b->size;
    b->playbackStatus = PLAYBACK_STATUS_STOP;

    if (dwFlags & (ALLOC_USER_MEM | USE_MAPPED_MEM)) {
        b->data     = (uint8_t *)pConfig->mapData.hBufferHdr;
        b->user_mem = 1;
    } else {
        /* Big stream buffers go to the direct-memory pool — flexible memory
         * is a small per-process budget the shader compiler and the driver
         * share; 24 MB stream buffers in flex starved every later malloc. */
        if (b->size >= (1u << 20)) {
            extern int sceKernelAllocateDirectMemory(int64_t, int64_t, int64_t,
                                                     int64_t, int32_t, int64_t *);
            extern int sceKernelMapDirectMemory(void **, int64_t, int, int,
                                                int64_t, int64_t);
            int64_t phys = -1; void *va = NULL; int ok = 0;
            static const int32_t mtypes[] = {12, 0, 3};
            extern int64_t sceKernelGetDirectMemorySize(void);
            static int64_t dlim; if (!dlim) dlim = sceKernelGetDirectMemorySize();
            for (unsigned t = 0; t < 3; t++)
                if (sceKernelAllocateDirectMemory(0, dlim, (int64_t)b->size, 0x4000,
                                                  mtypes[t], &phys) == 0) { ok = 1; break; }
            if (ok && sceKernelMapDirectMemory(&va, (int64_t)b->size, 0x33, 0x0,
                                               phys, 0x4000) == 0 && va) {
                b->data = (uint8_t *)va;
            }
        }
        if (!b->data)
            b->data = (uint8_t *)calloc(1, b->size);
        if (!b->data) { free(b); return SEGA_ERROR_OUT_OF_MEMORY; }
    }
    pConfig->mapData.hBufferHdr = b->data;
    pConfig->mapData.dwOffset   = 0;

    /* default routing: mono → L+R, stereo → L/R */
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        b->channelVolumes[ch] = VOL_MAX;
        for (int s = 0; s < MAX_SENDS; s++) {
            b->routing[ch][s]    = UNUSED_PORT;
            b->sendLevels[ch][s] = 0;
        }
    }
    if (pConfig->byNumChans == 1) {
        b->routing[0][0]    = FRONT_LEFT_PORT;
        b->routing[0][1]    = FRONT_RIGHT_PORT;
        b->sendLevels[0][0] = VOL_MAX;
        b->sendLevels[0][1] = VOL_MAX;
    } else if (pConfig->byNumChans >= 2) {
        b->routing[0][0]    = FRONT_LEFT_PORT;
        b->routing[1][0]    = FRONT_RIGHT_PORT;
        b->sendLevels[0][0] = VOL_MAX;
        b->sendLevels[1][0] = VOL_MAX;
    }
    recompute_gains(b);

    pthread_mutex_lock(&s_mutex);
    b->next   = s_bufList;
    s_bufList = b;
    pthread_mutex_unlock(&s_mutex);

    *phHandle = b;
    return SEGA_SUCCESS;
}

int SEGAAPI_DestroyBuffer(void *hHandle)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;

    pthread_mutex_lock(&s_mutex);
    Buffer **pp = &s_bufList;
    while (*pp && *pp != b) pp = &(*pp)->next;
    if (*pp) *pp = b->next;
    pthread_mutex_unlock(&s_mutex);

    /* Caller-supplied buffers (USE_MAPPED_MEM) are owned by the guest —
     * freeing them is a double-free on guest_brk memory. */
    if (!b->user_mem)
        free(b->data);
    free(b);
    return SEGA_SUCCESS;
}

int SEGAAPI_UpdateBuffer(void *hHandle, unsigned int dwStartOffset, unsigned int dwLength)
{
    /* Streaming voices pass a real (off,len) ring region — a write-cursor
     * notification, NOT a restart. Resetting pos here yanked the play cursor
     * back to startLoop on every refill → stutter/bursts. The mapped memory is
     * shared, so there is nothing to do but acknowledge. */
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;
    /* Freshness hash: the ring never appeared to change under ps5debug while
     * UpdateBuffer kept streaming — is the committed region actually being
     * rewritten, or does the guest write somewhere else entirely? Hash the
     * region now and remember per-cursor-slot hashes; when the same offset
     * recurs with a different hash the guest truly wrote it. */
    if (b->data && b->size && dwStartOffset < b->size) {
        unsigned int len = dwLength;
        if (len > b->size - dwStartOffset) len = b->size - dwStartOffset;
        const uint8_t *p = b->data + dwStartOffset;
        uint32_t h = 5381;
        for (unsigned int i = 0; i < len; i += 16)
            h = h * 33 + p[i];
        static struct { unsigned int off; uint32_t h; } s_hist[8];
        static int s_hi;
        int seen = -1;
        for (int i = 0; i < 8; i++)
            if (s_hist[i].off == dwStartOffset) { seen = i; break; }
        extern int sceKernelDebugOutText(int, const char*);
        char msg[160];
        int chg = (seen >= 0 && s_hist[seen].h != h);
        if (seen >= 0) s_hist[seen].h = h; else { s_hist[s_hi].off = dwStartOffset; s_hist[s_hi].h = h; s_hi = (s_hi + 1) & 7; }
        snprintf(msg, sizeof msg,
                 "[segapi] upb h=%p off=%#x len=%#x hash=%08x %s pos=%d\n",
                 hHandle, dwStartOffset, dwLength, h, chg ? "FRESH" : "same",
                 (int)bytes_to_src_samples(b, (unsigned)b->pos));
        static int s_upb_n;
        if (s_upb_n++ < 20)
            sceKernelDebugOutText(2, msg);
    }
    return SEGA_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Playback control                                                            */
/* -------------------------------------------------------------------------- */

int SEGAAPI_Play(void *hHandle)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;
    /* Audio content probe: dump what the guest actually wrote — real PCM has
     * small sample-to-sample deltas; ADX-compressed data is near-random. */
    {
        extern int sceKernelDebugOutText(int, const char*);
        static int np;
        if (np++ < 24 && b->data && b->size >= 0x40) {
            const int16_t *s = (const int16_t *)b->data;
            long dmax = 0; int nz = 0;
            for (int i = 1; i < 64; i++) {
                long d = (long)s[i] - s[i-1]; if (d < 0) d = -d;
                if (d > dmax) dmax = d;
                if (s[i]) nz++;
            }
            char b2[256];
            snprintf(b2, sizeof b2,
                "[segaapi] play rate=%u fmt=%x ch=%u sz=%x dmax=%ld nz=%d\n  s=%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x\n",
                b->sampleRate, b->sampleFormat, b->numChans, b->size,
                dmax, nz,
                (unsigned short)s[0], (unsigned short)s[1], (unsigned short)s[2], (unsigned short)s[3],
                (unsigned short)s[4], (unsigned short)s[5], (unsigned short)s[6], (unsigned short)s[7],
                (unsigned short)s[8], (unsigned short)s[9], (unsigned short)s[10], (unsigned short)s[11],
                (unsigned short)s[12], (unsigned short)s[13], (unsigned short)s[14], (unsigned short)s[15]);
            sceKernelDebugOutText(0, b2);
        }
    }
    pthread_mutex_lock(&s_mutex);
    b->playbackStatus = PLAYBACK_STATUS_ACTIVE;
    b->pos = (double)bytes_to_src_samples(b, b->startLoop); /* Play -> submit fresh, like lindbergh */
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

int SEGAAPI_Pause(void *hHandle)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;
    pthread_mutex_lock(&s_mutex);
    b->playbackStatus = PLAYBACK_STATUS_PAUSE;
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

int SEGAAPI_Stop(void *hHandle)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;
    pthread_mutex_lock(&s_mutex);
    b->playbackStatus = PLAYBACK_STATUS_STOP;
    b->pos = 0.0;
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

/* forward declarations for PlayWithSetup */
int SEGAAPI_SetSendRouting(void *, unsigned int, unsigned int, Routing);
int SEGAAPI_SetSendLevel(void *, unsigned int, unsigned int, unsigned int);
int SEGAAPI_SetStartLoopOffset(void *, unsigned int);
int SEGAAPI_SetEndLoopOffset(void *, unsigned int);
int SEGAAPI_SetEndOffset(void *, unsigned int);
int SEGAAPI_SetPlaybackPosition(void *, unsigned int);
int SEGAAPI_SetLoopState(void *, int);
int SEGAAPI_SetSynthParam(void *, SynthParams, int);

int SEGAAPI_PlayWithSetup(void *hHandle,
    unsigned int nRoute, SendRouteParamSet *pRoute,
    unsigned int nLevel, SendLevelParamSet *pLevel,
    unsigned int nVoice, VoiceParamSet     *pVoice,
    unsigned int nSynth, SynthParamSet     *pSynth)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    for (unsigned int i = 0; i < nRoute; i++)
        SEGAAPI_SetSendRouting(hHandle, pRoute[i].dwChannel, pRoute[i].dwSend, pRoute[i].dwDest);
    for (unsigned int i = 0; i < nLevel; i++)
        SEGAAPI_SetSendLevel(hHandle, pLevel[i].dwChannel, pLevel[i].dwSend, pLevel[i].dwLevel);
    for (unsigned int i = 0; i < nVoice; i++) {
        switch (pVoice[i].VoiceIoctl) {
        case SET_START_LOOP_OFFSET:      SEGAAPI_SetStartLoopOffset(hHandle, pVoice[i].dwParam1); break;
        case SET_END_LOOP_OFFSET:        SEGAAPI_SetEndLoopOffset(hHandle, pVoice[i].dwParam1);   break;
        case SET_END_OFFSET:             SEGAAPI_SetEndOffset(hHandle, pVoice[i].dwParam1);        break;
        case SET_PLAY_POSITION:          SEGAAPI_SetPlaybackPosition(hHandle, pVoice[i].dwParam1); break;
        case SET_LOOP_STATE:             SEGAAPI_SetLoopState(hHandle, pVoice[i].dwParam1);        break;
        default: break;
        }
    }
    for (unsigned int i = 0; i < nSynth; i++)
        SEGAAPI_SetSynthParam(hHandle, pSynth[i].param, pSynth[i].lPARWValue);
    return SEGAAPI_Play(hHandle);
}

PlaybackStatus SEGAAPI_GetPlaybackStatus(void *hHandle)
{
    if (!hHandle) return PLAYBACK_STATUS_INVALID;
    return ((Buffer *)hHandle)->playbackStatus;
}

/* -------------------------------------------------------------------------- */
/* Format / rate                                                               */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetFormat(void *hHandle, OutputFormat *pFormat)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    if (!pFormat) return SEGA_ERROR_BAD_CONFIG;
    Buffer *b = (Buffer *)hHandle;
    pthread_mutex_lock(&s_mutex);
    b->sampleRate   = pFormat->dwSampleRate;
    b->sampleFormat = pFormat->dwSampleFormat;
    b->numChans     = pFormat->byNumChans;
    recompute_gains(b);
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

int SEGAAPI_GetFormat(void *hHandle, OutputFormat *pFormat)
{
    if (!hHandle || !pFormat) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;
    pFormat->dwSampleRate   = b->sampleRate;
    pFormat->dwSampleFormat = b->sampleFormat;
    pFormat->byNumChans     = b->numChans;
    return SEGA_SUCCESS;
}

int SEGAAPI_SetSampleRate(void *hHandle, unsigned int rate)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    ((Buffer *)hHandle)->sampleRate = rate;
    return SEGA_SUCCESS;
}

unsigned int SEGAAPI_GetSampleRate(void *hHandle)
{
    if (!hHandle) return 0;
    return ((Buffer *)hHandle)->sampleRate;
}

/* -------------------------------------------------------------------------- */
/* Priority (no-op — unlimited voices)                                        */
/* -------------------------------------------------------------------------- */

int          SEGAAPI_SetPriority(void *h, unsigned int p) { (void)h; (void)p; return SEGA_SUCCESS; }
unsigned int SEGAAPI_GetPriority(void *h)                 { (void)h; return P_MAXIMUM; }

/* -------------------------------------------------------------------------- */
/* User data                                                                   */
/* -------------------------------------------------------------------------- */

int   SEGAAPI_SetUserData(void *hHandle, void *p)
{ if (!hHandle) return SEGA_ERROR_BAD_HANDLE; ((Buffer*)hHandle)->userData = p; return SEGA_SUCCESS; }
void *SEGAAPI_GetUserData(void *hHandle)
{ if (!hHandle) return NULL; return ((Buffer*)hHandle)->userData; }

/* -------------------------------------------------------------------------- */
/* Routing                                                                     */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetSendRouting(void *hHandle, unsigned int ch, unsigned int snd, Routing dst)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    if (ch >= MAX_CHANNELS) return SEGA_ERROR_INVALID_CHANNEL;
    if (snd >= MAX_SENDS)   return SEGA_ERROR_BAD_PARAM;
    Buffer *b = (Buffer *)hHandle;
    pthread_mutex_lock(&s_mutex);
    b->routing[ch][snd] = dst;
    recompute_gains(b);
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

Routing SEGAAPI_GetSendRouting(void *hHandle, unsigned int ch, unsigned int snd)
{
    if (!hHandle || ch >= MAX_CHANNELS || snd >= MAX_SENDS) return UNUSED_PORT;
    return ((Buffer *)hHandle)->routing[ch][snd];
}

int SEGAAPI_SetSendLevel(void *hHandle, unsigned int ch, unsigned int snd, unsigned int level)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    if (ch >= MAX_CHANNELS) return SEGA_ERROR_INVALID_CHANNEL;
    if (snd >= MAX_SENDS)   return SEGA_ERROR_BAD_PARAM;
    Buffer *b = (Buffer *)hHandle;
    pthread_mutex_lock(&s_mutex);
    b->sendLevels[ch][snd] = level;
    recompute_gains(b);
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

unsigned int SEGAAPI_GetSendLevel(void *hHandle, unsigned int ch, unsigned int snd)
{
    if (!hHandle || ch >= MAX_CHANNELS || snd >= MAX_SENDS) return 0;
    return ((Buffer *)hHandle)->sendLevels[ch][snd];
}

int SEGAAPI_SetChannelVolume(void *hHandle, unsigned int ch, unsigned int vol)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    if (ch >= MAX_CHANNELS) return SEGA_ERROR_INVALID_CHANNEL;
    Buffer *b = (Buffer *)hHandle;
    pthread_mutex_lock(&s_mutex);
    b->channelVolumes[ch] = vol;
    recompute_gains(b);
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

unsigned int SEGAAPI_GetChannelVolume(void *hHandle, unsigned int ch)
{
    if (!hHandle || ch >= MAX_CHANNELS) return 0;
    return ((Buffer *)hHandle)->channelVolumes[ch];
}

/* -------------------------------------------------------------------------- */
/* Playback position                                                           */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetPlaybackPosition(void *hHandle, unsigned int bytePos)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;
    /* Retrofan test: SetPlaybackPosition(0) on a stopped mono looping buffer
     * corrupts first-play state — lindbergh-loader skips it entirely. */
    if (bytePos == 0 && b->bLooping && b->numChans == 1
        && b->playbackStatus == PLAYBACK_STATUS_STOP)
        return SEGA_SUCCESS;
    pthread_mutex_lock(&s_mutex);
    unsigned int bps = (b->sampleFormat == SIGNED_16PCM) ? 2 : 1;
    unsigned int div = b->numChans * bps;
    b->pos = div ? (double)(bytePos / div) : 0.0;
    pthread_mutex_unlock(&s_mutex);
    return SEGA_SUCCESS;
}

unsigned int SEGAAPI_GetPlaybackPosition(void *hHandle)
{
    if (!hHandle) return 0;
    Buffer *b = (Buffer *)hHandle;
    unsigned int bps = (b->sampleFormat == SIGNED_16PCM) ? 2 : 1;
    return (unsigned int)(b->pos) * b->numChans * bps;
}

/* -------------------------------------------------------------------------- */
/* Loop points                                                                 */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetStartLoopOffset(void *hHandle, unsigned int v)
{ if (!hHandle) return SEGA_ERROR_BAD_HANDLE; ((Buffer*)hHandle)->startLoop = v; return SEGA_SUCCESS; }
unsigned int SEGAAPI_GetStartLoopOffset(void *hHandle)
{ if (!hHandle) return 0; return ((Buffer*)hHandle)->startLoop; }

int SEGAAPI_SetEndLoopOffset(void *hHandle, unsigned int v)
{ if (!hHandle) return SEGA_ERROR_BAD_HANDLE; ((Buffer*)hHandle)->endLoop = v; return SEGA_SUCCESS; }
unsigned int SEGAAPI_GetEndLoopOffset(void *hHandle)
{ if (!hHandle) return 0; return ((Buffer*)hHandle)->endLoop; }

int SEGAAPI_SetEndOffset(void *hHandle, unsigned int v)
{ if (!hHandle) return SEGA_ERROR_BAD_HANDLE; ((Buffer*)hHandle)->endOffset = v; return SEGA_SUCCESS; }
unsigned int SEGAAPI_GetEndOffset(void *hHandle)
{ if (!hHandle) return 0; return ((Buffer*)hHandle)->endOffset; }

int SEGAAPI_SetLoopState(void *hHandle, int loop)
{ if (!hHandle) return SEGA_ERROR_BAD_HANDLE; ((Buffer*)hHandle)->bLooping = loop; return SEGA_SUCCESS; }
int SEGAAPI_GetLoopState(void *hHandle)
{ if (!hHandle) return 0; return ((Buffer*)hHandle)->bLooping; }

/* -------------------------------------------------------------------------- */
/* Synth params (ATTENUATION and PITCH used by ID4)                           */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetSynthParam(void *hHandle, SynthParams param, int val)
{
    if (!hHandle) return SEGA_ERROR_BAD_HANDLE;
    Buffer *b = (Buffer *)hHandle;
    if ((unsigned)param > MOD_ENV_TO_FILTER_CUTOFF) return SEGA_ERROR_BAD_PARAM;
    b->synthParams[param] = val;
    if (param == ATTENUATION) {
        float db = 0.0f - val / 10.0f;
        b->masterGain = db > -100.0f ? powf(10.0f, db * 0.05f) : 0.0f;
    } else if (param == PITCH) {
        b->freqRatio = powf(2.0f, (val / 100.0f) / 12.0f);
    }
    return SEGA_SUCCESS;
}

int SEGAAPI_GetSynthParam(void *hHandle, SynthParams param)
{
    if (!hHandle) return 0;
    if ((unsigned)param > MOD_ENV_TO_FILTER_CUTOFF) return 0;
    return ((Buffer *)hHandle)->synthParams[param];
}

int SEGAAPI_SetSynthParamMultiple(void *hHandle, unsigned int n, SynthParamSet *p)
{
    for (unsigned int i = 0; i < n; i++)
        SEGAAPI_SetSynthParam(hHandle, p[i].param, p[i].lPARWValue);
    return SEGA_SUCCESS;
}

int SEGAAPI_GetSynthParamMultiple(void *hHandle, unsigned int n, SynthParamSet *p)
{
    for (unsigned int i = 0; i < n; i++)
        p[i].lPARWValue = SEGAAPI_GetSynthParam(hHandle, p[i].param);
    return SEGA_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Release state                                                               */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetReleaseState(void *hHandle, int bSet)
{
    if (!bSet) return SEGA_SUCCESS;
    return SEGAAPI_Stop(hHandle);
}

/* -------------------------------------------------------------------------- */
/* Notification (not used by ID4)                                             */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetNotificationFrequency(void *h, unsigned int f) { (void)h;(void)f; return SEGA_SUCCESS; }
int SEGAAPI_SetNotificationPoint(void *h, unsigned int o)     { (void)h;(void)o; return SEGA_SUCCESS; }
int SEGAAPI_ClearNotificationPoint(void *h, unsigned int o)   { (void)h;(void)o; return SEGA_SUCCESS; }

/* -------------------------------------------------------------------------- */
/* Master volume (SetIOVolume)                                                 */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetIOVolume(SoundBoardIO port, unsigned int vol)
{
    (void)port;
    s_masterVolume = vol / (float)VOL_MAX;
    return SEGA_SUCCESS;
}

unsigned int SEGAAPI_GetIOVolume(SoundBoardIO port)
{
    (void)port;
    return (unsigned int)(s_masterVolume * VOL_MAX);
}

/* -------------------------------------------------------------------------- */
/* SPDIF / EAX stubs (not applicable on PS5)                                  */
/* -------------------------------------------------------------------------- */

int SEGAAPI_SetGlobalEAXProperty(GUID *g, unsigned long p, void *d, unsigned long s)
{ (void)g;(void)p;(void)d;(void)s; return SEGA_SUCCESS; }
int SEGAAPI_GetGlobalEAXProperty(GUID *g, unsigned long p, void *d, unsigned long s)
{ (void)g;(void)p;(void)d;(void)s; return SEGA_SUCCESS; }

int SEGAAPI_SetSPDIFOutChannelStatus(unsigned int a, unsigned int b) { (void)a;(void)b; return SEGA_SUCCESS; }
int SEGAAPI_GetSPDIFOutChannelStatus(unsigned int *a, unsigned int *b)
{ if(a)*a=0; if(b)*b=0; return SEGA_SUCCESS; }
int SEGAAPI_SetSPDIFOutSampleRate(SPDIFOutputSampleRate r) { (void)r; return SEGA_SUCCESS; }
SPDIFOutputSampleRate SEGAAPI_GetSPDIFOutSampleRate(void) { return SPDIFOUT_48KHZ; }
int SEGAAPI_SetSPDIFOutChannelRouting(unsigned int ch, Routing src) { (void)ch;(void)src; return SEGA_SUCCESS; }
Routing SEGAAPI_GetSPDIFOutChannelRouting(unsigned int ch) { (void)ch; return UNUSED_PORT; }

/* -------------------------------------------------------------------------- */
/* Status                                                                      */
/* -------------------------------------------------------------------------- */

void SEGAAPI_SetLastStatus(int v) { s_lastStatus = (unsigned)v; }
int  SEGAAPI_GetLastStatus(void)  { return (int)s_lastStatus; }
