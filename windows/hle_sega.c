/*
 * hle_sega.c - the cabinet's own library, answered from here.
 *
 * A Lindbergh game does not talk to its hardware directly. It calls SEGA's
 * amLib: a C API, statically linked into this binary, that reaches a base
 * board over a serial link - the JVS I/O board with the coin slots, buttons
 * and guns on it, the EEPROM holding the bookkeeping, the security dongle.
 * None of that exists here, so every one of those calls fails and the game
 * stops on "Error 11 - JVS I/O board is not connected to main board" before
 * it ever reaches its attract mode.
 *
 * These are plain C symbols, so they override by name the same way the
 * XF86VidMode entry points do. What is answered here is only what the game
 * actually asks on the way to attract mode; the game narrates the rest
 * through _sDebug::putConsole (LINDBERGH_CONSOLE=1), which is how each of
 * these was found rather than guessed.
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lindbergh_rt.h"

/* The library's own success code. Every am* function returns 0 for success
 * and a negative value for a failure the game then reports. */
#define AM_OK 0

/* The JVS packet the game handed to the transport.
 *
 * Request layout, read off amJvspClearPacket and amJvsSendRequest: byte 0 is
 * the node the packet is addressed to, byte 1 the length of the frame, and
 * bytes 2.. the frame itself. The acknowledge comes back in the same object
 * at byte 0x101 (length) and 0x102.. (frame).
 *
 * LINDBERGH_JVS=1 prints what is asked, which is the only honest way to find
 * out what this board has to be able to answer. */
static void jvs_dump(const char *tag, uint32_t pkt, unsigned off, unsigned len)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)pkt;
    char line[512];
    int at = snprintf(line, sizeof line, "[jvs] %s node %u len %u:", tag, p[0], len);
    for (unsigned i = 0; i < len && i < 40 && at < 470; i++)
        at += snprintf(line + at, sizeof line - at, " %02X", p[off + i]);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

/* ---- the I/O board itself ----
 *
 * JVS is a small request/response protocol over a serial line. A frame is
 *
 *     E0 <dest> <count> <payload...> <sum>
 *
 * where count covers everything after itself including the checksum, and the
 * checksum is the bytes from <dest> to the last payload byte, added up and
 * truncated to eight bits. A reply is the same shape addressed to 00, with a
 * status byte ahead of the reports and one report per command asked.
 *
 * The packet object carries more than the frame. Bytes 1.. of a REQUEST hold
 * a zero-terminated list of where each command starts inside it, built as the
 * game appended them; the matching ACK has to come back with the same list
 * pointing at where each report starts. That list is why this does not need
 * to know how long any command is - the game already said.
 *
 * Nothing here is pressed, inserted or aimed. An attract mode wants a board
 * that answers, not one that plays.
 */

#define JVS_SYNC      0xE0u
#define JVS_STATUS_OK 0x01u
#define JVS_REPORT_OK 0x01u

#define PKT_INDEX     1u        /* command / report offsets, zero-terminated */
#define PKT_FRAME_LEN 0x101u
#define PKT_FRAME     0x102u

/* What the board calls itself. The game reads this back as a C string and
 * shows it in the test menu; the shape is the one every SEGA I/O board of
 * this era reports. */
static const char JVS_IO_IDENT[] =
    "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

/* Switches, coins, analog channels, and general purpose output - enough for a
 * two player gun cabinet. Each block is kind, then three parameters. */
static const unsigned char JVS_FEATURES[] = {
    0x01, 2, 12, 0,        /* players, buttons each */
    0x02, 2,  0, 0,        /* coin slots */
    0x03, 8, 16, 0,        /* analog channels, bits each */
    0x12, 6,  0, 0,        /* general purpose output */
    0x00                   /* end of list */
};

/* Send/recv pairs normally run on the same thread, but the orbis libc exposes
 * amLib entry points to any thread that linked against it (audio helpers,
 * timer callbacks) — a mutex keeps a stray recv from reading mid-build state
 * while another thread is still assembling the ack. */
static pthread_mutex_t g_jvs_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_ack[0x200];
static unsigned      g_ack_len;
static unsigned char g_report_at[0x40];
static unsigned      g_reports;

static void ack_begin(void)
{
    g_ack_len = 0; g_reports = 0;
    g_ack[g_ack_len++] = JVS_SYNC;
    g_ack[g_ack_len++] = 0x00;              /* addressed to the main board */
    g_ack[g_ack_len++] = 0x00;              /* count, filled in at the end */
    g_ack[g_ack_len++] = JVS_STATUS_OK;
}

/* Remember where this report starts before writing it: that offset is what
 * the game looks up to find the answer to the command it asked. */
static void ack_report(void)
{
    if (g_reports < sizeof g_report_at) g_report_at[g_reports++] = (unsigned char)g_ack_len;
    g_ack[g_ack_len++] = JVS_REPORT_OK;
}

static void ack_byte(unsigned char b)
{
    if (g_ack_len < sizeof g_ack - 1) g_ack[g_ack_len++] = b;
}

static void ack_zeros(unsigned n) { while (n--) ack_byte(0); }

static void ack_end(void)
{
    unsigned sum = 0;
    g_ack[2] = (unsigned char)(g_ack_len - 3 + 1);   /* payload + checksum */
    for (unsigned i = 1; i < g_ack_len; i++) sum += g_ack[i];
    g_ack[g_ack_len++] = (unsigned char)sum;
}

/* Answer one command. `a` is the command byte, `p` its parameters. */
static void jvs_command(const unsigned char *p)
{
    switch (p[0]) {
    case 0x10:                                   /* read board identity */
        ack_report();
        for (const char *s = JVS_IO_IDENT; *s; s++) ack_byte((unsigned char)*s);
        ack_byte(0);
        break;
    case 0x11: ack_report(); ack_byte(0x13); break;   /* command revision */
    case 0x12: ack_report(); ack_byte(0x30); break;   /* JVS revision */
    case 0x13: ack_report(); ack_byte(0x10); break;   /* communication version */
    case 0x14:                                   /* what this board has */
        ack_report();
        for (unsigned i = 0; i < sizeof JVS_FEATURES; i++) ack_byte(JVS_FEATURES[i]);
        break;
    case 0x20: {                                 /* switches: test, then players */
        /* One test byte, then `bytes` bytes for each of `players`. In the
         * first of those, bit 7 is start and bit 1 is the first push button,
         * which for a gun cabinet is the trigger. */
        CabinetInput in;
        host_cabinet_input(&in);
        unsigned players = p[1], bytes = p[2];
        {   /* Confirm the game is actually asking, and with what shape. */
            static int shown;
            if (shown < 3 && getenv("LINDBERGH_JVS")) {
                shown++;
                fprintf(stderr, "[jvs] switch read: %u players x %u bytes\n",
                        players, bytes);
            }
        }
        ack_report();
        ack_byte((in.buttons & CAB_TEST) ? 0x80 : 0x00);
        for (unsigned pl = 0; pl < players; pl++) {
            unsigned char b0 = 0;
            if (pl == 0) {
                if (in.buttons & CAB_P1_START)   b0 |= 0x80;
                if (in.buttons & CAB_SERVICE)    b0 |= 0x40;
                if (in.buttons & CAB_P1_TRIGGER) b0 |= 0x02;
                if (in.buttons & CAB_P1_RELOAD)  b0 |= 0x01;
            } else if (pl == 1) {
                if (in.buttons & CAB_P2_START)   b0 |= 0x80;
            }
            ack_byte(b0);
            ack_zeros(bytes > 1 ? bytes - 1 : 0);
        }
        break;
    }
    case 0x21: {                                 /* coins */
        /* Two bytes a slot: the top two bits are the mechanism's condition,
         * zero meaning normal, and the remaining fourteen are the count. */
        CabinetInput in;
        host_cabinet_input(&in);
        ack_report();
        for (unsigned slot = 0; slot < p[1]; slot++) {
            unsigned n = (slot < 2 ? in.coins[slot] : 0) & 0x3FFF;
            ack_byte((unsigned char)(n >> 8));
            ack_byte((unsigned char)(n & 0xFF));
        }
        break;
    }
    case 0x22: {                                 /* analog: where the gun points */
        CabinetInput in;
        host_cabinet_input(&in);
        int w = in.screen_w > 0 ? in.screen_w : 1;
        int h = in.screen_h > 0 ? in.screen_h : 1;
        unsigned x = (unsigned)((long)in.gun_x * 65535 / w);
        unsigned y = (unsigned)((long)in.gun_y * 65535 / h);
        if (in.gun_x < 0) x = 0; if (x > 65535) x = 65535;
        if (in.gun_y < 0) y = 0; if (y > 65535) y = 65535;
        ack_report();
        {   /* ID4 is a driving cabinet: JVS analog ch0 = wheel (centre 0x8000),
             * ch1 = accel, ch2 = brake. The gun layout above is for shooters;
             * steer/accel/brake come straight from the pad mapping. */
            static int flat = -1;
            if (flat < 0) { const char *v = getenv("LINDBERGH_NO_GUN");
                            flat = (v && *v && *v != '0') ? 1 : 0; }
            for (unsigned ch = 0; ch < p[1]; ch++) {
                unsigned v = flat ? 0x8000u
                           : ch == 0 ? (unsigned)(in.steer  & 0xFFFF)
                           : ch == 1 ? (unsigned)(in.accel  & 0xFFFF)
                           : ch == 2 ? (unsigned)(in.brake  & 0xFFFF)
                           : 0x8000u;
                ack_byte((unsigned char)(v >> 8));
                ack_byte((unsigned char)(v & 0xFF));
            }
        }
        break;
    }
    case 0x23: ack_report(); ack_zeros((unsigned)p[1] * 2u); break;  /* rotary */
    default:                                     /* acknowledged, nothing to say */
        ack_report();
        break;
    }
}

/* Counts JVS traffic so the EIP watcher can show whether the board link is
 * being polled at all — a silent JVS is a symptom, not proof, of a stall. */
unsigned long g_jvs_sends, g_jvs_recvs;

static void h_jvs_send(CPU *c)
{
    uint32_t pkt = A32(0);
    g_jvs_sends++;
    if (!pkt) { RET(-7); return; }
    const unsigned char *p = (const unsigned char *)(uintptr_t)pkt;
    const unsigned char *frame = p + 2;

    if (getenv("LINDBERGH_JVS")) jvs_dump("req", pkt, 2, p[1]);

    pthread_mutex_lock(&g_jvs_mu);
    ack_begin();
    for (unsigned i = 0; i < 0x40 && p[PKT_INDEX + 0x101 + i]; i++) {
        /* The command index lives at 0x102 in the request, where the reply
         * frame lives in an acknowledge packet. Same offset, different use. */
        unsigned off = p[PKT_FRAME + i];
        /* The guard is against running off the packet, not off the frame: a
         * command sits at the end of its frame and its parameters are read
         * past p[1] quite legitimately. Comparing against the frame length
         * rejected every single-command request there is. */
        if (off < 3u || off + 2u >= 0x100u) break;
        jvs_command(frame + off);
    }
    ack_end();
    pthread_mutex_unlock(&g_jvs_mu);
    RET(AM_OK);
}

