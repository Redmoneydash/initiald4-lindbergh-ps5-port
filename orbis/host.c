/*
 * host.c - run recompiled Initial D Arcade Stage 4 on PS5.
 *
 * The runtime maps the game where it was linked and builds the stack the
 * kernel would have; the lifted code is the game. This hands control over
 * at the ELF's entry point.
 *
 * ORBIS_EGL: real EGL context via ps5-opengl. eglGetProcAddress supplies GL.
 * ORBIS_PAD: DualSense input via scePad, mapped to the JVS cabinet model.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "lindbergh_rt.h"

/* -------------------------------------------------------------------------
 * EGL backend
 * ------------------------------------------------------------------------- */
#ifdef ORBIS_EGL

#include <EGL/egl.h>
#include <EGL/eglext.h>

static void egl_km(const char *m);
#define kmf(fmt, ...) do { char _b[160]; sprintf(_b, fmt, ##__VA_ARGS__); egl_km(_b); } while (0)

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_srf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

static void egl_km(const char *m)
{
    extern int sceKernelDebugOutText(int, const char*);
    sceKernelDebugOutText(0, m);
}

/* Bridge for the OpenGL archive objects: libps5_opengl_core33.a members
 * bind sceKernelDebugOutText to libkernel_stub_weak.so (libkernel_web.sprx),
 * which is not loaded — their debug output vanishes. This TU's import lands
 * on libkernel.prx's working NID, so driver klog calls route through here. */
void ps5_drv_debug_text(int channel, const char *text)
{
    extern int sceKernelDebugOutText(int, const char*);
    sceKernelDebugOutText(channel, text);
}

/* The game calls glBegin/glVertex* before (and on other threads than) its
 * glXMakeCurrent — without a current context Mesa's glapi stubs deref a NULL
 * dispatch table. Make the shared context current on whichever thread touches
 * GL first. */
int orbis_egl_make_current(void)
{
    if (s_ctx == EGL_NO_CONTEXT || s_srf == EGL_NO_SURFACE) return 0;
    egl_km("[egl] make_current call");
    int ok = eglMakeCurrent(s_dpy, s_srf, s_srf, s_ctx) ? 1 : 0;
    egl_km(ok ? "[egl] make_current ok" : "[egl] make_current ret 0");
    return ok;
}

static pthread_mutex_t s_egl_open_mutex = PTHREAD_MUTEX_INITIALIZER;

