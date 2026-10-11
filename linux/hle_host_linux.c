/* hle_host_linux.c — desktop GL host for the recompiled game.
 *
 * The PS5 build gets a real EGL/GLES window from ps5-opengl (ORBIS_EGL);
 * the Windows build uses WGL. On Linux the same emit/replay path drives a
 * plain desktop EGL context: llvmpipe on the surfaceless platform, a pbuffer
 * standing in for the window, and a PPM dump each swap so frames can be
 * inspected without a display server. No X11, no GPU — deterministic.
 *
 * Provides the entry points the Orbis-cut runtime calls by name
 * (orbis_egl_*) plus the host_* plumbing the stub file used to fake.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include "lindbergh_rt.h"

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;
static int g_fb_w = 1920, g_fb_h = 1080;
static Display   *g_xdpy;
static Window     g_xwin;

/* ---- desktop cabinet input ----
 * Windowed run keyboard map (arcade front-end convention):
 *   5 / 6     coin player 1 / 2        1 / 2  start
 *   T         test                     S      service
 *   Left/Right  wheel                  Up/Down  gas / brake
 *   Enter / Shift  trigger / reload (ID4: shift up/down style buttons)
 * The wheel spring-recentres like the cabinet's; coins only count up. */
static CabinetInput g_cab_lin;
static int g_steer_dir;              /* -1 left, +1 right, 0 released */

void linux_cabinet_fill(CabinetInput *out)
{
    /* Called from hle_window_orbis.c host_cabinet_input() (weak link) —
     * merges live key state into the JVS answer. */
    if (g_steer_dir) {
        int v = g_cab_lin.steer + g_steer_dir * 0x2000;
        g_cab_lin.steer = v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v;
    } else if (g_cab_lin.steer != 0x8000) {
        int d = 0x8000 - g_cab_lin.steer;
        g_cab_lin.steer += d / 4 ? d / 4 : (d > 0 ? 1 : -1);
    }
    *out = g_cab_lin;
    out->screen_w = g_fb_w;
    out->screen_h = g_fb_h;
}

extern uint32_t g_image_delta;
extern int id4_dbg(int, const char *);

/* Live view of the game's input state blocks — the JVS interface never
 * initialized, so these globals hold whatever we write. */
static void id4_probe_input(void)
{
    static int s_n;
    if ((s_n++ % 300) != 0) return;
    uint8_t *node0 = (uint8_t *)(uintptr_t)(0x8d7b420u + g_image_delta);
    uint8_t *ports = (uint8_t *)(uintptr_t)(0x8d74840u + g_image_delta);
    uint32_t *ptab = (uint32_t *)(uintptr_t)(0x8d74e1cu + g_image_delta);
    char _b[192]; snprintf(_b, sizeof _b,
        "[jin] node0: 118=%02x 119=%02x 135=%02x 137=%02x 139=%02x | "
        "ptab=%08x %08x %08x %08x | port0 dig=%08x a1a=%04x\n",
        node0[0x118], node0[0x119], node0[0x135], node0[0x137], node0[0x139],
        ptab[0], ptab[1], ptab[2], ptab[3],
        *(uint32_t *)ports, *(uint16_t *)(ports + 0x1a));
    id4_dbg(0, _b);
}

/* The JVS device interface never initialized — the manager's port records and
 * node-info blocks sit zeroed and nothing overwrites them (executeAck only
 * touches them on a real ack).  Feed them directly: port record +0 is the
 * packed digital dword, node info +0x135/7/9 the steering/accel/brake bytes.
 * JVS byte1 packing (byte <<8 in the dword): START 0x80, SERVICE 0x40,
 * UP 0x20, DOWN 0x10, LEFT 0x08, RIGHT 0x04, PUSH1 0x02, PUSH2 0x01. */