static void h_jvs_recv(CPU *c)
{
    uint32_t ack = A32(1);
    g_jvs_recvs++;
    if (!ack) { RET(-6); return; }
    unsigned char *a = (unsigned char *)(uintptr_t)ack;

    pthread_mutex_lock(&g_jvs_mu);
    if (!g_ack_len) { pthread_mutex_unlock(&g_jvs_mu); RET(-6); return; }
    memcpy(a + PKT_FRAME, g_ack, g_ack_len);
    a[PKT_FRAME_LEN] = (unsigned char)g_ack_len;
    /* The report offsets, in the order the commands were asked, zero
     * terminated - the same shape amJvspMakeReportIndex would have left. */
    for (unsigned i = 0; i < g_reports; i++) a[PKT_INDEX + i] = g_report_at[i];
    a[PKT_INDEX + g_reports] = 0;
    unsigned ack_len = g_ack_len;
    pthread_mutex_unlock(&g_jvs_mu);

    if (getenv("LINDBERGH_JVS")) jvs_dump("ack", ack, PKT_FRAME, ack_len);
    RET(AM_OK);
}

static void h_ok(CPU *c)   { RET(AM_OK); }
static void h_true(CPU *c) { RET(1); }

/* Patch-3 probe: 0x08078B84 sits in an unlifted code gap — only reachable via
 * indirect/vtable dispatch. Count hits to learn whether the stub is load-bearing
 * (fires → must keep, removal aborts) or dead weight (never fires → remove). */
static int s_ab3_hits;
static void h_ab3_probe(CPU *c)
{
    if (++s_ab3_hits <= 4 || (s_ab3_hits & 0xff) == 0)
        fprintf(stderr, "[ab3] blackscreen fn hit #%d esp=%#x ret=%#x\n",
                s_ab3_hits, c->esp, rd32(c->esp));
    RET(AM_OK);
}

/* Keychip/dongle — the game validates the Sega keychip via amDongleUpdate +
 * amDongleIsAvailable and latches error 0xf when the dongle reports no data at
 * [g_dongle+0x661]. Mark that byte present on every poll and report success so
 * seqStartup sees a healthy dongle instead of an error screen. */
static void h_dongle_ok(CPU *c)
{
    uint32_t dg = rd32(GVA(0x089c0940u));
    if (dg) wr8(GVA(dg + 0x661u), 1u);
    /* Also clear a latched startup-error code — the check only sets it when
     * zero, and the dongle now reports healthy, so clear any stale value. */
    uint32_t st = rd32(GVA(0x089ac144u));
    if (st && rd32(GVA(st + 0x11cu)) == 0xfu) wr32(GVA(st + 0x11cu), 0u);
    RET(1);
}

/* amDongleUserInfoEx(a0, a1, ctx) — the game reads the keychip's gameID and
 * compares it to its own; a keychip bound to this disc reports "SBJJ" (ID4
 * EXP_REVD, read from the ELF like lindbergh-loader's elfID). Empty ctx → the
 * IDs mismatch → seqStartup latches error 0xf. Fill it so validation passes. */
static void h_dongle_userinfo(CPU *c)
{
    uint32_t ctx = A32(2);
    if (ctx) wr32(GVA(ctx), 0x4a4a4253u);   /* "SBJJ" little-endian */
    RET(0);
}
static void h_one(CPU *c)  { RET(1); }
/* criAu_SetDebugCallback — the game's debug-callback goes into [0x8db8d20]
 * which every criAuVoice op dispatches through (`call edx`). The game passes a
 * bad value (8) → `call 8` → segv. Force the field to a bare `ret` so the
 * voice ops' callback dispatch is a safe no-op. */
static void h_cri_debugcb(CPU *c)
{
    extern void klogf(const char*, ...);
    klogf("[cri] SetDebugCallback arg=%#x\n", A32(0));
    wr32(GVA(0x08DB8D20u), GVA(0x082DE184u));
    RET(0);
}
/* criAu_Setup{Err,Warning,VoiceCancel}Callback — same deal, into the criAu
 * context's callback slots. Write a ret-stub so any dispatch is a no-op. */
static void h_cri_setupcb_at(CPU *c, unsigned off)
{
    uint32_t ctx = rd32(GVA(0x08DE9C78u));
    if (ctx) wr32(GVA(ctx + off), GVA(0x082DE184u));
    RET(0);
}
static void h_cri_setupcb_3c(CPU *c) { h_cri_setupcb_at(c, 0x3Cu); }
void L_087711FA(CPU *c);   /* criAu_Initialize */
/* criAu_Initialize — dump the criAu globals + the voice-object fields right
 * before the voice loop, then run the real init so the crash field is visible. */
static void h_criau_init(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t ctx = rd32(GVA(0x08DE9C78u));
    klogf("[cri] Initialize: ctx=%#x errcb=%#x gate[8dc63ac]=%d errlvl[8db332c]=%d errcb2[89a0f58]=%#x",
          ctx, rd32(GVA(0x08DB8D20u)),
          rd32(GVA(0x08DC63ACu)), (int32_t)rd32(GVA(0x08DB332Cu)),
          rd32(GVA(0x089A0F58u)));
    klogf("\n");
    push32(c, 0u);
    L_087711FA(c);
}
void L_08776610(CPU *c);   /* criAu_SetupInitializeParameter_LINDBERGH */
void L_08771854(CPU *c);   /* criau_GetHandlePtr */
void L_08779AAA(CPU *c);   /* criHeap_Create */
/* criHeap_Create — dump the heap object + the call [edi]/[esi+0x1c]=8 field. */
static void h_cri_heapcreate(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t es=c->esi, ed=c->edi, eb=c->ebx, espc=c->esp;
    klogf("[cri] HeapCreate ebx=%#x esi=%#x edi=%#x [ebp8]=%#x arg1=%#x\n",
          eb, es, ed,
          (espc>=0x8000000&&espc<0xa000000)?rd32(GVA(espc+8)):0xBADu,
          (espc>=0x8000000&&espc<0xa000000)?rd32(GVA(espc+0xc)):0xBADu);
    push32(c, 0u);
    L_08779AAA(c);
}
/* criau_GetHandlePtr — dump esi/edi/[edi]/[esi] to see what object the
 * call [edi]/[esi+0x1c]=8 dispatch reads. */
static void h_cri_gethandle(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t es = c->esi, ed = c->edi;
    klogf("[cri] GetHandlePtr esi=%#x edi=%#x [esi]=%#x [esi+1c]=%#x [edi]=%#x edx=%#x\n",
          es, ed,
          (es>=0x8000000&&es<0xa000000)?rd32(GVA(es)):0xBADu,
          (es>=0x8000000&&es<0xa000000)?rd32(GVA(es+0x1c)):0xBADu,
          (ed>=0x8000000&&ed<0xa000000)?rd32(GVA(ed)):0xBADu,
          c->edx);
    push32(c, 0u);
    L_08771854(c);
}
/* criAu_SetupInitializeParameter_LINDBERGH(param) — dump the param struct the
 * game hands the criAu middleware: allocator fnptrs (expect PLT 0x0807xxxx),
 * work buffer + size, voice count, etc. If allocators/work/voices are wrong,
 * criHeap→criAu objects build on nothing → every call [obj+X] faults. */
static void h_cri_setupparam(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t p = A32(0);
    klogf("[cri] SetupInitParam @%#x:", p);
    for (int i = 0; i < 16; i++)
        klogf(" %x", p ? rd32(GVA(p + i * 4u)) : 0xDEADu);
    klogf("\n");
    /* hand off to the real parser — same frame-shape as h_seq_init_model_tick:
     * a fake retaddr so L_08776610's `ret` pops it and returns to dispatch. */
    push32(c, 0u);
    L_08776610(c);
}
static void h_cri_setupcb_40(CPU *c) { h_cri_setupcb_at(c, 0x40u); }
static void h_cri_setupcb_44(CPU *c) { h_cri_setupcb_at(c, 0x44u); }

/* ADX decode probe — the mixer hears 0x8000/0x7fff alternation (divergent
 * predictor). Dump the decoder's real inputs: coef1/coef2 (expect +large/−small
 * for 48kHz), the ADX4 descramble LCG (state ptr/mult/add), the per-channel
 * history words, and 32 bytes of the ADX source frame. If coefs/LCG look sane
 * the bug is in lifted math; if not, the caller/handle setup is wrong. */
void L_0876CA8D(CPU *c);   /* ADX_DecodeSte4AsSte */
void L_0876C53C(CPU *c);   /* ADX_DecodeMono4 */
static void h_adx_probe(CPU *c, int mono)
{
    extern void klogf(const char*, ...);
    uint32_t src = A32(0), nsmp = A32(1), dst0 = A32(2), hist0 = A32(3);
    uint32_t dst1 = A32(4), hist1 = A32(5);
    uint32_t coef1 = A32(6), coef2 = A32(7);
    uint32_t stp = A32(8), lmul = A32(9), ladd = A32(10);
    static int s_adx_n;
    if (s_adx_n < 16) {
        s_adx_n++;
        klogf("[adx] %s src=%#x nsmp=%u dst=%#x/%#x hist=%#x/%#x coef=%#x/%#x lcg=%#x m=%#x a=%#x\n",
              mono ? "mono4" : "ste4s", src, nsmp, dst0, dst1, hist0, hist1,
              coef1, coef2, stp, lmul, ladd);
        if (src >= 0x04000000u) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)src;
            char hexb[160]; int ho = 0;
            for (int i = 0; i < 36 && ho < 140; i++)
                ho += sprintf(hexb + ho, " %02x", p[i]);
            klogf("[adx]   src bytes:%s\n", hexb);
        }
        if (hist0 >= 0x04000000u && hist0 < 0x20000000u)
            klogf("[adx]   hist0: %d %d  lcgstate=%u\n",
                  (int16_t)rd16(GVA(hist0)), (int16_t)rd16(GVA(hist0 + 2)),
                  stp ? rd16(GVA(stp)) : 0);
        if (hist1 >= 0x04000000u && hist1 < 0x20000000u)
            klogf("[adx]   hist1: %d %d\n",
                  (int16_t)rd16(GVA(hist1)), (int16_t)rd16(GVA(hist1 + 2)));
    }
    push32(c, 0u);
    if (mono) L_0876C53C(c); else L_0876CA8D(c);
}
static void h_adx_ste(CPU *c)  { h_adx_probe(c, 0); }
static void h_adx_mono(CPU *c) { h_adx_probe(c, 1); }

/* ADX_GetCoefficient(freq, srate, &coef1, &coef2) — the game's x87
 * implementation returns degenerate coefs (coef1=0x393, coef2=+0x1000), so
 * every decoded block diverges to the ±32767 rails — the "square wave"
 * garbage. The lifted fmulp/fchs/fistp chain mis-executes. Reimplement in C:
 *   a = sqrt(2) - cos(2*pi*freq/srate); b = sqrt(2) - 1;
 *   c = (a - sqrt(a^2 - b^2)) / b;
 *   coef1 = trunc(c * 2 * 4096);  coef2 = trunc(-c*c * 4096)
 * (constants verified against .rodata @0x8844dec-df8; game fldcw-truncates). */
