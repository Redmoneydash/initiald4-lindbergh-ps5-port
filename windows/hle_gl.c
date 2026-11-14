/*
 * hle_gl.c - OpenGL, passed through to the host driver.
 *
 * The game issues real OpenGL. There is nothing to emulate: the calls it makes
 * are the calls the host driver already implements, and the only work is
 * moving the arguments from the guest stack to the host one.
 *
 * Ninety-six hand-written thunks would be ninety-six chances to mistype an
 * argument, so there are none. On x86 every argument is a 4-byte stack slot -
 * a GLfloat is passed as its four raw bytes, not promoted - so a function's
 * entire signature reduces to one number: how many slots it takes. Copy that
 * many dwords across and call. A GLclampd is simply two slots, and a return
 * value comes back in eax whether it was a GLenum, a GLuint or a pointer.
 *
 * The table below is therefore the whole interface. It is worth being careful
 * with, because a wrong count corrupts the host stack rather than failing:
 * stdcall callees pop their own arguments, so too small a count leaves the
 * stack unbalanced and the fault arrives later somewhere else entirely.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>

#include "lindbergh_rt.h"

typedef struct {
    const char *name;
    int         slots;      /* argument dwords */
    void       *fn;         /* resolved lazily */
} GlEntry;

/* Resolution has two sources and needs both. Core OpenGL 1.1 lives in
 * opengl32.dll and wglGetProcAddress does NOT return it; everything newer is
 * the driver's and GetProcAddress does not have it. Try the driver first,
 * since that is where the great majority of these live. */
static void *gl_resolve(const char *name)
{
    static HMODULE gl32;
    if (!gl32) gl32 = LoadLibraryA("opengl32.dll");
    void *p = (void *)wglGetProcAddress(name);
    if (!p && gl32) p = (void *)GetProcAddress(gl32, name);
    return p;
}

/* Call a __stdcall function with `n` dwords taken verbatim from the guest
 * stack. The casts look alarming and are not: on x86 the callee reads its
 * arguments out of the same stack slots regardless of their declared types,
 * and __stdcall means it pops them itself. */
static uint32_t gl_forward(void *fn, const uint32_t *a, int n)
{
    switch (n) {
    case 0: return ((uint32_t(__stdcall *)(void))fn)();
    case 1: return ((uint32_t(__stdcall *)(uint32_t))fn)(a[0]);
    case 2: return ((uint32_t(__stdcall *)(uint32_t,uint32_t))fn)(a[0],a[1]);
    case 3: return ((uint32_t(__stdcall *)(uint32_t,uint32_t,uint32_t))fn)(a[0],a[1],a[2]);
    case 4: return ((uint32_t(__stdcall *)(uint32_t,uint32_t,uint32_t,uint32_t))fn)
                   (a[0],a[1],a[2],a[3]);
    case 5: return ((uint32_t(__stdcall *)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))fn)
                   (a[0],a[1],a[2],a[3],a[4]);
    case 6: return ((uint32_t(__stdcall *)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,
                    uint32_t))fn)(a[0],a[1],a[2],a[3],a[4],a[5]);
    case 7: return ((uint32_t(__stdcall *)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,
                    uint32_t,uint32_t))fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6]);
    case 8: return ((uint32_t(__stdcall *)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,
                    uint32_t,uint32_t,uint32_t))fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
    case 9: return ((uint32_t(__stdcall *)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,
                    uint32_t,uint32_t,uint32_t,uint32_t))fn)
                   (a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);
    default: return 0;
    }
}

/* name, argument dwords. Core OpenGL 1.1/1.2 first, then the ARB/EXT/NV
 * extensions this title uses. A GLclampd takes two slots (glClearDepth); a
 * GLubyte or GLboolean still takes one, because the stack slot is four bytes
 * wide whatever fits in it. */
static GlEntry g_gl[] = {
    { "glAlphaFunc", 2 },              { "glBegin", 1 },
    { "glBindTexture", 2 },            { "glBlendFunc", 2 },
    { "glClear", 1 },                  { "glClearColor", 4 },
    { "glClearDepth", 2 },             { "glClearStencil", 1 },
    { "glColor4ub", 4 },               { "glColorMask", 4 },
    { "glCopyTexSubImage2D", 8 },      { "glCullFace", 1 },
    { "glDeleteTextures", 2 },         { "glDepthFunc", 1 },
    { "glDepthMask", 1 },              { "glDisable", 1 },
    { "glDrawRangeElements", 6 },      { "glEnable", 1 },
    { "glEnableClientState", 1 },      { "glEnd", 0 },
    { "glFogf", 2 },                   { "glFogfv", 2 },
    { "glFogi", 2 },                   { "glFrontFace", 1 },
    { "glGenTextures", 2 },            { "glGetBooleanv", 2 },
    { "glGetFloatv", 2 },              { "glGetIntegerv", 2 },
    { "glGetString", 1 },              { "glHint", 2 },
    { "glIsEnabled", 1 },              { "glLightModeli", 2 },
    { "glPixelStorei", 2 },            { "glPolygonMode", 2 },
    { "glPolygonOffset", 2 },          { "glPopClientAttrib", 0 },
    { "glPushClientAttrib", 1 },       { "glReadPixels", 7 },
    { "glScissor", 4 },                { "glStencilFunc", 3 },
    { "glStencilMask", 1 },            { "glStencilOp", 3 },
    { "glTexCoord2f", 2 },             { "glTexImage2D", 9 },
    { "glTexParameterf", 3 },          { "glTexParameterfv", 3 },
    { "glTexParameteri", 3 },          { "glTexSubImage2D", 9 },
    { "glVertex3f", 3 },               { "glViewport", 4 },

    { "glActiveTextureARB", 1 },       { "glBeginQueryARB", 2 },
    { "glBindBufferARB", 2 },          { "glBindFramebufferEXT", 2 },
    { "glBindProgramARB", 2 },         { "glBlendColorEXT", 4 },
    { "glBlendEquationEXT", 1 },       { "glBlendEquationSeparateEXT", 2 },
    { "glBlendFuncSeparateEXT", 4 },   { "glBufferDataARB", 4 },
    { "glCheckFramebufferStatusEXT", 1 }, { "glCompileShaderARB", 1 },
    { "glCompressedTexImage2DARB", 8 },   { "glCompressedTexSubImage2DARB", 9 },
    { "glCreateShaderObjectARB", 1 },  { "glDeleteBuffersARB", 2 },
    { "glDeleteFramebuffersEXT", 2 },  { "glDeleteObjectARB", 1 },
    { "glDeleteProgramsARB", 2 },      { "glDeleteQueriesARB", 2 },
    { "glDisableVertexAttribArrayARB", 1 }, { "glEnableVertexAttribArrayARB", 1 },
    { "glEndQueryARB", 1 },            { "glFramebufferRenderbufferEXT", 4 },
    { "glFramebufferTexture2DEXT", 5 },{ "glGenBuffersARB", 2 },
    { "glGenFramebuffersEXT", 2 },     { "glGenProgramsARB", 2 },
    { "glGenQueriesARB", 2 },          { "glGetInfoLogARB", 4 },
    { "glGetObjectParameterivARB", 3 },{ "glGetQueryObjectuivARB", 3 },
    { "glIsProgramARB", 1 },           { "glLinkProgramARB", 1 },
    { "glMapBufferARB", 2 },           { "glMultiTexCoord2fARB", 3 },
    { "glMultiTexCoord2fvARB", 2 },    { "glPrimitiveRestartIndexNV", 1 },
    /* The game's wrapper calls these two with THREE args - (target, index,
     * params) for a single vec4 - so they are handled by dedicated thunks
     * below that synthesize count=1, not by the generic dispatcher. The slot
     * count here is moot (the binds override it) but kept at the real
     * four-arg shape for anyone reading the table. */
    { "glProgramEnvParameter4fvARB", 4 },   { "glProgramLocalParameter4fvARB", 4 },
    { "glProgramParameters4fvNV", 4 }, { "glProgramStringARB", 4 },
    { "glShaderSourceARB", 4 },        { "glUnmapBufferARB", 1 },
    { "glUseProgramObjectARB", 1 },    { "glVertexAttribPointerARB", 6 },

    /* Core entry points a GLUT-era game reaches for that the GLX-era one did
     * not: the unsuffixed multitexture and query calls, and the texture state
     * getters it uses to save and restore around its own drawing. Appended
     * rather than sorted in, because g_gl_handlers pairs by index. */
    { "glActiveTexture", 1 },          { "glGenQueries", 2 },
    { "glGetTexEnvfv", 3 },            { "glGetTexEnviv", 3 },
    { "glGetTexParameterfv", 3 },      { "glGetTexParameteriv", 3 },

    /* Fixed-function and client-array GL. Lets Go Jungle drew every
     * vertex through a shader and an immediate-mode glBegin; this game
     * uses the matrix stack, vertex arrays and display lists, which is
     * what most of the platform does. */
    { "glAttachObjectARB", 2 },        { "glBeginQuery", 2 },
    { "glBlendEquation", 1 },          { "glClientActiveTexture", 1 },
    { "glColor3f", 3 },                { "glColor4ubv", 1 },
    { "glColorPointer", 4 },           { "glCompressedTexImage2D", 8 },
    { "glCreateProgramObjectARB", 0 }, { "glDeleteRenderbuffersEXT", 2 },
    { "glDisableClientState", 1 },     { "glDrawArrays", 3 },
    { "glDrawElements", 4 },           { "glEndQuery", 1 },
    { "glGetAttribLocationARB", 2 },   { "glGetError", 0 },
    { "glGetProgramivARB", 3 },        { "glGetQueryObjectuiv", 3 },
    { "glGetTexImage", 5 },            { "glGetTexLevelParameteriv", 4 },
    { "glGetUniformLocationARB", 2 },  { "glIsTexture", 1 },
    { "glLineWidth", 1 },              { "glLoadIdentity", 0 },
    { "glLoadMatrixf", 1 },            { "glMultMatrixf", 1 },
    { "glMaterialf", 3 },              { "glMaterialfv", 3 },
    { "glMatrixMode", 1 },             { "glMultiTexCoord2f", 3 },
    { "glNormalPointer", 3 },          { "glPointParameterfARB", 2 },
    { "glPopAttrib", 0 },              { "glPopMatrix", 0 },
    { "glProgramLocalParameter4fARB", 6 }, { "glPushAttrib", 1 },
    { "glPushMatrix", 0 },             { "glSecondaryColorPointer", 4 },
    { "glTexCoordPointer", 4 },        { "glTexEnvf", 3 },
    { "glTexEnvfv", 3 },               { "glTexEnvi", 3 },
    { "glUniform1fvARB", 3 },          { "glUniform1iARB", 2 },
    { "glUniform1ivARB", 3 },          { "glUniformMatrix3fvARB", 4 },
    { "glUniformMatrix4fvARB", 4 },    { "glVertexPointer", 4 },
    { "glGenLists", 1 },               { "glNewList", 2 },
    { "glEndList", 0 },                { "glCallList", 1 },
    { "glDeleteLists", 2 },

    /* Fences, occlusion queries and NV_vertex_program: the older NVIDIA
     * extensions that a 2006 title targeting this exact GPU used directly. */
    { "glBeginOcclusionQueryNV", 1 }, { "glBindBuffer", 2 },
    { "glBindProgramNV", 2 }, { "glBlendColor", 4 },
    { "glBlendFuncSeparate", 4 }, { "glBufferData", 4 },
    { "glClampColorARB", 2 }, { "glClientActiveTextureARB", 1 },
    { "glColor3ub", 3 }, { "glColorMaterial", 2 },
    { "glDeleteBuffers", 2 }, { "glDeleteFencesNV", 2 },
    { "glDeleteProgramsNV", 2 }, { "glEndOcclusionQueryNV", 0 },
    { "glFinish", 0 }, { "glFinishFenceNV", 1 },
    { "glFlush", 0 }, { "glGenBuffers", 2 },
    { "glGenFencesNV", 2 }, { "glGenOcclusionQueriesNV", 2 },
    { "glGenProgramsNV", 2 }, { "glGetBufferParameterivARB", 3 },
    { "glGetMaterialfv", 3 }, { "glGetOcclusionQueryuivNV", 3 },
    { "glIsBufferARB", 1 }, { "glIsFenceNV", 1 },
    { "glIsProgramNV", 1 }, { "glLightfv", 3 },
    { "glLoadProgramNV", 4 }, { "glLoadTransposeMatrixf", 1 },
    { "glMapBuffer", 2 }, { "glMultTransposeMatrixf", 1 },
    { "glMultiTexCoord2fv", 2 }, { "glNormal3f", 3 },
    { "glPointParameterf", 2 }, { "glPointParameterfv", 2 },
    { "glPointSize", 1 }, { "glProgramEnvParameter4fARB", 6 },
    { "glProgramParameter4fNV", 6 }, { "glProgramParameter4fvNV", 4 },
    { "glRasterPos2f", 2 }, { "glRasterPos3f", 3 },
    { "glReadBuffer", 1 }, { "glRecti", 4 },
    { "glRotatef", 4 }, { "glScalef", 3 },
    { "glSecondaryColor3ub", 3 }, { "glSetFenceNV", 2 },
    { "glShadeModel", 1 }, { "glTexCoord2fv", 1 },
    { "glTexCoord4f", 4 }, { "glTrackMatrixNV", 4 },
    { "glTranslatef", 3 }, { "glUnmapBuffer", 1 },
    { "glVertex4f", 4 }, { "glVertexAttrib1f", 2 },
};