static void id4_inject_input(void)
{
    /* Analog update lives here — linux_cabinet_fill is only wired to the
     * runtime JVS path this game never takes; its lifted code reads the
     * port/node structs directly. */
    if (g_steer_dir) {
        int v = g_cab_lin.steer + g_steer_dir * 0x1400;
        g_cab_lin.steer = v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v;
    } else if (g_cab_lin.steer != 0x8000) {
        int d = 0x8000 - g_cab_lin.steer;
        g_cab_lin.steer += d / 4 ? d / 4 : (d > 0 ? 1 : -1);
    }

    uint8_t *node0 = (uint8_t *)(uintptr_t)(0x8d7b420u + g_image_delta);
    uint8_t *ports = (uint8_t *)(uintptr_t)(0x8d74840u + g_image_delta);
    uint8_t *credit = (uint8_t *)(uintptr_t)(0x8d97340u + g_image_delta);
    uint8_t *credcap = (uint8_t *)(uintptr_t)(0x8d97320u + g_image_delta);

    /* coins: the credit record is 5 bytes/player, byte0 = credit count */
    if (g_cab_lin.coins[0]) {
        uint8_t cap = *credcap ? *credcap : 99;
        if (credit[0] < cap) credit[0]++;
        g_cab_lin.coins[0] = 0;
    }
    if (g_cab_lin.coins[1]) {
        uint8_t cap = *credcap ? *credcap : 99;
        if (credit[5] < cap) credit[5]++;
        g_cab_lin.coins[1] = 0;
    }

    uint32_t b = g_cab_lin.buttons;
    uint32_t dig = 0;
    if (b & CAB_P1_START)   dig |= 0x00008000u;
    if (b & CAB_P2_START)   dig |= 0x00008000u;
    if (b & CAB_SERVICE)    dig |= 0x00004000u;
    if (b & CAB_TEST)       dig |= 0x00000080u;
    if (b & CAB_P1_TRIGGER) dig |= 0x00000200u;   /* PUSH1 */
    if (b & CAB_P1_RELOAD)  dig |= 0x00000100u;   /* PUSH2 */
    /* steering/acc/brake as digital too — menus move on UP/DOWN/LEFT/RIGHT */
    if (g_cab_lin.accel > 0x4000) dig |= 0x00002000u;   /* UP    */
    if (g_cab_lin.brake > 0x4000) dig |= 0x00001000u;   /* DOWN  */
    if (g_cab_lin.steer < 0x6000) dig |= 0x00000800u;   /* LEFT  */
    if (g_cab_lin.steer > 0xa000) dig |= 0x00000400u;   /* RIGHT */

    /* every port record's digital dword — port->record table is 0..3 linear */
    for (int i = 0; i < 4; i++)
        *(uint32_t *)(ports + i * 300) = dig;

    /* node info: analogs are bytes, steer idles at 0x80 */
    node0[0x135] = (uint8_t)(g_cab_lin.steer >> 8);
    node0[0x137] = (uint8_t)(g_cab_lin.accel >> 8);
    node0[0x139] = (uint8_t)(g_cab_lin.brake >> 8);
}

static void x11_pump(void)
{
    id4_inject_input();
    id4_probe_input();
    if (!g_xdpy) return;
    while (XPending(g_xdpy)) {
        XEvent e; XNextEvent(g_xdpy, &e);
        int down = (e.type == KeyPress || e.type == ButtonPress);
        if (e.type == KeyPress || e.type == KeyRelease) {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            switch (k) {
            case XK_5: if (down) g_cab_lin.coins[0]++; break;
            case XK_6: if (down) g_cab_lin.coins[1]++; break;
            case XK_1: if (down) g_cab_lin.buttons |= CAB_P1_START;
                       else      g_cab_lin.buttons &= ~CAB_P1_START; break;
            case XK_2: if (down) g_cab_lin.buttons |= CAB_P2_START;
                       else      g_cab_lin.buttons &= ~CAB_P2_START; break;
            case XK_t: if (down) g_cab_lin.buttons |= CAB_TEST;
                       else      g_cab_lin.buttons &= ~CAB_TEST; break;
            case XK_s: if (down) g_cab_lin.buttons |= CAB_SERVICE;
                       else      g_cab_lin.buttons &= ~CAB_SERVICE; break;
            case XK_Return: if (down) g_cab_lin.buttons |= CAB_P1_TRIGGER;
                       else      g_cab_lin.buttons &= ~CAB_P1_TRIGGER; break;
            case XK_Shift_L:
            case XK_Shift_R: if (down) g_cab_lin.buttons |= CAB_P1_RELOAD;
                       else      g_cab_lin.buttons &= ~CAB_P1_RELOAD; break;
            case XK_Left:  g_steer_dir = down ? -1 : (g_steer_dir < 0 ? 0 : g_steer_dir); break;
            case XK_Right: g_steer_dir = down ? +1 : (g_steer_dir > 0 ? 0 : g_steer_dir); break;
            case XK_Up:   if (down) g_cab_lin.accel = 0xFFFF; else g_cab_lin.accel = 0; break;
            case XK_Down: if (down) g_cab_lin.brake = 0xFFFF; else g_cab_lin.brake = 0; break;
            default: break;
            }
        }
    }
}