static void h_adx_getcoef(CPU *c)
{
    extern void klogf(const char*, ...);
    double f = (double)(int32_t)A32(0);
    double n = (double)(int32_t)A32(1);
    uint32_t o0 = A32(2), o1 = A32(3);
    double a = 1.4142135623730951 - cos(2.0 * M_PI * f / n);
    double b = 1.4142135623730951 - 1.0;
    double cc = (a - sqrt(a * a - b * b)) / b;
    int16_t c1 = (int16_t)(cc * 2.0 * 4096.0);
    int16_t c2 = (int16_t)(-(cc * cc) * 4096.0);
    if (o0) wr16(o0, (uint16_t)c1);
    if (o1) wr16(o1, (uint16_t)c2);
    klogf("[adxcoef] f=%g n=%g c=%g -> %#x %#x\n", f, n, cc,
          (uint16_t)c1, (uint16_t)c2);
    RET(0);
}
/* caManager ctor — the audio-subsystem root. cApplication::create news ~0x440
 * bytes then calls this; the real ctor spins up criAu + criAuPly + ADXT + the
 * lbrna renderer + the audio-manager thread — all missing hardware. Cut it:
 * zero the object so every field/handle is NULL and none of the CRI/lbrna
 * subsystem is ever built. Member functions are symbol-stubbed no-ops, so a
 * zeroed object is inert. */
static void h_camgr_ctor(CPU *c)
{
    uint32_t thiz = A32(0);
    if (thiz) {
        memset((void *)(uintptr_t)GVA(thiz), 0, 0x460);
        /* Give the zeroed object a fake vtable so any virtual call lands on a
         * ret-stub instead of deref-ing NULL. 0x082DE184 is a bare `ret`. A
         * scratch .bss block at 0x089AF800 holds 128 entries. */
        for (int i = 0; i < 128; i++)
            wr32(GVA(0x089AF800u + i * 4u), 0x082DE184u);
        wr32(GVA(thiz), 0x089AF800u);
    }
    RET(thiz);
}

/* cSequenceManager::regist(seq, link) — log each seq as it registers so we can
 * see which seqs exist and their heap addresses / vtables (vtable identifies
 * the concrete class). Useful to find the dormant seqs the iterator skips. */
/* seqInit* tick handlers — they poll cabinet hardware that does not exist on
 * PS5 (steering wheel IO, card reader, printer, stocker) and block forever,
 * stalling the init chain. Mark the seq done ([this+0x30]=1) and terminate it
 * via cSequence::kill so the seq manager advances to the next init step. */
uint32_t guest_call(CPU *c, uint32_t fn, const uint32_t *args, int nargs);
static void h_seq_tick_done(CPU *c)
{
    uint32_t thiz = A32(0);
    if (thiz) {
        wr32(GVA(thiz + 0x30u), 1u);
        uint32_t a = thiz;
        guest_call(c, 0x0820A7B2u, &a, 1);   /* cSequence::kill(this) */
    }
}

/* es::glh_util::putConsoleSeparate — the game's debug-print channel. With
 * s_logMask set the game narrates here; the stock path crashes (the loader
 * detours it for exactly that reason). Route the text to klog instead —
 * the args are (const char *str, char level). */
static void h_put_console_sep(CPU *c)
{
    uint32_t p = A32(0);
    if (p >= 0x04000000u) {
        extern void klogf(const char*, ...);
        const char *s = (const char *)(uintptr_t)p;
        if ((unsigned char)s[0] >= 0x20 && (unsigned char)s[0] < 0x7f)
            klogf("[game] %s\n", s);
    }
}

/* amDipswGetData(dip*) — no security board: all dips off. */
static void h_dipsw_get(CPU *c)
{
    uint32_t p = A32(0);
    if (p >= 0x04000000u) wr8(GVA(p), 0x00u);
    RET(AM_OK);
}

/* DCT_IsseTrans(co, out, flag) — the iSSE transposed IDCT the Sofdec video
 * decoder uses for the attract/movie sequences.  The lifted body is a wall of
 * MMX (movq/pmaddwd/pshufw/psrad/packssdw) the lifter left as TODOs; running it
 * aborts.  Reimplement as a separable float IDCT — numerically equivalent for
 * a video macroblock.
 *   flag==0 → DC-only: out[i] = (co[0]+4)>>3 splat (the asm's fast path).
 *   flag!=0 → full 8x8 IDCT:  out[i*8+j] = Σu Σv Cu Cv co[u*8+v]
 *                                   cos((2i+1)uπ/16) cos((2j+1)vπ/16) / 4 */
static void h_dct_issetrans(CPU *c)
{
    uint32_t co = A32(0), out = A32(1), flag = A32(2);
    const int16_t *src = (const int16_t *)(uintptr_t)co;
    int16_t *dst = (int16_t *)(uintptr_t)out;
    extern float cosf(float);

    if (!flag) {
        int16_t v = (int16_t)((src[0] + 4) >> 3);   /* arith shift handles neg */
        for (int i = 0; i < 64; i++) dst[i] = v;
        RET(0); return;
    }
    /* basis[u][i] = Cu * cos((2i+1) u π / 16) — precomputed once. */
    static float B[8][8]; static int init;
    if (!init) {
        init = 1;
        for (int u = 0; u < 8; u++)
            for (int i = 0; i < 8; i++)
                B[u][i] = (u ? 1.0f : 0.70710678f)
                        * cosf((2*i + 1) * u * (float)(3.14159265358979/16.0));
    }
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            float s = 0.f;
            for (int u = 0; u < 8; u++)
                for (int v = 0; v < 8; v++)
                    s += (float)src[u*8 + v] * B[u][i] * B[v][j];
            int iv = (int)(s * 0.25f + (s >= 0.f ? 0.5f : -0.5f));
            if (iv > 32767) iv = 32767; if (iv < -32768) iv = -32768;
            dst[i*8 + j] = (int16_t)iv;
        }
    RET(0);
}

/* MPVMC08_OneRef1p_TuneAMmx(ctx) — 8x8 motion-compensation block copy, MMX
 * movq in the lifted body.  ctx: [ctx+0x24]=src plane, [ctx+0x18]=dst plane,
 * [ctx+0x20]=src row stride; dst rows are contiguous at an 8-byte pitch. */
static void h_mpvmc08(CPU *c)
{
    uint32_t ctx = A32(0);
    const uint8_t *src = (const uint8_t *)(uintptr_t)rd32(GVA(ctx + 0x24u));
    uint8_t *dst = (uint8_t *)(uintptr_t)rd32(GVA(ctx + 0x18u));
    uint32_t stride = rd32(GVA(ctx + 0x20u));
    for (int r = 0; r < 8; r++)
        memcpy(dst + r * 8, src + r * (uint32_t)stride, 8);
    RET(0);
}

/* seqVSMatchingSelect onTick — the matching screen can't complete on a
 * standalone PS5 and its `this` arrives corrupt in our recomp path. Force
 * the seq done if the pointer is sane; otherwise just mark it dead in the
 * sequence manager's view by killing it via the same path. */
static void h_seq_matching_done(CPU *c)
{
    uint32_t thiz = A32(0);
    /* Sanity: seq objects live in the guest heap (~0x8xxxxxxx-0xfxxxxxxx).
     * A corrupt arg (like the 0xb that produced the 0x1f fault) must not
     * write — GVA of it lands on the image anyway, but skip obviously bad
     * values (NULL and tiny offsets). */
    if (thiz >= 0x04000000u) {
        wr32(GVA(thiz + 0x30u), 1u);
        uint32_t a = thiz;
        guest_call(c, 0x0820A7B2u, &a, 1);   /* cSequence::kill */
    }
}

/* seqInitModel::onTick drives three phases via [this+0x54]: 0 sets up the ACRO
 * loading layers + cCarModelCacher jobs, 1 polls completion, 2+ blocks waiting
 * on a wheel-sensor counter that never moves (no wheel hardware). Run the real
 * tick for status<2 so car-model loading still happens, kill on status>=2. */
void L_082DE64C(CPU *c);   /* seqInitModel::onTick */
static void h_seq_init_model_tick(CPU *c)
{
    uint32_t thiz = A32(0);
    if (!thiz) return;
    if (rd32(GVA(thiz + 0x54u)) >= 2u) {
        wr32(GVA(thiz + 0x30u), 1u);
        guest_call(c, 0x0820A7B2u, &thiz, 1);   /* cSequence::kill */
        return;
    }
    /* Run the real onTick for status 0/1 so car-model loading drives the job
     * workers. dispatch already consumed the caller's return address, so push
     * a fake one to keep L_082DE64C's arg reads aligned (its prologue expects
     * ret+args on the guest stack). */
    push32(c, 0u);
    L_082DE64C(c);
}

/* seqAdvertise::onTick probe — the attract stalls before the movie/3D seqs
 * spawn.  Dump the seq's phase/tick fields, its child-seq slot, the acro
 * driver object at [glob+0x664], and the Sofdec error globals. */
void L_0826F2C6(CPU *c);   /* seqAdvertise::onTick */
static void h_seqadv_tick(CPU *c)
{
    uint32_t thiz = A32(0);
    static int n;
    if (thiz >= 0x04000000u && (n++ % 120) == 0) {
        uint32_t mgr  = rd32(GVA(0x89ae160u));
        uint32_t glob = rd32(GVA(0x89c0940u));
        uint32_t idx  = rd32(GVA(thiz + 0x58u));
        uint32_t child = (mgr && idx < 64u) ? rd32(GVA(mgr + idx*4u + 8u)) : 0;
        uint32_t drv  = glob ? rd32(GVA(glob + 0x664u)) : 0;
        uint32_t dvt  = (drv >= 0x04000000u) ? rd32(GVA(drv)) : 0;
        klogf("[seqadv] this=%x ph=%x t50=%x t54=%x idx=%x child=%x c40=%x "
              "drv=%x vt=%x sfderr=%x cnt=%x en=%x fl=%x\n",
              thiz, rd32(GVA(thiz + 4u)), rd32(GVA(thiz + 0x50u)),
              rd32(GVA(thiz + 0x54u)), idx, child,
              child ? rd32(GVA(child + 0x40u)) : 0, drv, dvt,
              rd32(GVA(0x8d98e20u)), rd32(GVA(0x8d98e60u)),
              rd32(GVA(0x8d91629u)), rd32(GVA(0x89ae8b0u + 4u)));
    }
    push32(c, 0u);
    L_0826F2C6(c);
}

void L_0820A9DE(CPU *c);   /* cSequenceManager::regist */
void L_0820A9DE(CPU *c);   /* cSequenceManager::regist */

/* yabukita::FileStream::open(this, path, mode) — the gate every asset file
 * goes through.  Log the path so a failed deserialize names the file it
 * could not open (the scene-DB NULL is downstream of one of these). */