#define GL_COUNT (sizeof g_gl / sizeof g_gl[0])
static unsigned g_gl_calls[GL_COUNT];
static int g_in_begin, g_err_shown, g_err_watch;

/* Generic <-> conventional vertex attribute aliasing.
 *
 * The engine feeds every vertex stream through glVertexAttribPointerARB, but
 * cgc compiled many of its vertex programs against the CONVENTIONAL inputs:
 * vertex.color, vertex.normal, vertex.texcoord[n]. On the cabinet's 2005
 * NVIDIA driver those were the same slots - ARB_vertex_program lets an
 * implementation alias generic attribute 2 with the normal, 3 with the primary
 * colour and 8..15 with texture coordinates 0..7, and NVIDIA did. Current
 * drivers keep the two sets apart, so a program reading vertex.color gets the
 * current colour instead of the per-vertex shading baked into the mesh, and
 * every environment and car in the scene draws black.
 *
 * Doing the aliasing here restores it: each generic pointer / enable / disable
 * on an aliased index is repeated on the matching conventional array. The
 * generic array stays set too, so programs that read vertex.attrib[n] are
 * unaffected. LINDBERGH_NO_ATTR_ALIAS=1 turns it off for comparison. */
static int attr_alias_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("LINDBERGH_NO_ATTR_ALIAS");
                  on = (v && *v && *v != '0') ? 0 : 1; }
    return on;
}

static void attr_alias(const GlEntry *e, const uint32_t *args)
{
    typedef void (__stdcall *PFNCAT)(GLenum);
    static PFNCAT client_active;
    static int resolved;
    const char *n = e->name;
    int ptr = 0, en = 0, dis = 0;
    if (n[2] == 'V' && strcmp(n, "glVertexAttribPointerARB") == 0) ptr = 1;
    else if (n[2] == 'E' && strcmp(n, "glEnableVertexAttribArrayARB") == 0) en = 1;
    else if (n[2] == 'D' && strcmp(n, "glDisableVertexAttribArrayARB") == 0) dis = 1;
    else return;
    if (!attr_alias_on()) return;

    unsigned idx = args[0];
    GLenum array;
    if (idx == 2) array = GL_NORMAL_ARRAY;
    else if (idx == 3) array = GL_COLOR_ARRAY;
    else if (idx >= 8 && idx < 16) array = GL_TEXTURE_COORD_ARRAY;
    else return;                         /* 0 aliases position by spec already */

    if (!resolved) {
        resolved = 1;
        client_active = (PFNCAT)wglGetProcAddress("glClientActiveTextureARB");
        if (!client_active) client_active = (PFNCAT)wglGetProcAddress("glClientActiveTexture");
    }
    GLint prev_unit = 0x84C0;
    if (array == GL_TEXTURE_COORD_ARRAY) {
        if (!client_active) return;
        glGetIntegerv(0x84E1 /* GL_CLIENT_ACTIVE_TEXTURE */, &prev_unit);
        client_active(0x84C0 + (idx - 8));
    }

    if (ptr) {
        /* glVertexAttribPointerARB(index, size, type, normalized, stride, ptr).
         * The conventional pointer latches the same GL_ARRAY_BUFFER binding,
         * since it is issued immediately after. */
        GLint size = (GLint)args[1];
        GLenum type = (GLenum)args[2];
        GLsizei stride = (GLsizei)args[4];
        const void *p = (const void *)(uintptr_t)args[5];
        if (array == GL_NORMAL_ARRAY) {
            if (size == 3) glNormalPointer(type, stride, p);
        } else if (array == GL_COLOR_ARRAY) {
            /* conventional colour arrays always normalise integer types,
             * which is what the engine asks for (normalized=1, ubyte) */
            if (size == 3 || size == 4) glColorPointer(size, type, stride, p);
        } else {
            glTexCoordPointer(size, type, stride, p);
        }
    } else if (en) {
        glEnableClientState(array);
    } else if (dis) {
        glDisableClientState(array);
    }

    if (array == GL_TEXTURE_COORD_ARRAY) client_active((GLenum)prev_unit);
}

/* One handler for all of them. Which entry this is comes from the import id,
 * so the guest's own PLT tells us the name and the table tells us the shape. */
static void gl_dispatch(CPU *c, GlEntry *e)
{
    if (!e->fn) {
        e->fn = gl_resolve(e->name);
        if (!e->fn) {
            static int moaned;
            if (moaned < 20) {
                fprintf(stderr, "[gl] %s unavailable on this driver\n", e->name);
                moaned++;
            }
            RET(0);
            return;
        }
    }
    g_gl_calls[e - g_gl]++;
    uint32_t args[9];
    for (int i = 0; i < e->slots; i++) args[i] = A32(i);
    {   /* [ctxdiag] A WGL context is current on one thread. A GL call from
         * any other thread is a silent no-op - which is exactly how a worker
         * that uploads video frames would vanish. Name such calls. */
        static volatile LONG s_noctx;
        if (!wglGetCurrentContext()) {
            LONG n = InterlockedIncrement(&s_noctx);
            if (n <= 40 || (n & 4095) == 0)
                fprintf(stderr, "[ctxdiag] NO CONTEXT #%ld thread %lu: %s(%#x, %#x, %#x)\n",
                        n, GetCurrentThreadId(), e->name,
                        e->slots > 0 ? args[0] : 0, e->slots > 1 ? args[1] : 0,
                        e->slots > 2 ? args[2] : 0);
        }
        /* Big texture uploads, with the thread: a movie frame is one. */
        if (e->name[2] == 'T' && (!strcmp(e->name, "glTexImage2D") ||
                                  !strcmp(e->name, "glTexSubImage2D"))) {
            int sub = e->name[5] == 'S';
            int w = (int)args[sub ? 4 : 3], h = (int)args[sub ? 5 : 4];
            static volatile LONG s_big;
            if (w * h >= 256 * 192) {
                LONG n = InterlockedIncrement(&s_big);
                if (n <= 30 || (n % 600) == 0)
                    fprintf(stderr, "[texup] #%ld %s %dx%d fmt=%#x type=%#x thread %lu ctx=%p\n",
                            n, e->name, w, h, args[sub ? 6 : 6], args[sub ? 7 : 7],
                            GetCurrentThreadId(), (void *)wglGetCurrentContext());
            }
        }
    }
    RET(gl_forward(e->fn, args, e->slots));
    attr_alias(e, args);

    /* Which call first goes wrong. glGetError inside a Begin/End block is
     * itself an invalid operation, so the flag skips that window. */
    if (g_err_watch) {
        if (e->name[2] == 'B' && strcmp(e->name, "glBegin") == 0) g_in_begin = 1;
        else if (e->name[2] == 'E' && strcmp(e->name, "glEnd") == 0) g_in_begin = 0;
        if (!g_in_begin) {
            GLenum err = glGetError();
            if (err && g_err_shown < 24) {
                g_err_shown++;
                fprintf(stderr, "[err] 0x%X after %s(%u, %u, %u)\n",
                        err, e->name, e->slots > 0 ? args[0] : 0,
                        e->slots > 1 ? args[1] : 0, e->slots > 2 ? args[2] : 0);
                fflush(stderr);
            }
        }
    }
}

/* Which entry points the engine actually uses, and how often. Cheaper than
 * guessing one function at a time: the profile names the draw path, the
 * transform path and the state it sets, all in one run. */
void gl_report_calls(void)
{
    for (unsigned i = 0; i < GL_COUNT; i++)
        if (g_gl_calls[i])
            fprintf(stderr, "[prof] %-34s %u\n", g_gl[i].name, g_gl_calls[i]);
    fflush(stderr);
}

/* One small trampoline per entry. hle_call passes only the CPU, so there is
 * nowhere to carry which function this was - the trampoline IS the identity.
 * Written out rather than macro-generated: a token-pasting macro cannot
 * count, and one that looks like it can is worse than ninety-six plain
 * lines. One per table entry, in table order - g_gl_handlers[i] must be the
 * trampoline for g_gl[i], and hle_register_gl pairs them by index. */