void *orbis_egl_open(int w, int h)
{
    (void)w; (void)h;
    pthread_mutex_lock(&s_egl_open_mutex);
    if (s_ctx != EGL_NO_CONTEXT) {
        pthread_mutex_unlock(&s_egl_open_mutex);
        return s_ctx;
    }
    egl_km("[egl] open enter");
    s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s_dpy == EGL_NO_DISPLAY) {
        kmf("eglGetDisplay failed");
        pthread_mutex_unlock(&s_egl_open_mutex);
        return NULL;
    }

    EGLint major, minor;
    if (!eglInitialize(s_dpy, &major, &minor)) {
        kmf("eglInitialize failed: %#x", eglGetError());
        pthread_mutex_unlock(&s_egl_open_mutex);
        return NULL;
    }
    kmf("EGL %d.%d", major, minor);

    eglBindAPI(EGL_OPENGL_API);
    kmf("bindAPI done");

    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        kmf("eglChooseConfig failed: %#x", eglGetError());
        pthread_mutex_unlock(&s_egl_open_mutex);
        return NULL;
    }
    kmf("config n=%d cfg=%p", n, (void *)cfg);

    /* PS5 ps5-opengl: window handle 0 → the console's VideoOut surface.
     * eglCreateWindowSurface returns EGL_BAD_ALLOC (0x3003) while the shell
     * splash still owns VideoOut. The splash releases shortly after our
     * process is handed focus — retry for up to ~5s. Observed in klog_ab2:
     * both open attempts failed back-to-back; a second (unprotected) caller
     * tried immediately with the splash still up. Retrying inside the mutex
     * means the first caller blocks until the surface is available and
     * subsequent callers see s_ctx set and return straight away. */
    kmf("createWindowSurface call");
    s_srf = EGL_NO_SURFACE;
    for (int attempt = 0; attempt < 50; attempt++) {
        s_srf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)0, NULL);
        if (s_srf != EGL_NO_SURFACE) break;
        EGLint err = eglGetError();
        if (attempt == 0 || (attempt % 10) == 0)
            kmf("eglCreateWindowSurface attempt %d: %#x", attempt, err);
        if (err != 0x3003) break;   /* only retry on EGL_BAD_ALLOC */
        usleep(100 * 1000);
    }
    if (s_srf == EGL_NO_SURFACE) {
        kmf("eglCreateWindowSurface giving up");
        pthread_mutex_unlock(&s_egl_open_mutex);
        return NULL;
    }
    kmf("surface ok %p", (void *)s_srf);

    /* The game is a 2008-era fixed-function app — glBegin/glEnd/glVertex and
     * ARB programs are ILLEGAL in a core context and get dropped silently,
     * which was the black screen: swaps succeeded but nothing ever drew.
     * Ask for compatibility profile first; unprofiled as a last resort. */
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
        EGL_NONE
    };
    kmf("createContext call");
    s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (s_ctx == EGL_NO_CONTEXT) {
        static const EGLint ctx_any[] = { EGL_NONE };
        s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_any);
    }
    if (s_ctx == EGL_NO_CONTEXT) {
        kmf("eglCreateContext failed: %#x", eglGetError());
        pthread_mutex_unlock(&s_egl_open_mutex);
        return NULL;
    }
    kmf("ctx ok %p", (void *)s_ctx);

    kmf("makeCurrent call");
    eglMakeCurrent(s_dpy, s_srf, s_srf, s_ctx);
    kmf("makeCurrent done");
    eglSwapInterval(s_dpy, 1);
    kmf("ctx current srf=%p err=%#x", (void *)s_srf, eglGetError());
    /* Verify the profile — a core context drops glBegin/glEnd + ARB programs
     * silently. If this logs "core", everything above the GL layer is fine
     * and the black screen is a profile problem, not a game-logic one. */
    { typedef void (*PGI)(unsigned int, int *);
      PGI gi = (PGI)eglGetProcAddress("glGetIntegerv");
      if (gi) {
          int prof = 0;
          gi(0x9126 /*GL_CONTEXT_PROFILE_MASK*/, &prof);
          kmf("gl profile mask=%#x compat=%d", prof, !!(prof & 0x2));
      } }

    EGLint sw = w, sh = h;
    eglQuerySurface(s_dpy, s_srf, EGL_WIDTH,  &sw);
    eglQuerySurface(s_dpy, s_srf, EGL_HEIGHT, &sh);
    kmf("surface %dx%d ctx=%p", sw, sh, (void *)s_ctx);
    pthread_mutex_unlock(&s_egl_open_mutex);
    return (void *)s_ctx;
}

void orbis_egl_swap(void)
{
    static unsigned n;
    n++;
    if (s_dpy == EGL_NO_DISPLAY || s_srf == EGL_NO_SURFACE) {
        if ((n % 600) == 1)
            kmf("swap #%u no surface (dpy=%p srf=%p)", n, (void *)s_dpy, (void *)s_srf);
        return;
    }
    /* Display-leg probe: every 150th swap, paint the back buffer solid red
     * before presenting. If the TV flashes red every ~5s, fb0->scanout->panel
     * works and the dark screen means the game's own rendering is dark; if no
     * flash, the present path drops frames before the panel. */
    if ((n % 150u) == 0u) {
        static int resolved;
        static void (*p_cc)(float, float, float, float);
        static void (*p_cl)(unsigned);
        static void (*p_bfb)(unsigned, unsigned);
        static void (*p_gi)(unsigned, int *);
        if (!resolved) {
            resolved = 1;
            p_cc = (void (*)(float,float,float,float))eglGetProcAddress("glClearColor");
            p_cl = (void (*)(unsigned))eglGetProcAddress("glClear");
            p_bfb = (void (*)(unsigned,unsigned))eglGetProcAddress("glBindFramebufferEXT");
            p_gi = (void (*)(unsigned,int*))eglGetProcAddress("glGetIntegerv");
        }
        if (p_cc && p_cl && p_bfb && p_gi) {
            int prev = -1;
            p_gi(0x8CA6 /*GL_FRAMEBUFFER_BINDING_EXT*/, &prev);
            p_bfb(0x8D40 /*GL_FRAMEBUFFER_EXT*/, 0);
            p_cc(1.0f, 0.0f, 0.0f, 1.0f);
            p_cl(0x4000 /*GL_COLOR_BUFFER_BIT*/);
            p_bfb(0x8D40, (unsigned)prev);
            kmf("swap #%u FLASHRED prevfb=%d", n, prev);
        }
    }
    eglSwapBuffers(s_dpy, s_srf);
    if ((n % 600) == 1) {
        /* eglGetError shows whether the present path engaged; the ps5-agc
         * gate2 routines print failures to stdout, which we can't see. */
        kmf("swap #%u err=%#x", n, eglGetError());
    }
}