void L_086957B6(CPU *c);
static void h_fstream_open(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t p = A32(1);
    const char *path = (p >= 0x04000000u && p < 0x09000000u)
                     ? (const char *)(uintptr_t)p : "?";
    char b[256]; int n = 0;
    for (const char *q = path; *q && n < 200; q++) b[n++] = *q;
    b[n] = 0;
    klogf("[fsopen] %s\n", b);
    push32(c, 0u);            /* satisfy the lifted prologue's ret-slot reads */
    L_086957B6(c);
}

/* _sSceneDatabase::createFromFile(this, path, m, m) — the scene-DB loader.
 * Log the path; the NULL DB downstream means one of these returned NULL. */
void L_084B76D8(CPU *c);
static void h_db_createfile(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t p = A32(1);
    const char *path = (p >= 0x04000000u && p < 0x09000000u)
                     ? (const char *)(uintptr_t)p : "?";
    char b[256]; int n = 0;
    for (const char *q = path; *q && n < 200; q++) b[n++] = *q;
    b[n] = 0;
    klogf("[dbcreate] path=%s this=%#x\n", b, A32(0));
    push32(c, 0u);
    L_084B76D8(c);
}

/* assam::File::loadStreamRequest(this, path, StreamInfo*, l, m) — the async
 * file-load queue.  Log the path; if the scene DB loads through this we see
 * exactly which file the loader is asked for and whether it's requested at
 * all. */
void L_08699572(CPU *c);
static void h_stream_req(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t p = A32(1);
    const char *path = (p >= 0x04000000u && p < 0x09000000u)
                     ? (const char *)(uintptr_t)p : "?";
    char b[256]; int n = 0;
    for (const char *q = path; *q && n < 200; q++) b[n++] = *q;
    b[n] = 0;
    klogf("[strm] %s\n", b);
    push32(c, 0u);
    L_08699572(c);
}

/* assam::FileManager::open(this, File*, path, create) — the real asset open.
 * Log the path so a failed load names the file. */
void L_086A04F6(CPU *c);
static void h_fm_open(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t p = A32(2);
    const char *path = (p >= 0x04000000u && p < 0x09000000u)
                     ? (const char *)(uintptr_t)p : "?";
    char b[256]; int n = 0;
    for (const char *q = path; *q && n < 200; q++) b[n++] = *q;
    b[n] = 0;
    klogf("[fmopen] %s\n", b);
    push32(c, 0u);
    L_086A04F6(c);
}

/* assam::FileManager::loadRequest(this, File*) — submits the actual load job.
 * Log the file's path member so we can see whether the scene-DB load is
 * queued at all (workers sit in pickJob — if this never fires for a DB the
 * load was never asked for). */
void L_0869B984(CPU *c);
static void h_fm_loadreq(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t f = A32(1);             /* File* */
    uint32_t p = f ? rd32(f + 0x18u) : 0;   /* File path field guess */
    const char *path = (p >= 0x04000000u && p < 0x09000000u)
                     ? (const char *)(uintptr_t)p : NULL;
    char b[256]; int n = 0;
    if (path) for (const char *q = path; *q && n < 200; q++) b[n++] = *q;
    b[n] = 0;
    klogf("[loadreq] file=%#x path=%s\n", f, path ? b : "?");
    push32(c, 0u);
    L_0869B984(c);
}

/* _sSceneDatabase::createFromMemory / createFromFile — whichever builds the
 * inner DB.  If neither fires the async load never produces a DB. */
void L_084B70BE(CPU *c);
static void h_db_createmem(CPU *c)
{
    extern void klogf(const char*, ...);
    klogf("[dbmem] this=%#x buf=%#x sz=%#x\n", A32(0), A32(1), A32(2));
    push32(c, 0u);
    L_084B70BE(c);
}

/* Async-scene-DB completion stages — log which fire so we can see where the
 * load->deserialize->this+0x18 chain breaks. */
void L_0821DF6E(CPU *c);
void L_0821EFC0(CPU *c);
void L_0821E14C(CPU *c);
static void h_async_loaded(CPU *c)   { extern void klogf(const char*,...); klogf("[adbg] onLoadedResource this=%#x data=%#x size=%#x\n", A32(0), A32(1), A32(2)); push32(c,0); L_0821DF6E(c); }
static void h_async_ondes(CPU *c)    { extern void klogf(const char*,...); klogf("[adbg] _onDeserialize this=%#x job=%#x\n", A32(0), A32(1)); push32(c,0); L_0821EFC0(c); }
static void h_async_deser(CPU *c)    { extern void klogf(const char*,...); klogf("[adbg] onDeserialize this=%#x\n", A32(0)); push32(c,0); L_0821E14C(c); }

/* cAsyncSceneDatabase::getSceneDatabase — returns this+0x18, NULL until the
 * async deserialize finishes.  Callers (the seq) use it without a null check
 * because the board gives a dummy.  Give a real zeroed _sSceneDatabase-sized
 * dummy on first NULL access so the getters see an empty DB, not a fault. */
/* isDependencyComplete — `this` is the inner DB; it's NULL until the async
 * load stores it.  Returning 1 on NULL made the seq use a NULL db; returning
 * 0 keeps the seq polling so the real load can populate it. */
static void h_depcomplete(CPU *c)
{
    uint32_t thiz = A32(0);
    if (!thiz) { RET(0); return; }         /* not ready yet — keep waiting */
    uint32_t st = rd32(thiz + 0xbcu);
    extern void klogf(const char*, ...);
    static int said;
    if (!said) { said=1; klogf("[depc] this=%#x st=%#x\n", thiz, st); }
    RET(st ? 1 : 0);
}

static uint32_t s_dummy_db;
static uint32_t dummy_scenedb(void)
{
    if (!s_dummy_db) {
        /* Handed to anyone who asks while the real load is in flight; one
         * shared zeroed object is enough — every getter reads count/size
         * fields that come back 0.  It must live in the guest heap so rd32
         * on db+off resolves. */
        extern void *gmalloc(size_t, size_t);
        void *p = gmalloc(0x400, 16);
        memset(p, 0, 0x400);
        s_dummy_db = (uint32_t)(uintptr_t)p;
    }
    return s_dummy_db;
}

static void h_getscenedb(CPU *c)
{
    uint32_t thiz = A32(0);
    uint32_t db = (thiz >= 0x04000000u) ? rd32(thiz + 0x18u) : 0;
    RET(db ? db : dummy_scenedb());
}

/* Generic: run a lifted getSceneDatabase, substitute the dummy when it
 * returns NULL — every variant (async/storage/model) feeds callers that
 * don't null-check. */
#define H_GETDB_WRAPPER(fn)                                      \
    void fn(CPU *c);                                             \
    static void h_##fn(CPU *c) {                                 \
        push32(c, 0u); fn(c);                                    \
        if (!c->eax) c->eax = dummy_scenedb();                   \
        return;                                                  \
    }
H_GETDB_WRAPPER(L_081A94E4)   /* cCourseModel::getSceneDatabase */
H_GETDB_WRAPPER(L_08227E02)   /* cModelObject::getSceneDatabase */
H_GETDB_WRAPPER(L_0822B0F4)   /* cResourceStorage::getSceneDatabase(path) */
H_GETDB_WRAPPER(L_0822AEA6)   /* cResourceStorage::getSceneDatabase(idx) */

/* AbkDataInst::deserializeSceneDatabase(this, IzChunk*, m) — the fn that
 * should build the inner _sSceneDatabase.  If it doesn't run or returns
 * without setting LoadObj+0x18 the consumer reads NULL. */
void L_08381820(CPU *c);
static void h_deser_scenedb(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t thiz = A32(0), chunk = A32(1);
    push32(c, 0u);
    L_08381820(c);
    klogf("[deser] this=%#x chunk=%#x -> eax=%#x\n", thiz, chunk, c->eax);
}

/* cJobThread::addJob(this, cJobDesc*) — submits a job.  Log the desc +
 * the job's fn so we can see whether load jobs reach the queue (workers
 * sit in pickJob — if addJob fires but pickJob starves the queue is fed
 * but not drained). */
void L_08378FCE(CPU *c);
static void h_addjob(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t jd = A32(1);
    uint32_t fn = (jd >= 0x04000000u) ? rd32(jd) : 0;
    klogf("[job] add jobdesc=%#x fn=%#x\n", jd, fn);
    push32(c, 0u);
    L_08378FCE(c);
}

/* _sSceneDatabase::initialize — run the real body when `this` is valid; only
 * skip when NULL (the caller hands it a NULL db in the empty-DB path). */
void L_084B5CE6(CPU *c);
static void h_db_init(CPU *c)
{
    if (!A32(0)) return;                    /* NULL this — nothing to init */
    push32(c, 0u);
    L_084B5CE6(c);
}

/* glh_util::createTextureFromMem(data, w, h, &tex, &target) — log the args,
 * run the real body.  It's where the FBO/screen textures get made; it crashes
 * on a bad arg so dump them first. */
void L_0854D3F0(CPU *c);
static void h_ctex(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t ctx = A32(0);
    klogf("[ctex] ctx=%#x data=%#x w=%d h=%d tgt=%#x tex=%#x\n",
          ctx, A32(1), (int)A32(2), (int)A32(3), A32(4), A32(5));
    if (ctx >= 0x04000000u) {
        uint32_t m[0x11];
        for (int i = 0; i < 0x10; i++) m[i] = rd32(ctx + (uint32_t)i * 4u);
        klogf("[ctex] ctx: %08x %08x %08x %08x | %08x %08x %08x %08x |"
              " %08x %08x %08x %08x | %08x %08x %08x %08x\n",
              m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],
              m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]);
    }
    push32(c, 0u);
    L_0854D3F0(c);
}

/* _sRender::loadTexture(this, _sTexture*) — the post-effect textures pass a
 * _sTexture whose +0x18 image member is bogus; dump it before the deref. */
void L_08554022(CPU *c);
static void h_loadtex(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t tex = A32(1);
    static int lt;
    if (tex >= 0x04000000u) {
        uint32_t img = rd32(tex + 0x18u);
        if (lt++ < 200 || img == 1 || (img & 0xffff0000u) == 0x69730000u)
            klogf("[loadtex] this=%#x tex=%#x a0=%#x 9c=%#x 74=%#x img(+18)=%#x *img=%#x\n",
                  A32(0), tex,
                  rd32(tex + 0xa0u), rd32(tex + 0x9cu), rd32(tex + 0x74u),
                  img, img >= 0x04000000u ? rd32(img) : 0xdeadda7au);
    }
    push32(c, 0u);
    L_08554022(c);
}

/* _sSceneDatabase::LoadObj::procDeserialize(this, buf, worker) — the worker
 * job that runs yabukita::Serializer::deserializeFromMemory and stores the
 * resulting DB at this+0x24 (status at +0x28: 0 ok / 2 error).  Log the
 * outcome: a NULL +0x24 here is why registResources later calls
 * isDependencyComplete on NULL. */
