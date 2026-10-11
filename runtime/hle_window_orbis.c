/* hle_window_orbis.c - the display, cut for Orbis/PS5.
 *
 * The Win32 window + wgl context becomes: an EGL surface from ps5-opengl (or a
 * headless stub when EGL isn't up), scePad for the cabinet buttons + analogs,
 * and a fixed full-screen mode (the cabinet has no window manager).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lindbergh_rt.h"

/* ps5-opengl EGL — pulled in weakly so a headless build links without it. */
#ifdef ORBIS_EGL
#include <EGL/egl.h>
#include <GL/gl.h>
#endif

/* scePad input — the DualSense mapped onto the cabinet controls.
 * scePad prototypes live in host.c (the SDK ships no pad.h); the functions
 * are called through extern decls at the use sites below. */

/* The Lindbergh panel is 1360x768 and the game asks for exactly that —
 * initialD.ini carries Width=1360/Height=768 and sWinProc_MakeWindow picks
 * the VidMode entry matching its configured size. Reporting the host TV's
 * 1080p instead makes the mode match fail and window creation return 0,
 * which then skips _sScene::initialize entirely. The host surface can
 * letterbox-scale; the reported mode must be the game's. */
#define DEFAULT_W 1360
#define DEFAULT_H 768

static int g_w = DEFAULT_W, g_h = DEFAULT_H;
static int g_gl_ready = 0;
static int g_quit = 0;
unsigned g_frame = 0;   /* frames presented — the game's frame counter */

/* ---- the display ---- */

void lindbergh_window_size(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
}

void host_window_hint(int w, int h)
{
    if (w > 0 && h > 0) { g_w = w; g_h = h; }
}

int host_window_open(const char *title)
{
    (void)title;
#ifdef ORBIS_EGL
    /* ps5-opengl provides eglGetDisplay/eglInitialize/eglCreateContext/
     * eglCreateWindowSurface against the PS5's native video out. */
    extern void *orbis_egl_open(int w, int h);
    if (!orbis_egl_open(g_w, g_h)) {
        fprintf(stderr, "[window] egl open failed\n");
        return 0;
    }
#elif defined(LINUX_EGL)
    extern int linux_egl_open(int w, int h);
    if (!linux_egl_open(g_w, g_h)) {
        fprintf(stderr, "[window] linux egl open failed\n");
        return 0;
    }
#else
    fprintf(stderr, "[window] %dx%d (headless — no EGL)\n", g_w, g_h);
#endif
    g_gl_ready = 1;
    return 1;
}

int host_gl_context(void)
{
    return g_gl_ready;
}

void host_swap(void)
{
    g_frame++;
    /* report how many immediate-mode emits happened since the last swap —
       a healthy scene emits thousands of prims, not just a few quads */
    {
        static int s_last_emits;
        extern int ff_emit_count(void);
        int n = ff_emit_count();
        if (g_frame < 40 || (g_frame % 300u) == 0u) {
            extern int sceKernelDebugOutText(int, const char *);
            char b[48];
            sprintf(b, "[beat] frame %u emits=%d\n", g_frame, n - s_last_emits);
            sceKernelDebugOutText(0, b);
        }
        /* Dump the sequence-manager slots — which seq is live tells us where
         * the init chain stopped (e.g. still in seqStartup, never reached a
         * title/attract seq). */
        if ((g_frame % 300u) == 0u) {
            extern int sceKernelDebugOutText(int, const char *);
            uint32_t gm = rd32(GVA(0x089AE160u));    /* g_manager — guest VA */
            if (gm) {
                char b[512]; int o = 0;
                o += snprintf(b+o, sizeof(b)-o, "[seqdump] gm=%#x ", gm);
                for (int i = 0; i < 16; i++) {
                    uint32_t s = rd32(GVA(gm + 8u + i * 4u));   /* seq va */
                    if (s < 0x04000000u) continue;   /* NULL or corrupt slot */
                    if (o > 400) break;
                    o += snprintf(b+o, sizeof(b)-o, "s%d=%#x(st%d) ", i, rd32(GVA(s)),
                                  (int)rd32(GVA(s + 0x40u)));
                }
                o += snprintf(b+o, sizeof(b)-o, "\n");
                sceKernelDebugOutText(0, b);
            }
        }
        /* Probe the default framebuffer — DISABLED for now; reading via
         * eglGetProcAddress/dispatch was a crash vector.  The seqdump above
         * is the diagnostic that matters for the black screen. */
        s_last_emits = n;
    }
    extern void hle_gl_frame_summary(void);
    hle_gl_frame_summary();
#if defined(ORBIS_EGL) || defined(LINUX_EGL)
    extern void orbis_egl_swap(void);
    extern const char *g_gl_last_call;
    g_gl_last_call = "eglSwapBuffers";
    orbis_egl_swap();
    g_gl_last_call = NULL;
#endif
}