#endif /* ORBIS_EGL */

/* -------------------------------------------------------------------------
 * scePad / DualSense input backend
 *
 * ScePadData is 120 bytes; offsets verified against ps5_pad.hpp static_asserts:
 *   offsetof(Data, touch)       == 0x34
 *   offsetof(Data, connected)   == 0x4c
 *   offsetof(Data, timestamp_us)== 0x50
 * ------------------------------------------------------------------------- */
#ifdef ORBIS_PAD

extern int sceUserServiceInitialize(void *params);
extern int sceUserServiceGetInitialUser(int *user_id);
extern int scePadInit(void);
extern int scePadOpen(int user_id, int port_type, int index, const void *params);
extern int scePadReadState(int handle, void *data);

/* Raw 120-byte pad sample — accessed by fixed offsets */
typedef struct { unsigned char b[120]; } ScePadData;

static int s_pad_handle = -1;
static ScePadData s_pad;

/* Field accessors */
static inline uint32_t pad_buttons(void) {
    uint32_t v;
    __builtin_memcpy(&v, s_pad.b + 0, 4);
    return v;
}
static inline uint8_t pad_ls_x(void) { return s_pad.b[4]; }   /* 0=left, 255=right */
static inline uint8_t pad_r2(void)   { return s_pad.b[8]; }   /* 0=released, 255=floored */
static inline uint8_t pad_l2(void)   { return s_pad.b[9]; }
static inline int32_t pad_connected(void) {
    int32_t v;
    __builtin_memcpy(&v, s_pad.b + 0x4c, 4);
    return v;
}

/* Button masks from ps5_pad.hpp */
#define PAD_CROSS    0x00004000u
#define PAD_TRIANGLE 0x00001000u
#define PAD_CIRCLE   0x00002000u
#define PAD_SQUARE   0x00008000u
#define PAD_L1       0x00000400u
#define PAD_R1       0x00000800u
#define PAD_L2_BTN   0x00000100u
#define PAD_R2_BTN   0x00000200u
#define PAD_OPTIONS  0x00000008u
#define PAD_CREATE   0x00000001u
#define PAD_UP       0x00000010u
#define PAD_DOWN     0x00000040u
#define PAD_LEFT     0x00000080u
#define PAD_RIGHT    0x00000020u
#define PAD_TOUCHPAD 0x00100000u

static void pad_init(void)
{
    sceUserServiceInitialize(NULL);
    int user_id = 0;
    sceUserServiceGetInitialUser(&user_id);
    scePadInit();
    /* kPortTypeStandard = 0 */
    s_pad_handle = scePadOpen(user_id, 0, 0, NULL);
    fprintf(stderr, "[pad] handle=%d user=%d\n", s_pad_handle, user_id);
}

void orbis_pad_poll(void)
{
    if (s_pad_handle < 0) return;
    scePadReadState(s_pad_handle, &s_pad);
}