void L_084B1D9C(CPU *c);
static void h_procdeser(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t self = A32(0), buf = A32(1);
    uint32_t sz = self >= 0x04000000u ? rd32(self + 0x1cu) : 0;
    uint32_t magic = buf >= 0x04000000u ? rd32(buf) : 0;
    klogf("[pdeser] this=%#x buf=%#x this+0x18=%#x +0x1c=%#x +0x20=%#x magic=%08x\n",
          self, buf, self >= 0x04000000u ? rd32(self + 0x18u) : 0, sz,
          self >= 0x04000000u ? rd32(self + 0x20u) : 0, magic);
    push32(c, 0u);
    L_084B1D9C(c);
    if (self >= 0x04000000u)
        klogf("[pdeser] -> db=%#x status=%d done=%d\n", rd32(self + 0x24u),
              (int)rd32(self + 0x28u), (int)rd8(self + 0x2cu));
}

/* yabukita::Serializer::deserializeFromMemory(this, buf, size, type) */
void L_0868DB88(CPU *c);
static void h_desermem(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t buf = A32(1), size = A32(2);
    klogf("[ydeser] buf=%#x size=%#x type=%d hdr=%08x %08x %08x %08x\n",
          buf, size, (int)A32(3),
          buf >= 0x04000000u ? rd32(buf) : 0, buf >= 0x04000000u ? rd32(buf+4) : 0,
          buf >= 0x04000000u ? rd32(buf+8) : 0, buf >= 0x04000000u ? rd32(buf+12) : 0);
    push32(c, 0u);
    L_0868DB88(c);
    klogf("[ydeser] -> obj=%#x\n", c->eax);
}

/* lxci_read_func(req) — CRI's Linux sector reader: fseek(fp,off,0);
 * n=fread(buf,1,len,fp); ferror(fp).  req: +0 FILE*, +4 off, +8 buf, +c len.
 * The sync .efo loads produce size 0 — see what the libc layer returns. */
void L_0875C4F5(CPU *c);
extern FILE *fhdl_resolve(uint32_t h);
uint32_t g_watch_lo, g_watch_hi;
uint32_t g_watch_ranges[32][2];
uint32_t g_watch_n;
static int lxci_cid(CPU *c) { extern CPU *const *crash_cpu_list(void); CPU *const *r = crash_cpu_list(); for (int i = 0; i < 65 && r[i]; i++) if (r[i] == c) return i; return -1; }
static void h_lxci_read(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t r = A32(0);
    uint32_t fp = r >= 0x04000000u ? rd32(r) : 0, off = r >= 0x04000000u ? rd32(r+4) : 0;
    uint32_t buf = r >= 0x04000000u ? rd32(r+8) : 0, len = r >= 0x04000000u ? rd32(r+12) : 0;
    push32(c, 0u);
    L_0875C4F5(c);
    extern uint32_t g_watch_lo, g_watch_hi;
    extern uint32_t g_watch_ranges[32][2];
    extern uint32_t g_watch_n;
    if (getenv("LINDBERGH_WATCH") && len >= 0x100000u) {
        if (g_watch_n < 32) {
            g_watch_ranges[g_watch_n][0] = buf;
            g_watch_ranges[g_watch_n][1] = buf + len;
            g_watch_n++;
        }
        if (!g_watch_lo || buf < g_watch_lo) g_watch_lo = buf;
        if (buf + len > g_watch_hi)        g_watch_hi = buf + len;
    }
    klogf("[lxci] c%d read fp=%#x(F=%p) off=%#x buf=%#x len=%#x -> ret=%#x pos=%ld first=%08x mid=%08x deep=%08x\n",
          lxci_cid(c), fp, (void *)fhdl_resolve(fp), off, buf, len, c->eax,
          fhdl_resolve(fp) ? ftell(fhdl_resolve(fp)) : -1L,
          buf >= 0x04000000u ? rd32(buf) : 0,
          buf >= 0x04000000u ? rd32(buf + 0x400u) : 0,
          buf >= 0x04000000u ? rd32(buf + 0x7b0u) : 0);
}
/* lxci_open_func(path, ...) -> FILE*-ish handle */
void L_0875C2CC(CPU *c);
static void h_lxci_open(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t req = A32(0);                     /* req: +0 path, +4 FILE* out */
    uint32_t p = req >= 0x04000000u ? rd32(req) : 0;
    push32(c, 0u);
    L_0875C2CC(c);
    uint32_t h = req >= 0x04000000u ? rd32(req + 4) : 0;
    klogf("[lxci] c%d open(%s) -> ret=%#x fp=%#x(F=%p)\n",
          lxci_cid(c),
          p >= 0x04000000u ? (const char *)(uintptr_t)p : "?", c->eax, h,
          (void *)fhdl_resolve(h));
}
/* lxci_Close(req) — see when CRI actually closes so we can tell whether a
 * read lands on a rebound handle. */
void L_0875C380(CPU *c);
static void h_lxci_close(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t req = c->eax;                       /* arg in eax, not stack */
    uint32_t h = req >= 0x04000000u ? rd32(req) : 0;
    klogf("[lxci] c%d close fp=%#x(F=%p)\n", lxci_cid(c), h, (void *)fhdl_resolve(h));
    push32(c, 0u);
    L_0875C380(c);
}

/* Same pattern for the const getters / delete* cleanups: run the real body
 * on a valid this, return a safe empty result on NULL. */
#define H_NULLSAFE_DB(fn, name)                                  \
    void fn(CPU *c);                                             \
    static void h_##name(CPU *c) {                               \
        if (!A32(0)) return;                                     \
        push32(c, 0u); fn(c);                                    \
    }
H_NULLSAFE_DB(L_084AFEC6, db_delvtx)    /* deleteVertexArray */
H_NULLSAFE_DB(L_084B06EA, db_deltex)    /* deleteTextureFile */
H_NULLSAFE_DB(L_084AF98C, db_nshape)    /* getShapeHeaderNumber */
H_NULLSAFE_DB(L_084AEB68, db_ntex)      /* getTextureNumber */

static void h_regist_log(CPU *c)
{
    extern void klogf(const char*, ...);
    uint32_t a0 = A32(0), a1 = A32(1), a2 = A32(2);
    uint32_t thiz, seq, link;
    /* regist(this, seq, link) is a thiscall — `this` comes in ecx. Some
     * callers also push it as arg0 (3-arg cdecl shape); some push only
     * (seq, link) — then arg0=seq, arg1=link and the lifted code would
     * read seq=-1 -> wr32(seq+0x20) -> segv at 0x1f. Detect by arg1 being
     * a small value rather than a guest pointer. */
    if (a1 >= 0x04000000u) { thiz = a0; seq = a1; link = a2; }
    else                  { thiz = c->ecx; seq = a0; link = a1; }
    /* seq must be a sane guest pointer — a caller that slipped a -1 past the
     * convention probe would otherwise wr32(seq+0x20) at ~0x1f. */
    if ((int32_t)seq <= 0) {
        klogf("[regist] bad seq=0x%x this=0x%x link=0x%x — skip\n", seq, thiz, link);
        RET(0xFFFFFFFFu); return;
    }
    uint32_t vt = rd32(GVA(seq));
    klogf("[regist] this=0x%x seq=0x%x vt=0x%x link=0x%x\n", thiz, seq, vt, link);
    { extern uint32_t g_seqmgr; g_seqmgr = thiz; }
    /* Run regist's logic directly — calling L_0820A9DE with the wrong
     * arg layout is what crashed. Same body as the lifted code. */
    wr8(GVA(thiz + 0x80c), 1u);
    uint32_t eax = rd32(GVA(thiz + 4u));
    uint32_t ebx, edx = 0xFFu;
    for (;;) {
        ebx = rd32(GVA(thiz + eax * 4u + 8u));
        if (ebx) { eax = (eax + 1) & 0xFFu; wr32(GVA(thiz + 4u), eax);
                   if (--edx == 0xFFFFFFFFu) break; continue; }
        wr32(GVA(thiz + eax * 4u + 8u), seq); ebx = eax; break;
    }
    if (edx == 0xFFFFFFFFu) ebx = edx;
    wr32(GVA(seq + 0x20u), ebx);
    /* link is a PARENT SLOT INDEX — a caller that pushed only 2 args leaves
     * link = whatever was above the args (a float, e.g. 0x3f800000), which
     * routine() later uses as a slot index → reads a float as a pointer →
     * segv at 0x3f800004.  Only honor small, in-range indices. */
    if ((int32_t)link >= 0 && link < 0x100u) {
        uint32_t e = rd32(GVA(thiz + link * 4u + 8u));
        wr32(GVA(seq + 0x24u), link);
        wr32(GVA(seq + 0x2cu), rd32(GVA(e + 0x28u)));
        wr32(GVA(e + 0x28u), ebx);
    }
    RET(ebx);
}

/* seqInitPairLink::tickInitAddress — waits for a PairLink multi-cabinet
 * network address. No cabinet link exists, so it blocks forever and the
 * sequence machine never leaves seqInitialize. Force its state dword to 4
 * (done/skip) and return — lindbergh-loader bypasses the same wait with a
 * short-JMP patch at 0x082e0ec9. */
static void h_pair_link_skip(CPU *c)
{
    uint32_t thiz = A32(0);
    if (thiz) wr32(GVA(thiz + 0x08u), 4u);
    RET(0);
}

/* seqStartup::onTick — the startup sequence polls a hardware-error code at
 * [g_status+0x11c] (eth0=0x208, keychip=0xf, ...) and spawns seqErrorDisp when
 * nonzero. None of that hardware exists on PS5, so the check latched an error
 * and the game sits on an (invisible) error screen. Clear the code each tick
 * so seqStartup takes the no-error path and advances to seqGame, then run the
 * real tick. */
/* NOTE on seqStartup: the game is not stuck — it is in seqStartup + seqErrorDisp
 * (a hardware-error state). seqStartup's onTick polls the cabinet-hardware error
 * code at [g_status+0x11c] (eth0=0x208, keychip=0xf, ...); a check latches a
 * nonzero code → seqStartup spawns seqErrorDisp → the error screen is drawn but
 * produces no pixels. The seq framework is healthy; the game correctly reports
 * missing arcade hardware. Forcing the error to 0 pushes seqStartup into its
 * teardown/advance path which dereferences uninitialised audio/CRI objects and
 * segfaults — bypassing needs real keychip/network/CRI emulation, not a clear. */
/* The network-property query the DHCP wait polls. The real one does
 * socket()+ioctl(SIOCGIFADDR) on eth0 — returns -2 while DHCP is in
 * progress, 0 with an address when it finishes. The game accepts the lease
 * only when the low three bytes match the ALL.net range 192.168.37.x
 * (0x25a8c0 little-endian at dword 1 of the output struct). Fake a lease on
 * that range so the seqInitNetwork DHCP wait completes and the loader can
 * advance. */
static void h_amos_netprop(CPU *c)
{
    uint32_t out = A32(0);
    if (out) {
        wr32(out + 0x00u, 0);          /* state */
        wr32(out + 0x04u, 0x0025a8c0u);/* IP = 192.168.37.x (le 3 bytes) */
        wr32(out + 0x08u, 0x00ffffffu);/* netmask 255.255.255.0 */
        wr32(out + 0x0cu, 0x0125a8c0u);/* gateway 192.168.37.1 */
        wr32(out + 0x10u, 0x0125a8c0u);/* dns 192.168.37.1 */
        wr32(out + 0x14u, 0);
        wr32(out + 0x18u, 0);
        wr32(out + 0x1cu, 0);
    }
    RET(0);
}

