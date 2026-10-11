/* gl_sanity — minimal known-content render test through the full PS5 GL
 * stack (EGL -> Mesa compat context -> ps5 gallium -> AGC -> scanout),
 * identical to the path ID4 uses but with content we control end-to-end.
 *
 * Stages, ~3s each, looping forever — the stage tells us how far the
 * pipeline gets before the TV stops showing what we drew:
 *   0  pure glClear, cycling solid colors (red/green/blue/white/black)
 *   1  + fixed-function colored quads and a triangle
 *   2  + procedural checkerboard texture on a quad
 *   3  render stage-2 scene into an FBO, composite to fb0 (the game's path)
 */
#include <stddef.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

extern unsigned long strlen(const char *);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);

/* ---- logging: UDP 3334 + real klog (same scheme as hle_libc's id4_dbg) ---- */
int id4_dbg(int ch, const char *s)
{
    extern int sceKernelDebugOutText(int, const char *);
    (void)ch;
    if (!s) return 0;
    static int us = -2;
    if (us == -2) {
        extern int socket(int, int, int);
        us = socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
    }
    if (us >= 0) {
        struct { unsigned short fam, port; uint32_t addr; char z[8]; } sa;
        extern void *memset(void *, int, unsigned long);
        memset(&sa, 0, sizeof sa);
        sa.fam = 0x0210; sa.port = 0x060D /*htons(3334)*/;
        sa.addr = 0x0189A8C0 /*192.168.137.1*/;
        extern long sendto(int, const void *, unsigned long, int, const void *, int);
        sendto(us, s, strlen(s), 0, &sa, 16);
    }
    {
        extern int real_dbgtext(int, const char *) __asm__("sceKernelDebugOutText");
        real_dbgtext(0, s);
    }
    return 0;
}

/* Driver objects call this for their klog — route it to our logger. */
void ps5_drv_debug_text(int channel, const char *text) { id4_dbg(channel, text); }

/* The PSBC compiler (spirv_to_nir) wants klogf from the host — same sink. */
void klogf(const char *fmt, ...);

#include <stdarg.h>
static int kf_(const char *fmt, ...)
{
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    id4_dbg(0, b);
    return 0;
}

void klogf(const char *fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    id4_dbg(0, b);
}

extern int usleep(unsigned int);
extern int sprintf(char *, const char *, ...);

/* ---- EGL bring-up (mirrors orbis_egl_open) ---- */
static EGLDisplay s_dpy;
static EGLSurface s_srf;
static EGLContext s_ctx;

static int egl_open(void)
{
    s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s_dpy == EGL_NO_DISPLAY) { kf_("[san] no display\n"); return -1; }
    EGLint mj, mn;
    if (!eglInitialize(s_dpy, &mj, &mn)) {
        kf_("[san] eglInitialize %#x\n", eglGetError());
        return -1;
    }
    kf_("[san] EGL %d.%d\n", mj, mn);
    eglBindAPI(EGL_OPENGL_API);

    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        kf_("[san] chooseConfig %#x\n", eglGetError());
        return -1;
    }

    for (int i = 0; i < 50; i++) {
        s_srf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)0, NULL);
        if (s_srf != EGL_NO_SURFACE) break;
        if (eglGetError() != 0x3003) break;
        usleep(100 * 1000);
    }
    if (s_srf == EGL_NO_SURFACE) { kf_("[san] surface fail\n"); return -1; }

    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
        EGL_NONE
    };
    s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (s_ctx == EGL_NO_CONTEXT) {
        static const EGLint any[] = { EGL_NONE };
        s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, any);
    }
    if (s_ctx == EGL_NO_CONTEXT) { kf_("[san] ctx fail %#x\n", eglGetError()); return -1; }
    eglMakeCurrent(s_dpy, s_srf, s_srf, s_ctx);
    eglSwapInterval(s_dpy, 1);

    EGLint w = 0, h = 0;
    eglQuerySurface(s_dpy, s_srf, EGL_WIDTH, &w);
    eglQuerySurface(s_dpy, s_srf, EGL_HEIGHT, &h);
    int prof = 0;
    glGetIntegerv(0x9126 /*GL_CONTEXT_PROFILE_MASK*/, &prof);
    kf_("[san] surface %dx%d profmask=%#x compat=%d\n", w, h, prof, !!(prof & 2));
    return 0;
}

/* ---- stage drawing ---- */
static const float cycle[][3] = {
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {0, 0, 0},
    {1, 1, 0}, {0, 1, 1}, {1, 0, 1},
};

static void draw_quads(void)
{
    /* big colored shapes — impossible to miss */
    glBegin(GL_QUADS);
    glColor3f(1, 0, 0); glVertex2f(-0.9f, -0.9f); glVertex2f(-0.1f, -0.9f);
    glVertex2f(-0.1f, -0.1f); glVertex2f(-0.9f, -0.1f);
    glColor3f(0, 1, 0); glVertex2f(0.1f, -0.9f); glVertex2f(0.9f, -0.9f);
    glVertex2f(0.9f, -0.1f); glVertex2f(0.1f, -0.1f);
    glEnd();
    glBegin(GL_TRIANGLES);
    glColor3f(1, 1, 0); glVertex2f(-0.5f, 0.1f); glVertex2f(0.5f, 0.1f);
    glVertex2f(0.0f, 0.9f);
    glEnd();
}