void orbis_pad_state(CabinetInput *out)
{
    if (s_pad_handle < 0 || !pad_connected()) return;

    uint32_t btn = pad_buttons();

    /* Digital buttons */
    if (btn & PAD_CROSS)    out->buttons |= CAB_P1_START;
    if (btn & PAD_CIRCLE)   out->buttons |= CAB_P2_START;
    if (btn & PAD_TRIANGLE) out->buttons |= CAB_TEST;
    if (btn & PAD_SQUARE)   out->buttons |= CAB_SERVICE;
    if (btn & PAD_TOUCHPAD) out->buttons |= CAB_P1_TRIGGER;

    /* Analog steering: left stick X → gun_x (0..screen_w).
     * stick 128 = center, 0 = full left, 255 = full right. */
    int w = out->screen_w > 0 ? out->screen_w : 1360;
    int h = out->screen_h > 0 ? out->screen_h : 768;
    out->gun_x = (int)((unsigned)pad_ls_x() * (unsigned)w / 255u);

    /* Analog gas/brake remapped to gun_y channel:
     * R2 accelerates (pushes gun_y toward screen_h),
     * L2 brakes (pulls it toward 0). Net pedal position is r2-l2. */
    int gas   = (int)pad_r2();
    int brake = (int)pad_l2();
    int net   = gas - brake;          /* -255..255 */
    out->gun_y = (int)(((net + 255) * (long)h) / 510);

    /* Driving cabinet channels (ID4 JVS): ch0 steer, ch1 accel, ch2 brake —
     * all 0..65535, steer centred at 32768. */
    out->steer = (int)((unsigned)pad_ls_x() * 65535u / 255u);
    out->accel = gas   * 257;         /* 0..255 -> 0..65535 */
    out->brake = brake * 257;

    /* Coin insert: Options button pulses a coin */
    static uint32_t s_prev_btn;
    if ((btn & PAD_OPTIONS) && !(s_prev_btn & PAD_OPTIONS))
        out->coins[0]++;
    s_prev_btn = btn;
}

#endif /* ORBIS_PAD */

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
extern int sceKernelDebugOutText(int ch, const char *t);
static int g_log_sock = -2;   /* -2 = not yet tried */
static pthread_once_t g_log_sock_once = PTHREAD_ONCE_INIT;
static void g_log_sock_init(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_in a;
        __builtin_memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port   = __builtin_bswap16(4444);
        a.sin_addr.s_addr = (192u) | (168u<<8) | (137u<<16) | (1u<<24);
        if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
            close(s); s = -1;
        }
    }
    g_log_sock = s;
}
static void km(const char *m){
    sceKernelDebugOutText(0, m);
    pthread_once(&g_log_sock_once, g_log_sock_init);
    if (g_log_sock >= 0) {
        size_t n = strlen(m);
        send(g_log_sock, m, n, 0);
    }
}

/* The SIGPROF watchdog can't run under the managed sandbox, so this ordinary
 * host thread samples the guest CPUs on a timer instead. */
extern int  usleep(unsigned);
extern void guest_dump_eips(void);
extern uint32_t g_image_delta;
static uint32_t host_rd32(uint32_t abs) { return *(uint32_t *)(uintptr_t)(abs + g_image_delta); }
static void *eip_watcher(void *unused)
{
    (void)unused;
    for (;;) {
        usleep(3000000);
        guest_dump_eips();
        /* seq manager state — which sequence is live tells us where the
         * game is parked (init vs title vs race) while host_swap never
         * fires.  Same slots as hle_window_orbis.c's seqdump. */
        { extern unsigned g_frame; extern uint32_t g_seqmgr;
          extern int guest_mapped(uint32_t);
          uint32_t gm = g_seqmgr;
          char b[256]; int o = snprintf(b, sizeof b, "[seqw] gm=%#x frame=%u ", gm, g_frame);
          if (guest_mapped(gm))
              for (int i = 0; i < 16 && o < (int)sizeof b - 20 && guest_mapped(gm + 8u + i * 4u); i++) {
                  uint32_t s = host_rd32(gm + 8u + i * 4u);
                  if (!guest_mapped(s)) continue;
                  o += snprintf(b + o, sizeof b - o, "s%d=%#x(st%d) ", i,
                                host_rd32(s), (int)host_rd32(s + 0x40u));
              }
          sceKernelDebugOutText(0, b); }
    }
    return NULL;
}

static CPU *s_cpu;
static void *disp_main(void *a) {
    extern void guest_set_current_cpu(CPU *);
    /* The guest runs on this dispatch thread, but guest_init_cpu set g_current
     * on the main thread — without this, every fault on the dispatch thread
     * reports "(no CPU registered)" and the crash dump loses the guest eip,
     * register and return chain. Register it so fault reports are real. */
    guest_set_current_cpu(s_cpu);
    dispatch(s_cpu, guest_entry());
    return NULL;
}