void host_pump(void)
{
#ifdef ORBIS_PAD
    extern void orbis_pad_poll(void);
    orbis_pad_poll();
#endif
}

/* ---- input ----
 *
 * A driving game's cabinet is steering + gas + brake + a handful of buttons.
 * The DualSense maps onto it: left stick is the wheel, R2/L2 are gas/brake,
 * X is start, the rest are the game-specific buttons. host_cabinet_input()
 * hands that state up to whatever the JVS/baseboard seam asks for. */

static HostInput g_inq[64];
static unsigned g_in_head, g_in_tail;

static void in_push(int kind, int a, int b, int cc, int d)
{
    unsigned n = (g_in_head + 1) % (sizeof g_inq / sizeof g_inq[0]);
    if (n == g_in_tail) return;
    g_inq[g_in_head] = (HostInput){ kind, a, b, cc, d };
    g_in_head = n;
}

int host_input_pop(HostInput *out)
{
    if (g_in_head == g_in_tail) return 0;
    *out = g_inq[g_in_tail];
    g_in_tail = (g_in_tail + 1) % (sizeof g_inq / sizeof g_inq[0]);
    return 1;
}

void host_mouse_state(int *x, int *y, int *buttons)
{
    if (x) *x = 0; if (y) *y = 0; if (buttons) *buttons = 0;
}

void host_cabinet_input(CabinetInput *out)
{
    memset(out, 0, sizeof *out);   /* pad-less path leaves steer/accel/brake valid */
    out->steer = 0x8000;           /* wheel centred */
#ifdef ORBIS_PAD
    extern void orbis_pad_state(CabinetInput *out);
    orbis_pad_state(out);
    out->screen_w = g_w;
    out->screen_h = g_h;
#else
    /* Headless: nothing pressed, the game sees a quiet cabinet — unless the
     * desktop host provides a live keyboard cabinet (linux build windowed). */
    extern void linux_cabinet_fill(CabinetInput *out) __attribute__((weak));
    memset(out, 0, sizeof *out);
    out->steer = 0x8000;
    out->screen_w = g_w;
    out->screen_h = g_h;
    if (linux_cabinet_fill) linux_cabinet_fill(out);
#endif
}

void hle_register_window(void) { }

/* ---- X11 stubs ----
 *
 * The Lindbergh is a Linux/X11 game: it opens an X display, grabs the
 * keyboard/pointer, reads X events. On Orbis there is no X, so the whole
 * surface is faked - a nonzero Display handle, atoms that come back, no
 * events ever. The game only uses X for the window + input, both of which
 * the scePad/headless path replaces.
 */

static uint32_t g_xdisplay;              /* a guest-resident fake Display* */
uint32_t orbis_x_display(void) { return g_xdisplay; }
static uint32_t g_xatom = 0x20;