/* amOsinfoGetDhcpStatusEth0 — status enum for the same wait. 1 = lease
 * complete; anything else keeps the game polling. */
static void h_amos_dhcp(CPU *c) { RET(1); }

/* A base board serial, in the shape the cabinet's own would take. */
static void h_serial_id(CPU *c)
{
    static const char id[] = "AAFE-01A00000000";
    uint32_t out = A32(0);
    if (out) memcpy((void *)(uintptr_t)out, id, sizeof id);
    RET(AM_OK);
}

/* ---- the cabinet's non-volatile memory ----
 *
 * Bookkeeping, settings and credits live in two places on a Lindbergh: a
 * battery-backed SRAM on the base board and a small EEPROM. The game reaches
 * both through four wrapper functions, and everything above them - record
 * layout, duplicate copies, CRCs, validation - is the game's own code, which
 * works perfectly well as long as the bytes come back.
 *
 * So this is the whole of it: two byte arrays that persist to a file. Nothing
 * models a record, because nothing needs to.
 */

#define SRAM_SIZE   0x40000u
#define EEPROM_SIZE 0x10000u

static unsigned char g_sram[SRAM_SIZE];
static unsigned char g_eeprom[EEPROM_SIZE];
static int           g_nvram_loaded;

static const char *nvram_path(void)
{
    static char path[512];
    if (!*path) {
        const char *p = getenv("LINDBERGH_NVRAM");
        snprintf(path, sizeof path, "%s", (p && *p) ? p : "lindbergh_nvram.bin");
    }
    return path;
}

static void nvram_load(void)
{
    if (g_nvram_loaded) return;
    g_nvram_loaded = 1;
    FILE *f = fopen(nvram_path(), "rb");
    if (!f) return;
    if (fread(g_sram, 1, sizeof g_sram, f) != sizeof g_sram ||
        fread(g_eeprom, 1, sizeof g_eeprom, f) != sizeof g_eeprom) {
        /* A short or corrupt file is not worth keeping: the game rebuilds its
         * records from defaults the moment it finds them invalid. */
        memset(g_sram, 0, sizeof g_sram);
        memset(g_eeprom, 0, sizeof g_eeprom);
    }
    fclose(f);
}

static void nvram_save(void)
{
    FILE *f = fopen(nvram_path(), "wb");
    if (!f) return;
    fwrite(g_sram, 1, sizeof g_sram, f);
    fwrite(g_eeprom, 1, sizeof g_eeprom, f);
    fclose(f);
}

/* The wrappers answer 0, or -12 the way the real ones do when the base board
 * does not reply. An access outside the store is that failure. */
#define AM_BACKUP_FAIL (-12)

static int nvram_copy(unsigned char *store, unsigned size, uint32_t guest,
                      uint32_t addr, uint32_t len, int writing)
{
    nvram_load();
    if (!guest || addr > size || len > size - addr) return AM_BACKUP_FAIL;
    void *g = (void *)(uintptr_t)guest;
    if (writing) { memcpy(store + addr, g, len); nvram_save(); }
    else           memcpy(g, store + addr, len);
    return AM_OK;
}

/* Each wrapper reorders its arguments on the way down to a different device,
 * so which of the three is the buffer is not the same for any two of them.
 * LINDBERGH_NVLOG=1 prints all three and lets their magnitude say: a guest
 * pointer is up in the image, an offset is small. Read takes the offset
 * first, write takes the buffer first - reading both the same way puts a
 * pointer's value in as an address and reports a broken record. */
static void nv_log(const char *who, CPU *c)
{
    if (getenv("LINDBERGH_NVLOG"))
        fprintf(stderr, "[nv] %-10s a0=0x%08X a1=0x%08X a2=0x%08X\n",
                who, A32(0), A32(1), A32(2));
}

/* The EEPROM, reached directly rather than through the backup record layer.
 *
 * OutRun 2 SP SDX saves its assignment data by calling amEepromRead and
 * amEepromWrite itself. Their argument orders differ from each other the same
 * way the SRAM wrappers do - read takes the address first, write takes the
 * buffer first - and the address is a sixteen bit word in both. */
static void h_eeprom_read_api(CPU *c)
{
    nv_log("eepromRead", c);
    RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(1), A32(0) & 0xFFFFu, A32(2), 0));
}

static void h_eeprom_write_api(CPU *c)
{
    nv_log("eepromWrite", c);
    RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(0), A32(1) & 0xFFFFu, A32(2), 1));
}

static void h_sram_read(CPU *c)
{ nv_log("sramRead", c);  RET(nvram_copy(g_sram, SRAM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_sram_write(CPU *c)
{ nv_log("sramWrite", c); RET(nvram_copy(g_sram, SRAM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* The EEPROM pair reorders differently again and has not been reached yet;
 * these follow the SRAM shape and the log will say if that is wrong. */
static void h_eeprom_read(CPU *c)
{ nv_log("eepRead", c);   RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_eeprom_write(CPU *c)
{ nv_log("eepWrite", c);  RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* ---- the error the blank store raises ----
 *
 * acpSystem keeps one error code in a global and paints it over the screen
 * once set; anything other than -1 or -2 comes out as "Error 15 - Game
 * Program Not Found". The backup layer sets -3 when its records fail their
 * CRC, which they do here because the store is blank: the game's own repair
 * path writes through an EEPROM that lives on an I2C bus this runtime does
 * not have, so the records can never become valid and the error is permanent.
 *
 * A cabinet ships with that store already written. Suppressing this one code
 * is the same statement - the bookkeeping is assumed good - and it is scoped
 * to exactly that: every other error still reaches the game untouched, and
 * the suppression announces itself once on the way past.
 *
 * setError is reimplemented rather than skipped, which means knowing the two
 * globals it touches. Those are read out of the function's own first bytes
 * instead of being written down here, so this survives a different build of
 * the same framework:
 *
 *     55                 push ebp
 *     A1 <error var>     mov  eax, [error var]
 *     80 3D <latch> 00   cmp  byte [latch], 0
 *     ...
 *     A3/89 05 <var>     mov  [error var], eax
 */
#define ACP_ERR_BACKUP (-3)

static uint32_t g_err_var, g_err_latch;

static uint32_t rd32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int find_error_globals(uint32_t va)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)va;
    for (unsigned i = 0; i < 24; i++) {
        if (!g_err_var && p[i] == 0xA1) g_err_var = rd32le(p + i + 1);
        if (!g_err_latch && p[i] == 0x80 && p[i + 1] == 0x3D)
            g_err_latch = rd32le(p + i + 2);
    }
    return g_err_var != 0;
}

static void h_set_error(CPU *c)
{
    int32_t v = (int32_t)A32(0);

    if (v == ACP_ERR_BACKUP) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[sega] backup records are blank and cannot be "
                            "initialised without the EEPROM bus; treating the "
                            "bookkeeping as good\n");
        }
        RET(0);
        return;
    }

    /* Faithful otherwise: the original only stores while the latch is clear,
     * so that the first error to happen is the one that gets reported. */
    if (g_err_var &&
        (!g_err_latch || *(const unsigned char *)(uintptr_t)g_err_latch == 0))
        *(uint32_t *)(uintptr_t)g_err_var = (uint32_t)v;

    if (getenv("LINDBERGH_ACPERR")) {
        fprintf(stderr, "[acp] setError(%d) from:\n", (int)v);
        guest_backtrace(c);
    }
    RET(0);
}

/* ---- yabukita serializer guards ----
 *
 * ObjectDatabase::getObject(int) has no bounds check: when the objects vector
 * is empty (count == 0) the lifted code falls through with eax=0 and reads
 * [0+0xC], i.e. address 0x0000000C.  On Linux the null page is unmapped and
 * the process faults there.  On PS5 the page appears readable and returns 0,
 * so execution continues into Object::link(NULL) which then faults.
 *
 * Return NULL explicitly when the array is empty or the index is out of range,
 * matching what the game would have returned from a populated DB that has no
 * entry at that slot.
 *
 * Object::link() is called with that NULL `this` by deserializeFromFile when
 * getObject() fails.  Guard it too: the function's only job is to bump a
 * refcount, so a NULL receiver is a no-op with a diagnostic.  The caller gets
 * a clean error-code return (3) instead of a crash. */
static void h_yab_getobject(CPU *c)
{
    uint32_t db  = A32(0);
    uint32_t idx = A32(1);
    uint32_t vec = db + 0xACu;
    uint32_t cnt = rd32(vec + 0x0Cu);
    if (!cnt || idx >= cnt) { RET(0); return; }
    uint32_t data = rd32(vec + 4u) + idx * 20u;
    RET(rd32(data + 0x0Cu));
}

static void h_yab_link(CPU *c)
{
    uint32_t obj = A32(0);
    if (!obj) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[yabukita] Object::link(NULL) suppressed — "
                            "getObject returned NULL (empty ObjectDatabase after "
                            "failed .efo deserialize); this .efo will be absent\n");
        }
        return;
    }
    *(uint32_t *)(uintptr_t)(obj + 4u) += 1u;
}

/* ---- the security dongle ----
 *
 * A Lindbergh cabinet carries a hardware dongle, and a game does not simply
 * ask whether it is present. Ghost Squad Evolution generates sixteen random
 * bytes, has the dongle transform them, and checks the answer. The key for
 * that transform lives inside the device; it is not in the disc, not in the
 * binary, and not recoverable from either.
 *
 * So there is nothing to emulate here and no cleverness that would help. The
 * cabinet this game ran on had the dongle fitted, and the runtime makes the
 * same statement - the licence is present - by answering the one predicate
 * the game gates on, and saying so once on the way past. The challenge and
 * response themselves are left entirely alone.
 *
 * Bound by symbol name, so it applies to a game that has these and is simply
 * absent from one that does not. */
static void h_dongle_valid(CPU *c)
{
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "[sega] no security dongle is fitted; treating the "
                        "licence as present\n");
    }
    RET(1);
}