int main(int argc, char **argv)
{
    km("[host] main entered\n");
    kmf("[host] main=%p\n", (void *)&main);   /* text-slide anchor for offline addr2line of crash mc[] fields */
    /* SIGPROF's sigreturn trampoline is a banned syscall in a managed app. */
    setenv("LINDBERGH_NO_WATCHDOG", "1", 0);
    setenv("TEA_DIR", "/app0", 0);
    /* Real SEGAAPI audio path — creates the criAuPly/ADX objects that
     * seqStartup's transition teardown destroys. The earlier audio-thread
     * rip=0x8 crash was the h_sem_init reinit bug (BUG H), now fixed — retest
     * the real mixer so startup can advance instead of faulting on NULL
     * handle slots. */
    setenv("LINDBERGH_AUDIO", "1", 0);   /* real SEGAAPI — criAu builds real voices on it */
    setenv("LINDBERGH_CONSOLE", "1", 0);
    setenv("LINDBERGH_OPENS", "1", 0);
    /* Redirect fds 1+2 to the kernel log — a managed app's stdio goes nowhere
     * otherwise. /dev/klog accepts writes; etaHEN relays it on port 3232.
     * fd 1 matters because ps5-agc reports its present/flip path via printf. */
    {
        /* fd-redirect to /dev/klog — the managed sandbox's file paths are
         * overlaid so writes never reach the FTP-visible tree. */
        int lfd = open("/dev/klog", O_WRONLY);
        if (lfd >= 0) { dup2(lfd, 2); dup2(lfd, 1); close(lfd); }
        setvbuf(stderr, NULL, _IONBF, 0);   /* process never exits - unbuffered */
        setvbuf(stdout, NULL, _IONBF, 0);
    }
    km("[host] after klog redirect\n");
    fprintf(stderr, "[host] start\n");

    /* Debug toggles no launcher can pass: drop empty files next to the ELF
     * to flip these without a rebuild. */
    if (access("/app0/permissive", F_OK) == 0 ||
        access("/user/homebrew/id4/permissive", F_OK) == 0) {
        setenv("LINDBERGH_HLE_PERMISSIVE", "1", 0);
        fprintf(stderr, "[host] HLE permissive survey ON\n");
    }
    if (access("/app0/hletrace", F_OK) == 0 ||
        access("/user/homebrew/id4/hletrace", F_OK) == 0) {
        setenv("LINDBERGH_HLE_TRACE", "1", 0);
        fprintf(stderr, "[host] HLE call trace ON\n");
    }
    if (access("/app0/nofbo", F_OK) == 0 ||
        access("/user/homebrew/id4/nofbo", F_OK) == 0) {
        setenv("LINDBERGH_NO_FBO", "1", 0);
        km("[host] NO_FBO mode ON\n");
    }
    if (access("/app0/fbstats", F_OK) == 0 ||
        access("/user/homebrew/id4/fbstats", F_OK) == 0) {
        setenv("LINDBERGH_FBSTATS", "1", 0);
        km("[host] FBSTATS ON\n");
    }
    if (access("/app0/progtrace", F_OK) == 0 ||
        access("/user/homebrew/id4/progtrace", F_OK) == 0 ||
        access("/data/homebrew/PPSA99405/progtrace", F_OK) == 0) {
        setenv("LINDBERGH_PROG_TRACE", "1", 0);
        km("[host] PROG_TRACE ON\n");
    }
    if (access("/app0/fpsolid", F_OK) == 0 ||
        access("/user/homebrew/id4/fpsolid", F_OK) == 0) {
        setenv("LINDBERGH_FP_SOLID", "1", 0);
        km("[host] FP_SOLID mode ON\n");
    }

    km("[host] after access checks\n");
    /* Prefer game data from the app bundle; fall back to the legacy payload
     * staging area so the same eboot.bin works in both contexts. */
    const char *game_elf;
    const char *game_dir;
    /* argv[1] is only the elf path when it names a real file — a managed
     * launch passes the title id ("PPSA99405"), which fopen cannot open. */
    FILE *arg_tf = (argc > 1) ? fopen(argv[1], "rb") : NULL;
    if (arg_tf) {
        fclose(arg_tf);
        game_elf = argv[1];
        game_dir = "/app0";
    } else {
        FILE *tf = fopen("/app0/id4_game.elf", "rb");
        if (tf) {
            fclose(tf);
            game_elf = "/app0/id4_game.elf";
            game_dir = "/app0";
        } else {
            game_elf = "/user/homebrew/id4/id4_game.elf";
            game_dir = "/user/homebrew/id4";
            fprintf(stderr, "[host] id4_game.elf not in /app0, using %s\n", game_elf);
        }
    }
    /* TEA_DIR must match game_dir so guest relative opens land in the right tree */
    setenv("TEA_DIR", game_dir, 1);
    fprintf(stderr, "[host] loading %s (TEA_DIR=%s)\n", game_elf, game_dir);

    if (chdir(game_dir) != 0)
        fprintf(stderr, "[host] warning: chdir(%s) failed\n", game_dir);
    else
        fprintf(stderr, "[host] cwd -> %s\n", game_dir);

#ifdef ORBIS_PAD
    km("[host] pad_init...\n");
    pad_init();
    km("[host] pad_init done\n");
#endif

    km("[host] before crash_handler\n");
    guest_install_crash_handler();
    km("[host] after crash_handler\n");

    {
        const char *cands[] = {
            "/app0/id4_game.elf", "/app0/initialD.ini", "/app0/id4.elf",
            "/app0/../id4_game.elf", "/download0/id4_game.elf",
            "/system_ex/app/PPSA99405/id4_game.elf",
            "/data/homebrew/PPSA99405/id4_game.elf", 0 };
        extern int sceKernelDebugOutText(int, const char*);
        extern int sprintf(char*,const char*,...);
        char b[160];
        for (int i = 0; cands[i]; i++) {
            FILE *t = fopen(cands[i], "rb");
            sprintf(b, "[probe] %s -> %s\n", cands[i], t ? "OK" : "fail");
            sceKernelDebugOutText(0, b); if (t) fclose(t);
        }
    }
    { extern int sceKernelDebugOutText(int, const char*);
      char b[160]; extern int sprintf(char*,const char*,...);
      sprintf(b, "[host] game_elf=%s argc=%d argv1=%s\n", game_elf, argc,
              argc>1?argv[1]:"-");
      sceKernelDebugOutText(0, b); }
    km("[host] before guest_load\n");
    if (guest_load(game_elf, argc - 1, argv + 1) != 0)
        { fprintf(stderr, "[host] load failed\n"); return 1; }
    fprintf(stderr, "[host] loaded\n");

    /* The SIGPROF watchdog can't run under the managed sandbox — sample the
     * guest CPUs on a timer from a plain host thread instead. */
    { static pthread_t wt;
      if (pthread_create(&wt, NULL, eip_watcher, NULL) != 0)
          km("[host] eip watcher spawn failed\n"); }

    km("[host] before hle_register_all\n");
    hle_register_all();
    km("[host] after hle_register_all\n");
    fprintf(stderr, "[host] registered\n");

    CPU cpu;
    km("[host] before guest_init_cpu\n");
    guest_init_cpu(&cpu);
    km("[host] after guest_init_cpu\n");
    fprintf(stderr, "[host] cpu init, entry=%#x\n", guest_entry());

    fprintf(stderr, "[host] dispatching\n");
    km("[host] before dispatch\n");
    /* Lifted calls consume host stack per guest call frame — deep guest
     * recursion (init chains, parsers) plus instrumented frames can blow the
     * thin main-thread stack. Run the dispatcher on a dedicated thread with
     * a large stack. */
    s_cpu = &cpu;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64ul << 20);
    pthread_t dt;
    if (pthread_create(&dt, &attr, disp_main, NULL) == 0) {
        pthread_join(dt, NULL);
    } else {
        km("[host] dispatch thread failed — running inline\n");
        dispatch(&cpu, guest_entry());
    }

    fprintf(stderr, "[host] the entry point returned - _start should not.\n");
    return 1;
}