static uint32_t x_alloc(unsigned bytes)
{
    extern uint32_t guest_brk(uint32_t);
    uint32_t b = guest_brk(0);
    b = (b + 0xfu) & ~0xfu;
    guest_brk(b + bytes);
    memset((void *)(uintptr_t)b, 0, bytes);
    return b;
}

static void h_XInitThreads(CPU *c)      { (void)c; RET(1); }
static void h_XOpenDisplay(CPU *c)
{
    /* The game dereferences the returned Display - screen count, the default
     * screen pointer, the lock ops - so hand back a real guest block, not a
     * raw constant it would fault on. */
    if (!g_xdisplay) {
        /* _XPrivDisplay: the game walks screens[] for the root window + reads
         * the fd for select(). Give it a real Screen struct so those reads
         * land on guest memory instead of a null. Offsets match libX11's
         * private Display: ext_data@0x4, free_funcs@0x10, fd@0xC,
         * lock@0x40, screens@0x84 in the modern layout - the game reads the
         * root via ScreenOfDisplay -> display->screens + default_screen.
         *
         * CRITICAL: the Display* must be 256-aligned. sWinProc_MakeWindow
         * returns al = bl = the Display*'s low byte; initializeGraph ORs flag
         * bit1 (0x2) on nonzero, and _sSystem::initialize skips
         * initialize+initializeShader whenever bit1 is set -> g_render never
         * gets created -> nothing renders -> black screen.  A low byte of 0
         * keeps bit1 clear so the render path actually runs. */
        g_xdisplay = (x_alloc(0x2000) + 0xffu) & ~0xffu;
        uint32_t dpy = g_xdisplay;
        uint32_t screen = x_alloc(0x400);   /* a fake Screen */
        uint32_t root = 0x4000001;
        /* _XPrivDisplay: default_screen is an INDEX (the game does edi =
         * screen_index * sizeof(Screen) then index into screens[]), screens is
         * the array pointer. Putting the pointer where the index belongs turns
         * the index computation into a garbage pointer - which is what it
         * just did. libX11's layout keeps default_screen at 0x84 and the
         * screens array at 0x8c on the version this game was built against. */
        wr32(dpy + 0x84, 0);                /* default_screen = 0 (index) */
        wr32(dpy + 0x8c, screen);           /* screens array */
        wr32(dpy + 0x10, 4);                /* fd-ish */
        /* Screen fields: root, width, height, display */
        wr32(screen + 0x00, root);
        wr32(screen + 0x10, DEFAULT_W);
        wr32(screen + 0x14, DEFAULT_H);
        wr32(screen + 0x18, dpy);           /* back to the display */
    }
    RET(g_xdisplay);
}
static void h_XCloseDisplay(CPU *c)     { (void)c; RET(0); }
static void h_XLockDisplay(CPU *c)      { (void)c; RET(0); }
static void h_XUnlockDisplay(CPU *c)    { (void)c; RET(0); }
static void h_XInternAtom(CPU *c)       { (void)c; RET(++g_xatom); }
static void h_XCreateWindow(CPU *c)     { (void)c; RET(0x5000001); }
static void h_XDestroyWindow(CPU *c)    { (void)c; RET(0); }
static void h_XMapWindow(CPU *c)        { (void)c; RET(0); }
static void h_XUnmapWindow(CPU *c)      { (void)c; RET(0); }
static void h_XMoveWindow(CPU *c)       { (void)c; RET(0); }
static void h_XFlush(CPU *c)            { (void)c; RET(1); }
static void h_XSync(CPU *c)             { (void)c; RET(1); }
static void h_XNextEvent(CPU *c)        { (void)c; RET(0); }
static void h_XPending(CPU *c)          { (void)c; RET(0); }
static void h_XPeekEvent(CPU *c)        { (void)c; RET(0); }
static void h_XQueryPointer(CPU *c)     { (void)c; RET(0); }
static void h_XSetWMProperties(CPU *c)  { (void)c; RET(0); }
static void h_XSetInputFocus(CPU *c)    { (void)c; RET(1); }
static void h_XGrabKeyboard(CPU *c)     { (void)c; RET(0); }
static void h_XGrabPointer(CPU *c)      { (void)c; RET(0); }
static void h_XUngrabKeyboard(CPU *c)   { (void)c; RET(0); }
static void h_XUngrabPointer(CPU *c)    { (void)c; RET(0); }
static void h_XDefineCursor(CPU *c)     { (void)c; RET(0); }
static void h_XUndefineCursor(CPU *c)   { (void)c; RET(0); }
static void h_XFreeCursor(CPU *c)       { (void)c; RET(0); }
static void h_XFreePixmap(CPU *c)       { (void)c; RET(0); }
static void h_XCreateColormap(CPU *c)   { (void)c; RET(0x6000001); }
static void h_XCreatePixmapCursor(CPU *c) { (void)c; RET(0x7000001); }
static void h_XCreatePixmapFromBitmapData(CPU *c) { (void)c; RET(0x6000002); }
static void h_XParseColor(CPU *c)       { (void)c; RET(1); }
static void h_XLookupColor(CPU *c)      { (void)c; RET(1); }
static void h_XGetErrorText(CPU *c)     { (void)c; RET(0); }
static void h_XSetErrorHandler(CPU *c)  { (void)c; RET(0); }
static void h_XSetCloseDownMode(CPU *c) { (void)c; RET(0); }
static void h_XAutoRepeatOn(CPU *c)     { (void)c; RET(1); }
static void h_XAutoRepeatOff(CPU *c)    { (void)c; RET(1); }
static void h_XLookupString(CPU *c)     { (void)c; RET(0); }
static void h_XDefaultScreen(CPU *c)    { (void)c; RET(0); }
static void h_XDefaultRootWindow(CPU *c){ (void)c; RET(0x4000001); }
static void h_XDisplayWidth(CPU *c)     { (void)c; RET(DEFAULT_W); }
static void h_XDisplayHeight(CPU *c)    { (void)c; RET(DEFAULT_H); }
static void h_XBlackPixel(CPU *c)       { (void)c; RET(0); }
static void h_XWhitePixel(CPU *c)       { (void)c; RET(0xFFFFFF); }
static void h_XFillRectangle(CPU *c)    { (void)c; RET(0); }
static void h_XDrawString(CPU *c)       { (void)c; RET(0); }
static void h_XSetForeground(CPU *c)    { (void)c; RET(0); }
static void h_XCreateGC(CPU *c)         { (void)c; RET(0x8000001); }
static void h_XFreeGC(CPU *c)           { (void)c; RET(0); }
static void h_XGetWindowAttributes(CPU *c) { (void)c; RET(1); }
static void h_XQueryExtension(CPU *c)   { (void)c; RET(0); }
static void h_XFree(CPU *c)             { (void)c; RET(0); }
static void h_XKeysymToKeycode(CPU *c)  { (void)c; RET(0); }
static void h_XQueryKeymap(CPU *c)      { (void)c; RET(0); }
static void h_XSetWMName(CPU *c)        { (void)c; RET(0); }
static void h_XStoreName(CPU *c)        { (void)c; RET(1); }
static void h_XChangeProperty(CPU *c)   { (void)c; RET(1); }
static void h_XSelectInput(CPU *c)      { (void)c; RET(1); }
static void h_XRaiseWindow(CPU *c)      { (void)c; RET(0); }
static void h_XLowerWindow(CPU *c)      { (void)c; RET(0); }
static void h_XStringListToTextProperty(CPU *c) { (void)c; RET(0); }
static void h_XSetWMName_prop(CPU *c)   { (void)c; RET(0); }
static void h_XSendEvent(CPU *c)        { (void)c; RET(0); }
static void h_XWarpPointer(CPU *c)      { (void)c; RET(0); }
static void h_XUngrabServer(CPU *c)     { (void)c; RET(0); }
static void h_XGrabServer(CPU *c)       { (void)c; RET(0); }
static void h_XGetGeometry(CPU *c)      { (void)c; RET(1); }
static void h_XQueryTree(CPU *c)        { (void)c; RET(1); }
static void h_XGetVisualInfo(CPU *c)    { (void)c; RET(0); }
static void h_XGetKeyboardMapping(CPU *c) { (void)c; RET(0); }
static void h_XLookupKeysym(CPU *c)     { (void)c; RET(0); }
static void h_XRefreshKeyboardMapping(CPU *c) { (void)c; RET(0); }
static void h_XInitImage(CPU *c)        { (void)c; RET(0); }
static void h_XGetImage(CPU *c)         { (void)c; RET(0); }
static void h_XPutImage(CPU *c)         { (void)c; RET(0); }
static void h_XDestroyImage(CPU *c)     { (void)c; RET(0); }
static void h_XListFonts(CPU *c)        { (void)c; RET(0); }
static void h_XFreeFontNames(CPU *c)    { (void)c; RET(0); }
static void h_XLoadFont(CPU *c)         { (void)c; RET(0); }
static void h_XUnloadFont(CPU *c)       { (void)c; RET(0); }
static void h_XGetFontProperty(CPU *c)  { (void)c; RET(0); }
static void h_XDrawText(CPU *c)         { (void)c; RET(0); }
static void h_XSetLineAttributes(CPU *c){ (void)c; RET(0); }
static void h_XGetAtomName(CPU *c)      { (void)c; RET(0); }
static void h_XGetWindowProperty(CPU *c){ (void)c; RET(0); }
static void h_XDeleteProperty(CPU *c)   { (void)c; RET(0); }
static void h_XMapRaised(CPU *c)        { (void)c; RET(0); }
static void h_XIconifyWindow(CPU *c)    { (void)c; RET(0); }
static void h_XWithdrawWindow(CPU *c)   { (void)c; RET(0); }
static void h_XMoveResizeWindow(CPU *c) { (void)c; RET(0); }
static void h_XResizeWindow(CPU *c)     { (void)c; RET(0); }
static void h_XReparentWindow(CPU *c)   { (void)c; RET(0); }
static void h_XChangeWindowAttributes(CPU *c) { (void)c; RET(0); }
static void h_XSetWindowBackground(CPU *c) { (void)c; RET(0); }
static void h_XSetWindowBorder(CPU *c)  { (void)c; RET(0); }
static void h_XSetStandardProperties(CPU *c) { (void)c; RET(0); }
static void h_XSetWMHints(CPU *c)       { (void)c; RET(0); }
static void h_XSetSizeHints(CPU *c)     { (void)c; RET(0); }
static void h_XSetIconName(CPU *c)      { (void)c; RET(0); }
static void h_XSetTransientForHint(CPU *c) { (void)c; RET(0); }
static void h_XSetWMProtocols(CPU *c)   { (void)c; RET(0); }
static void h_XKillClient(CPU *c)       { (void)c; RET(0); }
static void h_XSetInputFocus2(CPU *c)   { (void)c; RET(1); }
static void h_XNoOp(CPU *c)             { (void)c; }
/* Full-screen single window: src and dest coordinates are the same.
 * Args: dpy, src_w, dest_w, src_x, src_y, dest_x*, dest_y*, child*. */