static void gl_t0(CPU *c) { gl_dispatch(c, &g_gl[0]); }
static void gl_t1(CPU *c) { gl_dispatch(c, &g_gl[1]); }
static void gl_t2(CPU *c) { gl_dispatch(c, &g_gl[2]); }
static void gl_t3(CPU *c) { gl_dispatch(c, &g_gl[3]); }
static void gl_t4(CPU *c) { gl_dispatch(c, &g_gl[4]); }
static void gl_t5(CPU *c) { gl_dispatch(c, &g_gl[5]); }
static void gl_t6(CPU *c) { gl_dispatch(c, &g_gl[6]); }
static void gl_t7(CPU *c) { gl_dispatch(c, &g_gl[7]); }
static void gl_t8(CPU *c) { gl_dispatch(c, &g_gl[8]); }
static void gl_t9(CPU *c) { gl_dispatch(c, &g_gl[9]); }
static void gl_t10(CPU *c) { gl_dispatch(c, &g_gl[10]); }
static void gl_t11(CPU *c) { gl_dispatch(c, &g_gl[11]); }
static void gl_t12(CPU *c) { gl_dispatch(c, &g_gl[12]); }
static void gl_t13(CPU *c) { gl_dispatch(c, &g_gl[13]); }
static void gl_t14(CPU *c) { gl_dispatch(c, &g_gl[14]); }
static void gl_t15(CPU *c) { gl_dispatch(c, &g_gl[15]); }
static void gl_t16(CPU *c) { gl_dispatch(c, &g_gl[16]); }
static void gl_t17(CPU *c) { gl_dispatch(c, &g_gl[17]); }
static void gl_t18(CPU *c) { gl_dispatch(c, &g_gl[18]); }
static void gl_t19(CPU *c) { gl_dispatch(c, &g_gl[19]); }
static void gl_t20(CPU *c) { gl_dispatch(c, &g_gl[20]); }
static void gl_t21(CPU *c) { gl_dispatch(c, &g_gl[21]); }
static void gl_t22(CPU *c) { gl_dispatch(c, &g_gl[22]); }
static void gl_t23(CPU *c) { gl_dispatch(c, &g_gl[23]); }
static void gl_t24(CPU *c) { gl_dispatch(c, &g_gl[24]); }
static void gl_t25(CPU *c) { gl_dispatch(c, &g_gl[25]); }
static void gl_t26(CPU *c) { gl_dispatch(c, &g_gl[26]); }
static void gl_t27(CPU *c) { gl_dispatch(c, &g_gl[27]); }
static void gl_t28(CPU *c) { gl_dispatch(c, &g_gl[28]); }
static void gl_t29(CPU *c) { gl_dispatch(c, &g_gl[29]); }
static void gl_t30(CPU *c) { gl_dispatch(c, &g_gl[30]); }
static void gl_t31(CPU *c) { gl_dispatch(c, &g_gl[31]); }
static void gl_t32(CPU *c) { gl_dispatch(c, &g_gl[32]); }
static void gl_t33(CPU *c) { gl_dispatch(c, &g_gl[33]); }
static void gl_t34(CPU *c) { gl_dispatch(c, &g_gl[34]); }
static void gl_t35(CPU *c) { gl_dispatch(c, &g_gl[35]); }
static void gl_t36(CPU *c) { gl_dispatch(c, &g_gl[36]); }
static void gl_t37(CPU *c) { gl_dispatch(c, &g_gl[37]); }
static void gl_t38(CPU *c) { gl_dispatch(c, &g_gl[38]); }
static void gl_t39(CPU *c) { gl_dispatch(c, &g_gl[39]); }
static void gl_t40(CPU *c) { gl_dispatch(c, &g_gl[40]); }
static void gl_t41(CPU *c) { gl_dispatch(c, &g_gl[41]); }
static void gl_t42(CPU *c) { gl_dispatch(c, &g_gl[42]); }
static void gl_t43(CPU *c) { gl_dispatch(c, &g_gl[43]); }
static void gl_t44(CPU *c) { gl_dispatch(c, &g_gl[44]); }
static void gl_t45(CPU *c) { gl_dispatch(c, &g_gl[45]); }
static void gl_t46(CPU *c) { gl_dispatch(c, &g_gl[46]); }
static void gl_t47(CPU *c) { gl_dispatch(c, &g_gl[47]); }
static void gl_t48(CPU *c) { gl_dispatch(c, &g_gl[48]); }
static void gl_t49(CPU *c) { gl_dispatch(c, &g_gl[49]); }
static void gl_t50(CPU *c) { gl_dispatch(c, &g_gl[50]); }
static void gl_t51(CPU *c) { gl_dispatch(c, &g_gl[51]); }
static void gl_t52(CPU *c) { gl_dispatch(c, &g_gl[52]); }
static void gl_t53(CPU *c) { gl_dispatch(c, &g_gl[53]); }
static void gl_t54(CPU *c) { gl_dispatch(c, &g_gl[54]); }
static void gl_t55(CPU *c) { gl_dispatch(c, &g_gl[55]); }
static void gl_t56(CPU *c) { gl_dispatch(c, &g_gl[56]); }
static void gl_t57(CPU *c) { gl_dispatch(c, &g_gl[57]); }
static void gl_t58(CPU *c) { gl_dispatch(c, &g_gl[58]); }
static void gl_t59(CPU *c) { gl_dispatch(c, &g_gl[59]); }
static void gl_t60(CPU *c) { gl_dispatch(c, &g_gl[60]); }
static void gl_t61(CPU *c) { gl_dispatch(c, &g_gl[61]); }
static void gl_t62(CPU *c) { gl_dispatch(c, &g_gl[62]); }
static void gl_t63(CPU *c) { gl_dispatch(c, &g_gl[63]); }
static void gl_t64(CPU *c) { gl_dispatch(c, &g_gl[64]); }
static void gl_t65(CPU *c) { gl_dispatch(c, &g_gl[65]); }
static void gl_t66(CPU *c) { gl_dispatch(c, &g_gl[66]); }
static void gl_t67(CPU *c) { gl_dispatch(c, &g_gl[67]); }
static void gl_t68(CPU *c) { gl_dispatch(c, &g_gl[68]); }
static void gl_t69(CPU *c) { gl_dispatch(c, &g_gl[69]); }
static void gl_t70(CPU *c) { gl_dispatch(c, &g_gl[70]); }
static void gl_t71(CPU *c) { gl_dispatch(c, &g_gl[71]); }
static void gl_t72(CPU *c) { gl_dispatch(c, &g_gl[72]); }
static void gl_t73(CPU *c) { gl_dispatch(c, &g_gl[73]); }
static void gl_t74(CPU *c) { gl_dispatch(c, &g_gl[74]); }
static void gl_t75(CPU *c) { gl_dispatch(c, &g_gl[75]); }
static void gl_t76(CPU *c) { gl_dispatch(c, &g_gl[76]); }
static void gl_t77(CPU *c) { gl_dispatch(c, &g_gl[77]); }
static void gl_t78(CPU *c) { gl_dispatch(c, &g_gl[78]); }
static void gl_t79(CPU *c) { gl_dispatch(c, &g_gl[79]); }
static void gl_t80(CPU *c) { gl_dispatch(c, &g_gl[80]); }
static void gl_t81(CPU *c) { gl_dispatch(c, &g_gl[81]); }
static void gl_t82(CPU *c) { gl_dispatch(c, &g_gl[82]); }
static void gl_t83(CPU *c) { gl_dispatch(c, &g_gl[83]); }
static void gl_t84(CPU *c) { gl_dispatch(c, &g_gl[84]); }
static void gl_t85(CPU *c) { gl_dispatch(c, &g_gl[85]); }
static void gl_t86(CPU *c) { gl_dispatch(c, &g_gl[86]); }
static void gl_t87(CPU *c) { gl_dispatch(c, &g_gl[87]); }
static void gl_t88(CPU *c) { gl_dispatch(c, &g_gl[88]); }
static void gl_t89(CPU *c) { gl_dispatch(c, &g_gl[89]); }
static void gl_t90(CPU *c) { gl_dispatch(c, &g_gl[90]); }
static void gl_t91(CPU *c) { gl_dispatch(c, &g_gl[91]); }
static void gl_t92(CPU *c) { gl_dispatch(c, &g_gl[92]); }
static void gl_t93(CPU *c) { gl_dispatch(c, &g_gl[93]); }
static void gl_t94(CPU *c) { gl_dispatch(c, &g_gl[94]); }
static void gl_t95(CPU *c) { gl_dispatch(c, &g_gl[95]); }
static void gl_t96(CPU *c) { gl_dispatch(c, &g_gl[96]); }
static void gl_t97(CPU *c) { gl_dispatch(c, &g_gl[97]); }
static void gl_t98(CPU *c) { gl_dispatch(c, &g_gl[98]); }
static void gl_t99(CPU *c) { gl_dispatch(c, &g_gl[99]); }
static void gl_t100(CPU *c) { gl_dispatch(c, &g_gl[100]); }
static void gl_t101(CPU *c) { gl_dispatch(c, &g_gl[101]); }
static void gl_t102(CPU *c) { gl_dispatch(c, &g_gl[102]); }
static void gl_t103(CPU *c) { gl_dispatch(c, &g_gl[103]); }
static void gl_t104(CPU *c) { gl_dispatch(c, &g_gl[104]); }
static void gl_t105(CPU *c) { gl_dispatch(c, &g_gl[105]); }
static void gl_t106(CPU *c) { gl_dispatch(c, &g_gl[106]); }
static void gl_t107(CPU *c) { gl_dispatch(c, &g_gl[107]); }
static void gl_t108(CPU *c) { gl_dispatch(c, &g_gl[108]); }
static void gl_t109(CPU *c) { gl_dispatch(c, &g_gl[109]); }
static void gl_t110(CPU *c) { gl_dispatch(c, &g_gl[110]); }
static void gl_t111(CPU *c) { gl_dispatch(c, &g_gl[111]); }
static void gl_t112(CPU *c) { gl_dispatch(c, &g_gl[112]); }
static void gl_t113(CPU *c) { gl_dispatch(c, &g_gl[113]); }
static void gl_t114(CPU *c) { gl_dispatch(c, &g_gl[114]); }
static void gl_t115(CPU *c) { gl_dispatch(c, &g_gl[115]); }
static void gl_t116(CPU *c) { gl_dispatch(c, &g_gl[116]); }
static void gl_t117(CPU *c) { gl_dispatch(c, &g_gl[117]); }
static void gl_t118(CPU *c) { gl_dispatch(c, &g_gl[118]); }
static void gl_t119(CPU *c) { gl_dispatch(c, &g_gl[119]); }
static void gl_t120(CPU *c) { gl_dispatch(c, &g_gl[120]); }
static void gl_t121(CPU *c) { gl_dispatch(c, &g_gl[121]); }
static void gl_t122(CPU *c) { gl_dispatch(c, &g_gl[122]); }
static void gl_t123(CPU *c) { gl_dispatch(c, &g_gl[123]); }
static void gl_t124(CPU *c) { gl_dispatch(c, &g_gl[124]); }
static void gl_t125(CPU *c) { gl_dispatch(c, &g_gl[125]); }
static void gl_t126(CPU *c) { gl_dispatch(c, &g_gl[126]); }
static void gl_t127(CPU *c) { gl_dispatch(c, &g_gl[127]); }
static void gl_t128(CPU *c) { gl_dispatch(c, &g_gl[128]); }
static void gl_t129(CPU *c) { gl_dispatch(c, &g_gl[129]); }
static void gl_t130(CPU *c) { gl_dispatch(c, &g_gl[130]); }
static void gl_t131(CPU *c) { gl_dispatch(c, &g_gl[131]); }
static void gl_t132(CPU *c) { gl_dispatch(c, &g_gl[132]); }
static void gl_t133(CPU *c) { gl_dispatch(c, &g_gl[133]); }
static void gl_t134(CPU *c) { gl_dispatch(c, &g_gl[134]); }
static void gl_t135(CPU *c) { gl_dispatch(c, &g_gl[135]); }
static void gl_t136(CPU *c) { gl_dispatch(c, &g_gl[136]); }
static void gl_t137(CPU *c) { gl_dispatch(c, &g_gl[137]); }
static void gl_t138(CPU *c) { gl_dispatch(c, &g_gl[138]); }
static void gl_t139(CPU *c) { gl_dispatch(c, &g_gl[139]); }
static void gl_t140(CPU *c) { gl_dispatch(c, &g_gl[140]); }
static void gl_t141(CPU *c) { gl_dispatch(c, &g_gl[141]); }
static void gl_t142(CPU *c) { gl_dispatch(c, &g_gl[142]); }
static void gl_t143(CPU *c) { gl_dispatch(c, &g_gl[143]); }
static void gl_t144(CPU *c) { gl_dispatch(c, &g_gl[144]); }
static void gl_t145(CPU *c) { gl_dispatch(c, &g_gl[145]); }
static void gl_t146(CPU *c) { gl_dispatch(c, &g_gl[146]); }
static void gl_t147(CPU *c) { gl_dispatch(c, &g_gl[147]); }
static void gl_t148(CPU *c) { gl_dispatch(c, &g_gl[148]); }
static void gl_t149(CPU *c) { gl_dispatch(c, &g_gl[149]); }
static void gl_t150(CPU *c) { gl_dispatch(c, &g_gl[150]); }
static void gl_t151(CPU *c) { gl_dispatch(c, &g_gl[151]); }
static void gl_t152(CPU *c) { gl_dispatch(c, &g_gl[152]); }
static void gl_t153(CPU *c) { gl_dispatch(c, &g_gl[153]); }
static void gl_t154(CPU *c) { gl_dispatch(c, &g_gl[154]); }
static void gl_t155(CPU *c) { gl_dispatch(c, &g_gl[155]); }
static void gl_t156(CPU *c) { gl_dispatch(c, &g_gl[156]); }
static void gl_t157(CPU *c) { gl_dispatch(c, &g_gl[157]); }
static void gl_t158(CPU *c) { gl_dispatch(c, &g_gl[158]); }
static void gl_t159(CPU *c) { gl_dispatch(c, &g_gl[159]); }
static void gl_t160(CPU *c) { gl_dispatch(c, &g_gl[160]); }
static void gl_t161(CPU *c) { gl_dispatch(c, &g_gl[161]); }
static void gl_t162(CPU *c) { gl_dispatch(c, &g_gl[162]); }
static void gl_t163(CPU *c) { gl_dispatch(c, &g_gl[163]); }
static void gl_t164(CPU *c) { gl_dispatch(c, &g_gl[164]); }
static void gl_t165(CPU *c) { gl_dispatch(c, &g_gl[165]); }
static void gl_t166(CPU *c) { gl_dispatch(c, &g_gl[166]); }
static void gl_t167(CPU *c) { gl_dispatch(c, &g_gl[167]); }
static void gl_t168(CPU *c) { gl_dispatch(c, &g_gl[168]); }
static void gl_t169(CPU *c) { gl_dispatch(c, &g_gl[169]); }
static void gl_t170(CPU *c) { gl_dispatch(c, &g_gl[170]); }
static void gl_t171(CPU *c) { gl_dispatch(c, &g_gl[171]); }
static void gl_t172(CPU *c) { gl_dispatch(c, &g_gl[172]); }
static void gl_t173(CPU *c) { gl_dispatch(c, &g_gl[173]); }
static void gl_t174(CPU *c) { gl_dispatch(c, &g_gl[174]); }
static void gl_t175(CPU *c) { gl_dispatch(c, &g_gl[175]); }
static void gl_t176(CPU *c) { gl_dispatch(c, &g_gl[176]); }
static void gl_t177(CPU *c) { gl_dispatch(c, &g_gl[177]); }
static void gl_t178(CPU *c) { gl_dispatch(c, &g_gl[178]); }
static void gl_t179(CPU *c) { gl_dispatch(c, &g_gl[179]); }
static void gl_t180(CPU *c) { gl_dispatch(c, &g_gl[180]); }
static void gl_t181(CPU *c) { gl_dispatch(c, &g_gl[181]); }
static void gl_t182(CPU *c) { gl_dispatch(c, &g_gl[182]); }
static void gl_t183(CPU *c) { gl_dispatch(c, &g_gl[183]); }
static void gl_t184(CPU *c) { gl_dispatch(c, &g_gl[184]); }
static void gl_t185(CPU *c) { gl_dispatch(c, &g_gl[185]); }
static void gl_t186(CPU *c) { gl_dispatch(c, &g_gl[186]); }
static void gl_t187(CPU *c) { gl_dispatch(c, &g_gl[187]); }
static void gl_t188(CPU *c) { gl_dispatch(c, &g_gl[188]); }
static void gl_t189(CPU *c) { gl_dispatch(c, &g_gl[189]); }
static void gl_t190(CPU *c) { gl_dispatch(c, &g_gl[190]); }
static void gl_t191(CPU *c) { gl_dispatch(c, &g_gl[191]); }
static void gl_t192(CPU *c) { gl_dispatch(c, &g_gl[192]); }
static void gl_t193(CPU *c) { gl_dispatch(c, &g_gl[193]); }
static void gl_t194(CPU *c) { gl_dispatch(c, &g_gl[194]); }
static void gl_t195(CPU *c) { gl_dispatch(c, &g_gl[195]); }
static void gl_t196(CPU *c) { gl_dispatch(c, &g_gl[196]); }
static void gl_t197(CPU *c) { gl_dispatch(c, &g_gl[197]); }
static void gl_t198(CPU *c) { gl_dispatch(c, &g_gl[198]); }
static void gl_t199(CPU *c) { gl_dispatch(c, &g_gl[199]); }
static void gl_t200(CPU *c) { gl_dispatch(c, &g_gl[200]); }
static void gl_t201(CPU *c) { gl_dispatch(c, &g_gl[201]); }
static void gl_t202(CPU *c) { gl_dispatch(c, &g_gl[202]); }
static void gl_t203(CPU *c) { gl_dispatch(c, &g_gl[203]); }
static void gl_t204(CPU *c) { gl_dispatch(c, &g_gl[204]); }
static void gl_t205(CPU *c) { gl_dispatch(c, &g_gl[205]); }
static void gl_t206(CPU *c) { gl_dispatch(c, &g_gl[206]); }
static void gl_t207(CPU *c) { gl_dispatch(c, &g_gl[207]); }
static void gl_t208(CPU *c) { gl_dispatch(c, &g_gl[208]); }
static void gl_t209(CPU *c) { gl_dispatch(c, &g_gl[209]); }
static void gl_t210(CPU *c) { gl_dispatch(c, &g_gl[210]); }