void hle_register_sega(void)
{
    int n = 0;

    /* yabukita ObjectDatabase::getObject and Object::link — null-guards so
     * a failed .efo deserialize returns a clean error rather than faulting.
     * ID4 needs the real link() — it wires parsed objects into the db; a stub
     * leaves every ObjectDatabase empty. The guards only fire on empty or
     * out-of-range lookups and NULL receivers, so they cannot shadow a
     * healthy deserialize. */
    n += guest_override("_ZNK8yabukita14ObjectDatabase9getObjectEi", h_yab_getobject);
    n += guest_override("_ZN8yabukita6Object4linkEv", h_yab_link);

    /* Ghost Squad Evolution gates its whole boot on this one predicate;
     * a game without these symbols simply does not get the override. */
    n += guest_override("_Z18is_dongle_validatev", h_dongle_valid);
    n += guest_override("_Z21check_dongle_validatev", h_dongle_valid);

    /* The base board. amLibInit failing is what produces "SEGA BaseBD not
     * available", and everything else is gated behind it. */
    n += guest_override("amLibInit", h_ok);
    n += guest_override("amLibExit", h_ok);
    n += guest_override("amLibIsBasebdAvailable", h_true);

    /* The JVS link itself. amJvsCheckInit is a predicate, not a status code:
     * amJvsGetNodes reads it as a boolean and answers -5 when it is false,
     * which is how answering it with the library's success value of 0 turned
     * into "-5 JVS node(s) found". */
    n += guest_override("amJvsInit", h_ok);
    n += guest_override("amJvsExit", h_ok);
    n += guest_override("amJvsCheckInit", h_true);

    /* One I/O board on the link - the cabinet's own, with the buttons, the
     * coin slots and the guns. */
    n += guest_override("amJvsGetNodes", h_one);

    /* The security dongle. Nothing here reads a real one, and the game only
     * asks whether it is present before carrying on. */
    n += guest_override("amDongleInit", h_ok);
    n += guest_override("amDongleExit", h_ok);
    n += guest_override("amDongleIsAvailable", h_dongle_ok);

    /* The dipswitches and the bookkeeping the game reads at start up. It
     * asks for these by name in its console when they are missing. */
    n += guest_override("amDipswInit", h_ok);
    n += guest_override("amDipswExit", h_ok);

    /* acpSystem::checkDongle polls amDongleUpdate and raises "Error 15 -
     * Game Program Not Found" the moment it answers anything but zero. */
    n += guest_override("amDongleUpdate", h_ok);
    n += guest_override("amDongleUserInfo",  h_dongle_userinfo);
    n += guest_override("amDongleUserInfoEx", h_dongle_userinfo);
    n += guest_override("amDongleIsDevelop",  h_true);
    n += guest_override("amDongleChipInfo",   h_ok);

    /* The base board's serial. The game prints it and stores it with its
     * bookkeeping; nothing here depends on the value, only on having one. */
    n += guest_override("amOsinfoGetBaseBoardSerialId", h_serial_id);

    /* The battery-backed store the bookkeeping lives in. */
    n += guest_override("amEepromInit", h_ok);
    n += guest_override("amEepromExit", h_ok);
    n += guest_override("amBackupWrapper_BbBuSramRead", h_sram_read);
    n += guest_override("amBackupWrapper_BbBuSramWrite", h_sram_write);
    n += guest_override("amBackupWrapper_KeyEepromRead", h_eeprom_read);
    n += guest_override("amBackupWrapper_KeyEepromWrite", h_eeprom_write);
    n += guest_override("amEepromRead", h_eeprom_read_api);
    n += guest_override("amEepromWrite", h_eeprom_write_api);

    /* LINDBERGH_NO_JVS=1 leaves the board unanswered, so the game finds no
     * I/O and says so. A test for whether a fault lives in this transport or
     * merely happens to be near it. */
    if (!getenv("LINDBERGH_NO_JVS")) {
        n += guest_override("amJvsSendRequest", h_jvs_send);
        n += guest_override("amJvsRecvAcknowledge", h_jvs_recv);
    }

    /* Ethernet link probe. isEthLinkUp() does socket()+ioctl(SIOCGIFFLAGS)
     * on a NIC that does not exist here — socket() returns -1 and the game
     * reads "link down". The cabinet loader's job dispatch waits for link-up
     * before posting work, so its worker threads park at L_0837909A forever
     * and the loading screen never advances. lindbergh-loader detours this
     * exact function to stubRetOne for the same reason. */
    n += guest_override("_Z11isEthLinkUpPKc", h_true);

    /* The ALL.net DHCP wait. tickWaitDHCP polls amOsinfoGetNetworkProperty
     * for a lease on the cabinet's 192.168.37.x range — with no NIC the real
     * call returns -2 forever and the loader sequence never advances. Answer
     * a fake lease so the sequence moves on. */
    n += guest_override("amOsinfoGetNetworkProperty", h_amos_netprop);
    n += guest_override("amOsinfoGetDhcpStatusEth0", h_amos_dhcp);

    /* Shader-source freshness check. The loader detours this to stubRetOne
     * so the game always thinks the .fx source is newer than its cache and
     * recompiles — with the real mtime compare ours returns false and the
     * whole compile+upload path is skipped. */
    n += guest_override("_ZN14cShaderUtility18isExistNewerSourceEPKcS1_S1_PS1_", h_true);

    /* Patch #13 — init-sequence gates that park the state machine:
     * seqInitPairLink blocks on a cabinet-link address that never arrives;
     * cFeedbackSCI::initPolling blocks on absent SCI force-feedback hardware;
     * seqInitNetwork::onExit has a DHCP wait the loader skips with a JMP. */
    n += guest_override("_ZN15seqInitPairLink15tickInitAddressEv", h_pair_link_skip);
    n += guest_override("_ZN12cFeedbackSCI11initPollingEl",         h_true);
    n += guest_override("_ZN14seqInitNetwork6onExitEPv",            h_ok);
    /* NOTE: seqStartup::onTick stays UN-overridden. Clearing the latched
     * hardware-error code pushed seqStartup into its teardown/advance path,
     * which dereferences audio/CRI objects our stub audio never initialized →
     * segv-at-4 storm. The error state (invisible seqErrorDisp) is the game's
     * *designed* response to missing cabinet hardware; forcing past it needs a
     * working CRI/audio subsystem, not a code clear. */

    /* seqStartup latches a hardware-error code at [g_status+0x11c] when a
     * cabinet check fails; 0xf = keychip/dongle missing. The watchdog only
     * latches when [g_dongle+0x661]==0 (no dongle data). Mark the dongle present
     * and no-op the two check functions so no error is raised — the game then
     * proceeds to seqGame instead of sitting on seqErrorDisp. */
    {   uint32_t dg = rd32(GVA(0x089c0940u));
        if (dg) wr8(GVA(dg + 0x661u), 1u); }
    n += guest_override("_ZN10seqStartup16checkKeyChipFuncEv", h_ok);
    n += guest_override("_ZN10seqStartup9checkEth0Ev",         h_ok);
    n += guest_override("_ZN16cSequenceManager6registEP9cSequencel", h_regist_log);

    /* lindbergh-loader ret patches — these functions are function-start VAs
     * absent from the lifted symbol table, so they're bound by address.
     * dispatch() hits the override before running the lifted body, which is
     * exactly what a ret at the first byte does on real hardware. */
    n += guest_override_va(0x08078B84u, h_ab3_probe, "ret:race-finish-blackscreen");
    n += guest_override_va(0x082E99ECu, h_ok, "ret:clSteerErrorDisp::run");
    n += guest_override_va(0x082D2414u, h_ok, "ret:clSerialLindbergh::send");
    n += guest_override_va(0x082D252Cu, h_ok, "ret:clSerialLindbergh::receive");
    /* 0x821B3EA cFeedbackSCI::initPolling is overridden by name above; bind the
     * VA too — if the symbol resolved to a different address the named hook
     * never fires, and kickback-init spins on hardware that isn't there. */
    n += guest_override_va(0x0821B3EAu, h_ok, "ret:kickback-init");


    /* ALL.net address self-configuration — the game tries to ioctl its own
     * IP onto eth0. Nothing to talk to; silence it. */
    n += guest_override("amOsinfoModifyNetworkAdr",      h_ok);
    n += guest_override("amOsinfoModifyNetworkProperty", h_ok);

    /* Patch #16 — ROOT CAUSE of the empty scene. seqInitialize::
     * childTerminationHandler dispatches via a jump table at 0x087BEB6C indexed
     * by [seqInit+4]; case 0 (the startup kick that launches the first child)
     * points to L_082DE30F = a bare return, so the init chain never starts and
     * seqInitialize stays state-0 forever. lindbergh-loader patchMemory
     * (0x087beb6c, "5b") redirects it to L_082DE35B (spawn seqInitPairLink). */
    wr32(GVA(0x087BEB6Cu), GVA(0x082DE35Bu));

    /* CRI bring-up — [0x8db8d20] is the criAu debug/voice callback the whole
     * criAuVoice layer dispatches through (`call edx` in DestroyBuffer/Play).
     * criAu_Initialize should set it to a real fn but it ends up as 8, so
     * every dispatch → `call 8` → segv. Point it at a bare `ret` so the call
     * is a harmless no-op; a real callback set later overwrites it. */
    wr32(GVA(0x08DB8D20u), 0u);   /* NULL cb → `if(edx!=0) call` skips it */
    /* CRIERR_CallErr dispatches through [0x89a0f58] (the global error cb)
     * whenever [0x8db332c] != -1; level -1 = suppress → returns without a call.
     * With errcb=0 + level!=-1 it dispatches to 0. Force suppress. */
    wr32(GVA(0x08DB332Cu), 0xFFFFFFFFu);
    n += guest_override("criAu_SetDebugCallback", h_cri_debugcb);
    n += guest_override("criAu_Initialize", h_criau_init);
    n += guest_override("criau_GetHandlePtr", h_cri_gethandle);
    n += guest_override("criHeap_Create", h_cri_heapcreate);
    /* The criAu context's error/warning/voice-cancel callbacks (+0x3c/+0x40/+0x44)
     * get the same `call`-dispatch — point them at a ret-stub too so a bad arg
     * can't crash the voice ops the same way the debug cb does. */
    n += guest_override("criAu_SetupErrCallback",         h_cri_setupcb_3c);
    n += guest_override("criAu_SetupWarningCallback",     h_cri_setupcb_40);
    n += guest_override("criAu_SetupVoiceCancelCallback", h_cri_setupcb_44);

    /* CRI audio middleware — criAu/criAuPly/ADXT/ADXCS/lbrna/criSnd/CRIDLG is
     * the game's bundled CRI ADX stack, initialised in caManager::C1 from
     * cApplication::create. It is structurally interdependent (voices → players
     * → decoders → renderers → work-queues share real vtables and fnptrs) so it
     * cannot be surface-stubbed: it must run for real on top of a working
     * SEGAAPI. The real bring-up lives in libsegaapi_ps5.c + the h_segapi_*
     * layer; enable it with LINDBERGH_AUDIO=1. */

    /* Patch #17 — the init chain's hardware-poll seqs. seqInitWheel polls the
     * steering-wheel IO board and seqInitCard polls the card reader, receipt
     * printer and card stocker — none present on PS5, so their ticks would
     * block the chain forever. Force each to done and kill it. */
    n += guest_override("_ZN12seqInitWheel6onTickEPv",        h_seq_tick_done);
    n += guest_override("_ZN11seqInitCard14tickInitializeEv",  h_seq_tick_done);
    n += guest_override("_ZN11seqInitCard14tickCleanErrorEv",  h_seq_tick_done);
    n += guest_override("_ZN11seqInitCard16tickCleanPrinterEv",h_seq_tick_done);
    n += guest_override("_ZN11seqInitCard16tickCleanStockerEv",h_seq_tick_done);
    n += guest_override("_ZN11seqInitModel6onTickEPv",         h_seq_init_model_tick);

    /* seqVSMatchingSelect is the network-matching screen: on a standalone
     * PS5 the matching handshake can never complete, and its tick path was
     * our segv-at-0x1f site (corrupt `this`/arg reaching tickInitMatching).
     * Kill the seq like the hardware-poll seqs so the chain advances to a
     * locally playable state. this may be garbage — range-check it. */
    n += guest_override("_ZN19seqVSMatchingSelect6onTickEPv",
                        h_seq_matching_done);
    n += guest_override("_ZN19seqVSMatchingSelect16tickInitMatchingEv",
                        h_seq_matching_done);
    n += guest_override("_ZN19seqVSMatchingSelect18tickMatchingSearchEv",
                        h_seq_matching_done);
    n += guest_override("_ZN19seqVSMatchingSelect18tickWaitModeSelectEv",
                        h_seq_matching_done);
    n += guest_override("_ZN19seqVSMatchingSelect14tickEntryErrorEv",
                        h_seq_matching_done);
    n += guest_override("_ZN19seqVSMatchingSelect14tickRivalFoundEv",
                        h_seq_matching_done);

    /* seqInitPairLink is the cabinet-to-cabinet link handshake.  Our
     * tickInitAddress bypass gets it past DHCP, but tickWaitPairLink /
     * tickInitPairLink still block waiting for a partner that can't exist.
     * Force every pair-link phase to complete so the init chain advances. */
    n += guest_override("_ZN15seqInitPairLink6onTickEPv",         h_seq_tick_done);
    n += guest_override("_ZN15seqInitPairLink14tickInitializeEv", h_seq_tick_done);
    n += guest_override("_ZN15seqInitPairLink16tickInitPairLinkEv",h_seq_tick_done);
    n += guest_override("_ZN15seqInitPairLink16tickWaitPairLinkEv",h_seq_tick_done);
    n += guest_override("_ZN15seqInitPairLink14tickPairLinkNGEv", h_seq_tick_done);

    /* seqAdvertise probe — runs the original, logs why the attract stalls */
    n += guest_override("_ZN12seqAdvertise6onTickEPv",            h_seqadv_tick);

    /* seqUserEntry — the "insert card / user entry" attract->entry gate.  No
     * card reader exists, so it waits forever.  Force it done so seqGame
     * advances to mode/car select. */
    n += guest_override("_ZN12seqUserEntry6onTickEPv",          h_seq_tick_done);

    /* _sSerial::_sSceneDatabase::isDependencyComplete — called on a NULL
     * scene-DB singleton (asset init we skipped / no HDD data); it reads
     * this+0xbc → segv at 0xbc.  Returning 1 (deps complete) lets the seq
     * proceed instead of dereferencing NULL. */
    /* ENGINE-INTERNAL STUBS REMOVED — run the game's real code (lindbergh
     * style: only the hardware boundary is faked).  Removed:
     *   isDependencyComplete, _sSceneDatabase::initialize, deleteTextureFile,
     *   deleteVertexArray, getShapeHeaderNumber, getTextureNumber, and all
     *   getSceneDatabase dummy-return variants — they masked the real
     *   deserialize and let half-built objects through. */
    /* FileStream::open — log every asset open so a failing deserialize names
     * the file it couldn't read. */
    n += guest_override("_ZN8yabukita10FileStream4openEPKcNS_6Stream11STREAM_MODEE",
                        h_fstream_open);
    n += guest_override("_ZN8_sSerial15_sSceneDatabase14createFromFileEPKcmm",
                        h_db_createfile);
    n += guest_override("_ZN8_sSerial15_sSceneDatabase16createFromMemoryEPvmmm",
                        h_db_createmem);
    /* assam::File::loadStreamRequest — log which assets get queued. */
    n += guest_override("_ZN5assam4File17loadStreamRequestEPKcPNS_10StreamInfoElm",
                        h_stream_req);
    n += guest_override("_ZN5assam11FileManager4openEPNS_4FileEPKcb",
                        h_fm_open);
    n += guest_override("_ZN5assam11FileManager11loadRequestEPNS_4FileE",
                        h_fm_loadreq);
    /* async scene-DB completion chain — which stages run? */
    n += guest_override("_ZN19cAsyncSceneDatabase16onLoadedResourceEPvl",
                        h_async_loaded);
    n += guest_override("_ZN19cAsyncSceneDatabase14_onDeserializeEP8cJobDesc",
                        h_async_ondes);
    n += guest_override("_ZN19cAsyncSceneDatabase13onDeserializeEv",
                        h_async_deser);
    n += guest_override("_ZN11AbkDataInst24deserializeSceneDatabaseEP7IzChunkm",
                        h_deser_scenedb);
    n += guest_override("_ZN10cJobThread6addJobEP8cJobDesc",   h_addjob);

    /* Sofdec video decoder — the MMX hot paths the lifter left as TODOs.
     * IDCT + motion-comp block copy; without them the movie decodes nothing. */
    /* A/B2 RESULT: removing this kills the Sofdec pipeline on PS5 (decode
     * thread dies → interleaved movie audio stops; process keeps rendering).
     * Lifted IDCT diverges on Orbis codegen — keep the override. */
    n += guest_override("DCT_IsseTrans",               h_dct_issetrans);
    /* A/B3 RESULT: same crash — lifted motion-comp also diverges on Orbis.
     * Both removed stubs are the MMX-lifted functions: shared MMX-emission
     * divergence on the PS5 toolchain, not two independent bugs. */
    n += guest_override("MPVMC08_OneRef1p_TuneAMmx",   h_mpvmc08);

    /* criAu bring-up diagnostic — run criAu for real and dump the init-param
     * struct to find why the object's call [obj+X] fields come out wrong. The
     * caManager cut is commented out: criAu_Initialize must run to be probed. */
    n += guest_override("criAu_SetupInitializeParameter_LINDBERGH", h_cri_setupparam);
    /* ADX decode probes — dump decoder inputs (coefs, LCG, src bytes) to find
     * why output is 0x8000/0x7fff alternation (divergent predictor). */
    n += guest_override_va(0x0876CA8Du, h_adx_ste,  "probe:ADX_DecodeSte4AsSte");
    n += guest_override_va(0x0876C53Cu, h_adx_mono, "probe:ADX_DecodeMono4");
    /* ADX_GetCoefficient — the game's x87 lift mis-computes coef2 to +0x1000,
     * diverging every decode. Host C version of the canonical CRI formula. */
    /* A/B: lifted ADX_GetCoefficient body now runs unmasked — if audio
     * regresses on PS5 with the fixed x87 stack this is a codegen divergence,
     * not a coef-formula bug. Keep disabled pending console listen test. */
    /* n += guest_override_va(0x087652C4u, h_adx_getcoef, "fix:ADX_GetCoefficient"); */

    /* ---- lindbergh-loader steals (REVD) ----
     * s_logMask = all bits on — makes the game narrate via putConsoleSeparate
     * (detoured below to klog so the debug path can't crash) and putConsole.
     * With no security board the dips read as all-off — 0x00. */
    wr32(GVA(0x08D97564u), 0x0FFFFFFFu);          /* s_logMask */
    /* lindbergh-loader showDebugMessages: am*DebugLevel = 2 so the AM libs
     * narrate their errors (addresses from `nm id4.elf`). */
    {
        static const uint32_t dbg[] = {
            0x08d972d0u /*amBackup*/,  0x08d972e0u /*amCredit*/,
            0x08d97538u /*amDipsw*/,   0x08d9753cu /*amDongle*/,
            0x08d97540u /*amEeprom*/,  0x08d97544u /*amHwmonitor*/,
            0x08d97548u /*amJvs*/,     0x08d9754cu /*amLib*/,
            0x08d97550u /*amMisc*/,    0x08d97554u /*amOsinfo*/,
            0x08d97558u /*amSysData*/, 0x08d97560u /*bcLib*/,
        };
        for (unsigned i = 0; i < sizeof dbg / sizeof dbg[0]; i++)
            wr32(GVA(dbg[i]), 2u);
    }
    n += guest_override("_ZN8_sSerial15_sSceneDatabase7LoadObj15procDeserializeEPvPN5candy12WorkerThreadE",
                        h_procdeser);
    n += guest_override("_ZN8_sRender11loadTextureEmPKN8_sSerial9_sTextureE",
                        h_loadtex);
    n += guest_override("lxci_read_func", h_lxci_read);
    n += guest_override("lxci_open_func", h_lxci_open);
    n += guest_override("lxci_Close", h_lxci_close);
    n += guest_override("_ZN8yabukita10Serializer21deserializeFromMemoryEPKhjNS0_11BUFFER_TYPEE",
                        h_desermem);
    n += guest_override("_ZN2es8glh_util20createTextureFromMemEPKviiRjS3_",
                        h_ctex);
    n += guest_override("_ZN2es8glh_util18putConsoleSeparateEPKcc",
                        h_put_console_sep);
    n += guest_override("amDipswGetData", h_dipsw_get);
    n += guest_override("amDipswSetLed",  h_ok);
    /* cgCreateProgram's `args` argument forced to 0 — the loader does this on
     * non-NVIDIA GPUs because Cg's arg list carries options the PS5 Cg path
     * can't take. 0854b4a0: nop nop; 085700d3: xor eax,eax; nop. */
    wr16(GVA(0x0854B4A0u), 0x9090u);
    wr32(GVA(0x085700D3u), 0x0090C031u);

    /* Patch #14 — EXP_REVD data patches (startup writes). FSAA on, avoid the
     * game's own fullscreen set, force renderbuffers to 1920x1080. The
     * addresses land in the original image; data-slot writes take effect for
     * anything the lifted code reads back through the guest address space. */
    wr8(GVA(0x0857f879u), 1u);           /* FSAA Enabled flag */
    wr32(GVA(0x089ed930u), 1u);          /* FSAA Quality = 1 */
    wr32(GVA(0x08580979u), 0x000126e9u); /* avoid full-screen mode set */
    wr32(GVA(0x0825728au), 1920u); wr32(GVA(0x08257282u), 1080u);
    wr32(GVA(0x0825734au), 1920u); wr32(GVA(0x08257342u), 1080u);
    wr32(GVA(0x0825740au), 1920u); wr32(GVA(0x08257402u), 1080u);
    wr32(GVA(0x08257469u), 1920u); wr32(GVA(0x08257461u), 1080u);
    wr32(GVA(0x08257d0fu), 1920u); wr32(GVA(0x08257d07u), 1080u);
    wr32(GVA(0x08257d9au), 1920u); wr32(GVA(0x08257d92u), 1080u);

    {   /* Only worth installing if the two globals can be found; without
         * them this would swallow every error instead of one. */
        uint32_t va = guest_symbol("_ZN9acpSystem8setErrorEl");
        if (va && find_error_globals(va))
            n += guest_override("_ZN9acpSystem8setErrorEl", h_set_error);
    }

    { extern void klogf(const char*, ...); klogf("[sega] %d overrides installed\n", n); }
}