extern int id4_dbg(int, const char *);
#define LDBG(...) do { char _b[192]; snprintf(_b, sizeof _b, __VA_ARGS__); \
                       id4_dbg(0, _b); } while (0)

void *linux_egl_resolve(const char *name)
{
    void *fn = (void *)eglGetProcAddress(name);
    if (!fn) fn = dlsym(RTLD_DEFAULT, name);
    return fn;
}

int linux_egl_open(int w, int h)
{
    if (g_ctx != EGL_NO_CONTEXT) return 1;
    if (w > 0) g_fb_w = w;
    if (h > 0) g_fb_h = h;

    /* LINDBERGH_WINDOW=1 opens a plain X11 window; frames are blitted into it
     * from the swap readback (this host's EGL only supports the surfaceless
     * platform — eglInitialize on x11/gbm/wayland all fail, verified with
     * eglinfo — so a real window surface is not available). */
    if (getenv("LINDBERGH_WINDOW")) {
        g_xdpy = XOpenDisplay(NULL);
        if (g_xdpy) {
            g_xwin = XCreateSimpleWindow(g_xdpy, DefaultRootWindow(g_xdpy),
                                       100, 100, g_fb_w, g_fb_h, 0,
                                       BlackPixel(g_xdpy, 0),
                                       BlackPixel(g_xdpy, 0));
            XStoreName(g_xdpy, g_xwin, "id4-recomp");
            XSelectInput(g_xdpy, g_xwin,
                         KeyPressMask | KeyReleaseMask);
            XMapWindow(g_xdpy, g_xwin);
            XFlush(g_xdpy);
            g_cab_lin.steer = 0x8000;
        }
    }
    /* Surfaceless platform + pbuffer: llvmpipe serves the whole thing in
     * software, so the frames are identical on any machine with no GPU. */
    g_dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                  EGL_DEFAULT_DISPLAY, NULL);
    if (g_dpy == EGL_NO_DISPLAY)
        g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(g_dpy, &major, &minor)) {
        LDBG("[leegl] eglInitialize failed %x\n", eglGetError());
        return 0;
    }
    eglBindAPI(EGL_OPENGL_API);

    static const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attribs, &cfg, 1, &ncfg) || ncfg < 1) {
        LDBG("[leegl] eglChooseConfig failed %x\n", eglGetError());
        return 0;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
            EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_NONE
    };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (g_ctx == EGL_NO_CONTEXT) {
        /* older Mesa: no profile attribute = compat anyway */
        g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, NULL);
    }
    if (g_ctx == EGL_NO_CONTEXT) {
        LDBG("[leegl] eglCreateContext failed %x\n", eglGetError());
        return 0;
    }

    const EGLint surf_attribs[] = {
        EGL_WIDTH, g_fb_w, EGL_HEIGHT, g_fb_h, EGL_NONE
    };
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, surf_attribs);
    if (g_surf == EGL_NO_SURFACE) {
        LDBG("[leegl] surface failed %x\n", eglGetError());
        return 0;
    }
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        LDBG("[leegl] eglMakeCurrent failed %x\n", eglGetError());
        return 0;
    }
    LDBG("[leegl] EGL %d.%d ctx %dx%d — GL_RENDERER=%s\n",
         (int)major, (int)minor, g_fb_w, g_fb_h,
         (const char *)glGetString(GL_RENDERER));
    return 1;
}