static HleHandler g_gl_handlers[] = {
    gl_t0, gl_t1, gl_t2, gl_t3, gl_t4, gl_t5,
    gl_t6, gl_t7, gl_t8, gl_t9, gl_t10, gl_t11,
    gl_t12, gl_t13, gl_t14, gl_t15, gl_t16, gl_t17,
    gl_t18, gl_t19, gl_t20, gl_t21, gl_t22, gl_t23,
    gl_t24, gl_t25, gl_t26, gl_t27, gl_t28, gl_t29,
    gl_t30, gl_t31, gl_t32, gl_t33, gl_t34, gl_t35,
    gl_t36, gl_t37, gl_t38, gl_t39, gl_t40, gl_t41,
    gl_t42, gl_t43, gl_t44, gl_t45, gl_t46, gl_t47,
    gl_t48, gl_t49, gl_t50, gl_t51, gl_t52, gl_t53,
    gl_t54, gl_t55, gl_t56, gl_t57, gl_t58, gl_t59,
    gl_t60, gl_t61, gl_t62, gl_t63, gl_t64, gl_t65,
    gl_t66, gl_t67, gl_t68, gl_t69, gl_t70, gl_t71,
    gl_t72, gl_t73, gl_t74, gl_t75, gl_t76, gl_t77,
    gl_t78, gl_t79, gl_t80, gl_t81, gl_t82, gl_t83,
    gl_t84, gl_t85, gl_t86, gl_t87, gl_t88, gl_t89,
    gl_t90, gl_t91, gl_t92, gl_t93, gl_t94, gl_t95,
    gl_t96, gl_t97, gl_t98, gl_t99, gl_t100, gl_t101,
    gl_t102, gl_t103, gl_t104, gl_t105, gl_t106, gl_t107,
    gl_t108, gl_t109, gl_t110, gl_t111, gl_t112, gl_t113,
    gl_t114, gl_t115, gl_t116, gl_t117, gl_t118, gl_t119,
    gl_t120, gl_t121, gl_t122, gl_t123, gl_t124, gl_t125,
    gl_t126, gl_t127, gl_t128, gl_t129, gl_t130, gl_t131,
    gl_t132, gl_t133, gl_t134, gl_t135, gl_t136, gl_t137,
    gl_t138, gl_t139, gl_t140, gl_t141, gl_t142, gl_t143,
    gl_t144, gl_t145, gl_t146, gl_t147, gl_t148, gl_t149,
    gl_t150, gl_t151, gl_t152, gl_t153, gl_t154,
    gl_t155, gl_t156, gl_t157, gl_t158, gl_t159, gl_t160,
    gl_t161, gl_t162, gl_t163, gl_t164, gl_t165, gl_t166,
    gl_t167, gl_t168, gl_t169, gl_t170, gl_t171, gl_t172,
    gl_t173, gl_t174, gl_t175, gl_t176, gl_t177, gl_t178,
    gl_t179, gl_t180, gl_t181, gl_t182, gl_t183, gl_t184,
    gl_t185, gl_t186, gl_t187, gl_t188, gl_t189, gl_t190,
    gl_t191, gl_t192, gl_t193, gl_t194, gl_t195, gl_t196,
    gl_t197, gl_t198, gl_t199, gl_t200, gl_t201, gl_t202,
    gl_t203, gl_t204, gl_t205, gl_t206, gl_t207, gl_t208,
    gl_t209, gl_t210,
};

/* glProgramStringARB, watched.
 *
 * An ARB program that the driver rejects leaves the program object unusable,
 * and every draw afterwards fails with GL_INVALID_OPERATION - which is exactly
 * what a black window with a busy render loop looks like. The driver will say
 * where and why if asked, so ask. */
#define GL_PROGRAM_ERROR_POSITION_ARB 0x864B
#define GL_PROGRAM_ERROR_STRING_ARB   0x8874

/* Rewrite an NV_fragment_program2 program to full precision: SHORT TEMP ->
 * TEMP, and every half-precision opcode (MULH, MADH, SGTHC, ...) to its full
 * form (MUL, MAD, SGTC, ...). Only the precision changes; registers, swizzles,
 * condition codes and the instruction count stay as cgc wrote them. Returns a
 * malloc'd string, or NULL when nothing needed changing. */
static int fp_base_op(const char *op, size_t n)
{
    static const char *const ops[] = {
        "ABS","ADD","CMP","COS","DDX","DDY","DIV","DP2","DP2A","DP3","DP4","DPH",
        "DST","EX2","FLR","FRC","LG2","LIT","LRP","MAD","MAX","MIN","MOV","MUL",
        "NRM","POW","RCP","RFL","RSQ","SCS","SEQ","SFL","SGE","SGT","SIN","SLE",
        "SLT","SNE","STR","SUB","SWZ","TEX","TXB","TXD","TXP","X2D","XPD",
        "PK2H","PK2US","PK4B","PK4UB","UP2H","UP2US","UP4B","UP4UB" };
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (strlen(ops[i]) == n && strncmp(ops[i], op, n) == 0) return 1;
    return 0;
}

static char *fp_full_precision(const char *src, size_t len)
{
    if (!strstr(src, "NV_fragment_program")) return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    int changed = 0;
    while (i < len) {
        /* one line at a time */
        size_t ls = i;
        while (i < len && src[i] != '\n') i++;
        size_t le = i;                       /* excludes '\n' */
        if (i < len) i++;
        const char *l = src + ls;
        size_t ll = le - ls;

        if (ll >= 10 && strncmp(l, "SHORT TEMP", 10) == 0) {
            memcpy(out + o, l + 6, ll - 6); o += ll - 6;   /* drop "SHORT " */
            changed = 1;
        } else {
            /* mnemonic = leading [A-Z0-9_]+ token */
            size_t t = 0;
            while (t < ll && ((l[t] >= 'A' && l[t] <= 'Z') || (l[t] >= '0' && l[t] <= '9') || l[t] == '_')) t++;
            size_t keep = t;                 /* bytes of the mnemonic to emit */
            int drop_at = -1;                /* index of an 'H' to remove */
            if (t >= 2 && t < ll && (l[t] == ' ' || l[t] == '\t')) {
                size_t m = t;
                int sat = 0;
                if (m >= 4 && strncmp(l + m - 4, "_SAT", 4) == 0) { m -= 4; sat = 1; }
                size_t base_end = m;
                if (m >= 1 && l[m - 1] == 'C') base_end = m - 1;
                if (base_end >= 1 && l[base_end - 1] == 'H' &&
                    fp_base_op(l, base_end - 1) && !fp_base_op(l, base_end))
                    drop_at = (int)(base_end - 1);
                (void)sat;
            }
            if (drop_at >= 0) {
                memcpy(out + o, l, (size_t)drop_at); o += (size_t)drop_at;
                memcpy(out + o, l + drop_at + 1, ll - (size_t)drop_at - 1);
                o += ll - (size_t)drop_at - 1;
                changed = 1;
            } else {
                (void)keep;
                memcpy(out + o, l, ll); o += ll;
            }
        }
        if (le < len) out[o++] = '\n';
    }
    out[o] = 0;
    if (!changed) { free(out); return NULL; }
    return out;
}

static int fp_fullprec_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("LINDBERGH_FP_FULLPREC");
                  on = (v && *v && *v != '0') ? 1 : 0; }
    return on;
}

static void gl_program_string(CPU *c)
{
    static GlEntry *e;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glProgramStringARB") == 0) e = &g_gl[i]; }
    if (!e) { RET(0); return; }

    {   /* pcarbonDSF: NV_fp2 + sampler2DSHADOW + samplerCUBE — the psbc
         * compile of the fragment program aborts the process (kernel-raised
         * SIGABRT). Skip the GL upload entirely; the shader object is created
         * but never compiled, so draws that use it render unlit rather than
         * killing init. */
        /* PS5-only: its compiler crashed on these. NVIDIA on Windows compiles
         * them fine, and skipping the upload left every car body black (the
         * program object exists but is empty). Opt in with
         * LINDBERGH_SKIP_CARBON=1 only to reproduce the old behaviour. */
        static int skip = -1;
        if (skip < 0) { const char *v = getenv("LINDBERGH_SKIP_CARBON");
                        skip = (v && *v && *v != '0') ? 1 : 0; }
        const char *src = (const char *)(uintptr_t)A32(3);
        int len = (int)A32(2);
        if (skip && src && len > 200 &&
            strstr(src, "NV_fragment_program2") &&
            strstr(src, "sampler2DSHADOW") &&
            strstr(src, "samplerCUBE")) {
            extern int sceKernelDebugOutText(int, const char*);
            sceKernelDebugOutText(0, "[ps] pcarbonDSF-class skipped\n");
            RET(0); return;
        }
    }

    {   /* LINDBERGH_DUMP_FP=<dir> writes every program the engine supplies, so
         * the text the Cg seam actually produced can be read rather than
         * assumed correct. */
        const char *dir = getenv("LINDBERGH_DUMP_FP");
        if (dir && *dir) {
            /* Name each dump by the program object it was loaded into, so a
             * program seen bound at draw time can actually be found again. */
            GLint id = 0;
            { typedef void (__stdcall *PFNGPA)(unsigned, unsigned, GLint *);
              static PFNGPA getprog;
              if (!getprog) getprog = (PFNGPA)wglGetProcAddress("glGetProgramivARB");
              if (getprog) getprog(A32(0), 0x8677 /* PROGRAM_BINDING_ARB */, &id); }
            char path[512];
            snprintf(path, sizeof path, "%s/%s_%03d.txt", dir,
                     A32(0) == 0x8804 ? "fp" : "vp", (int)id);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite((const void *)(uintptr_t)A32(3), 1, A32(2), f); fclose(f); }
        }
    }

    /* LINDBERGH_FP_SOLID=1 replaces every fragment program with one that
     * writes solid green. Geometry that rasterises then shows up whatever the
     * real shader would have computed; geometry that never reaches a pixel
     * stays black. That separates "the scene is shaded black" from "the scene
     * is not on screen" without guessing at either. */
    {
        static int solid = -1;
        if (solid < 0) { const char *v = getenv("LINDBERGH_FP_SOLID");
                         solid = (v && *v) ? atoi(v) : 0; }
        if (solid && A32(0) == 0x8804 /* GL_FRAGMENT_PROGRAM_ARB */) {
            /* =1 solid green: does the geometry rasterise at all.
             * =2 straight texture fetch: is the material the shader samples
             *    black, or is the arithmetic around it what zeroes the frame. */
            static const char green[] =
                "!!ARBfp1.0\nMOV result.color, {0.0, 1.0, 0.0, 1.0};\nEND\n";
            static const char tex[] =
                "!!ARBfp1.0\nTEMP t;\nTEX t, fragment.texcoord[0], texture[0], 2D;\n"
                "MOV t.a, 1.0;\nMOV result.color, t;\nEND\n";
            static const char uv[] =
                "!!ARBfp1.0\nTEMP t;\nMOV t, fragment.texcoord[0];\nMOV t.a, 1.0;\n"
                "MOV result.color, t;\nEND\n";
            /* =4 the interpolated vertex colour alone (the self-shading the
             *    vertex program computes); =5 texture x vertex colour. */
            static const char vcol[] =
                "!!ARBfp1.0\nMOV result.color, fragment.color.primary;\nEND\n";
            static const char texcol[] =
                "!!ARBfp1.0\nTEMP t;\nTEX t, fragment.texcoord[0], texture[0], 2D;\n"
                "MUL result.color, t, fragment.color.primary;\nEND\n";
            /* =6 the same texture fetch, but through NV_fragment_program2's
             *    half-precision path (SHORT TEMP + MOVH into result.color). */
            static const char nvhalf[] =
                "!!ARBfp1.0\nOPTION NV_fragment_program2;\nTEMP R0;\nSHORT TEMP H0;\n"
                "OUTPUT oCol = result.color;\n"
                "TEX R0, fragment.texcoord[0], texture[0], 2D;\nMOVH H0.xyz, R0;\n"
                "MOVH oCol.xyz, H0;\nMOVH oCol.w, 1;\nEND\n";
            const char *src = solid == 6 ? nvhalf : solid == 5 ? texcol : solid == 4 ? vcol :
                              solid >= 3 ? uv : solid == 2 ? tex : green;
            uint32_t args[4] = { A32(0), A32(1), (uint32_t)strlen(src),
                                 (uint32_t)(uintptr_t)src };
            RET(gl_forward(e->fn, args, 4));
            return;
        }
    }
    {   /* log only uploads whose program kind disagrees with the target -
         * the driver rejects those outright, so they are always a bug. */
        const char *src = (const char *)(uintptr_t)A32(3);
        unsigned tgt = A32(0);
        int isvp = src && !strncmp(src, "!!ARBvp", 7);
        int isfp = src && !strncmp(src, "!!ARBfp", 7);
        if ((tgt == 0x8620 && isfp) || (tgt == 0x8804 && isvp)) {
            const char *nl = src ? strchr(src, '\n') : NULL;
            extern const char *cg_gstr_owner(uint32_t);
            const char *owner = cg_gstr_owner(A32(3));
            fprintf(stderr, "[gl] CROSS upload tgt=%#x head=%.*s owner=%s\n", tgt,
                    nl ? (int)(nl - src) : 40, src ? src : "?",
                    owner ? owner : "(not a cg string)");
            /* Dump the offending program text only when asked:
             * LINDBERGH_CROSS_DUMP=<dir> (was a hard-coded E:\ path). */
            const char *dd = getenv("LINDBERGH_CROSS_DUMP");
            if (dd && *dd && src) {
                static int cross_n;
                char cp[512]; snprintf(cp, sizeof cp, "%s/cross_%02d.txt", dd, cross_n++);
                FILE *cf = fopen(cp, "wb");
                if (cf) { fwrite(src, 1, A32(2), cf); fclose(cf); }
            }
        }
    }
    /* Full-precision rewrite: swap the program text the guest passed for the
     * rewritten one (arguments 2 and 3 on the guest stack), upload, restore. */
    char *fullp = NULL;
    uint32_t saved_len = A32(2), saved_ptr = A32(3);
    if (fp_fullprec_on() && A32(0) == 0x8804 && A32(3)) {
        fullp = fp_full_precision((const char *)(uintptr_t)A32(3), (size_t)A32(2));
        if (fullp) {
            static int said;
            if (!said++) fprintf(stderr, "[gl] FP_FULLPREC: rewriting NV_fp2 half ops to full precision\n");
            wr32(c->esp + 8,  (uint32_t)strlen(fullp));
            wr32(c->esp + 12, (uint32_t)(uintptr_t)fullp);
        }
    }
    gl_dispatch(c, e);
    if (fullp) {
        wr32(c->esp + 8, saved_len);
        wr32(c->esp + 12, saved_ptr);
        free(fullp);                 /* the driver has its own copy */
    }

    static int complained;
    static int cap = -1;
    if (cap < 0) { const char *v = getenv("LINDBERGH_PROGERR_CAP");
                   cap = v ? atoi(v) : 5; }
    GLint pos = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
    if (pos >= 0 && complained < cap) {
        complained++;
        const char *why = (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
        const char *src = (const char *)(uintptr_t)A32(3);
        fprintf(stderr, "[gl] program rejected at byte %d: %s\n", (int)pos,
                why ? why : "(no message)");
        if (src) {
            /* first line identifies which program text failed */
            const char *nl = strchr(src, '\n');
            fprintf(stderr, "[gl]   head: %.*s\n",
                    nl ? (int)(nl - src) : 80, src);
            int from = pos > 40 ? pos - 40 : 0;
            fprintf(stderr, "[gl]   near: %.80s\n", src + from);
        }
        fflush(stderr);
    }
}