static void h_XTranslateCoordinates(CPU *c)
{
    if (A32(5)) wr32(A32(5), A32(3));
    if (A32(6)) wr32(A32(6), A32(4));
    if (A32(7)) wr32(A32(7), 0x4000001);
    RET(1);
}

#define XSTUB(n) hle_bind(#n, h_##n)

void hle_register_x11(void)
{
    XSTUB(XInitThreads); XSTUB(XOpenDisplay); XSTUB(XCloseDisplay);
    XSTUB(XLockDisplay); XSTUB(XUnlockDisplay); XSTUB(XInternAtom);
    XSTUB(XCreateWindow); XSTUB(XDestroyWindow); XSTUB(XMapWindow);
    XSTUB(XUnmapWindow); XSTUB(XMoveWindow); XSTUB(XFlush); XSTUB(XSync);
    XSTUB(XNextEvent); XSTUB(XPending); XSTUB(XPeekEvent); XSTUB(XQueryPointer);
    XSTUB(XSetWMProperties); XSTUB(XSetInputFocus); XSTUB(XGrabKeyboard);
    XSTUB(XGrabPointer); XSTUB(XUngrabKeyboard); XSTUB(XUngrabPointer);
    XSTUB(XDefineCursor); XSTUB(XUndefineCursor); XSTUB(XFreeCursor);
    XSTUB(XFreePixmap); XSTUB(XCreateColormap); XSTUB(XCreatePixmapCursor);
    XSTUB(XCreatePixmapFromBitmapData); XSTUB(XParseColor); XSTUB(XLookupColor);
    XSTUB(XGetErrorText); XSTUB(XSetErrorHandler); XSTUB(XSetCloseDownMode);
    XSTUB(XAutoRepeatOn); XSTUB(XAutoRepeatOff); XSTUB(XLookupString);
    XSTUB(XDefaultScreen); XSTUB(XDefaultRootWindow); XSTUB(XDisplayWidth);
    XSTUB(XDisplayHeight); XSTUB(XBlackPixel); XSTUB(XWhitePixel);
    XSTUB(XFillRectangle); XSTUB(XDrawString); XSTUB(XSetForeground);
    XSTUB(XCreateGC); XSTUB(XFreeGC); XSTUB(XGetWindowAttributes);
    XSTUB(XQueryExtension); XSTUB(XFree); XSTUB(XKeysymToKeycode);
    XSTUB(XQueryKeymap); XSTUB(XSetWMName); XSTUB(XStoreName);
    XSTUB(XChangeProperty); XSTUB(XSelectInput); XSTUB(XRaiseWindow);
    XSTUB(XLowerWindow); XSTUB(XStringListToTextProperty);
    XSTUB(XSendEvent); XSTUB(XWarpPointer); XSTUB(XUngrabServer);
    XSTUB(XGrabServer); XSTUB(XGetGeometry); XSTUB(XQueryTree);
    XSTUB(XGetVisualInfo); XSTUB(XGetKeyboardMapping); XSTUB(XLookupKeysym);
    XSTUB(XRefreshKeyboardMapping); XSTUB(XInitImage); XSTUB(XGetImage);
    XSTUB(XPutImage); XSTUB(XDestroyImage); XSTUB(XListFonts);
    XSTUB(XFreeFontNames); XSTUB(XLoadFont); XSTUB(XUnloadFont);
    XSTUB(XGetFontProperty); XSTUB(XDrawText); XSTUB(XSetLineAttributes);
    XSTUB(XGetAtomName); XSTUB(XGetWindowProperty); XSTUB(XDeleteProperty);
    XSTUB(XMapRaised); XSTUB(XIconifyWindow); XSTUB(XWithdrawWindow);
    XSTUB(XMoveResizeWindow); XSTUB(XResizeWindow); XSTUB(XReparentWindow);
    XSTUB(XChangeWindowAttributes); XSTUB(XSetWindowBackground);
    XSTUB(XSetWindowBorder); XSTUB(XSetStandardProperties); XSTUB(XSetWMHints);
    XSTUB(XSetSizeHints); XSTUB(XSetIconName); XSTUB(XSetTransientForHint);
    XSTUB(XSetWMProtocols); XSTUB(XKillClient); XSTUB(XNoOp);
    XSTUB(XTranslateCoordinates);
}