int orbis_egl_make_current(void)
{
    if (g_ctx == EGL_NO_CONTEXT) return 0;
    return eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx) ? 1 : 0;
}

static void dump_frame(int idx)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/id4frames/f%05d.ppm", idx);
    FILE *f = fopen(path, "wb");
    if (!f) {
        mkdir("/tmp/id4frames", 0777);
        f = fopen(path, "wb");
        if (!f) return;
    }
    uint8_t *px = malloc((size_t)g_fb_w * g_fb_h * 3);
    if (!px) { fclose(f); return; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, g_fb_w, g_fb_h, GL_RGB, GL_UNSIGNED_BYTE, px);
    fprintf(f, "P6\n%d %d\n255\n", g_fb_w, g_fb_h);
    /* GL reads bottom-up — flip so viewers see it right-side up */
    const size_t row = (size_t)g_fb_w * 3;
    for (int y = g_fb_h - 1; y >= 0; y--) fwrite(px + (size_t)y * row, 1, row, f);
    fclose(f); free(px);
}

void orbis_egl_swap(void)
{
    static int s_n;
    /* hash every frame — cheap motion detector without saving the pixels */
    {
        static uint8_t *px;
        if (!px) px = malloc((size_t)g_fb_w * g_fb_h * 3);
        if (px) {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, g_fb_w, g_fb_h, GL_RGB, GL_UNSIGNED_BYTE, px);
            uint64_t h = 1469598103934665603ull;
            const uint32_t *p32 = (const uint32_t *)px;
            for (size_t i = 0; i < (size_t)g_fb_w * g_fb_h * 3 / 4; i++)
                { h ^= p32[i]; h *= 1099511628211ull; }
            uint32_t c = ((const uint32_t *)px)[
                ((size_t)g_fb_h / 2 * g_fb_w + g_fb_w / 2) * 3 / 4];
            if (s_n < 20 || (s_n % 60) == 0)
                LDBG("[frame] %d hash=%016llx center=%06x\n",
                     s_n, (unsigned long long)h, c & 0xffffff);
            /* dump every 30th frame for the first 5 minutes, then sparse */
            if (s_n % 30 == 0 || (s_n % 300) == 0) dump_frame(s_n);
        }
    }
    /* Windowed view (LINDBERGH_WINDOW=1): a second readback as BGRA, flipped
     * into the XImage's top-down rows — must happen before the swap, which
     * leaves the back buffer undefined. TrueColor 24-bit wants 0xRRGGBB
     * words, which is what little-endian BGRA bytes land as. */
    if (g_xdpy && g_xwin) {
        static XImage *xi;
        static uint8_t *bgra;
        if (!xi) {
            bgra = malloc((size_t)g_fb_w * g_fb_h * 4);
            xi = XCreateImage(g_xdpy, DefaultVisual(g_xdpy, 0), 24, ZPixmap,
                              0, NULL, g_fb_w, g_fb_h, 32, g_fb_w * 4);
            if (xi) xi->data = malloc((size_t)g_fb_w * g_fb_h * 4);
        }
        if (xi && bgra) {
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, g_fb_w, g_fb_h, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
            const size_t row = (size_t)g_fb_w * 4;
            for (int y = 0; y < g_fb_h; y++)
                memcpy(xi->data + (size_t)y * row,
                       bgra + (size_t)(g_fb_h - 1 - y) * row, row);
            GC gc = DefaultGC(g_xdpy, 0);
            XPutImage(g_xdpy, g_xwin, gc, xi, 0, 0, 0, 0, g_fb_w, g_fb_h);
            XFlush(g_xdpy);
            x11_pump();
        }
    }
    eglSwapBuffers(g_dpy, g_surf);
    s_n++;
}