/* The render target currently bound, and the draws attributed to it. */
static uint32_t g_cur_fbo;
static unsigned g_begin_default, g_begin_fbo;
static int      g_vp_on;      /* GL_VERTEX_PROGRAM_ARB currently enabled */
static unsigned g_begin_vp, g_begin_ff;

/* Render-to-texture, optionally refused.
 *
 * The engine draws its frame into framebuffer objects and composites at the
 * end. If that composite never reaches the default framebuffer the window
 * stays black while every draw call still happens - which is exactly what a
 * call count cannot distinguish from working.
 *
 * LINDBERGH_NO_FBO=1 makes every bind select the default framebuffer, so the
 * drawing lands on the screen directly. A diagnostic, not a fix: passes that
 * expected to read back what they rendered will read the screen instead. */
static int no_fbo(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_NO_FBO");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

/* The only draw call this engine makes.
 *
 * It submits no immediate-mode geometry at all: every primitive arrives
 * through glDrawRangeElements out of a vertex buffer object. Counting these
 * against the bound render target answers, without any theory in between,
 * whether the empty scene target ever had geometry aimed at it. */
static unsigned g_dre[16];
static unsigned g_dre_verts[16];

static void gl_draw_range(CPU *c)
{
    static GlEntry *e;
    static int shown;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glDrawRangeElements") == 0) e = &g_gl[i]; }

    unsigned slot = g_cur_fbo < 16 ? g_cur_fbo : 15;
    g_dre[slot]++;
    g_dre_verts[slot] += A32(3);

    if (shown < 12 && getenv("LINDBERGH_FBSTATS")) {
        GLint vbo = 0, ibo = 0, vp[4] = {0,0,0,0};
        glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING_ARB */, &vbo);
        glGetIntegerv(0x8895 /* ELEMENT_ARRAY_BUFFER_BINDING_ARB */, &ibo);
        glGetIntegerv(GL_VIEWPORT, vp);
        shown++;
        fprintf(stderr, "[dre] fbo %u mode 0x%X range %u..%u count %u type 0x%X "
                        "idx 0x%X | vbo %d ibo %d vp %dx%d\n",
                g_cur_fbo, A32(0), A32(1), A32(2), A32(3), A32(4), A32(5),
                (int)vbo, (int)ibo, vp[2], vp[3]);
    }
    if (e) gl_dispatch(c, e);

    /* Same chosen-frame probe as gl_end_watch, but this engine draws through
     * DrawRangeElements so the trace has to live here. After each draw, read
     * the centre pixel of the CURRENT target: a scene that submits 50k
     * primitives per frame and still reads back black is a state problem,
     * not a submission problem. */
    {   extern unsigned g_frame;
        static unsigned trace_at = 0u - 1u, nth;
        if (trace_at == 0u - 1u) {
            const char *v = getenv("LINDBERGH_TRACE_FRAME");
            trace_at = (v && *v) ? (unsigned)atoi(v) : 0u - 2u;
        }
        if (g_frame == trace_at) {
            nth++;
            if (nth <= 400 || nth % 500 == 0) {
                GLint vp[4] = {0,0,0,0};
                glGetIntegerv(GL_VIEWPORT, vp);
                unsigned char px[4] = {0,0,0,0};
                glReadBuffer(g_cur_fbo ? 0x8CE0 : GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                if (vp[2] >= 8 && vp[3] >= 8)
                    glReadPixels(vp[0] + vp[2]/2 - 1, vp[1] + vp[3]/2 - 1,
                                 2, 2, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
                GLint depth = glIsEnabled(GL_DEPTH_TEST);
                GLint fprog = 0, vprog = 0;
                { typedef void (__stdcall *PFNGPA)(unsigned, unsigned, GLint *);
                  static PFNGPA getprog;
                  if (!getprog) getprog = (PFNGPA)wglGetProcAddress("glGetProgramivARB");
                  if (getprog) { getprog(0x8620, 0x8677, &vprog);
                                 getprog(0x8804, 0x8677, &fprog); } }
                fprintf(stderr, "[tdr] %u fbo %u cnt %u vp %dx%d px %02x%02x%02x%02x "
                                "depth %d vp1 %d fp1 %d\n",
                        nth, g_cur_fbo, A32(3), vp[2], vp[3],
                        px[0], px[1], px[2], px[3], (int)depth,
                        (int)vprog, (int)fprog);
                /* [fpc] The fragment program's inputs: env + local params
                 * (non-zero ones), first time each program is seen in the
                 * main scene target. A material shaded black with a good
                 * texture means one of these is zero where it should not be. */
                if (g_cur_fbo == 1) {
                    static int seen[1024];
                    if (fprog > 0 && fprog < 1024 && !seen[fprog]++) {
                        typedef void (__stdcall *PFNGEP)(unsigned, unsigned, float *);
                        static PFNGEP genv, gloc;
                        if (!genv) genv = (PFNGEP)wglGetProcAddress("glGetProgramEnvParameterfvARB");
                        if (!gloc) gloc = (PFNGEP)wglGetProcAddress("glGetProgramLocalParameterfvARB");
                        GLint fpon = glIsEnabled(0x8804);
                        char l[1400]; int at = 0;
                        at += snprintf(l + at, sizeof l - at, "[fpc] fp %d enabled %d env:", (int)fprog, (int)fpon);
                        for (unsigned i = 0; i < 32 && genv && at < 1300; i++) {
                            float v[4] = {0,0,0,0}; genv(0x8804, i, v);
                            if (v[0] || v[1] || v[2] || v[3])
                                at += snprintf(l + at, sizeof l - at, " e%u=%.3g,%.3g,%.3g,%.3g", i, v[0], v[1], v[2], v[3]);
                        }
                        at += snprintf(l + at, sizeof l - at, " | local:");
                        for (unsigned i = 0; i < 32 && gloc && at < 1380; i++) {
                            float v[4] = {0,0,0,0}; gloc(0x8804, i, v);
                            if (v[0] || v[1] || v[2] || v[3])
                                at += snprintf(l + at, sizeof l - at, " l%u=%.3g,%.3g,%.3g,%.3g", i, v[0], v[1], v[2], v[3]);
                        }
                        fprintf(stderr, "%s\n", l);

                        /* [va] The vertex streams this draw reads: every
                         * enabled generic attribute, plus the first vertex's
                         * bytes for each, read back out of the bound VBO. */
                        typedef void (__stdcall *PFNGVA)(unsigned, unsigned, GLint *);
                        typedef void (__stdcall *PFNGVAP)(unsigned, unsigned, void **);
                        typedef void (__stdcall *PFNGBSD)(unsigned, ptrdiff_t, ptrdiff_t, void *);
                        typedef void (__stdcall *PFNBB)(unsigned, unsigned);
                        static PFNGVA gva; static PFNGVAP gvap; static PFNGBSD gbsd; static PFNBB bb;
                        if (!gva)  gva  = (PFNGVA)wglGetProcAddress("glGetVertexAttribivARB");
                        if (!gvap) gvap = (PFNGVAP)wglGetProcAddress("glGetVertexAttribPointervARB");
                        if (!gbsd) gbsd = (PFNGBSD)wglGetProcAddress("glGetBufferSubDataARB");
                        if (!bb)   bb   = (PFNBB)wglGetProcAddress("glBindBufferARB");
                        GLint prev_vbo = 0; glGetIntegerv(0x8894 /*ARRAY_BUFFER_BINDING*/, &prev_vbo);
                        for (unsigned ai = 0; ai < 16 && gva; ai++) {
                            GLint en = 0, sz = 0, ty = 0, nm = 0, st = 0, buf = 0;
                            gva(ai, 0x8622, &en);
                            if (!en) continue;
                            gva(ai, 0x8623, &sz); gva(ai, 0x8625, &ty);
                            gva(ai, 0x886A, &nm); gva(ai, 0x8624, &st);
                            gva(ai, 0x889F, &buf);
                            void *off = 0; if (gvap) gvap(ai, 0x8645, &off);
                            unsigned char vb[16]; memset(vb, 0xEE, sizeof vb);
                            if (buf && gbsd && bb) {
                                bb(0x8892, (unsigned)buf);
                                gbsd(0x8892, (ptrdiff_t)off, 16, vb);
                            }
                            fprintf(stderr, "[va]   fp %d attr %u size %d type %#x norm %d stride %d vbo %d off %p : "
                                    "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                                    (int)fprog, ai, sz, ty, nm, st, buf, off,
                                    vb[0],vb[1],vb[2],vb[3],vb[4],vb[5],vb[6],vb[7],
                                    vb[8],vb[9],vb[10],vb[11],vb[12],vb[13],vb[14],vb[15]);
                        }
                        if (bb) bb(0x8892, (unsigned)prev_vbo);
                        {   float cc[4] = {0,0,0,0}; glGetFloatv(GL_CURRENT_COLOR, cc);
                            void *cp = 0; GLint csz = 0, cty = 0, cst = 0, cbuf = 0;
                            glGetPointerv(GL_COLOR_ARRAY_POINTER, &cp);
                            glGetIntegerv(GL_COLOR_ARRAY_SIZE, &csz);
                            glGetIntegerv(GL_COLOR_ARRAY_TYPE, &cty);
                            glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &cst);
                            glGetIntegerv(0x8898 /*COLOR_ARRAY_BUFFER_BINDING*/, &cbuf);
                            fprintf(stderr, "[va]   fp %d vp %d colorarray %d (size %d type %#x stride %d vbo %d ptr %p) "
                                    "current %.2f %.2f %.2f %.2f\n", (int)fprog, (int)vprog,
                                    (int)glIsEnabled(GL_COLOR_ARRAY), csz, cty, cst, cbuf, cp,
                                    cc[0], cc[1], cc[2], cc[3]); }
                    }
                }
            }
        }
    }
}

void gl_report_draws(void)
{
    fprintf(stderr, "[dre] draws per target:\n");
    for (unsigned i = 0; i < 16; i++)
        if (g_dre[i])
            fprintf(stderr, "[dre]   fbo %u: %u draws, %u vertices\n",
                    i, g_dre[i], g_dre_verts[i]);
    fflush(stderr);
}

/* Per-frame deltas for the orbis [beat] line — the whole scene's draw call
 * is glDrawRangeElements; zero per frame means nothing was aimed at a
 * render target at all. */
unsigned g_dre_count(void)
{
    static unsigned last;
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) n += g_dre[i];
    unsigned d = n - last; last = n;
    return d;
}
unsigned g_dre_verts_report(void)
{
    static unsigned last;
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) n += g_dre_verts[i];
    unsigned d = n - last; last = n;
    return d;
}