static GLuint make_checker_tex(void)
{
    static uint8_t px[64 * 64 * 4];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int c = ((x >> 3) ^ (y >> 3)) & 1;
            px[(y * 64 + x) * 4 + 0] = c ? 255 : 16;
            px[(y * 64 + x) * 4 + 1] = c ? 255 : 16;
            px[(y * 64 + x) * 4 + 2] = c ? 255 : 16;
            px[(y * 64 + x) * 4 + 3] = 255;
        }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

static void draw_tex_quad(GLuint t)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-0.6f, -0.6f);
    glTexCoord2f(4, 0); glVertex2f(0.6f, -0.6f);
    glTexCoord2f(4, 4); glVertex2f(0.6f, 0.6f);
    glTexCoord2f(0, 4); glVertex2f(-0.6f, 0.6f);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    kf_("[san] enter\n");
    if (egl_open() != 0) {
        kf_("[san] egl_open failed — sleeping\n");
        for (;;) usleep(1000000);
    }
    kf_("[san] egl up, entering stage loop\n");

    /* FBO bits for stage 3 — resolved lazily; the lib exports GL 1.x +
     * EXT_framebuffer_object like the game uses. */
    static int fbo_ready = -1;
    static GLuint fbo = 0, fbo_tex = 0;

    GLuint checker = make_checker_tex();
    kf_("[san] checker tex=%u err=%#x\n", checker, glGetError());

    uint32_t frame = 0;
    int stage = -1;
    for (;;) {
        int st = (frame / 180) % 4;
        if (st != stage) { stage = st; kf_("[san] stage %d\n", stage); }

        if (stage == 3 && fbo_ready < 0) {
            PFNGLGENFRAMEBUFFERSEXTPROC gen_fbo =
                (PFNGLGENFRAMEBUFFERSEXTPROC)eglGetProcAddress("glGenFramebuffersEXT");
            PFNGLBINDFRAMEBUFFEREXTPROC bind_fbo =
                (PFNGLBINDFRAMEBUFFEREXTPROC)eglGetProcAddress("glBindFramebufferEXT");
            PFNGLFRAMEBUFFERTEXTURE2DEXTPROC fb_tex =
                (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)eglGetProcAddress("glFramebufferTexture2DEXT");
            PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC chk_fbo =
                (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)eglGetProcAddress("glCheckFramebufferStatusEXT");
            if (gen_fbo && bind_fbo && fb_tex && chk_fbo) {
                gen_fbo(1, &fbo);
                glGenTextures(1, &fbo_tex);
                glBindTexture(GL_TEXTURE_2D, fbo_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                bind_fbo(GL_FRAMEBUFFER_EXT, fbo);
                fb_tex(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                       GL_TEXTURE_2D, fbo_tex, 0);
                GLenum s = chk_fbo(GL_FRAMEBUFFER_EXT);
                bind_fbo(GL_FRAMEBUFFER_EXT, 0);
                kf_("[san] fbo=%u tex=%u status=%#x\n", fbo, fbo_tex, s);
                fbo_ready = (s == GL_FRAMEBUFFER_COMPLETE_EXT);
            } else {
                kf_("[san] FBO ext missing\n");
                fbo_ready = 0;
            }
        }

        const float *c = cycle[(frame / 60) % 8];
        glClearColor(c[0] * 0.15f, c[1] * 0.15f, c[2] * 0.15f, 1.0f);
        /* stage 0 = pure bright solid color: if even this doesn't show,
         * the display path itself is broken */
        if (stage == 0)
            glClearColor(c[0], c[1], c[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (stage == 1) draw_quads();
        if (stage == 2) { draw_quads(); draw_tex_quad(checker); }

        if (stage == 3 && fbo_ready == 1) {
            PFNGLBINDFRAMEBUFFEREXTPROC bind_fbo =
                (PFNGLBINDFRAMEBUFFEREXTPROC)eglGetProcAddress("glBindFramebufferEXT");
            bind_fbo(GL_FRAMEBUFFER_EXT, fbo);
            glViewport(0, 0, 512, 512);
            glClearColor(c[0] * 0.4f, c[1] * 0.4f, c[2] * 0.4f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            draw_quads();
            draw_tex_quad(checker);
            bind_fbo(GL_FRAMEBUFFER_EXT, 0);
            glViewport(0, 0, 1920, 1080);
            glClear(GL_COLOR_BUFFER_BIT);
            draw_tex_quad(fbo_tex);   /* composite the FBO to the window */
        } else if (stage == 3) {
            draw_quads();
        }

        eglSwapBuffers(s_dpy, s_srf);
        if ((frame % 60) == 0)
            kf_("[san] presented f=%u err=%#x\n", frame, glGetError());
        frame++;
    }
}