/* Every draw of one chosen frame, with what the back buffer held after it.
 *
 * 16,612 primitives are submitted to the default framebuffer each frame and it
 * stays black. Either none of them ever writes a pixel, or something clears
 * after them. Reading the target after each glEnd says which, and names the
 * exact draw where the picture appears or fails to. */
extern unsigned g_frame;

static void gl_end_watch(CPU *c)
{
    static GlEntry *e;
    static unsigned trace_at = 0u - 1u;
    static unsigned nth;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glEnd") == 0) e = &g_gl[i]; }
    if (trace_at == 0u - 1u) {
        const char *v = getenv("LINDBERGH_TRACE_FRAME");
        trace_at = (v && *v) ? (unsigned)atoi(v) : 0u - 2u;
    }
    if (e) gl_dispatch(c, e);
    if (g_frame != trace_at) return;

    nth++;
    unsigned char px[32 * 32 * 3];
    GLint vp[4] = {0,0,0,0}, sc[4] = {0,0,0,0}, prog = 0;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    /* GL_PROGRAM_BINDING_ARB is not a glGetIntegerv pname; it has to be asked
     * of a specific program target, and only means anything when that target
     * is enabled. Asking it the wrong way returns a flat 0 for every draw. */
    GLint fprog = 0;
    { typedef void (__stdcall *PFNGPA)(unsigned, unsigned, GLint *);
      static PFNGPA getprog;
      if (!getprog) getprog = (PFNGPA)wglGetProcAddress("glGetProgramivARB");
      if (getprog) { getprog(0x8620, 0x8677, &prog); getprog(0x8804, 0x8677, &fprog); } }
    glReadBuffer(g_cur_fbo ? 0x8CE0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    memset(px, 0, sizeof px);
    glReadPixels(vp[0] + vp[2] / 2 - 16, vp[1] + vp[3] / 2 - 16, 32, 32,
                 GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
    unsigned lit = 0, peak = 0;
    for (int i = 0; i < 32 * 32; i++) {
        unsigned v = px[i*3] | px[i*3+1] | px[i*3+2];
        if (v > 8) lit++;
        if (v > peak) peak = v;
    }
    GLboolean cm[4] = {1,1,1,1};
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);

    /* The vertex program multiplies the vertex colour by program.env[5] and
     * the fragment program multiplies the texture by that result. The texture
     * is known good and the geometry is known to rasterise, so these eight
     * constants and the current colour are what is left to decide the pixel. */
    if (nth <= 12) {
        typedef void (__stdcall *PFNGEP)(unsigned, unsigned, float *);
        static PFNGEP getenvp;
        if (!getenvp) getenvp = (PFNGEP)
            wglGetProcAddress("glGetProgramEnvParameterfvARB");
        float cc[4] = {0,0,0,0};
        glGetFloatv(GL_CURRENT_COLOR, cc);
        char l[512]; int at = 0;
        at += snprintf(l + at, sizeof l - at,
                       "[env] draw %u colour %.2f %.2f %.2f %.2f | vp env",
                       nth, cc[0], cc[1], cc[2], cc[3]);
        for (unsigned i = 0; i < 8 && getenvp; i++) {
            float v[4] = {0,0,0,0};
            getenvp(0x8620, i, v);
            at += snprintf(l + at, sizeof l - at, " [%u]%.3g,%.3g,%.3g,%.3g",
                           i, v[0], v[1], v[2], v[3]);
        }
        fprintf(stderr, "%s\n", l);
    }

    /* What the shader is actually sampling. A fragment program forced to a
     * plain texture fetch renders black, so either nothing is bound to the
     * unit it reads or the texture itself is black. Naming the bound object
     * per unit, and reading one back, tells those apart. */
    if (nth <= 12) {
        typedef void (__stdcall *PFNAT)(unsigned);
        static PFNAT activetex;
        if (!activetex) activetex = (PFNAT)wglGetProcAddress("glActiveTextureARB");
        char line[512]; int at = 0;
        at += snprintf(line + at, sizeof line - at, "[tex] draw %u units:", nth);
        for (unsigned u = 0; u < 8 && activetex; u++) {
            GLint bound = 0, tw = 0, th = 0;
            activetex(0x84C0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            if (!bound) continue;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
            unsigned pct = 0;
            /* Read a small mip rather than the base level: a 2048x2048 atlas
             * skipped for being too big reports 0% and reads exactly like a
             * black texture, which is how this probe lied the first time. */
            GLint lv = 0, lw = tw, lh = th;
            while ((lw > 256 || lh > 256) && lv < 12) {
                lv++;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, lv, GL_TEXTURE_WIDTH, &lw);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, lv, GL_TEXTURE_HEIGHT, &lh);
                if (lw <= 0 || lh <= 0) { lw = tw; lh = th; lv = 0; break; }
            }
            if (lw > 0 && lh > 0 && (long)lw * lh <= 4096L * 4096L) {
                unsigned char *tb = (unsigned char *)malloc((size_t)lw * lh * 3);
                if (tb) {
                    memset(tb, 0, (size_t)lw * lh * 3);
                    glGetTexImage(GL_TEXTURE_2D, lv, GL_BGR_EXT, GL_UNSIGNED_BYTE, tb);
                    size_t l = 0, n = (size_t)lw * lh;
                    for (size_t i = 0; i < n; i++)
                        if ((tb[i*3] | tb[i*3+1] | tb[i*3+2]) > 8) l++;
                    pct = (unsigned)(100 * l / n);
                    free(tb);
                }
            }
            /* A texture whose min filter mipmaps but which has no mip chain is
             * incomplete, and an incomplete texture samples as solid black with
             * no error anywhere. That is indistinguishable from a black image
             * unless the filter and the level count are read together. */
            GLint minf = 0, maxlvl = 0, l1w = 0, ifmt = 0;
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
            glGetTexParameteriv(GL_TEXTURE_2D, 0x813D /* TEXTURE_MAX_LEVEL */, &maxlvl);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &l1w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1003 /* INTERNAL_FORMAT */, &ifmt);
            at += snprintf(line + at, sizeof line - at,
                           " u%u=#%d(%dx%d %u%% min0x%X maxlvl%d lvl1w%d fmt0x%X)",
                           u, (int)bound, tw, th, pct,
                           (unsigned)minf, (int)maxlvl, (int)l1w, (unsigned)ifmt);
            if (at > 400) break;
        }
        activetex(0x84C0);
        fprintf(stderr, "%s\n", line);
    }
    fprintf(stderr, "[draw] %4u fbo %u vp %d,%d %dx%d sc%s %d,%d %dx%d "
                    "vp%d/fp%d prog %d/%d | centre %u%% peak %u "
                    "| mask %d%d%d%d depth %d alpha %d blend %d\n",
            nth, g_cur_fbo, vp[0], vp[1], vp[2], vp[3],
            glIsEnabled(GL_SCISSOR_TEST) ? "ON" : "off", sc[0], sc[1], sc[2], sc[3],
            glIsEnabled(0x8620), glIsEnabled(0x8804), (int)prog, (int)fprog,
            100 * lit / (32 * 32), peak,
            cm[0], cm[1], cm[2], cm[3],
            glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_ALPHA_TEST),
            glIsEnabled(GL_BLEND));
}

static void gl_bind_framebuffer(CPU *c)
{
    static GlEntry *e;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glBindFramebufferEXT") == 0) e = &g_gl[i]; }
    if (e && !e->fn) e->fn = gl_resolve(e->name);
    if (!e || !e->fn) { RET(0); return; }

    uint32_t fb = no_fbo() ? 0u : A32(1);

    /* LINDBERGH_FBO_DUMP=<dir>: in the LINDBERGH_TRACE_FRAME frame, write each
     * offscreen target out as a BMP the moment the engine moves off it - the
     * scene, the shadow map, every post pass - so which pass first goes
     * black is a picture rather than one probed pixel. */
    {   extern unsigned g_frame;
        static unsigned trace_at = 0u - 1u, every; static int seq;
        static const char *dir, *only;
        if (trace_at == 0u - 1u) {
            const char *v = getenv("LINDBERGH_TRACE_FRAME");
            trace_at = (v && *v) ? (unsigned)atoi(v) : 0u - 2u;
            dir = getenv("LINDBERGH_FBO_DUMP");
            /* LINDBERGH_FBO_DUMP_EVERY=N repeats the dump every N frames from
             * TRACE_FRAME on (the attract sequence is not deterministic, so
             * a single frame number rarely lands on the same shot twice);
             * LINDBERGH_FBO_DUMP_ONLY=",1,3," limits it to those targets. */
            const char *e = getenv("LINDBERGH_FBO_DUMP_EVERY");
            every = (e && *e) ? (unsigned)atoi(e) : 0;
            only = getenv("LINDBERGH_FBO_DUMP_ONLY");
        }
        int frame_hit = every ? (g_frame >= trace_at && (g_frame - trace_at) % every == 0)
                              : g_frame == trace_at;
        int fbo_ok = 1;
        if (only && *only) { char k[16]; snprintf(k, sizeof k, ",%u,", g_cur_fbo); fbo_ok = strstr(only, k) != NULL; }
        if (dir && *dir && frame_hit && fbo_ok && g_cur_fbo && fb != g_cur_fbo && seq < 4000) {
            GLint vp[4] = {0,0,0,0};
            glGetIntegerv(GL_VIEWPORT, vp);
            int w = vp[2], h = vp[3];
            if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
                int stride = (w * 3 + 3) & ~3;
                unsigned char *px = (unsigned char *)malloc((size_t)stride * h);
                if (px) {
                    glReadBuffer(0x8CE0);
                    glPixelStorei(GL_PACK_ALIGNMENT, 4);
                    glReadPixels(vp[0], vp[1], w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
                    char path[600];
                    snprintf(path, sizeof path, "%s/f%06u_pass%02d_fbo%u_%dx%d.bmp", dir, g_frame, seq, g_cur_fbo, w, h);
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        unsigned char hd[54] = {0};
                        uint32_t pix = (uint32_t)stride * (uint32_t)h, off = 54, size = off + pix;
                        hd[0] = 'B'; hd[1] = 'M';
                        memcpy(hd + 2, &size, 4); memcpy(hd + 10, &off, 4);
                        uint32_t hs = 40; int32_t ww = w, hh = h; uint16_t planes = 1, bpp = 24;
                        memcpy(hd + 14, &hs, 4); memcpy(hd + 18, &ww, 4); memcpy(hd + 22, &hh, 4);
                        memcpy(hd + 26, &planes, 2); memcpy(hd + 28, &bpp, 2); memcpy(hd + 34, &pix, 4);
                        fwrite(hd, 1, 54, f); fwrite(px, 1, pix, f); fclose(f);
                        fprintf(stderr, "[fbodump] %s\n", path);
                    }
                    free(px);
                }
                seq++;
            }
        }
    }
    g_cur_fbo = fb;
    uint32_t args[2] = { A32(0), fb };
    RET(gl_forward(e->fn, args, 2));
}

/* Draws, attributed to the render target that was bound when they happened.
 *
 * A frame that submits hundreds of primitives and shows nothing has either
 * drawn them somewhere invisible or had them all discarded. Counting glBegin
 * against the bound framebuffer separates those two: if the default
 * framebuffer never receives a single primitive, the composite is missing
 * rather than failing. */

static void gl_begin_watch(CPU *c)
{
    static GlEntry *e;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glBegin") == 0) e = &g_gl[i]; }
    {   /* Which thread is actually drawing? */
        static unsigned long seen[4]; static int n;
        unsigned long me = GetCurrentThreadId();
        int known = 0;
        for (int i = 0; i < n; i++) if (seen[i] == me) known = 1;
        if (!known && n < 4) {
            seen[n++] = me;
            fprintf(stderr, "[gl] draws arriving from thread %lu\n", me);
        }
    }
    if (g_cur_fbo) g_begin_fbo++; else g_begin_default++;
    if (g_vp_on) g_begin_vp++; else g_begin_ff++;
    if (getenv("LINDBERGH_FBSTATS") && (g_begin_default + g_begin_fbo) % 20000 == 0)
        fprintf(stderr, "[gl] glBegin: %u default / %u FBO | %u with vertex program, "
                        "%u fixed-function\n", g_begin_default, g_begin_fbo,
                g_begin_vp, g_begin_ff);
    if (e) gl_dispatch(c, e);
}

/* The shader constants, as the guest computed them.
 *
 * The engine transforms entirely in vertex programs, so its matrices arrive
 * here and nowhere else. If the lifted float maths that produced them is
 * wrong, every vertex lands outside the clip volume and the screen stays black
 * while every draw call still runs - which is the shape of the problem. A
 * matrix of NaNs or of enormous numbers says so immediately. */
/* The range form of the same upload. The engine sends its matrices through
 * this, four rows at a time; env[0] arriving as zeros while env[1..3] hold
 * sensible numbers is either a write that never happened or a source row that
 * really is zero, and only the arguments say which. */
static void gl_env_params_watch(CPU *c)
{
    static GlEntry *e;
    static int shown;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glProgramParameters4fvNV") == 0) e = &g_gl[i]; }
    if (shown < 24 && getenv("LINDBERGH_PARAMS")) {
        shown++;
        const float *v = (const float *)(uintptr_t)A32(3);
        unsigned n = A32(2) > 4 ? 4 : A32(2);
        char l[256]; int at = 0;
        at += snprintf(l + at, sizeof l - at, "[par] target 0x%X index %u count %u:",
                       A32(0), A32(1), A32(2));
        for (unsigned i = 0; i < n && v; i++)
            at += snprintf(l + at, sizeof l - at, " (%.4g %.4g %.4g %.4g)",
                           v[i*4], v[i*4+1], v[i*4+2], v[i*4+3]);
        fprintf(stderr, "%s\n", l);
        /* Which guest code produced this matrix. The upload is faithful, so
         * the zero row was computed that way; the caller chain names the
         * lifted function to look at. */
        if (shown == 1) guest_backtrace(c);
    }
    if (e) gl_dispatch(c, e);
}

static void gl_env_param_watch(CPU *c)
{
    static GlEntry *e;
    static int shown;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glProgramEnvParameter4fvARB") == 0) e = &g_gl[i]; }
    const char *dbg = getenv("LINDBERGH_FBSTATS");
    if (shown < 12 && dbg && *dbg && *dbg != '0') {
        shown++;
        const float *v = (const float *)(uintptr_t)A32(3);
        if (v) fprintf(stderr, "[gl] env[%u] = %g %g %g %g\n", A32(1), v[0], v[1], v[2], v[3]);
    }
    static int once;
    if (!once) { once = 1;
        fprintf(stderr, "[gl] envParam4fv fn=%p args=%x %x %x %x\n",
                e ? e->fn : 0, A32(0), A32(1), A32(2), A32(3)); }
    if (e) gl_dispatch(c, e);
}

/* Culling and depth, optionally refused.
 *
 * Splits the two ways a submitted primitive disappears. If the screen lights
 * up with LINDBERGH_NO_REJECT=1, the geometry was reaching the framebuffer and
 * being thrown away by a face winding or a depth comparison. If it stays
 * black, the geometry was never on screen to begin with and the transform is
 * wrong. A diagnostic either way - a game drawn with no depth test is not a
 * game. */
static int no_reject(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_NO_REJECT");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

static void gl_enable_filter(CPU *c)
{
    static GlEntry *e;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glEnable") == 0) e = &g_gl[i]; }
    if (e && !e->fn) e->fn = gl_resolve(e->name);
    if (!e || !e->fn) { RET(0); return; }

    uint32_t cap = A32(0);

    /* LINDBERGH_NO_VP=1 refuses the vertex programs. Fixed-function then runs
     * with the identity matrices this engine never sets - which is exactly
     * right for geometry that is already in clip space, and wrong for
     * everything else. A test of whether the vertices are where they should
     * be, not a way to play. */
    if (cap == 0x8620) {                     /* GL_VERTEX_PROGRAM_ARB */
        static int off = -1;
        if (off < 0) { const char *v = getenv("LINDBERGH_NO_VP");
                       off = (v && *v && *v != '0') ? 1 : 0; }
        if (off) { RET(0); return; }
        g_vp_on = 1;
    }

    /* LINDBERGH_NO_FP=1 refuses the fragment programs, leaving fixed-function
     * shading. Geometry that is correctly placed but shaded black is
     * indistinguishable from geometry that was never on screen; this tells the
     * two apart, because fixed-function will paint it in its vertex colours. */
    if (cap == 0x8804) {                     /* GL_FRAGMENT_PROGRAM_ARB */
        static int off = -1;
        if (off < 0) { const char *v = getenv("LINDBERGH_NO_FP");
                       off = (v && *v && *v != '0') ? 1 : 0; }
        if (off) { RET(0); return; }
    }
    /* GL_SCISSOR_TEST belongs here too: glClear obeys it, so a stale scissor
     * box left over from a small offscreen pass clips the whole frame - the
     * clears included - and the result is indistinguishable from drawing
     * nothing at all. */
    if (no_reject() && (cap == GL_CULL_FACE || cap == GL_DEPTH_TEST ||
                        cap == GL_SCISSOR_TEST || cap == GL_TEXTURE_2D)) { RET(0); return; }
    uint32_t args[1] = { cap };
    RET(gl_forward(e->fn, args, 1));
}

/* glDisable, watched for the same reason glEnable is. */
static void gl_disable_watch(CPU *c)
{
    static GlEntry *e;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glDisable") == 0) e = &g_gl[i]; }
    if (A32(0) == 0x8620) g_vp_on = 0;
    if (e) gl_dispatch(c, e);
}

/* The vertex coordinates, as the guest computed them.
 *
 * Everything downstream has been measured and cleared, so the remaining
 * unmeasured input is the geometry itself. The engine computes these in
 * lifted code; if that arithmetic is wrong the vertices are simply somewhere
 * else, and no amount of correct GL state will show them. */
static void gl_vertex_watch(CPU *c)
{
    static GlEntry *e;
    static int shown;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glVertex3f") == 0) e = &g_gl[i]; }
    const char *dbg = getenv("LINDBERGH_FBSTATS");
    if (shown < 10 && dbg && *dbg && *dbg != '0') {
        shown++;
        float v[3];
        for (int i = 0; i < 3; i++) { uint32_t b = A32(i); memcpy(&v[i], &b, 4); }
        fprintf(stderr, "[gl] vertex %g %g %g\n", v[0], v[1], v[2]);
    }
    if (e) gl_dispatch(c, e);
}

/* Compressed texture uploads, checked.
 *
 * A texture that fails to upload is not an error the game will see - it binds
 * the name, samples it, and gets black. With every geometry stage cleared, a
 * scene drawn entirely in black textures is exactly what a black frame looks
 * like. */
static void gl_compressed_tex(CPU *c)
{
    static GlEntry *e;
    static int shown;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glCompressedTexImage2DARB") == 0) e = &g_gl[i]; }
    while (glGetError() != GL_NO_ERROR) { }          /* start clean */
    if (e) gl_dispatch(c, e);
    GLenum err = glGetError();
    if (shown < 100000 && getenv("LINDBERGH_FBSTATS")) {
        shown++;
        /* Is there anything IN the texture? A correct upload of an empty
         * buffer samples black, and the call itself reports success. */
        const unsigned char *d = (const unsigned char *)(uintptr_t)A32(7);
        unsigned nz = 0, n = A32(6) < 4096 ? A32(6) : 4096;
        if (d) for (unsigned i = 0; i < n; i++) if (d[i]) nz++;
        fprintf(stderr, "[gl] compressedTex #%u level %u fmt 0x%X %ux%u size %u -> 0x%X, "
                        "%u/%u bytes non-zero\n",
                shown, A32(1), A32(2), A32(3), A32(4), A32(6), err, nz, n);
    }
}

/* Every clear, with where it went and whether it took.
 *
 * The one contradiction left: the engine clears to white and the back buffer
 * never shows white, while an identical clear issued from this runtime reads
 * back perfectly. Reading one pixel straight after each clear says which of
 * those two stories is true, per call, with no theory in between. */
static void gl_clear_watch(CPU *c)
{
    static GlEntry *e;
    static int shown;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glClear") == 0) e = &g_gl[i]; }

    int look = shown < 14 && getenv("LINDBERGH_FBSTATS") != NULL;
    GLfloat cc[4] = {0,0,0,0};
    GLint fbo = 0;
    if (look) {
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glGetIntegerv(0x8CA6 /* FRAMEBUFFER_BINDING_EXT */, &fbo);
    }

    if (e) gl_dispatch(c, e);

    if (look) {
        shown++;
        unsigned char px[3] = {0,0,0};
        glReadBuffer(fbo ? 0x8CE0 /* GL_COLOR_ATTACHMENT0_EXT */ : GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(4, 4, 1, 1, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
        fprintf(stderr, "[clr] mask 0x%X fbo %d colour %.2f %.2f %.2f -> pixel %u %u %u\n",
                A32(0), (int)fbo, cc[0], cc[1], cc[2], px[2], px[1], px[0]);
        fflush(stderr);
    }
}

/* The framebuffer completeness check, and what it answered.
 *
 * An incomplete framebuffer swallows every draw into it without error. The
 * engine asks; if the answer is anything but COMPLETE it has been told its
 * render targets are unusable, and a scene rendered into them goes nowhere. */
static void gl_check_fbo(CPU *c)
{
    static GlEntry *e;
    static int shown;
    if (!e) { for (unsigned i = 0; i < GL_COUNT; i++)
                  if (strcmp(g_gl[i].name, "glCheckFramebufferStatusEXT") == 0) e = &g_gl[i]; }
    if (e) gl_dispatch(c, e);
    if (shown < 10 && getenv("LINDBERGH_FBSTATS")) {
        shown++;
        GLint fbo = 0;
        glGetIntegerv(0x8CA6, &fbo);
        /* Attachment sizes: a viewport bigger than the colour attachment is
         * clipped to nothing, which looks exactly like a black render target. */
        typedef void (__stdcall *PFNGA)(unsigned, unsigned, unsigned, GLint *);
        static PFNGA getatt;
        if (!getatt) getatt = (PFNGA)wglGetProcAddress("glGetFramebufferAttachmentParameterivEXT");
        GLint cw = 0, ch = 0, dw = 0, dh = 0, cobj = 0, dobj = 0;
        if (getatt) {
            /* GL_FRAMEBUFFER_EXT=0x8D40, COLOR_ATTACHMENT0=0x8CE0,
             * DEPTH_ATTACHMENT=0x8D00, OBJECT_NAME=0x8CD1 */
            getatt(0x8D40, 0x8CE0, 0x8CD1, &cobj);
            getatt(0x8D40, 0x8D00, 0x8CD1, &dobj);
            if (cobj) { GLint o = cobj;
                glBindTexture(0x0DE1, o);
                glGetTexLevelParameteriv(0x0DE1, 0, 0x1000 /*GL_TEXTURE_WIDTH*/, &cw);
                glGetTexLevelParameteriv(0x0DE1, 0, 0x1001 /*GL_TEXTURE_HEIGHT*/, &ch); }
            if (dobj) { GLint o = dobj;
                typedef void (__stdcall *PFNBR)(unsigned, unsigned);
                static PFNBR bindrb;
                if (!bindrb) bindrb = (PFNBR)wglGetProcAddress("glBindRenderbufferEXT");
                if (bindrb) bindrb(0x8D41, o);
                typedef void (__stdcall *PFNGR)(unsigned, unsigned, GLint *);
                static PFNGR getrb;
                if (!getrb) getrb = (PFNGR)wglGetProcAddress("glGetRenderbufferParameterivEXT");
                if (getrb) { getrb(0x8D41, 0x8D42 /*WIDTH*/, &dw); getrb(0x8D41, 0x8D43 /*HEIGHT*/, &dh); }
                if (bindrb) bindrb(0x8D41, 0); }
        }
        fprintf(stderr, "[fbo] status of %d = 0x%X %s | color %dx%d obj %d depth %dx%d obj %d\n",
                (int)fbo, c->eax, c->eax == 0x8CD5 ? "(COMPLETE)" : "(NOT COMPLETE)",
                (int)cw, (int)ch, (int)cobj, (int)dw, (int)dh, (int)dobj);
    }
}

/* ---- GLU ----
 *
 * Three functions from a different library, and all three take GLdouble, so
 * each argument is two stack slots rather than one and the generic forwarder
 * cannot describe them. They are also small enough to just do.
 */
static double a_dbl(CPU *c, int slot)
{
    uint64_t bits = (uint64_t)A32(slot) | ((uint64_t)A32(slot + 1) << 32);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

/* Six GLdoubles, so twelve stack slots. The projection matrix calls are the
 * one part of fixed-function GL that still uses double precision. */
static void h_glOrtho(CPU *c)
{
    glOrtho(a_dbl(c, 0), a_dbl(c, 2), a_dbl(c, 4),
            a_dbl(c, 6), a_dbl(c, 8), a_dbl(c, 10));
    RET(0);
}

static void h_glFrustum(CPU *c)
{
    glFrustum(a_dbl(c, 0), a_dbl(c, 2), a_dbl(c, 4),
              a_dbl(c, 6), a_dbl(c, 8), a_dbl(c, 10));
    RET(0);
}

/* gluLookAt is nine doubles and no GL call of its own: it builds a basis from
 * the eye, the centre and the up vector, and multiplies it in. */
static void h_gluLookAt(CPU *c)
{
    double ex = a_dbl(c,0),  ey = a_dbl(c,2),  ez = a_dbl(c,4);
    double cx = a_dbl(c,6),  cy = a_dbl(c,8),  cz = a_dbl(c,10);
    double ux = a_dbl(c,12), uy = a_dbl(c,14), uz = a_dbl(c,16);

    double fx = cx - ex, fy = cy - ey, fz = cz - ez;
    double fl = sqrt(fx*fx + fy*fy + fz*fz);
    if (fl > 0.0) { fx /= fl; fy /= fl; fz /= fl; }

    double sx = fy*uz - fz*uy, sy = fz*ux - fx*uz, sz = fx*uy - fy*ux;
    double sl = sqrt(sx*sx + sy*sy + sz*sz);
    if (sl > 0.0) { sx /= sl; sy /= sl; sz /= sl; }

    double tx = sy*fz - sz*fy, ty = sz*fx - sx*fz, tz = sx*fy - sy*fx;

    float m[16];
    m[0]=(float)sx; m[4]=(float)sy; m[8] =(float)sz;  m[12]=0.0f;
    m[1]=(float)tx; m[5]=(float)ty; m[9] =(float)tz;  m[13]=0.0f;
    m[2]=(float)-fx;m[6]=(float)-fy;m[10]=(float)-fz; m[14]=0.0f;
    m[3]=0.0f;      m[7]=0.0f;      m[11]=0.0f;       m[15]=1.0f;
    glMultMatrixf(m);
    glTranslatef((float)-ex, (float)-ey, (float)-ez);
    RET(0);
}

static void h_gluOrtho2D(CPU *c)
{
    glOrtho(a_dbl(c, 0), a_dbl(c, 2), a_dbl(c, 4), a_dbl(c, 6), -1.0, 1.0);
    RET(0);
}

/* The textbook definition: a symmetric frustum with the near plane sized by
 * half the vertical field of view. */
static void h_gluPerspective(CPU *c)
{
    double fovy = a_dbl(c, 0), aspect = a_dbl(c, 2);
    double zn = a_dbl(c, 4), zf = a_dbl(c, 6);
    double top = zn * tan(fovy * 3.14159265358979323846 / 360.0);
    double right = top * aspect;
    glFrustum(-right, right, -top, top, zn, zf);
    RET(0);
}

static void h_gluErrorString(CPU *c)
{
    static const char *msg[] = { "no error", "invalid enum", "invalid value",
                                 "invalid operation", "stack overflow",
                                 "stack underflow", "out of memory" };
    uint32_t e = A32(0);
    const char *t = (e >= 0x500 && e <= 0x505) ? msg[e - 0x500 + 1] : msg[0];
    RET((uint32_t)(uintptr_t)t);
}

/* The game's GL wrapper calls the *Parameter4fvARB uploads with a THREE-arg
 * convention - (target, index, params) - where params points at a single
 * vec4 and count is implicit 1. The real entry point wants four args, so
 * forwarding the guest stack verbatim either starves the driver (slots=3:
 * count reads as a wild pointer-sized value and the upload is rejected, which
 * is why every shader constant arrived wrong) or feeds it a bogus params
 * pointer (slots=4: arg3 is whatever sat above the call, e.g. 0xff000000,
 * and the driver faults reading it). Synthesize count=1 explicitly. */
static void h_ProgramParam4fvARB(CPU *c, const char *name)
{
    /* The game's wrapper passes (target, index, params) for a single vec4.
     * The scalar 4f variant takes the same leading args plus x,y,z,w inline -
     * feed it the four floats read out of the guest buffer. */
    char nm[64]; snprintf(nm, sizeof nm, "%.60s", name);
    { size_t l = strlen(nm); if (l > 5 && !strcmp(nm + l - 5, "fvARB"))
          memcpy(nm + l - 4, "ARB", 4); }   /* ...4fvARB -> ...4fARB */
    typedef void (APIENTRY *PFN)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
    static struct { const char *n; PFN f; } res[2];
    PFN fn = NULL;
    for (int i = 0; i < 2; i++) {
        if (!res[i].n) { res[i].n = i ? "glProgramLocalParameter4fARB"
                                     : "glProgramEnvParameter4fARB"; }
        if (strcmp(res[i].n, nm) == 0) {
            if (!res[i].f) res[i].f = (PFN)gl_resolve(res[i].n);
            fn = res[i].f;
        }
    }
    const float *v = (const float *)(uintptr_t)A32(2);
    float x = v ? v[0] : 0.f, y = v ? v[1] : 0.f,
          z = v ? v[2] : 0.f, w = v ? v[3] : 0.f;
    if (fn) fn(A32(0), A32(1), x, y, z, w);
    { static int pd; if (pd < 30) { pd++;
        fprintf(stderr, "[pp] %s t=%x i=%u p=%#x v=%g %g %g %g fn=%p\n", name,
                A32(0), A32(1), A32(2), (double)x, (double)y, (double)z, (double)w,
                (void *)fn); } }
    RET(0);
}
static void h_glProgramEnvParameter4fvARB(CPU *c)
{ h_ProgramParam4fvARB(c, "glProgramEnvParameter4fvARB"); }
static void h_glProgramLocalParameter4fvARB(CPU *c)
{ h_ProgramParam4fvARB(c, "glProgramLocalParameter4fvARB"); }

/* The four imports that never made it into g_gl[] - small enough to write
 * directly rather than grow the trampoline table. */
static void h_glDrawArraysEXT(CPU *c)
{
    glDrawArrays(A32(0), A32(1), A32(2));
    RET(0);
}
static void h_glWindowPos2sARB(CPU *c)
{
    /* GLshort x,y - the low 16 bits of each slot, sign-extended. glWindowPos2i
     * is GL 1.4 and not in opengl32's export table, so resolve it the same way
     * the extension entries do. */
    typedef void (APIENTRY *PFN)(GLint, GLint);
    static PFN fn;
    if (!fn) fn = (PFN)wglGetProcAddress("glWindowPos2i");
    if (fn) fn((GLint)(int16_t)A32(0), (GLint)(int16_t)A32(1));
    RET(0);
}
static void h_glCopyPixels(CPU *c)
{
    /* (x, y, width, height, pixels) - pixels is a guest pointer into the
     * flat map, so it is a host pointer here. */
    glCopyPixels((GLint)A32(0), (GLint)A32(1), (GLsizei)A32(2),
                 (GLsizei)A32(3), (const GLvoid *)(uintptr_t)A32(4));
    RET(0);
}
static void h_glGetQueryObjectivARB(CPU *c)
{
    typedef void (APIENTRY *PFN)(GLuint, GLenum, GLint *);
    static PFN fn;
    if (!fn) fn = (PFN)wglGetProcAddress("glGetQueryObjectivARB");
    if (fn) fn(A32(0), A32(1), (GLint *)(uintptr_t)A32(2));
    RET(0);
}

void hle_register_gl(void)
{
    int n = 0;
    { const char *e = getenv("LINDBERGH_GLERR"); g_err_watch = e && *e && *e != '0'; }
    for (unsigned i = 0; i < GL_COUNT; i++)
        n += hle_bind(g_gl[i].name, g_gl_handlers[i]);
    hle_bind("glProgramStringARB", gl_program_string);   /* watched, see above */
    hle_bind("glBindFramebufferEXT", gl_bind_framebuffer);
    hle_bind("glBegin", gl_begin_watch);
    hle_bind("glEnd", gl_end_watch);
    hle_bind("glDrawRangeElements", gl_draw_range);
    hle_bind("glCheckFramebufferStatusEXT", gl_check_fbo);
    hle_bind("glClear", gl_clear_watch);
    hle_bind("glCompressedTexImage2DARB", gl_compressed_tex);
    hle_bind("glVertex3f", gl_vertex_watch);
    hle_bind("glEnable", gl_enable_filter);
    hle_bind("glDisable", gl_disable_watch);
    hle_bind("glProgramEnvParameter4fvARB", h_glProgramEnvParameter4fvARB);
    hle_bind("glProgramLocalParameter4fvARB", h_glProgramLocalParameter4fvARB);
    hle_bind("glProgramParameters4fvNV", gl_env_params_watch);
    hle_bind("glOrtho", h_glOrtho);
    hle_bind("glFrustum", h_glFrustum);
    hle_bind("gluLookAt", h_gluLookAt);
    hle_bind("gluOrtho2D", h_gluOrtho2D);
    hle_bind("gluPerspective", h_gluPerspective);
    hle_bind("gluErrorString", h_gluErrorString);

    /* Imported by the game but absent from the table above - each was being
     * dropped on the floor:
     *  glDrawArraysEXT     - display lists without an index buffer
     *                        (cShaderUtility::loadDisplayListWithoutIBE)
     *  glWindowPos2sARB +  - cPostEffect::copyDepth2Color blits depth into a
     *  glCopyPixels           colour target for post effects
     *  glGetQueryObjectivARB - cTimerQuery::getTimer (perf only) */
    if (!getenv("LINDBERGH_NO_NEWGL")) {
        hle_bind("glDrawArraysEXT", h_glDrawArraysEXT);
        hle_bind("glWindowPos2sARB", h_glWindowPos2sARB);
        hle_bind("glCopyPixels", h_glCopyPixels);
        hle_bind("glGetQueryObjectivARB", h_glGetQueryObjectivARB);
    }

    fprintf(stderr, "[gl] %d of %u entry points bound\n", n, (unsigned)GL_COUNT);
}


/* ---- entry points the game asks for by name ----
 *
 * es::glh_helper::init_extensions resolves 547 extension functions through
 * glXGetProcAddressARB and sets its feature flags only when they all arrive.
 * Ninety-six of those are in the game's PLT and already have handlers; the
 * rest have no import to bind, so there is nothing to hand back - and a null
 * leaves a flag clear, which is why graphics init gave up after its first
 * glClear.
 *
 * So: resolve the name on the host driver and hand back a synthetic address
 * that dispatch() knows how to route. The guest stores it like any function
 * pointer and calls it like any other.
 *
 * The awkward part is the argument count, which is not knowable for an
 * arbitrary name - and getting it wrong matters, because a __stdcall callee
 * pops its own arguments and a mismatch unbalances the stack. Saving esp
 * across the call removes the question entirely: push a fixed generous number
 * of slots, let the callee take however many it wants, and put esp back where
 * it was. Correct for cdecl and stdcall alike, at any arity.
 */
#define GL_TOKEN_BASE 0xF0000000u
#define GL_TOKEN_MAX  1024
#define GL_TOKEN_ARGS 12        /* the widest GL call here takes nine */

static struct { const char *name; void *fn; } g_tok[GL_TOKEN_MAX];
static int g_tok_n;

static uint32_t asm_forward(void *fn, const uint32_t *a, int n)
{
    uint32_t rv = 0, saved = 0;   /* not "ret": a reserved word in MSVC asm */
    __asm {
        mov     saved, esp
        mov     ecx, n
        mov     edx, a
        test    ecx, ecx
        jz      do_call
    push_loop:
        mov     eax, [edx + ecx*4 - 4]
        push    eax
        dec     ecx
        jnz     push_loop
    do_call:
        mov     eax, fn
        call    eax
        mov     esp, saved      /* whoever popped, esp is right again */
        mov     rv, eax
    }
    return rv;
}

uint32_t gl_token_for(const char *name)
{
    for (int i = 0; i < g_tok_n; i++)
        if (strcmp(g_tok[i].name, name) == 0)
            return GL_TOKEN_BASE + (uint32_t)i;   /* same name, same pointer */

    void *fn = gl_resolve(name);
    /* A name the driver will not hand back returns NULL to the engine, which
     * then quietly does without that entry point - no error, no crash, just a
     * feature that never runs. Worth seeing rather than assuming. */
    if (!fn || g_tok_n >= GL_TOKEN_MAX) {
        fprintf(stderr, "[tok] %s -> NULL%s\n", name,
                g_tok_n >= GL_TOKEN_MAX ? " (token table full)" : "");
        fflush(stderr);
        return 0;
    }

    g_tok[g_tok_n].name = _strdup(name);
    g_tok[g_tok_n].fn   = fn;
    return GL_TOKEN_BASE + (uint32_t)(g_tok_n++);
}

int gl_token_call(CPU *c, uint32_t va)
{
    if (va < GL_TOKEN_BASE || va >= GL_TOKEN_BASE + (uint32_t)g_tok_n) return 0;
    uint32_t args[GL_TOKEN_ARGS];
    for (int i = 0; i < GL_TOKEN_ARGS; i++) args[i] = A32(i);
    RET(asm_forward(g_tok[va - GL_TOKEN_BASE].fn, args, GL_TOKEN_ARGS));
    return 1;
}

int gl_token_count(void) { return g_tok_n; }

#else
#ifndef LINUX_EGL
void hle_register_gl(void) { }
#endif
#endif
