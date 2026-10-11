/* hle_gl_orbis.c - the GL layer, cut for Orbis/PS5.
 *
 * The Win32 hle_gl resolves every GL function off GetProcAddress and forwards
 * the guest's args to the real driver. On the PS5 the same job is ps5-opengl:
 * eglGetProcAddress / dlsym hands back the real GL entry points, and the
 * forwarded call lands the game's draw on the console's GPU.
 *
 * Headless path: with no EGL driver yet, every GL call is a logged no-op that
 * returns a sane default — the game boots, registers its classes, probes the
 * hardware and runs its update logic while the GL side stays inert. Wire the
 * real driver by linking ps5-opengl and flipping ORBIS_EGL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lindbergh_rt.h"
#include <unistd.h>
#include <pthread.h>

#define GL_TOKEN_BASE  0x60000000u
#define GL_TOKEN_MAX   4096
#define GL_TOKEN_ARGS  9

/* GL names the game calls — the same surface the Win32 hle_gl lists. Each
 * resolves to the real entry point on the PS5's GL driver (ps5-opengl). */
static const char *const g_gl_names[] = {
    "glAccum","glAlphaFunc","glArrayElement","glBegin","glBindBuffer","glBindTexture",
    "glBitmap","glBlendFunc","glCallList","glCallLists","glClear","glClearColor",
    "glClearDepth","glClearStencil","glClipPlane","glColor3f","glColor3fv","glColor4f",
    "glColor4fv","glColorMaterial","glColorMask","glColorPointer","glCopyTexImage2D",
    "glCopyTexSubImage2D","glCullFace","glDeleteBuffers","glDeleteLists","glDeleteTextures",
    "glDepthFunc","glDepthMask","glDepthRange","glDisable","glDisableClientState",
    "glDrawArrays","glDrawBuffer","glDrawElements","glDrawPixels","glEnable","glEnableClientState",
    "glEnd","glEndList","glEvalCoord1f","glEvalCoord2f","glEvalMesh1","glEvalMesh2",
    "glEvalPoint1","glEvalPoint2","glFeedbackBuffer","glFinish","glFlush","glFogf",
    "glFogfv","glFogi","glFogiv","glFrontFace","glFrustum","glGenBuffers","glGenLists",
    "glGenTextures","glGetBooleanv","glGetDoublev","glGetError","glGetFloatv","glGetIntegerv",
    "glGetMaterialfv","glGetMaterialiv","glGetPointerv","glGetString","glGetTexEnvfv",
    "glGetTexEnviv","glGetTexImage","glGetTexLevelParameterfv","glGetTexLevelParameteriv",
    "glGetTexParameterfv","glGetTexParameteriv","glHint","glIndexf","glIndexi","glIndexPointer",
    "glInitNames","glIsEnabled","glIsList","glIsTexture","glLightf","glLightfv",
    "glLighti","glLightiv","glLightModelf","glLightModelfv","glLightModeli","glLightModeliv",
    "glLineStipple","glLineWidth","glListBase","glLoadIdentity","glLoadMatrixf","glLoadName",
    "glLogicOp","glMap1f","glMap2f","glMapGrid1f","glMapGrid2f","glMaterialf","glMaterialfv",
    "glMateriali","glMaterialiv","glMatrixMode","glMultMatrixf","glNewList","glNormal3f",
    "glNormal3fv","glNormalPointer","glOrtho","glPassThrough","glPixelStoref","glPixelStorei",
    "glPixelTransferf","glPixelTransferi","glPixelZoom","glPointSize","glPolygonMode",
    "glPolygonOffset","glPopAttrib","glPopClientAttrib","glPopMatrix","glPopName","glPushAttrib",
    "glPushClientAttrib","glPushMatrix","glPushName","glRasterPos2f","glRasterPos3f",
    "glReadBuffer","glReadPixels","glRectf","glRenderMode","glRotatef","glScalef",
    "glScissor","glSelectBuffer","glShadeModel","glStencilFunc","glStencilMask","glStencilOp",
    "glTexCoord1f","glTexCoord2f","glTexCoord3f","glTexCoord4f","glTexCoordPointer",
    "glTexEnvf","glTexEnvfv","glTexEnvi","glTexEnviv","glTexGend","glTexGenf","glTexGeni",
    "glTexImage1D","glTexImage2D","glTexParameterf","glTexParameterfv","glTexParameteri",
    "glTexParameteriv","glTexSubImage1D","glTexSubImage2D","glTranslatef","glVertex2f",
    "glVertex3f","glVertex4f","glVertexPointer","glViewport",
    /* the GLU + the ARB entry points the engine reaches for */
    "glUseProgramObjectARB","glGetUniformLocationARB","glUniformMatrix4fvARB",
    "glBindAttribLocationARB","glCreateShaderObjectARB","glShaderSourceARB",
    "glCompileShaderARB","glCreateProgramObjectARB","glAttachObjectARB",
    "glLinkProgramARB","glGetObjectParameterivARB","glGetInfoLogARB",
    "glActiveTextureARB","glClientActiveTextureARB","glTexImage3D","glTexSubImage3D",
    "glCompressedTexImage2DARB","glCompressedTexSubImage2DARB",
    "glGetBufferParameterivARB","glBufferDataARB","glBufferSubDataARB","glMapBufferARB",
    "glUnmapBufferARB","glIsBufferARB","glGenVertexArrays","glBindVertexArray",
    "glEnableVertexAttribArrayARB","glVertexAttribPointerARB","glGetStringi",
    "glDrawRangeElements","glDrawArraysInstancedARB","glFramebufferTexture2DEXT",
    "glBindFramebufferEXT","glGenFramebuffersEXT","glCheckFramebufferStatusEXT",
    "glGenRenderbuffersEXT","glBindRenderbufferEXT","glRenderbufferStorageEXT",
    "glFramebufferRenderbufferEXT","glDeleteFramebuffersEXT","glDeleteRenderbuffersEXT",
    "glGetFramebufferAttachmentParameterivEXT","glIsFramebufferEXT","glIsRenderbufferEXT",
    "glGenerateMipmapEXT","glBlitFramebufferEXT","glMultiTexCoord2fARB",
    "glBindMultiTextureEXT","glDrawBuffersARB","glVertexAttrib1f","glVertexAttrib2f",
    "glVertexAttrib3f","glVertexAttrib4f","glUniform1iARB","glUniform1fARB",
    "glUniform2fARB","glUniform3fARB","glUniform4fARB","glUniform1fvARB",
    "glUniform2fvARB","glUniform3fvARB","glUniform4fvARB","glUniformMatrix3fvARB",
    "glUniformMatrix4x3fvARB","glUniformMatrix3x4fvARB","glGetAttribLocationARB",
    "glGenQueriesARB","glDeleteQueriesARB","glBeginQueryARB","glEndQueryARB",
    "glGetQueryObjectivARB","glGetQueryObjectuivARB",
    /* additional PLT imports not in the original list */
    "glBindBufferARB","glDeleteBuffersARB","glGenBuffersARB",
    "glBindProgramARB","glDeleteProgramsARB","glGenProgramsARB","glIsProgramARB",
    "glProgramStringARB","glProgramEnvParameter4fvARB","glProgramLocalParameter4fvARB",
    "glProgramParameters4fvNV",
    "glBlendColorEXT","glBlendEquationEXT","glBlendEquationSeparateEXT","glBlendFuncSeparateEXT",
    "glColor4ub","glCopyPixels","glDrawArraysEXT","glDisableVertexAttribArrayARB",
    "glPointParameterfARB","glPrimitiveRestartIndexNV","glWindowPos2sARB",
    "glDeleteObjectARB",
    0
};

#define GL_COUNT (sizeof g_gl_names / sizeof g_gl_names[0])

typedef void (*GlFn)(void);
static GlFn   g_gl_fn[GL_COUNT];
static uint32_t g_gl_calls[GL_COUNT];

static void *gl_resolve(const char *name); /* forward — defined below */

/* ---- Fixed-function immediate-mode shim (gl4es-style) --------------------
 *
 * Mesa's ACO backend crashes compiling its internal FF→shader translation
 * the first time glBegin is called in a compat context.  Intercept the
 * entire immediate-mode path here: accumulate vertices, convert legacy
 * primitive types to core ones, emit once at glEnd via our own GLSL prog
 * + VBO.  Matrix state is queried from Mesa (which still tracks it via
 * glLoadMatrixf etc.) at flush time; ARB vertex/fragment program state is
 * completely undisturbed.
 */

typedef struct { float x,y,z,w; float r,g,b,a; float s,t; } FfVert;
#define FF_MAX_VERTS 16384

static int      s_ff_active;
int             g_ff_emit_count;
int ff_emit_count(void) { return g_ff_emit_count; }
static uint32_t s_ff_prim;
static FfVert   s_ff_verts[FF_MAX_VERTS];
static int      s_ff_nv;
/* current per-vertex state */
static float    s_ff_cr=1,s_ff_cg=1,s_ff_cb=1,s_ff_ca=1;
static float    s_ff_ts=0,s_ff_tt=0;
static uint32_t s_ff_saved_attr;

/* GL function pointers needed by the FF shim, resolved once from the driver.
 * Use the ARB program API (glProgramStringARB) — the same path the game
 * itself uses for its !!ARBfp1.0/!!ARBvp1.0 programs, and classic client
 * arrays (glVertexPointer/glColorPointer/glTexCoordPointer) which the game
 * also exercises. No GLSL shader objects, no VAO/VBO, no uniforms —
 * state.matrix.mvp supplies the fixed-function transform. */
typedef void    (*PFGP)(int,uint32_t*);                    /* glGenProgramsARB */
typedef void    (*PFBP)(uint32_t,uint32_t);                /* glBindProgramARB */
typedef void    (*PFPS)(uint32_t,int,int,const void*);     /* glProgramStringARB */
typedef void    (*PFECS)(uint32_t);                        /* glEnableClientState */
typedef void    (*PFDCS)(uint32_t);                        /* glDisableClientState */
typedef void    (*PFVP)(int,uint32_t,int,const void*);     /* glVertexPointer */
typedef void    (*PFCP)(int,uint32_t,int,const void*);     /* glColorPointer */
typedef void    (*PFTP)(int,uint32_t,int,const void*);     /* glTexCoordPointer */
typedef void    (*PFEN)(uint32_t);                         /* glEnable */
typedef void    (*PFDIS)(uint32_t);                        /* glDisable */
typedef void    (*PFUM4)(int32_t,int,int,const float*);
typedef void    (*PFUI)(int32_t,int);
typedef void    (*PFGFV)(uint32_t,float*);
typedef uint8_t (*PFGIE)(uint32_t);
typedef void    (*PFGIV)(uint32_t,int32_t*);
typedef void    (*PFDA)(uint32_t,int,int);

static PFGP  ff_glGenPrograms;
static PFBP  ff_glBindProgram;
/* bounded substring search — memmem is a GNU ext Orbis may not provide */
static const char *fmemfind(const char *h, int hl, const char *n, int nl)
{
    if (nl <= 0) return h;
    for (int i = 0; i + nl <= hl; i++)
        if (h[i] == n[0] && memcmp(h + i, n, nl) == 0) return h + i;
    return NULL;
}

/* Patch an ARB program source for PS5 Mesa before upload. Handles every
 * construct the driver rejects — OPTION ARB_fragment_program_shadow,
 * OPTION NV_fragment_program2 (+ the SHORT qualifiers and half-precision
 * opcodes it brings), SHADOW2D samplers. Shared by the PLT-dispatch path and
 * the resolved-token path so an NV_fp2 fragment program can't slip through
 * one side and get thrown back as a C++ exception.
 * Operates on args[]: target=arg0, len=arg2, src=arg3. On a hit the source is
 * rewritten into a static buffer and arg2/arg3 repoint to it.
 * Returns -1 to skip the upload entirely, 0 unchanged, 1 patched. */

/* Rewrite opcodes that exist in NV_fp2/NV_vp3 but have no ARB fp/vp 1.0 form
 * at all into equivalent valid sequences, line by line. A bare rename leaves
 * an unrecognised identifier at line start, which is exactly the
 * "unexpected IDENTIFIER" progck failures seen on console and desktop.
 *   NRM d, s    -> DP3 _SC.x,s,s; RSQ _SC.x,_SC.x; MUL d,s,_SC.x;
 *   SGT d, a, b -> SLT d, b, a;          SLE d, a, b -> SGE d, b, a;
 *   SEQ d, a, b -> SGE _SC,a,b; SGE _SD,b,a; MUL d,_SC,_SD;
 *   SNE d, a, b -> SLT _SC,a,b; SLT _SD,b,a; ADD d,_SC,_SD;
 * Vector compares need full-width temps so multi-component dest masks (e.g.
 * SEQ R1.zw) keep working. *sc_used gets bit0 for _SC, bit1 for _SD — the
 * caller injects TEMP decls. cap bounds writes back into buf. */
static int arb_expand_setops(char *buf, int out, int cap, int *sc_used)
{
    static char p5[320000]; int o5 = 0;
    char *r = buf;
    while (*r && o5 < cap - 256) {
        char *nl = strchr(r, '\n');
        int ll = nl ? (int)(nl - r + 1) : (int)strlen(r);
        int k = 0;
        while (k < ll && (r[k] == ' ' || r[k] == '\t')) k++;
        int oe = k;
        while (oe < ll && ((r[oe] >= 'A' && r[oe] <= 'Z') ||
                           (r[oe] >= '0' && r[oe] <= '9') || r[oe] == '_'))
            oe++;
        int opl = oe - k;
        char op[8] = {0};
        if (opl > 0 && opl < 8) memcpy(op, r + k, opl);
        char *sc = NULL;
        for (int i = oe; i < ll; i++) if (r[i] == ';') { sc = r + i; break; }
        /* "-|src|" (negated absolute) is NV syntax — and Mesa's ARB parser
         * doesn't take |src| at all (VP Pass 5 strips pipes for the same
         * reason). Emit "MAX _SC, src, -src;" up front — abs() in one
         * instruction — and swap the operand to "-_SC": keeps both abs and
         * negation exactly. */
        if (sc) {
            char line[512]; int li = 0;
            char ab[2][96]; int nab = 0, o6 = 0;
            static char p6[512]; int has = 0;
            for (int i = 0; i < ll && i < 500; i++) {
                if (r[i] == '-' && r[i+1] == '|' && nab < 2) {
                    char *e = (char *)memchr(r + i + 2, '|',
                                             r + ll - (r + i + 2));
                    if (e && e - (r + i) < 90) {
                        int tl = (int)(e - (r + i)); /* offset of closing | */
                        memcpy(ab[nab], r + i + 2, tl - 2); /* bare operand */
                        ab[nab][tl - 2] = 0;
                        li += snprintf(line + li, sizeof line - li,
                                       "-%s", nab ? "_SD" : "_SC");
                        i += tl; nab++; has = 1;
                        continue;
                    }
                }
                line[li++] = r[i];
            }
            if (has) {
                for (int a = 0; a < nab; a++)
                    o6 += snprintf(p6 + o6, sizeof p6 - o6,
                                   "MAX %s, %s, -%s;\n",
                                   a ? "_SD" : "_SC", ab[a], ab[a]);
                line[li] = 0;
                o6 += snprintf(p6 + o6, sizeof p6 - o6, "%s", line);
                if (o5 + o6 < cap - 1) { memcpy(p5 + o5, p6, o6); o5 += o6; }
                *sc_used |= nab == 2 ? 3 : 1;
                if (!nl) break;
                r = nl + 1;
                continue;
            }
        }
        int isp5 = (opl == 3 && (op[0] == 'N' || op[0] == 'S') &&
                    (!memcmp(op, "NRM", 3) || !memcmp(op, "SGT", 3) ||
                     !memcmp(op, "SLE", 3) || !memcmp(op, "SEQ", 3) ||
                     !memcmp(op, "SNE", 3)));
        if (isp5 && sc) {
            char a1[80] = {0}, a2[80] = {0}, a3[80] = {0};
            char *av[3] = {a1, a2, a3}; int na = 0, w = 0;
            for (char *q = r + oe; q < sc && na < 3; q++) {
                if (*q == ',') { av[na][w] = 0; w = 0; na++; }
                else if (*q != ' ' && *q != '\t' && *q != '\n' && w < 79)
                    av[na][w++] = *q;
            }
            if (na < 3) av[na][w] = 0;
            na++;
            char eb[400]; int el = 0;
            if (!memcmp(op, "NRM", 3) && na == 2) {
                el = snprintf(eb, sizeof eb,
                    "DP3 _SC.x, %s, %s;\nRSQ _SC.x, _SC.x;\nMUL %s, %s, _SC.x;",
                    a2, a2, a1, a2);
                *sc_used |= 1;
            } else if ((!memcmp(op, "SGT", 3) || !memcmp(op, "SLE", 3))
                       && na == 3) {
                el = snprintf(eb, sizeof eb, "%s %s, %s, %s;",
                    op[1] == 'G' ? "SLT" : "SGE", a1, a3, a2);
            } else if ((!memcmp(op, "SEQ", 3) || !memcmp(op, "SNE", 3))
                       && na == 3) {
                const char *cmp = op[1] == 'E' ? "SGE" : "SLT";
                const char *cat = op[1] == 'E' ? "MUL" : "ADD";
                el = snprintf(eb, sizeof eb,
                    "%s _SC, %s, %s;\n%s _SD, %s, %s;\n%s %s, _SC, _SD;",
                    cmp, a2, a3, cmp, a3, a2, cat, a1);
                *sc_used |= 3;
            }
            if (el > 0 && o5 + el < cap - 1) {
                memcpy(p5 + o5, eb, el); o5 += el;
                p5[o5++] = '\n';
            } else { memcpy(p5 + o5, r, ll); o5 += ll; }
        } else { memcpy(p5 + o5, r, ll); o5 += ll; }
        if (!nl) break;
        r = nl + 1;
    }
    p5[o5] = 0;
    memcpy(buf, p5, o5 + 1);
    return o5;
}

static int gl_patch_arb_prog(uint32_t *args)
{
    uint32_t target = args[0];
    const char *src = (const char *)(uintptr_t)args[3];
    int len = (int)args[2];
    if (!src || len <= 0) return 0;
    if (strstr(src, "vertex.attrib[1]")) return -1;
    /* The patched source is passed to Mesa as args[3] — a 32-bit value. A host
     * static buffer lives above 4 GB, so (uint32_t)s_patched truncates and
     * glProgramStringARB reads a bad address (rip=0). Allocate the buffer from
     * guest_brk instead: the 32-bit guest VA IS a valid host pointer because
     * the guest arena is identity-mapped. */
    /* The patched source is passed to Mesa as args[3] — a 32-bit value, so it
     * must be a guest-arena pointer (guest VA = host VA). A single shared
     * buffer is unsafe: Mesa can compile lazily on a bg thread and read the
     * source after the NEXT program has overwritten it → stale/racing text →
     * rip=0 in the compiler. Each call gets its own guest_brk slot so the
     * text stays stable for the life of the program. */
#define PATCH_BUFSZ 16384
#define PATCH_SLOTS 512
    static char *s_base;
    static uint32_t s_base_gva, s_slot;
    if (!s_base) {
        /* Two threads compiling lazily could both see s_base==NULL and carve
         * separate arenas, then interleave s_base/s_base_gva — writes would
         * land in whichever region lost the race. Serialize the init. */
        extern void guest_heap_lock(void), guest_heap_unlock(void);
        guest_heap_lock();
        if (!s_base) {
            extern uint32_t guest_brk(uint32_t);
            s_base_gva = (guest_brk(0) + 0xfu) & ~0xfu;
            guest_brk(s_base_gva + (uint32_t)PATCH_BUFSZ * PATCH_SLOTS);
            s_base = (char *)(uintptr_t)s_base_gva;
        }
        guest_heap_unlock();
    }
    /* Fragment program text uploaded to the vertex target (game bug: vcarbonDSF
     * vertex init path passes !!ARBfp1.0/NV_fragment_program2 src to 0x8620).
     * Mesa rejects it on a background thread and raises SIGABRT — skip it.
     * The mirror bug exists too: vp text uploaded to 0x8804 fails at errpos=1
     * (bad header for the target). Real HW rejects both; skip both ways. */
    if (target == 0x8620u && src && strstr(src, "NV_fragment_program2"))
        return -1;
    if (target == 0x8804u && src &&
        (strstr(src, "!!ARBvp") || strstr(src, "NV_vertex_program3")))
        return -1;
    { extern void klogf(const char*, ...);
      klogf("[patch] tgt=%#x fp2=%d vp3=%d sh2d=%d len=%d\n",
            target, src && strstr(src,"NV_fragment_program2")?1:0,
            src && strstr(src,"NV_vertex_program3")?1:0,
            src && strstr(src,"SHADOW2D")?1:0, len); }
    int need_patch = ((target == 0x8804u /*GL_FRAGMENT_PROGRAM_ARB*/) &&
                      (strstr(src, "ARB_fragment_program_shadow") ||
                       strstr(src, "SHADOW2D") ||
                       strstr(src, "NV_fragment_program2"))) ||
                     ((target == 0x8620u /*GL_VERTEX_PROGRAM_ARB*/) &&
                      (strstr(src, "NV_vertex_program3") ||
                       strstr(src, "_SAT")));
    if (!need_patch) return 0;
    /* consume a slot only for shaders that actually get patched */
    uint32_t slot = __sync_fetch_and_add(&s_slot, 1) & (PATCH_SLOTS - 1);
    uint32_t s_patch_gva = s_base_gva + slot * PATCH_BUFSZ;
    char *s_patched = s_base + slot * PATCH_BUFSZ;
    { static int _ppn;
      if (_ppn < 8) {
          char pf[64];
          snprintf(pf, sizeof pf, "/tmp/progpre_%d.txt", _ppn++);
          FILE *f = fopen(pf, "w");
          if (f) { fwrite(src, 1, len, f); fclose(f); }
      } }
    /* A/B5: fpstub early-return disabled — fp2 sources now flow through the
     * full rewrite path below (half-precision downgrade, CC-mask strip,
     * OUTPUT-alias resolve). If Mesa/psbc accepts the rewritten text, real
     * shading replaces the unlit stub. Watch for [patch] tgt lines followed
     * by compile errors or submit rejects. */
    int out = 0, need_sc = 0;
    for (int i = 0; i < len && out < PATCH_BUFSZ - 1; i++) {
        /* Strip the cgc '#' metadata comments — Mesa's ARB parser treats a
         * bare '#' as a comment too, but the fp40 headers carry #program /
         * #semantic / #var / "# N instructions" lines that some parsers trip
         * on. Dropping whole comment lines is lossless. */
        if (src[i] == '#') {
            while (i < len && src[i] != '\n') i++;
            if (i >= len) break;
            s_patched[out++] = '\n';      /* keep the newline, drop the line */
            continue;
        }
        /* comment out the OPTION lines PS5 Mesa rejects */
        if (src[i] == 'O' && strncmp(src + i, "OPTION NV_fragment_program2;", 28) == 0)
            s_patched[out++] = '#';
        if (src[i] == 'O' && strncmp(src + i, "OPTION NV_vertex_program3;", 26) == 0)
            s_patched[out++] = '#';
        /* SHADOW2D -> 2D: skip the "SHADOW" prefix */
        if (src[i] == 'S' && strncmp(src + i, "SHADOW2D", 8) == 0)
            i += 6;
        /* NV_fp2 precision qualifiers: drop "SHORT " / "LONG " so the line
         * is plain TEMP/OUTPUT. */
        if (src[i] == 'S' && strncmp(src + i, "SHORT TEMP", 10) == 0)
            i += 6;
        if (src[i] == 'S' && strncmp(src + i, "SHORT OUTPUT", 12) == 0)
            i += 6;
        if (src[i] == 'L' && strncmp(src + i, "LONG TEMP", 9) == 0)
            i += 5;
        /* NV_fp2 condition-code registers RC / HC are implicit and must not
         * appear as TEMP decls in plain ARBfp1.0 (reserved keyword clash).
         * Drop the whole declaration line.  Also fires for "SHORT TEMP RC;"
         * because the SHORT-skip above already advanced i to 'T'. */
        if (src[i] == 'T' && (strncmp(src+i,"TEMP RC;",8)==0 ||
                               strncmp(src+i,"TEMP HC;",8)==0 ||
                               strncmp(src+i,"TEMP RC,",8)==0 ||
                               strncmp(src+i,"TEMP HC,",8)==0)) {
            while (i < len && src[i] != '\n') i++;
            if (i < len) s_patched[out++] = '\n';
            continue;
        }
        /* fragment.color.secondary / .primary are NV_fp2 interpolant names —
         * strict ARB_fp1 only knows fragment.color. Map both onto the single
         * interpolant so the Mesa frontend doesn't reject the qualifier. */
        if (src[i] == 'f' && strncmp(src + i, "fragment.color.secondary", 24) == 0) {
            if (out + 14 >= PATCH_BUFSZ) break;
            memcpy(s_patched + out, "fragment.color", 14); out += 14; i += 23; continue;
        }
        if (src[i] == 'f' && strncmp(src + i, "fragment.color.primary", 22) == 0) {
            if (out + 14 >= PATCH_BUFSZ) break;
            memcpy(s_patched + out, "fragment.color", 14); out += 14; i += 21; continue;
        }
        if (out < PATCH_BUFSZ - 1)
            s_patched[out++] = src[i];
    }
    s_patched[out] = 0;

    /* Second pass — downgrade NV_fp2 half-precision opcodes (MOVH, MULH,
     * MADR…) to their full-precision ARB_fp1 forms. Only when the OPTION was
     * actually present in this source. */
    if (strstr(src, "NV_fragment_program2")) {
        /* Strip NV_fp2 precision qualifiers: SHORT/LONG before TEMP/PARAM/OUTPUT.
         * Mesa ARBfp1.0 parser doesn't understand SHORT/LONG and crashes on them. */
        {
            static const char *quals[] = {"SHORT ", "LONG ", NULL};
            for (int qi = 0; quals[qi]; qi++) {
                int qlen = (int)strlen(quals[qi]);
                char *p = s_patched;
                while ((p = (char*)fmemfind(p, out - (int)(p - s_patched), quals[qi], qlen))) {
                    int tail = out - (int)(p - s_patched) - qlen;
                    memmove(p, p + qlen, tail + 1);
                    out -= qlen;
                }
            }
        }
        static const struct { const char *f; int fl; const char *t; int tl; }
        nvhalf[] = {
            {"MOVH",4,"MOV",3},{"ADDH",4,"ADD",3},{"MULH",4,"MUL",3},
            {"SUBH",4,"SUB",3},{"MADH",4,"MAD",3},{"MADR",4,"MAD",3},
            {"MINH",4,"MIN",3},{"MAXH",4,"MAX",3},{"RSQH",4,"RSQ",3},
            {"RCPH",4,"RCP",3},{"DP3H",4,"DP3",3},{"DP4H",4,"DP4",3},
            {"DPAH",4,"DP4",3},{"FRCH",4,"FRC",3},{"FLRH",4,"FLR",3},
            {"ABSH",4,"ABS",3},{"NRMH",4,"NRM",3},{"LGHH",4,"LG2",3},
            {"LRPH",4,"LRP",3},{"LPRH",4,"LRP",3},
            {"SUBC",4,"SUB",3},
            {"SGEH",4,"SGE",3},{"SGTH",4,"SGT",3},
            /* NV_fp2 R-precision opcodes → ARBfp1.0 base forms */
            {"ADDR",4,"ADD",3},{"SUBR",4,"SUB",3},{"MULR",4,"MUL",3},
            {"MINR",4,"MIN",3},{"MAXR",4,"MAX",3},{"RCPR",4,"RCP",3},
            {"RSQR",4,"RSQ",3},{"MOVR",4,"MOV",3},{"FLRR",4,"FLR",3},
            {"FRCR",4,"FRC",3},{"DP3R",4,"DP3",3},{"DP4R",4,"DP4",3},
            {"LRPR",4,"LRP",3},{"EX2R",4,"EX2",3},{"LG2R",4,"LG2",3},
            {"SLTR",4,"SLT",3},{"SGER",4,"SGE",3},{"ABSR",4,"ABS",3},
            /* bare-C condition-write ops (HC/RC are dropped by pass 4a):
             * strip the C so the data result is still produced */
            {"MOVC",4,"MOV",3},{"MULC",4,"MUL",3},{"ADDC",4,"ADD",3},
            {"SGTC",4,"SGT",3},{"SLTC",4,"SLT",3},{"SGEC",4,"SGE",3},
            {"MOVXC",5,"MOV",3},
            {"MULH_SAT ",9,"MUL_SAT ",8},
            {NULL,0,NULL,0}
        };
        for (int t = 0; nvhalf[t].f; t++) {
            char *p = s_patched;
            int rem;
            while ((rem = out - (int)(p - s_patched)) > 0 &&
                   (p = (char*)fmemfind(p, rem, nvhalf[t].f, nvhalf[t].fl))) {
                char next = p[nvhalf[t].fl];
                /* only replace on an opcode boundary (space/tab/_ after) */
                if (next != ' ' && next != '\t' && next != '_' && next != ';') { p++; continue; }
                int tail = out - (int)(p - s_patched) - nvhalf[t].fl;
                memmove(p + nvhalf[t].tl, p + nvhalf[t].fl, tail + 1);
                memcpy(p, nvhalf[t].t, nvhalf[t].tl);
                out += nvhalf[t].tl - nvhalf[t].fl;
                p += nvhalf[t].tl;
            }
        }

        /* Pass 4a — NV_fp2 condition-register write instructions (SLTHC, SGTRC,
         * MOVHC, etc.) write to HC./RC. and have no ARBfp1.0 equivalent.
         * Drop any line whose opcode (first space-delimited token) ends in HC or RC.
         * These instructions only exist to drive the predicate in the next
         * instruction, which Pass 4b handles by making it unconditional. */
        {
            static char p4[160000]; int o4 = 0;
            char *r = s_patched;
            while (*r && o4 < (int)sizeof(p4) - 1) {
                char *nl = strchr(r, '\n');
                int ll = nl ? (int)(nl - r + 1) : (int)strlen(r);
                int drop = 0;
                /* scan for end of first token (opcode) */
                for (int k = 1; k < ll && k < 12; k++) {
                    if (r[k] == ' ' || r[k] == '\t') {
                        if (k >= 2 && ((r[k-2]=='H' && r[k-1]=='C') ||
                                        (r[k-2]=='R' && r[k-1]=='C')))
                            drop = 1;
                        break;
                    }
                }
                /* also drop a plain opcode whose DESTINATION is a condition
                 * register — "MOV RC.x, H1" has no ARBfp1.0 form either */
                if (!drop) {
                    int k = 0;
                    while (k < ll && (r[k] == ' ' || r[k] == '\t')) k++;
                    while (k < ll && r[k] != ' ' && r[k] != '\t') k++;
                    while (k < ll && (r[k] == ' ' || r[k] == '\t')) k++;
                    if (k + 3 <= ll &&
                        ((r[k]=='R'&&r[k+1]=='C'&&r[k+2]=='.') ||
                         (r[k]=='H'&&r[k+1]=='C'&&r[k+2]=='.') ||
                         (r[k]=='C'&&r[k+1]=='C'&&r[k+2]=='.')))
                        drop = 1;
                }
                if (!drop) { memcpy(p4+o4, r, ll); o4 += ll; }
                else        { p4[o4++] = '\n'; }
                if (!nl) break; r = nl + 1;
            }
            p4[o4] = 0;
            memcpy(s_patched, p4, o4+1); out = o4;
        }
        /* Pass 4b — strip NV_fp2 conditional execution masks from instructions.
         * Pattern: " (GT.x)" / " (LT)" etc. after a destination register.
         * Makes the instruction execute unconditionally (visual artefact but
         * no crash). */
        {
            static const char *cctok[] = {
                "(GT","(LT","(GE","(LE","(EQ","(NE","(TR","(FL",NULL};
            for (int ci = 0; cctok[ci]; ci++) {
                int clen = (int)strlen(cctok[ci]);
                char *p = s_patched;
                while ((p = (char*)fmemfind(p, out-(int)(p-s_patched),
                                            cctok[ci], clen))) {
                    char *cl = strchr(p, ')');
                    if (!cl) break;
                    /* eat the space/tab before '(' as well */
                    char *start = (p > s_patched &&
                                   (p[-1]==' '||p[-1]=='\t')) ? p-1 : p;
                    int rm = (int)(cl - start + 1);
                    int tail = out - (int)(cl+1 - s_patched);
                    memmove(start, cl+1, tail+1);
                    out -= rm;
                    /* re-scan from same position — don't advance p */
                }
            }
        }

        /* Third pass — NV_fp2 result aliases. "OUTPUT <alias> = <result.reg>;"
         * has no ARB_fp1 form: results are implicit and there is no OUTPUT
         * keyword, so the line is a parse error and every use of the alias is
         * an undeclared identifier. Drop the decl line and rename the alias to
         * the real result register it points at (e.g. oCol -> result.color). */
        {
            static char alias[8][64], target[8][64];
            int na = 0;
            static char pass2[160000];
            int o2 = 0;
            char *q = s_patched;
            while (*q && o2 < (int)sizeof(pass2) - 1) {
                char *e = strchr(q, '\n');
                int ll = e ? (int)(e - q + 1) : (int)strlen(q);
                if (strncmp(q, "OUTPUT ", 7) == 0 && na < 8) {
                    char a[64] = {0}, t[64] = {0};
                    if (sscanf(q, "OUTPUT %63s = %63[^;];", a, t) == 2) {
                        strcpy(alias[na], a); strcpy(target[na], t); na++;
                        if (!e) break;
                        q = e + 1;          /* drop the OUTPUT decl */
                        continue;
                    }
                }
                memcpy(pass2 + o2, q, ll); o2 += ll;
                if (!e) break;
                q = e + 1;
            }
            pass2[o2] = 0;
            if (na) {
                static char pass3[160000];
                int o3 = 0;
                for (int i = 0; i < o2 && o3 < (int)sizeof(pass3) - 1; i++) {
                    int done = 0;
                    for (int a = 0; a < na && !done; a++) {
                        size_t al = strlen(alias[a]);
                        char prev = i ? pass2[i - 1] : 0;
                        char nx = pass2[i + al];
                        int pw = (prev == '_') || (prev >= '0' && prev <= '9') ||
                                 (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z');
                        int nw = (nx == '_') || (nx >= '0' && nx <= '9') ||
                                 (nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z');
                        if (strncmp(pass2 + i, alias[a], al) == 0 && !pw && !nw) {
                            int tl = strlen(target[a]);
                            memcpy(pass3 + o3, target[a], tl); o3 += tl;
                            i += (int)al - 1; done = 1;
                        }
                    }
                    if (!done) pass3[o3++] = pass2[i];
                }
                pass3[o3] = 0;
                memcpy(s_patched, pass3, o3 + 1);
                out = o3;
            }
        }

        /* Pass 5 — NRM / SGT / SLE / SEQ / SNE have no ARBfp1.0 form at all;
         * arb_expand_setops rewrites each into a valid sequence (details in
         * the helper). Scratch temps _SC/_SD get TEMP decls injected below. */
        out = arb_expand_setops(s_patched, out, PATCH_BUFSZ, &need_sc);
    }

    /* ---- NV_vertex_program3 / ARBvp _SAT patching ----
     * The VP block covers two cases: cgc vp40 output (OPTION
     * NV_vertex_program3) and plain !!ARBvp1.0 programs that still use
     * OP_SAT — the _SAT opcode suffix is NV_vp grammar; base ARBvp1.0 and
     * Mesa's parser both reject it. */
    if (target == 0x8620u &&
        (strstr(src, "NV_vertex_program3") || strstr(src, "_SAT"))) {
        /* VP Pass 0: OP_SAT dst, a, ...; -> OP dst, a, ...;
         *            MIN dst, dst, {1};  MAX dst, dst, {0};
         * The dest operand is copied verbatim so writemasks survive
         * (SAT only clamps the components the write touches). Lines whose
         * first operand can't be a dest (KIL_SAT, comments, labels) keep
         * their _SAT stripped bare — still compiles, clamps lost. */
        {
            static char vp0[160000]; int o0 = 0;
            char *r = s_patched;
            while (*r && o0 < (int)sizeof(vp0) - 256) {
                char *nl = strchr(r, '\n');
                int ll = nl ? (int)(nl - r + 1) : (int)strlen(r);
                /* find first token (opcode) */
                int k = 0;
                while (k < ll && (r[k] == ' ' || r[k] == '\t')) k++;
                int os = k;
                while (k < ll && ((r[k] >= 'A' && r[k] <= 'Z') ||
                                  (r[k] >= '0' && r[k] <= '9') || r[k] == '_'))
                    k++;
                int opl = k - os;
                int sat = (opl >= 7 && !memcmp(r + k - 4, "_SAT", 4));
                if (sat && os < ll && r[os] != '#' && r[os] != '!') {
                    /* dest operand = text between op end and first ',' */
                    int d0 = k;
                    while (d0 < ll && (r[d0] == ' ' || r[d0] == '\t')) d0++;
                    int d1 = d0;
                    while (d1 < ll && r[d1] != ',' && r[d1] != ';' &&
                           r[d1] != '\n' && r[d1] != ' ' && r[d1] != '\t')
                        d1++;
                    int valid_dst = d1 > d0 && d1 < ll && r[d1] == ',' &&
                        r[d0] != '-' && r[d0] != '{' &&
                        !(d1 - d0 >= 4 && !memcmp(r + d0, "c[", 2)) &&
                        !(d1 - d0 >= 7 && !memcmp(r + d0, "program.", 7)) &&
                        !(d1 - d0 >= 5 && !memcmp(r + d0, "state.", 5));
                    if (valid_dst) {
                        int lend = nl ? ll - 1 : ll;   /* exclude the newline */
                        /* emit: <indent><op minus _SAT><rest> then clamps */
                        for (int i = 0; i < os; i++) vp0[o0++] = r[i];
                        for (int i = os; i < k - 4; i++) vp0[o0++] = r[i];
                        for (int i = k; i < lend; i++) vp0[o0++] = r[i];
                        vp0[o0++] = '\n';
                        /* clamp lines reuse the dest verbatim */
                        for (int i = 0; i < os; i++) vp0[o0++] = r[i];
                        o0 += __builtin_snprintf(vp0 + o0, sizeof(vp0) - o0,
                            "MIN %.*s, %.*s, {1};\n",
                            d1 - d0, r + d0, d1 - d0, r + d0);
                        for (int i = 0; i < os; i++) vp0[o0++] = r[i];
                        o0 += __builtin_snprintf(vp0 + o0, sizeof(vp0) - o0,
                            "MAX %.*s, %.*s, {0};\n",
                            d1 - d0, r + d0, d1 - d0, r + d0);
                        if (!nl) break;
                        r = nl + 1;
                        continue;
                    }
                    /* no usable dst — just strip the _SAT suffix in place */
                    for (int i = 0; i < k - 4; i++) vp0[o0++] = r[i];
                    for (int i = k; i < ll; i++) vp0[o0++] = r[i];
                    if (!nl) break;
                    r = nl + 1;
                    continue;
                }
                memcpy(vp0 + o0, r, ll); o0 += ll;
                if (!nl) break;
                r = nl + 1;
            }
            vp0[o0] = 0;
            memcpy(s_patched, vp0, o0 + 1); out = o0;
        }
        /* VP Pass 1: rename NV_vp3 opcodes to ARBvp1.0 equivalents.
         * MOVC lines that write to CC/RC/HC are dropped in VP Pass 2, so the
         * few MOVC lines that survived write to a normal register — rename them. */
        static const struct { const char *f; int fl; const char *t; int tl; }
        nvvp3[] = {
            {"SUBC",4,"SUB",3},
            {"MOVC",4,"MOV",3},
            {"SGTC",4,"SGT",3},
            {"SEQC",4,"SEQ",3},
            {"SLEC",4,"SLE",3},
            {"SGEC",4,"SGE",3},
            {"SLTC",4,"SLT",3},
            {NULL,0,NULL,0}
        };
        for (int t = 0; nvvp3[t].f; t++) {
            char *p = s_patched;
            int rem;
            while ((rem = out - (int)(p - s_patched)) > 0 &&
                   (p = (char*)fmemfind(p, rem, nvvp3[t].f, nvvp3[t].fl))) {
                char next = p[nvvp3[t].fl];
                if (next != ' ' && next != '\t' && next != '_' && next != ';') { p++; continue; }
                int tail = out - (int)(p - s_patched) - nvvp3[t].fl;
                memmove(p + nvvp3[t].tl, p + nvvp3[t].fl, tail + 1);
                memcpy(p, nvvp3[t].t, nvvp3[t].tl);
                out += nvvp3[t].tl - nvvp3[t].fl;
                p += nvvp3[t].tl;
            }
        }

        /* VP Pass 2: drop lines whose first argument (destination) is a
         * condition register CC./RC./HC. — these are NV_vp3 condition writes
         * with no ARBvp1.0 equivalent. */
        {
            static char vp2[160000]; int ov = 0;
            char *r = s_patched;
            while (*r && ov < (int)sizeof(vp2) - 1) {
                char *nl = strchr(r, '\n');
                int ll = nl ? (int)(nl - r + 1) : (int)strlen(r);
                int drop = 0;
                /* skip opcode token, find first arg */
                int sp = 0;
                for (int k = 0; k < ll && k < 20; k++) {
                    if (r[k] == ' ' || r[k] == '\t') { sp = k + 1; break; }
                }
                if (sp > 0) {
                    while (sp < ll && (r[sp]==' '||r[sp]=='\t')) sp++;
                    if (sp + 3 <= ll &&
                        ((r[sp]=='C'&&r[sp+1]=='C'&&r[sp+2]=='.') ||
                         (r[sp]=='R'&&r[sp+1]=='C'&&r[sp+2]=='.') ||
                         (r[sp]=='H'&&r[sp+1]=='C'&&r[sp+2]=='.')))
                        drop = 1;
                }
                if (!drop) { memcpy(vp2+ov, r, ll); ov += ll; }
                else        { vp2[ov++] = '\n'; }
                if (!nl) break; r = nl + 1;
            }
            vp2[ov] = 0;
            memcpy(s_patched, vp2, ov+1); out = ov;
        }

        /* VP Pass 3: strip conditional execution masks (GT.x), (LE), etc.
         * Works on BRA target (LE.x) too — makes BRA unconditional which is
         * acceptable vs. a Mesa crash. */
        {
            static const char *cctok[] = {
                "(GT","(LT","(GE","(LE","(EQ","(NE","(TR","(FL",NULL};
            for (int ci = 0; cctok[ci]; ci++) {
                int clen = (int)strlen(cctok[ci]);
                char *p = s_patched;
                while ((p = (char*)fmemfind(p, out-(int)(p-s_patched),
                                            cctok[ci], clen))) {
                    char *cl = strchr(p, ')');
                    if (!cl) break;
                    char *start = (p > s_patched &&
                                   (p[-1]==' '||p[-1]=='\t')) ? p-1 : p;
                    int rm = (int)(cl - start + 1);
                    int tail = out - (int)(cl+1 - s_patched);
                    memmove(start, cl+1, tail+1);
                    out -= rm;
                }
            }
        }

        /* VP Pass 4: fix ARL scalar — NV_vp3 allows "ARL A0, Rn" but
         * ARBvp1.0 requires "ARL A0.x, Rn.x". Insert .x on dest and source
         * when no swizzle is already present. */
        {
            char *p = s_patched;
            int rem;
            while ((rem = out - (int)(p - s_patched)) > 0 &&
                   (p = (char*)fmemfind(p, rem, "\nARL ", 5))) {
                p++; /* advance past newline, now points at 'A' of ARL */
                char *comma = (char*)memchr(p, ',', 24);
                if (!comma) continue;
                int has_dot = 0;
                for (char *q = p + 4; q < comma; q++) if (*q == '.') { has_dot = 1; break; }
                if (!has_dot) {
                    memmove(comma + 2, comma, out - (int)(comma - s_patched) + 1);
                    comma[0] = '.'; comma[1] = 'x';
                    out += 2;
                    /* also fix source: skip whitespace after comma */
                    char *src2 = comma + 2;
                    while (*src2 == ' ' || *src2 == '\t') src2++;
                    char *src_end = s_patched + out;
                    char *nl2 = (char*)memchr(src2, '\n', src_end - src2);
                    if (nl2) src_end = nl2;
                    int src_has_dot = 0;
                    for (char *q = src2; q < src_end; q++) if (*q == '.') { src_has_dot = 1; break; }
                    if (!src_has_dot && src_end > src2) {
                        int ip = (int)(src_end - s_patched);
                        memmove(s_patched + ip + 2, s_patched + ip, out - ip + 1);
                        s_patched[ip] = '.'; s_patched[ip+1] = 'x';
                        out += 2;
                    }
                }
            }
        }

        /* VP Pass 5: strip |abs| pipe-absolute syntax — "|Rn|" → "Rn".
         * Drops the absolute-value semantic but avoids a Mesa parse error. */
        {
            char *p = s_patched;
            while ((p = (char*)fmemfind(p, out - (int)(p - s_patched), "|", 1))) {
                int rem2 = out - (int)(p - s_patched);
                memmove(p, p + 1, rem2); /* rem2 includes terminating NUL */
                out--;
                /* don't advance p — scan again from same pos for consecutive | */
            }
        }

        /* VP Pass 6: replace address-register swizzle .y/.z/.w → .x —
         * ARBvp1.0 address register A0 only has a single .x component. */
        {
            static const struct { const char *f; int fl; } adr_sw[] = {
                {"A0.y",4},{"A0.z",4},{"A0.w",4},
                {NULL,0}
            };
            for (int t = 0; adr_sw[t].f; t++) {
                char *p = s_patched;
                int rem2;
                while ((rem2 = out - (int)(p - s_patched)) > 0 &&
                       (p = (char*)fmemfind(p, rem2, adr_sw[t].f, adr_sw[t].fl))) {
                    p[3] = 'x'; /* overwrite the swizzle in place */
                    p += adr_sw[t].fl;
                }
            }
        }

        /* VP Pass 7: same opcode expansion as the fp path — Pass 1 renamed
         * SGTC/SEQC/etc to their bare forms, which ARBvp1.0 doesn't have
         * either (SGT/SEQ/SLE/SNE are NV_vp3-only). */
        out = arb_expand_setops(s_patched, out, PATCH_BUFSZ, &need_sc);
    }

    /* Final pass — declare used-but-undeclared H<n> registers. NV_fp2 treats
     * H0..Hn as implicit half temporaries; ARBfp1.0 predeclares R0..R3 only,
     * so a bare "MOV H0.xz, ..." is an undeclared identifier and fails the
     * whole program. Scan non-comment, non-decl text for H<n> operands and
     * inject "TEMP H<n>;" for the missing set right after the !!ARB header.
     * Bytes outside printable ASCII (except \n,\t) are forced to spaces —
     * embedded control bytes abort Mesa's lexer mid-string. */
    {
        unsigned huse = 0, hdecl = 0;
        char *r = s_patched;
        while (*r) {
            char *nl = strchr(r, '\n');
            int ll = nl ? (int)(nl - r) : (int)strlen(r);
            char *s2 = r;
            while (s2 < r + ll && (*s2 == ' ' || *s2 == '\t')) s2++;
            int is_comment = (s2 < r + ll && *s2 == '#');
            int is_decl = 0;
            if (!is_comment && s2 + 4 <= r + ll) {
                static const char *dkw[] = {
                    "TEMP", "ATTRIB", "OUTPUT", "PARAM", "ALIAS", "OPTION", 0};
                for (int d = 0; dkw[d]; d++) {
                    int dl = (int)strlen(dkw[d]);
                    if (s2 + dl <= r + ll && strncmp(s2, dkw[d], dl) == 0 &&
                        (s2[dl] == ' ' || s2[dl] == '\t')) { is_decl = 1; break; }
                }
            }
            for (int i = 0; i < ll; i++) {
                unsigned char ch = (unsigned char)r[i];
                if ((ch < 32 && ch != '\t') || ch > 126) r[i] = ' ';
            }
            if (!is_comment) {
                for (int i = 0; i + 1 < ll; i++) {
                    if (r[i] != 'H' || r[i+1] < '0' || r[i+1] > '9') continue;
                    /* word boundary: previous char must not be ident */
                    if (i > 0 && ((r[i-1] >= 'A' && r[i-1] <= 'Z') ||
                                  (r[i-1] >= 'a' && r[i-1] <= 'z') ||
                                  (r[i-1] >= '0' && r[i-1] <= '9') || r[i-1] == '_'))
                        continue;
                    int n = 0, j = i + 1;
                    while (j < ll && r[j] >= '0' && r[j] <= '9')
                        n = n * 10 + (r[j++] - '0');
                    /* next char must end the token (not an ident char) */
                    if (j < ll && ((r[j] >= 'A' && r[j] <= 'Z') ||
                                   (r[j] >= 'a' && r[j] <= 'z') || r[j] == '_'))
                        continue;
                    if (n < 32) { if (is_decl) hdecl |= 1u << n; else huse |= 1u << n; }
                    i = j;
                }
            }
            if (!nl) break;
            r = nl + 1;
        }
        unsigned hmiss = huse & ~hdecl;
        int inj_sc = need_sc && !strstr(s_patched, "TEMP _SC");
        if (hmiss || inj_sc) {
            char inj[220]; int ij = 0;
            if (inj_sc & 1)
                ij += snprintf(inj + ij, sizeof(inj) - ij, "TEMP _SC;\n");
            if (inj_sc & 2)
                ij += snprintf(inj + ij, sizeof(inj) - ij, "TEMP _SD;\n");
            for (int n = 0; n < 32; n++)
                if ((hmiss >> n) & 1)
                    ij += snprintf(inj + ij, sizeof(inj) - ij, "TEMP H%d;\n", n);
            char *h2 = strstr(s_patched, "!!ARB");
            char *ins = h2 ? strchr(h2, '\n') : NULL;
            if (ins && ij > 0 && out + ij < PATCH_BUFSZ - 1) {
                ins++;
                int mv = out - (int)(ins - s_patched);
                memmove(ins + ij, ins, mv + 1);
                memcpy(ins, inj, ij);
                out += ij;
            }
        }
    }

    args[2] = (uint32_t)out;
    args[3] = s_patch_gva;
    /* Log the first lines of the patched source so the klog shows exactly what
     * Mesa receives for the program that crashes glProgramStringARB. */
    { static int _pn;
      if (_pn++ < 4) {
        extern int sceKernelDebugOutText(int, const char*);
        char b[220]; int n = 0;
        n += __builtin_snprintf(b + n, sizeof(b) - n, "[patch] fp gva=%#x len=%d\n", s_patch_gva, out);
        int nl2 = 0;
        for (int i = 0; i < out && n < (int)sizeof(b) - 3 && nl2 < 8; i++) {
            char ch = s_patched[i];
            b[n++] = (ch >= 32 && ch < 127) ? ch : (ch == '\n' ? (nl2++, '|') : '.');
        }
        b[n++] = '\n'; b[n] = 0;
        sceKernelDebugOutText(0, b);
      } }
    return 1;
}
static PFPS  ff_glProgramString;
static PFECS ff_glEnableClientState;
static PFDCS ff_glDisableClientState;
static PFVP  ff_glVertexPointer;
static PFCP  ff_glColorPointer;
static PFTP  ff_glTexCoordPointer;
static PFEN  ff_glEnable;
static PFDIS ff_glDisable;
static PFUM4 ff_glUniformMatrix4fv;
static PFUI  ff_glUniform1i;
static PFGFV ff_glGetFloatv;
static PFGIE ff_glIsEnabled;
static PFGIV ff_glGetIntegerv;
static PFDA  ff_glDrawArrays;

static uint32_t s_ff_vp, s_ff_fp_tex, s_ff_fp_flat;
static int      s_ff_ready;

static void ff_matmul(float *C, const float *A, const float *B)
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += A[r+k*4]*B[k+c*4];
            C[r+c*4] = v;
        }
}

static void ff_init(void)
{
#if defined(ORBIS_EGL) || defined(LINUX_EGL)
    if (s_ff_ready) return;
    s_ff_ready = 1;

#define FFRES(T,sym,name) ff_##sym = (T)gl_resolve(name)
    FFRES(PFGP, glGenPrograms,          "glGenProgramsARB");
    FFRES(PFBP, glBindProgram,          "glBindProgramARB");
    FFRES(PFPS, glProgramString,        "glProgramStringARB");
    FFRES(PFECS,glEnableClientState,    "glEnableClientState");
    FFRES(PFDCS,glDisableClientState,   "glDisableClientState");
    FFRES(PFVP, glVertexPointer,        "glVertexPointer");
    FFRES(PFCP, glColorPointer,         "glColorPointer");
    FFRES(PFTP, glTexCoordPointer,      "glTexCoordPointer");
    FFRES(PFEN, glEnable,               "glEnable");
    FFRES(PFDIS,glDisable,              "glDisable");
    FFRES(PFGIE,glIsEnabled,            "glIsEnabled");
    FFRES(PFGIV,glGetIntegerv,          "glGetIntegerv");
    FFRES(PFDA, glDrawArrays,           "glDrawArrays");
#undef FFRES

    {
        static const char *const names[] = {
            "glGenProgramsARB","glBindProgramARB","glProgramStringARB",
            "glEnableClientState","glDisableClientState",
            "glVertexPointer","glColorPointer","glTexCoordPointer",
            "glEnable","glDisable","glIsEnabled","glGetIntegerv","glDrawArrays"};
        const void *ptrs[] = {
            ff_glGenPrograms,ff_glBindProgram,ff_glProgramString,
            ff_glEnableClientState,ff_glDisableClientState,
            ff_glVertexPointer,ff_glColorPointer,ff_glTexCoordPointer,
            ff_glEnable,ff_glDisable,ff_glIsEnabled,ff_glGetIntegerv,ff_glDrawArrays};
        int miss = 0;
        for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++)
            if (!ptrs[i]) {
                extern int sceKernelDebugOutText(int, const char*);
                char b[80]; snprintf(b, sizeof(b), "[ff] NULL resolve: %s\n", names[i]);
                sceKernelDebugOutText(0, b);
                miss = 1;
            }
        if (miss) return;
    }

    /* ARB assembly programs — the same pipeline the game's own shaders use.
     * Position passes straight through: the ACROBATA UI emits clip-space
     * vertices and the game's real transforms go through ARB program
     * parameters, not the GL matrix stack — state.matrix.mvp here would be
     * Mesa's uninitialized FF matrix (garbage). */
    static const char vprog[] =
        "!!ARBvp1.0\n"
        "DP4 result.position.x, state.matrix.mvp.row[0], vertex.position;\n"
        "DP4 result.position.y, state.matrix.mvp.row[1], vertex.position;\n"
        "DP4 result.position.z, state.matrix.mvp.row[2], vertex.position;\n"
        "DP4 result.position.w, state.matrix.mvp.row[3], vertex.position;\n"
        "MOV result.color, vertex.color;\n"
        "MOV result.texcoord[0], vertex.texcoord[0];\n"
        "END\n";
    static const char fprog_tex[] =
        "!!ARBfp1.0\n"
        "TEMP t;\n"
        "TEX t, fragment.texcoord[0], texture[0], 2D;\n"
        "MUL result.color, fragment.color, t;\n"
        "END\n";
    static const char fprog_flat[] =
        "!!ARBfp1.0\n"
        "MOV result.color, fragment.color;\n"
        "END\n";

    {extern int sceKernelDebugOutText(int,const char*); sceKernelDebugOutText(0,"[ff] gen progs\n");}
    ff_glGenPrograms(1, &s_ff_vp);
    {extern int sceKernelDebugOutText(int,const char*); char _b[48]; __builtin_snprintf(_b,sizeof _b,"[ff] vp=%u\n",s_ff_vp); sceKernelDebugOutText(0,_b);}
    ff_glBindProgram(0x8620u /*GL_VERTEX_PROGRAM_ARB*/, s_ff_vp);
    ff_glProgramString(0x8620u, 0x8875u /*GL_PROGRAM_FORMAT_ASCII_ARB*/,
                       (int)sizeof(vprog)-1, vprog);
    ff_glGenPrograms(1, &s_ff_fp_tex);
    ff_glBindProgram(0x8804u /*GL_FRAGMENT_PROGRAM_ARB*/, s_ff_fp_tex);
    ff_glProgramString(0x8804u, 0x8875u, (int)sizeof(fprog_tex)-1, fprog_tex);
    ff_glGenPrograms(1, &s_ff_fp_flat);
    ff_glBindProgram(0x8804u, s_ff_fp_flat);
    ff_glProgramString(0x8804u, 0x8875u, (int)sizeof(fprog_flat)-1, fprog_flat);
    {extern int sceKernelDebugOutText(int,const char*); char _b[64];
     __builtin_snprintf(_b,sizeof _b,"[ff] progs vp=%u fp_tex=%u fp_flat=%u\n",
                        s_ff_vp,s_ff_fp_tex,s_ff_fp_flat);
     sceKernelDebugOutText(0,_b);}

    /* Report ARB program compile status — a syntax error leaves the program
     * unusable and emits nothing. */
    {
        typedef const char *(*PFGPES)(uint32_t);
        PFGPES geterr = (PFGPES)gl_resolve("glGetProgramErrorStringARB");
        int32_t errpos = -99;
        extern int sceKernelDebugOutText(int,const char*);
        char _b[128];
        ff_glBindProgram(0x8620u, s_ff_vp);
        ff_glGetIntegerv(0x864Bu /*GL_PROGRAM_ERROR_POSITION_ARB*/, &errpos);
        const char *es = geterr ? geterr(0x8620u) : "?";
        __builtin_snprintf(_b,sizeof _b,"[ff] vp errpos=%d err=%s\n",
                           errpos, es ? es : "null");
        sceKernelDebugOutText(0,_b);
        ff_glBindProgram(0x8804u, s_ff_fp_tex);
        errpos = -99;
        ff_glGetIntegerv(0x864Bu, &errpos);
        es = geterr ? geterr(0x8804u) : "?";
        __builtin_snprintf(_b,sizeof _b,"[ff] fp errpos=%d err=%s\n",
                           errpos, es ? es : "null");
        sceKernelDebugOutText(0,_b);
    }
#endif
}

/* Emit accumulated immediate-mode vertices */
static void ff_emit(void)
{
#if defined(ORBIS_EGL) || defined(LINUX_EGL)
    if (!s_ff_nv || !s_ff_vp || !ff_glDrawArrays) { s_ff_nv=0; return; }

    /* Convert legacy primitives to triangles */
    static FfVert s_emit_buf[FF_MAX_VERTS*3];
    const FfVert *src = s_ff_verts;
    int sn = s_ff_nv;
    uint32_t prim = s_ff_prim;
    FfVert *dst = s_emit_buf;
    int dn = 0;

    if (prim == 0x0007u /*GL_QUADS*/) {
        for (int i = 0; i+3 < sn; i += 4) {
            dst[dn++]=src[i]; dst[dn++]=src[i+1]; dst[dn++]=src[i+2];
            dst[dn++]=src[i]; dst[dn++]=src[i+2]; dst[dn++]=src[i+3];
        }
        prim = 0x0004u; src = dst; sn = dn;
    } else if (prim == 0x0009u /*GL_POLYGON*/) {
        for (int i = 1; i+1 < sn; i++) {
            dst[dn++]=src[0]; dst[dn++]=src[i]; dst[dn++]=src[i+1];
        }
        prim = 0x0004u; src = dst; sn = dn;
    } else if (prim == 0x0008u /*GL_QUAD_STRIP*/) {
        for (int i = 0; i+3 < sn; i += 2) {
            dst[dn++]=src[i];   dst[dn++]=src[i+1]; dst[dn++]=src[i+2];
            dst[dn++]=src[i+1]; dst[dn++]=src[i+3]; dst[dn++]=src[i+2];
        }
        prim = 0x0004u; src = dst; sn = dn;
    } else if (prim == 0x0005u /*GL_TRIANGLE_STRIP*/) {
        for (int i = 0; i+2 < sn; i++) {
            if (i & 1) { dst[dn++]=src[i+1]; dst[dn++]=src[i]; dst[dn++]=src[i+2]; }
            else       { dst[dn++]=src[i];   dst[dn++]=src[i+1]; dst[dn++]=src[i+2]; }
        }
        prim = 0x0004u; src = dst; sn = dn;
    } else if (prim == 0x0006u /*GL_TRIANGLE_FAN*/) {
        for (int i = 1; i+1 < sn; i++) {
            dst[dn++]=src[0]; dst[dn++]=src[i]; dst[dn++]=src[i+1];
        }
        prim = 0x0004u; src = dst; sn = dn;
    }

    if (sn == 0) { s_ff_nv=0; return; }

    /* Push ALL server+client attrib state so the kill-states, write masks,
     * array disables and program binds below can't leak into the game's own
     * pipeline — restoring only the bits we explicitly touch left depth
     * test, blend and the generic attrib arrays permanently trashed. */
    typedef void (*PFPA)(uint32_t);
    typedef void (*PFPV)(void);
    static PFPA s_push, s_pushc; static PFPV s_pop, s_popc;
    if (!s_push)  s_push  = (PFPA)gl_resolve("glPushAttrib");
    if (!s_pop)   s_pop   = (PFPV)gl_resolve("glPopAttrib");
    if (!s_pushc) s_pushc = (PFPA)gl_resolve("glPushClientAttrib");
    if (!s_popc)  s_popc  = (PFPV)gl_resolve("glPopClientAttrib");
    if (s_push)  s_push(0x000FFFFFu /*GL_ALL_ATTRIB_BITS*/);
    if (s_pushc) s_pushc(0x000FFFFFu /*GL_CLIENT_ALL_ATTRIB_BITS*/);

    /* Save the game's ARB program state, then draw with our own FF programs.
     * Drawing with no program active causes a null-deref inside Mesa even in
     * compat mode — we must always have a valid pipeline. Our ARB programs
     * read vertex.position/color/texcoord from the client arrays and
     * state.matrix.mvp for the transform, same as the game's own programs. */
    int32_t prev_vp = 0, prev_fp = 0;
    uint8_t vp_en = 0, fp_en = 0;
    typedef void (*PFGPIV)(uint32_t,uint32_t,int32_t*);
    static PFGPIV gpiv;
    if (!gpiv) gpiv = (PFGPIV)gl_resolve("glGetProgramivARB");
    if (gpiv) {
        gpiv(0x8620u, 0x8677u /*GL_PROGRAM_BINDING_ARB*/, &prev_vp);
        gpiv(0x8804u, 0x8677u, &prev_fp);
    }
    vp_en = ff_glIsEnabled(0x8620u /*GL_VERTEX_PROGRAM_ARB*/);
    fp_en = ff_glIsEnabled(0x8804u /*GL_FRAGMENT_PROGRAM_ARB*/);

    uint8_t tex_en = ff_glIsEnabled(0x0DE1u/*GL_TEXTURE_2D*/);
    /* Kill only the rasterizer states that would eat geometry outright —
     * keep BLEND and ALPHA_TEST: the game composites translucent fade and
     * vignette quads over the frame, and forcing them opaque paints the
     * screen with a dark overlay. */
    static const uint32_t kill_states[] = {
        0x0B71u /*DEPTH_TEST*/, 0x0B44u /*CULL_FACE*/, 0x0C11u /*SCISSOR_TEST*/,
        0x0B90u /*STENCIL_TEST*/,
        0x3000u /*CLIP_PLANE0*/, 0x3001u /*CLIP_PLANE1*/, 0x3002u /*CLIP_PLANE2*/,
        0x3003u /*CLIP_PLANE3*/, 0x3004u /*CLIP_PLANE4*/, 0x3005u /*CLIP_PLANE5*/};
    for (unsigned i = 0; i < sizeof(kill_states)/sizeof(kill_states[0]); i++)
        ff_glDisable(kill_states[i]);
    /* The game may have left writes masked off — force colour+depth+stencil
     * write enables back on so the quad can't draw to nothing. */
    {
        typedef void (*PFCM)(uint8_t,uint8_t,uint8_t,uint8_t);
        typedef void (*PFDM)(uint8_t);
        typedef void (*PFSM)(uint32_t);
        static PFCM cm; static PFDM dm; static PFSM sm;
        if (!cm) cm = (PFCM)gl_resolve("glColorMask");
        if (!dm) dm = (PFDM)gl_resolve("glDepthMask");
        if (!sm) sm = (PFSM)gl_resolve("glStencilMask");
        if (cm) cm(1,1,1,1); if (dm) dm(1); if (sm) sm(0xFFFFFFFFu);
    }
    /* Environment LINDBERGH_FF_GAMEPROG=1 keeps the game's bound programs
     * for transform/shading; default uses ours — the game's VPs transform
     * via program.env banks that emit-path draws don't always populate,
     * collapsing clip-space quads to a point. Our VP applies
     * state.matrix.mvp: identity for clip verts, live ortho for
     * window-space verts (real now that ARB-alias names resolve). */
    static int s_use_gameprog = -1;
    if (s_use_gameprog < 0) { extern char *getenv(const char *);
        const char *e = getenv("LINDBERGH_FF_GAMEPROG");
        s_use_gameprog = e ? (*e == '1') : 1; }
    int use_vp = s_use_gameprog && prev_vp && vp_en;
    int use_fp = s_use_gameprog && prev_fp && fp_en;
    ff_glEnable(0x8620u /*GL_VERTEX_PROGRAM_ARB*/);
    ff_glBindProgram(0x8620u, use_vp ? (uint32_t)prev_vp : s_ff_vp);
    ff_glEnable(0x8804u /*GL_FRAGMENT_PROGRAM_ARB*/);
    ff_glBindProgram(0x8804u, use_fp ? (uint32_t)prev_fp
                                    : (tex_en ? s_ff_fp_tex : s_ff_fp_flat));

    /* The game leaves other client arrays enabled (e.g. NORMAL_ARRAY with a
     * NULL pointer); Mesa reads them at draw time and faults on NULL. Enable
     * only the three arrays we feed; kill the rest. */
    static const uint32_t kill_arrays[] = {
        0x8075u /*NORMAL_ARRAY*/, 0x8077u /*INDEX_ARRAY*/,
        0x8079u /*EDGE_FLAG_ARRAY*/, 0x807Au /*FOG_COORD_ARRAY*/,
        0x845Eu /*SECONDARY_COLOR_ARRAY*/, 0x86ADu /*WEIGHT_ARRAY_ARB*/};
    for (unsigned i = 0; i < sizeof(kill_arrays)/sizeof(kill_arrays[0]); i++)
        if (ff_glIsEnabled(kill_arrays[i])) ff_glDisableClientState(kill_arrays[i]);
    {
        /* generic attrib arrays — the game may have enabled a few. Save the
         * enable bits and restore them after the draw: whether
         * glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS) covers generic
         * attribs is driver-defined, and leaking a disabled array silences
         * every glDrawRangeElements the game sends afterwards. */
        typedef void (*PFDVA)(uint32_t);
        typedef void (*PFEVA)(uint32_t);
        typedef void (*PFGVA2)(uint32_t,uint32_t,int32_t*);
        static PFDVA dva; static PFEVA eva; static PFGVA2 gva2;
        if (!dva) dva = (PFDVA)gl_resolve("glDisableVertexAttribArrayARB");
        if (!eva) eva = (PFEVA)gl_resolve("glEnableVertexAttribArrayARB");
        if (!gva2) gva2 = (PFGVA2)gl_resolve("glGetVertexAttribivARB");
        { static int once; if (!once) { once = 1;
            extern int sceKernelDebugOutText(int, const char*);
            char b[112]; __builtin_snprintf(b,sizeof b,
                "[ff] clientstack pushc=%p popc=%p attrsave dva=%p gva=%p\n",
                (void*)s_pushc,(void*)s_popc,(void*)dva,(void*)gva2);
            sceKernelDebugOutText(0,b); } }
        s_ff_saved_attr = 0;
        if (gva2) for (uint32_t i = 0; i < 16; i++) {
            int32_t e = 0; gva2(i, 0x8622u /*GL_VERTEX_ATTRIB_ARRAY_ENABLED*/, &e);
            if (e) s_ff_saved_attr |= (1u << i);
        }
        if (dva) for (uint32_t i = 0; i < 16; i++) dva(i);
    }

    ff_glEnableClientState(0x8074u /*GL_VERTEX_ARRAY*/);
    ff_glVertexPointer(4, 0x1406u /*GL_FLOAT*/, (int)sizeof(FfVert), src);
    ff_glEnableClientState(0x8076u /*GL_COLOR_ARRAY*/);
    ff_glColorPointer(4, 0x1406u, (int)sizeof(FfVert), (const char*)src + 16);
    ff_glEnableClientState(0x8078u /*GL_TEXTURE_COORD_ARRAY*/);
    ff_glTexCoordPointer(2, 0x1406u, (int)sizeof(FfVert), (const char*)src + 32);

    static int s_emit_logged;
    /* Sample the bound framebuffer every 512th emit forever — are any
     * immediate-mode quads still landing on fb0 mid-run, or does the whole
     * frame stay inside FBOs? */
    if (s_emit_logged < 8 || (g_ff_emit_count & 0x1ff) == 0x1ff) {
        s_emit_logged++;
        extern int sceKernelDebugOutText(int, const char*);
        int32_t fb = -1;
        ff_glGetIntegerv(0x8CA6u /*GL_FRAMEBUFFER_BINDING_EXT*/, &fb);
        typedef uint32_t (*PFGE)(void);
        static PFGE ge;
        if (!ge) ge = (PFGE)gl_resolve("glGetError");
        uint32_t err = ge ? ge() : 0xFFFFFFFFu;
        char _b[256];
        float mv[16] = {0}, pr[16] = {0};
        typedef void (*PFGFV)(uint32_t, float*);
        static PFGFV gfv;
        if (!gfv) gfv = (PFGFV)gl_resolve("glGetFloatv");
        if (gfv) { gfv(0x0BA6u /*GL_MODELVIEW_MATRIX*/, mv);
                   gfv(0x0BA7u /*GL_PROJECTION_MATRIX*/, pr); }
        int32_t vp4[4] = {-1,-1,-1,-1};
        typedef void (*PFGIV)(uint32_t,int32_t*);
        typedef void (*PFGPIV)(uint32_t,uint32_t,int32_t*);
        static PFGIV giv; static PFGPIV gpiv;
        if (!giv) giv = (PFGIV)gl_resolve("glGetIntegerv");
        if (!gpiv) gpiv = (PFGPIV)gl_resolve("glGetProgramivARB");
        int32_t bound_vp = -1, bound_fp = -1, bound_tex = -1;
        if (giv) { giv(0x0BA2u /*GL_VIEWPORT*/, vp4);
                   giv(0x8069u /*GL_TEXTURE_BINDING_2D*/, &bound_tex); }
        if (gpiv) { gpiv(0x8620u /*VP*/, 0x8677u /*BINDING*/, &bound_vp);
                    gpiv(0x8804u /*FP*/, 0x8677u, &bound_fp); }
        __builtin_snprintf(_b,sizeof _b,
            "[ff] emit prim=%#x n=%d tex=%d fb=%d glerr=%#x bound=%d/%d texobj=%d vp=(%d,%d %dx%d)\n",
            prim, sn, tex_en, fb, err, bound_vp, bound_fp, bound_tex,
            vp4[0], vp4[1], vp4[2], vp4[3]);
        sceKernelDebugOutText(0,_b);
        /* One-shot per distinct program: when we keep the game's program,
         * dump its source + env params — tells us exactly what transform /
         * attribs the verts were authored against. Fires for the first 3
         * distinct prev_vp seen drawing to fb0. */
        static int s_prog_dumped[8];
        if (prev_vp && vp_en) {
            int slot = -1;
            for (int i = 0; i < 8; i++)
                if (s_prog_dumped[i] == (int)prev_vp) slot = -2;
            if (slot != -2) {
                for (int i = 0; i < 8; i++) if (!s_prog_dumped[i]) { slot = i; break; }
                if (slot >= 0) {
                    s_prog_dumped[slot] = (int)prev_vp;
            typedef void (*PFGPS)(uint32_t,uint32_t,void*);
            typedef void (*PFGPES)(uint32_t,uint32_t,float*);
            static PFGPS gps; static PFGPES ges;
            if (!gps) gps = (PFGPS)gl_resolve("glGetProgramStringARB");
            typedef void (*PFGPLS)(uint32_t,uint32_t,float*);
            static PFGPLS gls;
            if (!gls) gls = (PFGPLS)gl_resolve("glGetProgramLocalParameterfvARB");
            if (!ges) ges = (PFGPES)gl_resolve("glGetProgramEnvParameterfvARB");
            static char pbuf[8192];
            if (gps) {
                gps(0x8620u, 0x8628u /*GL_PROGRAM_STRING_ARB*/, pbuf);
                pbuf[8191] = 0;
                /* skip the blank-run padding Cg emits, then print the body */
                char *lp = pbuf; int li = 0, last_nl = 0;
                for (char *s = pbuf; *s && li < 200; s++, li++) {
                    if (*s != '\n' && *s != '\r' && *s != ' ' && *s != '\t')
                        break;
                    last_nl = (int)(s - pbuf);
                }
                lp = pbuf + last_nl; li = 0;
                while (*lp && li++ < 160) {
                    char *nl = strchr(lp, '\n');
                    int ll = nl ? (int)(nl - lp) : (int)strlen(lp);
                    __builtin_snprintf(_b, sizeof _b, "[ff] vp| %.*s\n",
                                       ll < 100 ? ll : 100, lp);
                    sceKernelDebugOutText(0, _b);
                    if (!nl) break; lp = nl + 1;
                }
            }
            if (ges) {
                float e[4];
                for (int i = 0; i < 8; i++) {
                    ges(0x8620u, (uint32_t)i, e);
                    __builtin_snprintf(_b, sizeof _b,
                        "[ff] env[%d] = %.3f %.3f %.3f %.3f\n",
                        i, e[0], e[1], e[2], e[3]);
                    sceKernelDebugOutText(0, _b);
                }
            }
            if (gls) {
                float e[4];
                for (int i = 0; i < 8; i++) {
                    gls(0x8620u, (uint32_t)i, e);
                    __builtin_snprintf(_b, sizeof _b,
                        "[ff] loc[%d] = %.3f %.3f %.3f %.3f\n",
                        i, e[0], e[1], e[2], e[3]);
                    sceKernelDebugOutText(0, _b);
                }
            }
                }
            }
        }
        /* Composite-source probe: on fb0 textured emits, read back the
         * bound texture and hash it — if the game's source images are
         * dark, the composite faithfully shows dark. */
        if (fb == 0 && tex_en && bound_tex > 0) {
            typedef void (*PFGTI)(uint32_t,int32_t,uint32_t,uint32_t,void*);
            static PFGTI gti;
            if (!gti) gti = (PFGTI)gl_resolve("glGetTexImage");
            int32_t tw = -1, th = -1;
            typedef void (*PFGTLI)(uint32_t,int32_t,uint32_t,int32_t*);
            static PFGTLI gtli;
            if (!gtli) gtli = (PFGTLI)gl_resolve("glGetTexLevelParameteriv");
            if (gtli) { gtli(0x0DE1u,0,0x1000u,&tw); gtli(0x0DE1u,0,0x1001u,&th); }
            if (gti && tw > 0 && th > 0 && (size_t)tw*th <= 8*1024*1024) {
                uint8_t *tb = calloc(1, (size_t)tw*th*4);
                if (tb) {
                    gti(0x0DE1u, 0, 0x1908u, 0x1401u, tb);
                    uint64_t h = 0xcbf29ce484222325ull;
                    size_t nb = (size_t)tw*th*4;
                    for (size_t i = 0; i < nb; i += 4096)
                        { h ^= tb[i]; h *= 0x100000001b3ull; }
                    size_t mid = (size_t)(th/2)*tw*4 + (tw/2)*4;
                    __builtin_snprintf(_b, sizeof _b,
                        "[ff] texsrc id=%d %dx%d h=%016llx c=%02x%02x%02x%02x\n",
                        bound_tex, (int)tw, (int)th, (unsigned long long)h,
                        tb[mid], tb[mid+1], tb[mid+2], tb[mid+3]);
                    sceKernelDebugOutText(0, _b);
                    free(tb);
                }
            }
        }
        for (int vi = 0; vi < sn && vi < 6; vi++) {
            __builtin_snprintf(_b,sizeof _b,
                "[ff] v%d pos=(%.2f,%.2f,%.2f,%.2f) col=(%.2f,%.2f,%.2f,%.2f) uv=(%.2f,%.2f)\n",
                vi, src[vi].x, src[vi].y, src[vi].z, src[vi].w,
                src[vi].r, src[vi].g, src[vi].b, src[vi].a,
                src[vi].s, src[vi].t);
            sceKernelDebugOutText(0,_b);
        }
    }
    { extern int sceKernelDebugOutText(int,const char*);
      /* Count emits + flag anything that isn't the 6-vert composite quad —
         if only quads exist the 3D scene never submits geometry. */
      static int s_odd_logged;
      g_ff_emit_count++;
      /* Periodic env sample — did the matrix upload reach the bound program
       * by draw time? (one-shot dump fires too early, before uploads) */
      if ((g_ff_emit_count % 600) == 599) {
        typedef void (*PFGPES2)(uint32_t,uint32_t,float*);
        static PFGPES2 ges2;
        if (!ges2) ges2 = (PFGPES2)gl_resolve("glGetProgramEnvParameterfvARB");
        if (ges2) {
          float e[4];
          ges2(0x8620u, 0, e);
          char b[96]; __builtin_snprintf(b,sizeof b,
              "[ff] emit#%d env0=%.3f %.3f %.3f %.3f\n",
              g_ff_emit_count, e[0], e[1], e[2], e[3]);
          sceKernelDebugOutText(0,b);
        }
      }
      if ((prim != 0x4 && prim != 0x7) || sn != 6) {
        if (s_odd_logged++ < 12) {
          char b[80]; __builtin_snprintf(b,sizeof b,
              "[ff] odd prim=%#x n=%d\n", prim, sn);
          sceKernelDebugOutText(0,b);
        }
      }
      if (s_emit_logged == 8) { s_emit_logged=9; sceKernelDebugOutText(0,"[ff] draw\n"); } }
    ff_glDrawArrays(prim, 0, sn);
    { extern int sceKernelDebugOutText(int,const char*);
      if (s_emit_logged == 9) {
        s_emit_logged=10;
        /* Read back pixels where the quad should have drawn — proves whether
         * fragments land in the framebuffer vs the draw being dropped. */
        typedef void (*PFRP)(int,int,int,int,uint32_t,uint32_t,void*);
        static PFRP rp;
        if (!rp) rp = (PFRP)gl_resolve("glReadPixels");
        if (rp) {
            uint32_t px[4] = {0xDEAD,0xDEAD,0xDEAD,0xDEAD};
            rp(680, 384, 1, 1, 0x1908u /*GL_RGBA*/, 0x1401u /*GL_UNSIGNED_BYTE*/, &px[0]);
            rp(100, 100, 1, 1, 0x1908u, 0x1401u, &px[1]);
            char b[96];
            __builtin_snprintf(b,sizeof b,
                "[ff] drawn px=(%08x %08x)\n", px[0], px[1]);
            sceKernelDebugOutText(0,b);
        } else sceKernelDebugOutText(0,"[ff] drawn (no rp)\n");
      } }

    ff_glDisableClientState(0x8078u);
    ff_glDisableClientState(0x8076u);
    ff_glDisableClientState(0x8074u);
    /* Re-enable the generic attrib arrays we snapshot-disabled above, in case
     * the client-attrib pop doesn't cover them on this driver. */
    {
        typedef void (*PFEVA2)(uint32_t);
        static PFEVA2 eva2;
        if (!eva2) eva2 = (PFEVA2)gl_resolve("glEnableVertexAttribArrayARB");
        if (eva2) for (uint32_t i = 0; i < 16; i++)
            if (s_ff_saved_attr & (1u << i)) eva2(i);
    }
    /* Restore the game's program state: rebind what it had, and if it had no
     * program bound, unbind AND disable — an enabled target with program 0 is
     * what crashes this Mesa inside gl_nir_link_varyings on the next draw. */
    if (prev_fp) {
        ff_glBindProgram(0x8804u, (uint32_t)prev_fp);
        if (!fp_en) ff_glDisable(0x8804u);
    } else {
        ff_glDisable(0x8804u);   /* was unbound before — leave disabled */
    }
    if (prev_vp) {
        ff_glBindProgram(0x8620u, (uint32_t)prev_vp);
        if (!vp_en) ff_glDisable(0x8620u);
    } else {
        ff_glDisable(0x8620u);   /* was unbound before — leave disabled */
    }
    if (s_popc) s_popc();
    if (s_pop)  s_pop();
    /* Drain the latched GL error so a stale INVALID_ENUM/OPERATION from the
     * emit doesn't pollute the game's own glGetError on the next frame. */
    { typedef uint32_t (*PFGE)(void); static PFGE ge;
      if (!ge) ge = (PFGE)gl_resolve("glGetError"); if (ge) (void)ge(); }
    s_ff_nv = 0;
#endif
}

/* The token table — gl_token_for(name) hands the engine a callable address
 * for a GL function it asks for by name; the lifted code calls it through
 * dispatch() like any other address. */
typedef struct { const char *name; void *fn; } GlTok;
static GlTok g_tok[GL_TOKEN_MAX];
static int   g_tok_n;

/* Resolve a GL entry point on the PS5. ps5-opengl's eglGetProcAddress handles
 * the GL API; the Orbis loader's dlsym catches anything else. A name with no
 * resolution returns NULL — the engine quietly does without that feature. */
static void *gl_resolve(const char *name)
{
#if defined(ORBIS_EGL) || defined(LINUX_EGL)
    /* ps5-opengl exposes all GL 4.6 entry points through eglGetProcAddress */
    extern void (*eglGetProcAddress(const char *name))(void);
    void *fn = (void *)eglGetProcAddress(name);
    if (fn) return fn;
    /* NVIDIA's driver exports ARB-suffixed aliases for every entry point and
     * the game queries names like glMatrixModeARB; Mesa does not. Falling
     * back to the core name keeps the game's resolved pointer real — a NULL
     * here used to mint a dead token the game called as a no-op, which is
     * how the whole matrix stack went silent. */
    {
        size_t L = strlen(name);
        if (L > 5 && !strcmp(name + L - 3, "ARB") &&
            strncmp(name, "gl", 2) == 0) {
            char base[96];
            if (L - 3 < sizeof base) {
                memcpy(base, name, L - 3); base[L - 3] = 0;
                fn = (void *)eglGetProcAddress(base);
                if (fn) return fn;
            }
        }
    }
#endif
#ifdef LINUX_EGL
    extern void *linux_egl_resolve(const char *name);
    void *lfn = linux_egl_resolve(name);
    if (lfn) return lfn;
    {
        size_t L = strlen(name);
        if (L > 5 && !strcmp(name + L - 3, "ARB") &&
            strncmp(name, "gl", 2) == 0) {
            char base[96];
            if (L - 3 < sizeof base) {
                memcpy(base, name, L - 3); base[L - 3] = 0;
                return linux_egl_resolve(base);
            }
        }
    }
    return NULL;
#else
    return NULL;
#endif
}

static __thread int s_ctx_tls;
static pthread_t s_ctx_owner;
static int s_ctx_owned;
const char *g_gl_last_call;    /* crash breadcrumb — read in guest_report_state */
/* Forward a guest call to the real GL function. The lifted code hands its
 * args as 32-bit guest values; the host GL entry point takes them in the
 * natural slots. Headless returns a sane default so the game logic runs. */
static uint32_t gl_forward(GlFn fn, const uint32_t *a, int n, const char *name)
{
#if defined(ORBIS_EGL) || defined(LINUX_EGL)
    /* GLdouble args: the guest pushes each double as two 32-bit stack slots;
     * a real host function takes true doubles (SysV reads them from XMM regs,
     * i386 from 8-byte stack slots) — the generic 9-slot thunk would feed it
     * whatever garbage happens to be in the wrong registers. glClearDepth is
     * imported directly; the rest can arrive via glXGetProcAddressARB. */
    if (fn && name) {
        union { uint32_t u[2]; double d; } w[6];
#define GD(i) (w[i].u[0]=a[(i)*2], w[i].u[1]=a[(i)*2+1], w[i].d)
        size_t L = strlen(name);
        if (L > 2 && name[L-1] == 'd' && name[L-2] >= '1' && name[L-2] <= '4'
            && strncmp(name, "gl", 2) == 0) {
            int nd = name[L-2] - '0';
            switch (nd) {
            case 1: { ((void(*)(double))fn)(GD(0)); return 0; }
            case 2: { ((void(*)(double,double))fn)(GD(0),GD(1)); return 0; }
            case 3: { ((void(*)(double,double,double))fn)(GD(0),GD(1),GD(2)); return 0; }
            case 4: { ((void(*)(double,double,double,double))fn)
                          (GD(0),GD(1),GD(2),GD(3)); return 0; }
            }
        }
        if (!strcmp(name,"glClearDepth")) {
            ((void(*)(double))fn)(GD(0)); return 0;
        }
        if (!strcmp(name,"glDepthRange")) {
            ((void(*)(double,double))fn)(GD(0),GD(1)); return 0;
        }
        if (!strcmp(name,"glOrtho") || !strcmp(name,"glFrustum")) {
            ((void(*)(double,double,double,double,double,double))fn)
                (GD(0),GD(1),GD(2),GD(3),GD(4),GD(5)); return 0;
        }
        if (!strcmp(name,"glRectd")) {
            ((void(*)(double,double,double,double))fn)
                (GD(0),GD(1),GD(2),GD(3)); return 0;
        }
        if (!strcmp(name,"glClipPlane")) {
            ((void(*)(uint32_t,const double*))fn)
                (a[0],(const double*)(uintptr_t)a[1]); return 0;
        }
        if (!strcmp(name,"glRotated") || !strcmp(name,"glScaled") ||
            !strcmp(name,"glTranslated")) {
            ((void(*)(double,double,double))fn)(GD(0),GD(1),GD(2)); return 0;
        }
#undef GD
    }
#endif
#if defined(ORBIS_EGL) || defined(LINUX_EGL)
    /* Gate on OWNER, not s_ctx_tls — s_ctx_tls stays set on a worker that
     * grabbed the ctx early even after glXMakeCurrent reclaims it; checking
     * pthread_self()==s_ctx_owner drops the worker's calls correctly. */
    if (fn && s_ctx_tls && pthread_equal(s_ctx_owner, pthread_self())) {
        /* Call through with up to 9 args in the standard slots. */
#ifdef LINUX_EGL
        /* i386 cdecl: every guest slot is a 32-bit stack arg — a uint64
         * thunk would push 8-byte slots and desync the callee's args. */
        uint32_t r = ((uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t,
                                  uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))fn)
            ((uint32_t)(n>0?a[0]:0),(uint32_t)(n>1?a[1]:0),(uint32_t)(n>2?a[2]:0),
             (uint32_t)(n>3?a[3]:0),(uint32_t)(n>4?a[4]:0),(uint32_t)(n>5?a[5]:0),
             (uint32_t)(n>6?a[6]:0),(uint32_t)(n>7?a[7]:0),(uint32_t)(n>8?a[8]:0));
        return r;
#else
        uint64_t r = ((uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,
                                  uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))fn)
            ((uint64_t)(n>0?a[0]:0),(uint64_t)(n>1?a[1]:0),(uint64_t)(n>2?a[2]:0),
             (uint64_t)(n>3?a[3]:0),(uint64_t)(n>4?a[4]:0),(uint64_t)(n>5?a[5]:0),
             (uint64_t)(n>6?a[6]:0),(uint64_t)(n>7?a[7]:0),(uint64_t)(n>8?a[8]:0));
        return (uint32_t)r;
#endif
    }
#endif
    (void)fn; (void)a; (void)n;
    /* Headless sane-defaults: glGetError->0, glGetString handled by a real
     * call, the rest silently succeed. */
    return 0;
}

/* ARB vp/fp source rewriter — see call site in h_glProgramStringARB. The
 * game's Cg output puts "!!ARBvp1.0" on the header but freely uses NV
 * constructs Mesa's ARB_vertex_program 1.0 parser does not accept:
 *   - OPTION NV_vertex_program3; / OPTION NV_fragment_program2;
 *   - BBN: basic-block labels and BRA/CAL/RET branches
 *   - condition-code setters (SLTC, SEQC, SNEC, SGEC, SGTC, SLEC, MOVC)
 *   - R-suffixed round variants (MADR/MOVR/MULR/DP3R/ADDR/SUBR/...)
 * We zero out each unsupported statement in-place (overwriting with spaces
 * so error positions stay comparable) and strip R suffixes. Branches are
 * dropped, so programs with loops/conditionals degrade to one straight-line
 * pass — functionally wrong for lighting-accumulation shaders but at least
 * the program parses and renders *something*. */
static int arb_tokc(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
}

void hle_gl_arb_rewrite(char *src, uint32_t slen)
{
    char *p = src;
    char *end = src + slen;
    int ended = 0;
    while (p < end) {
        /* skip whitespace */
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        /* skip line comments */
        if (*p == '#') { while (p < end && *p != '\n') p++; continue; }
        /* skip header `!!...` */
        if (*p == '!') { while (p < end && *p != '\n') p++; continue; }

        char *tok = p;
        while (p < end && arb_tokc(*p)) p++;
        int tlen = (int)(p - tok);
        if (tlen == 0) { p++; continue; }

        /* After END, everything else is garbage — overwrite the tail */
        if (ended) {
            char *k = tok;
            while (k < end) *k++ = ' ';
            return;
        }

        /* BBN: label (optional whitespace, then ':') */
        char *q = p;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q < end && *q == ':' && tlen >= 2 && tok[0] == 'B' && tok[1] == 'B') {
            int all_digit = (tlen > 2);
            for (int i = 2; i < tlen; i++)
                if (!(tok[i] >= '0' && tok[i] <= '9')) { all_digit = 0; break; }
            if (all_digit) {
                for (char *k = tok; k <= q; k++) *k = ' ';
                p = q + 1;
                continue;
            }
        }

        /* END keyword — keep it, nuke anything that follows */
        if (tlen == 3 && !__builtin_memcmp(tok, "END", 3)) {
            ended = 1;
            continue;
        }

        #define TEQ(s) (tlen == (int)sizeof(s)-1 && !__builtin_memcmp(tok, s, sizeof(s)-1))
        int kill = TEQ("OPTION") || TEQ("BRA") || TEQ("CAL") || TEQ("RET")
                || TEQ("REP") || TEQ("ENDREP") || TEQ("IF") || TEQ("ELSE") || TEQ("ENDIF")
                || TEQ("SLTC") || TEQ("SEQC") || TEQ("SNEC") || TEQ("SGEC")
                || TEQ("SGTC") || TEQ("SLEC") || TEQ("MOVC");
        if (kill) {
            char *stop = p;
            while (stop < end && *stop != ';' && *stop != '\n') stop++;
            if (stop < end && *stop == ';') stop++;
            for (char *k = tok; k < stop; k++) *k = ' ';
            p = stop;
            continue;
        }
        #undef TEQ

        /* NV R-suffix: strip the trailing R to recover the ARB opcode */
        if (tlen > 2 && tok[tlen-1] == 'R') {
            static const char *rops[] = {
                "MAD","MOV","MUL","DP3","DP4","ADD","SUB","MAX","MIN",
                "FRC","FLR","ABS","EX2","LG2","SIN","COS","RCP","RSQ","POW","DST","LRP", 0
            };
            for (int i = 0; rops[i]; i++) {
                int n = 0; while (rops[i][n]) n++;
                if (tlen == n + 1 && !__builtin_memcmp(tok, rops[i], n)) {
                    tok[n] = ' ';
                    break;
                }
            }
        }

        /* advance past rest of statement */
        while (p < end && *p != ';' && *p != '\n') p++;
        if (p < end) p++;
    }
}

/* 3D-pipeline diagnostics: 2D text renders fine while 3D is garbage, so we
 * need per-frame visibility into which ARB programs the game bound and how
 * many draws fired under each. The swap handler flushes glGetError and emits
 * a one-line [swap] summary; the per-call hooks just maintain the counters. */
static uint32_t s_bound_vp, s_bound_fp;
static int32_t  s_cur_fb, s_bound_tex;
static uint32_t s_frame_no;
static uint32_t s_frame_draws, s_frame_draws_with_vp, s_frame_draws_with_fp;

/* Called from host_swap() in hle_window_orbis.c — the single swap funnel
 * every path converges on. Flushes GL errors and emits the per-frame
 * [swap] summary, then resets the counters. */
void hle_gl_frame_summary(void)
{
    typedef uint32_t (*PFGE)(void);
    static PFGE s_ge;
    if (!s_ge) s_ge = (PFGE)gl_resolve("glGetError");
    uint32_t first_err = 0, nerrs = 0;
    if (s_ge) {
        for (int i = 0; i < 8; i++) {
            uint32_t e = s_ge(); if (!e) break;
            if (!first_err) first_err = e;
            nerrs++;
        }
    }
    s_frame_no++;
    if (first_err || s_frame_no <= 60 || (s_frame_no % 30) == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        char b[160];
        __builtin_snprintf(b, sizeof b,
            "[swap] f=%u draws=%u vp_bound=%u fp_bound=%u vpID=%u fpID=%u err=%#x nerr=%u\n",
            s_frame_no, s_frame_draws, s_frame_draws_with_vp, s_frame_draws_with_fp,
            s_bound_vp, s_bound_fp, first_err, nerrs);
        sceKernelDebugOutText(0, b);
    }
    /* FBO readback: bind each live FBO, check status + sample a center pixel.
     * If the 3D scene lands in an FBO, its contents should be non-black. */
    if ((s_frame_no % 240) == 120) {
        typedef void (*PFBF)(uint32_t,uint32_t);
        typedef uint32_t (*PFCFS)(uint32_t);
        typedef void (*PFRP)(int,int,int,int,uint32_t,uint32_t,void*);
        typedef void (*PFGI)(uint32_t,int32_t*);
        static PFBF s_bf; static PFCFS s_cfs; static PFRP s_rp; static PFGI s_gi;
        if (!s_bf)  s_bf  = (PFBF)gl_resolve("glBindFramebufferEXT");
        if (!s_cfs) s_cfs = (PFCFS)gl_resolve("glCheckFramebufferStatusEXT");
        if (!s_rp)  s_rp  = (PFRP)gl_resolve("glReadPixels");
        if (!s_gi)  s_gi  = (PFGI)gl_resolve("glGetIntegerv");
        if (s_bf && s_cfs && s_rp) {
            extern int sceKernelDebugOutText(int, const char*);
            int32_t prevfb = 0;
            if (s_gi) s_gi(0x8CA6u /*GL_FRAMEBUFFER_BINDING*/, &prevfb);
            for (int fb = 1; fb <= 18; fb++) {
                s_bf(0x8D40u, (uint32_t)fb);
                uint32_t st = s_cfs(0x8D40u);
                /* hash a 480x270 center strip — uniform clears vs real content */
                static uint8_t srp[480*270*4];
                uint64_t h = 0xcbf29ce484222325ull;
                int32_t aw = 0, ah = 0, atex = 0, atype = 0, prevtex = 0;
                {
                    typedef void (*PFGA)(uint32_t,uint32_t,uint32_t,int32_t*);
                    static PFGA ga;
                    if (!ga) ga = (PFGA)gl_resolve("glGetFramebufferAttachmentParameterivEXT");
                    if (ga) {
                        ga(0x8D40u, 0x8CE0u, 0x8CD0u /*OBJ_TYPE*/, &atype);
                        ga(0x8D40u, 0x8CE0u, 0x8CD1u /*OBJ_NAME*/, &atex);
                    }
                    if (atex) {
                        typedef void (*PFBT)(uint32_t,uint32_t);
                        typedef void (*PFGL)(uint32_t,int32_t,int32_t,int32_t*);
                        static PFBT bt; static PFGL gl2;
                        if (!bt) bt = (PFBT)gl_resolve("glBindTexture");
                        if (!gl2) gl2 = (PFGL)gl_resolve("glGetTexLevelParameteriv");
                        if (bt && gl2) {
                            if (s_gi) s_gi(0x8069u /*TEXTURE_BINDING_2D*/, &prevtex);
                            bt(0x0DE1u, (uint32_t)atex);
                            gl2(0x0DE1u, 0, 0x1000u /*WIDTH*/, &aw);
                            gl2(0x0DE1u, 0, 0x1001u /*HEIGHT*/, &ah);
                            bt(0x0DE1u, (uint32_t)prevtex);
                        }
                    }
                }
                if (st == 0x8CD5u) {
                    /* read the WHOLE attachment (capped) not a fixed strip —
                     * a 256x256 target read at x>256 returns all-zero */
                    int rw = aw > 0 && aw < 512 ? aw : 480;
                    int rh = ah > 0 && ah < 400 ? ah : 270;
                    s_rp(0, 0, rw, rh, 0x1908u, 0x1401u, srp);
                    unsigned lit = 0;
                    for (size_t i = 0; i + 3 < (size_t)rw * rh * 4 && i + 3 < sizeof srp; i += 977)
                        { h ^= srp[i]; h *= 0x100000001b3ull;
                          if ((srp[i] | srp[i+1] | srp[i+2]) > 8) lit++; }
                    char b[160];
                    __builtin_snprintf(b, sizeof b,
                        "[fbread] fb=%d status=%#x att=%d(%dx%d) hash=%016llx lit=%u\n",
                        fb, st, (int)atex, (int)aw, (int)ah,
                        (unsigned long long)h, lit);
                    sceKernelDebugOutText(0, b);
                    continue;
                }
                char b[128];
                __builtin_snprintf(b, sizeof b,
                    "[fbread] fb=%d status=%#x att=%d\n", fb, st, (int)atex);
                sceKernelDebugOutText(0, b);
            }
            s_bf(0x8D40u, (uint32_t)prevfb);
            if (s_ge) (void)s_ge();
        }
    }
    s_frame_draws = s_frame_draws_with_vp = s_frame_draws_with_fp = 0;
}

/* Async GPU exceptions kill the process with no thread info — the only way
 * to name the guilty call is to log it before it runs. One short line per
 * call; the klog tail at the crash IS the answer. Expensive (a kernel call
 * per GL call) so it stays opt-in via /app0/gltrace or LINDBERGH_GL_TRACE. */
static int prog_trace_on(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_PROG_TRACE");
        cached = (v && *v && *v != '0') ? 1
                : access("/app0/progtrace", 0) == 0 ? 1 : 0;
    }
    return cached;
}

static int gl_trace_on(void)
{
    static int cached = -1;
    if (cached < 0) {
        extern int access(const char *, int);
        extern char *getenv(const char *);
        const char *v = getenv("LINDBERGH_GL_TRACE");
        cached = (v && *v && *v != '0') ? 1
                : access("/app0/gltrace", 0) == 0 ? 1 : 0;
    }
    return cached;
}

static void gl_trace(const char *name)
{
    extern int sceKernelDebugOutText(int, const char*);
    char b[72];
    unsigned n = 0;
    for (const char *p = name; *p && n < 66; p++) b[n++] = *p;
    b[n++] = '\n'; b[n] = 0;
    sceKernelDebugOutText(0, b);
}

/* The game calls gl* before (and on threads other than) its glXMakeCurrent.
 * Without a current context the Mesa glapi stubs jump through a NULL dispatch
 * table — segv at small offsets like 0x20 (the slot number). Ensure the shared
 * context is current on whatever thread touches GL first. */
static int s_ctx_logged;
static int s_ff_logged;
static void gl_ensure_ctx(void)
{
#if defined(ORBIS_EGL) || defined(LINUX_EGL)
    if (s_ctx_tls) return;
    extern int host_gl_context(void);
    extern int host_window_open(const char *);
    extern int orbis_egl_make_current(void);
    extern int sceKernelDebugOutText(int, const char*);
    /* One shared EGL context can be current on exactly one thread — if a
     * worker thread makes it current it detaches from the render thread and
     * the render thread's next call crashes on a NULL TLS dispatch table.
     * The RIGHTFUL owner is the render thread — the one that calls
     * glXMakeCurrent / glXSwapBuffers (h_glXMakeCurrent claims it below).
     * Any other thread (e.g. the Sofdec video uploader) keeps s_ctx_tls=0
     * and gl_forward drops its calls. */
    static pthread_mutex_t s_ctx_ensure_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&s_ctx_ensure_mu);
    if (!s_ctx_owned) {
        if (!host_gl_context()) {
            sceKernelDebugOutText(0, "[gl] lazy window open\n");
            host_window_open("id4");
        }
        g_gl_last_call = "eglMakeCurrent";
        if (orbis_egl_make_current()) {
            s_ctx_tls = 1;
            s_ctx_owned = 1;
            s_ctx_owner = pthread_self();
            if (!s_ctx_logged) { s_ctx_logged = 1; sceKernelDebugOutText(0, "[gl] ctx ok\n"); }
        } else sceKernelDebugOutText(0, "[gl] make_current failed\n");
        g_gl_last_call = NULL;
        pthread_mutex_unlock(&s_ctx_ensure_mu);
        return;
    }
    pthread_mutex_unlock(&s_ctx_ensure_mu);
    if (pthread_equal(s_ctx_owner, pthread_self())) {
        /* The owning thread re-arms in case another mechanism detached it. */
        g_gl_last_call = "eglMakeCurrent";
        if (orbis_egl_make_current()) s_ctx_tls = 1;
        else s_ctx_tls = 0;
        g_gl_last_call = NULL;
        return;
    }
    /* Not the owner — leave s_ctx_tls=0 so gl_forward drops the call. */
    static __thread int s_nctx_logged;
    if (!s_nctx_logged) { s_nctx_logged = 1;
        sceKernelDebugOutText(0, "[gl] not ctx owner, dropping calls\n"); }
#endif
}

static void gl_dispatch(CPU *c, unsigned idx)
{
    const char *name = g_gl_names[idx];
    gl_ensure_ctx();
    if (!s_ctx_logged) {   /* ensure_ctx couldn't log — it never got this far */
        extern int sceKernelDebugOutText(int, const char*);
        sceKernelDebugOutText(0, "[gl] past ensure_ctx\n"); s_ctx_logged = 1;
    }
    if (strcmp(name, "glBegin") == 0 && !s_ff_logged) {
        extern int sceKernelDebugOutText(int, const char*);
        s_ff_logged = 1;
        sceKernelDebugOutText(0, "[gl] first glBegin enter\n");
    }
    {
        /* One-shot markers for the engine's real draw paths — if these never
         * print, the game isn't submitting geometry and the screen stays
         * black no matter what the FF shim does. */
        static uint8_t seen_draw, seen_progstr, seen_bindprog;
        extern int sceKernelDebugOutText(int, const char*);
        if (!seen_draw && (strcmp(name,"glDrawRangeElements")==0 ||
                           strcmp(name,"glDrawElements")==0 ||
                           strcmp(name,"glDrawArrays")==0)) {
            seen_draw = 1; sceKernelDebugOutText(0,"[gl] first draw\n");
        }
        if (!seen_progstr && strcmp(name,"glProgramStringARB")==0) {
            seen_progstr = 1; sceKernelDebugOutText(0,"[gl] first progstr\n");
        }
        if (!seen_bindprog && strcmp(name,"glBindProgramARB")==0) {
            seen_bindprog = 1; sceKernelDebugOutText(0,"[gl] first bindprog\n");
        }
        {
            static const char *const vs2[] = {
                "glVertexPointer","glColorPointer","glNormalPointer",
                "glTexCoordPointer","glInterleavedArrays",
                "glEnableClientState","glDisableClientState",
                "glClientActiveTexture",
                "glVertexAttribPointerARB","glEnableVertexAttribArrayARB",
                "glDisableVertexAttribArrayARB","glVertexAttribPointer",
                "glEnableVertexAttribArray","glDisableVertexAttribArray",
                "glBindBufferARB","glBindBuffer","glBufferDataARB",
                "glBufferData","glBufferSubDataARB","glBufferSubData",
                "glPushClientAttrib","glPopClientAttrib","glLockArraysEXT",0 };
            static int vs2_seen[64];
            for (int i = 0; vs2[i]; i++)
                if (!strcmp(name, vs2[i])) {
                    if (vs2_seen[i] < 4) {
                        vs2_seen[i]++;
                        char b[144];
                        __builtin_snprintf(b,sizeof b,
                            "[vsetup:plt] %s a0=%#x a1=%#x a2=%#x a3=%#x a4=%#x\n",
                            name,A32(0),A32(1),A32(2),A32(3),A32(4));
                        sceKernelDebugOutText(0,b);
                    }
                    break;
                }
        }
        static int s_dr, s_fbo, s_dl;
        if (strcmp(name,"glDrawElements")==0 || strcmp(name,"glDrawRangeElements")==0 ||
            strcmp(name,"glDrawArrays")==0 || strcmp(name,"glDrawArraysEXT")==0 ||
            strcmp(name,"glDrawArraysInstancedARB")==0) {
            s_dr++;
            s_frame_draws++;
            if (s_bound_vp) s_frame_draws_with_vp++;
            if (s_bound_fp) s_frame_draws_with_fp++;
            if (s_dr <= 10 || (s_dr % 300) == 0) {
                char b[128]; __builtin_snprintf(b,sizeof b,"[draw] %s #%d a0=%#x a1=%d a2=%d vp=%u fp=%u fb=%d tex=%d\n",
                        name, s_dr, A32(0), (int)A32(1), (int)A32(2), s_bound_vp, s_bound_fp,
                        s_cur_fb, s_bound_tex);
                sceKernelDebugOutText(0,b);
            }
            /* Vertex-source probe — on the first few FBO draws, dump which
             * arrays feed the shader and a couple of live vertices, so a
             * collapsed-transform vs dead-array failure is distinguishable. */
            static int s_vsrc;
            static unsigned s_dattr_on, s_dattr_off;
            if (s_cur_fb > 0) {
                typedef void (*PFGVA4)(uint32_t,uint32_t,int32_t*);
                static PFGVA4 gv4;
                if (!gv4) gv4 = (PFGVA4)gl_resolve("glGetVertexAttribivARB");
                int32_t e0 = 0, e1 = 0, e2 = 0;
                if (gv4) { gv4(0,0x8622u,&e0); gv4(1,0x8622u,&e1); gv4(2,0x8622u,&e2); }
                int any = e0|e1|e2;
                if (any) s_dattr_on++; else s_dattr_off++;
                if ((s_dr % 3000) == 0) {
                    char b[96]; __builtin_snprintf(b,sizeof b,
                        "[vsrc-sum] draws attron=%u attroff=%u\n",
                        s_dattr_on, s_dattr_off);
                    sceKernelDebugOutText(0,b);
                }
            }
            if (s_cur_fb > 0 && s_vsrc < 20) {
                s_vsrc++;
                typedef void (*PFGI2)(uint32_t,int32_t*);
                typedef void (*PFGBV)(uint32_t,int32_t*);
                typedef void (*PFGPV2)(uint32_t,void**);
                static PFGI2 gi2; static PFGBV gbv; static PFGPV2 gpv;
                if (!gi2) gi2 = (PFGI2)gl_resolve("glGetIntegerv");
                if (!gbv) gbv = (PFGBV)gl_resolve("glGetBooleanv");
                if (!gpv) gpv = (PFGPV2)gl_resolve("glGetPointerv");
                extern int sceKernelDebugOutText(int, const char*);
                char b[160]; int en[10]={0};
                if (gbv) {
                    gbv(0x8074u,&en[0]); /* GL_VERTEX_ARRAY */
                    gbv(0x8075u,&en[1]); /* GL_NORMAL_ARRAY */
                    gbv(0x8076u,&en[2]); /* GL_COLOR_ARRAY */
                    gbv(0x8078u,&en[3]); /* GL_TEXTURE_COORD_ARRAY */
                    gbv(0x8625u,&en[4]); /* GL_VERTEX_ATTRIB_ARRAY0? use per-index below */
                }
                if (gi2) {
                    for (int i=0;i<4;i++) {
                        int32_t e=0; gi2(0x8622u /*GL_VERTEX_ATTRIB_ARRAY_ENABLED*/+0 ,&e);
                        (void)e;
                    }
                    /* per-attrib enabled: GL_VERTEX_ATTRIB_ARRAY_ENABLED is indexed via glGetVertexAttrib */
                    typedef void (*PFGVA)(uint32_t,uint32_t,int32_t*);
                    static PFGVA gva;
                    if (!gva) gva = (PFGVA)gl_resolve("glGetVertexAttribivARB");
                    for (int i=0;i<6;i++){ int32_t e=-1; if(gva)gva(i,0x8622u,&e); en[4+i]=e; }
                }
                void *vpp=NULL; if (gpv) gpv(0x808Eu /*GL_VERTEX_ARRAY_POINTER*/,&vpp);
                __builtin_snprintf(b,sizeof b,
                    "[vsrc] fb=%d arr=%d%d%d%d attr=%d%d%d%d%d%d vptr=%p\n",
                    s_cur_fb, en[0],en[1],en[2],en[3], en[4],en[5],en[6],en[7],en[8], vpp);
                sceKernelDebugOutText(0,b);
                if (vpp) {
                    const float *v=(const float*)vpp;
                    char b2[160];
                    __builtin_snprintf(b2,sizeof b2,
                        "[vsrc] v0=%.2f %.2f %.2f %.2f  v1=%.2f %.2f %.2f %.2f\n",
                        v[0],v[1],v[2],v[3], v[4],v[5],v[6],v[7]);
                    sceKernelDebugOutText(0,b2);
                }
            }
            /* Per-program one-shot: dump the bound VP's source + local/env
             * banks — settles which constant bank 3D shaders read. */
            static int s_vpd[10]; int slot3 = -1;
            if (s_bound_vp) {
                for (int i = 0; i < 10; i++)
                    if (s_vpd[i] == (int)s_bound_vp) { slot3 = -2; break; }
                if (slot3 != -2)
                    for (int i = 0; i < 10; i++)
                        if (!s_vpd[i]) { slot3 = i; break; }
            }
            if (slot3 >= 0) {
                s_vpd[slot3] = (int)s_bound_vp;
                typedef void (*PFGPS3)(uint32_t,uint32_t,void*);
                typedef void (*PFGPV)(uint32_t,uint32_t,float*);
                static PFGPS3 gps3; static PFGPV gls3, ges3;
                if (!gps3) gps3 = (PFGPS3)gl_resolve("glGetProgramStringARB");
                if (!gls3) gls3 = (PFGPV)gl_resolve("glGetProgramLocalParameterfvARB");
                if (!ges3) ges3 = (PFGPV)gl_resolve("glGetProgramEnvParameterfvARB");
                static char pbuf3[16384];
                if (gps3) {
                    gps3(0x8620u, 0x8628u, pbuf3); pbuf3[16383] = 0;
                    char b[160];
                    __builtin_snprintf(b,sizeof b,"[vp%d-src] ----\n", s_bound_vp);
                    sceKernelDebugOutText(0,b);
                    int li = 0; char *p = pbuf3;
                    while (*p && li++ < 200) {
                        char *nl = strchr(p,'\n');
                        int ll = nl ? (int)(nl-p) : (int)strlen(p);
                        int nb = 0; for (int k=0;k<ll;k++) if (p[k]!=' '&&p[k]!='\t') nb=1;
                        char lbuf[160]; int lc = ll<159?ll:159;
                        memcpy(lbuf,p,lc); lbuf[lc]=0;
                        if (nb && (strstr(lbuf,"PARAM")||strstr(lbuf,"program.")||strstr(lbuf,"DP4")||
                            strstr(lbuf,"MOV")||strstr(lbuf,"result.position")||li<4||strstr(lbuf,"END")||
                            strstr(lbuf,"ARBvp")||strstr(lbuf,"ARBfp")||strstr(lbuf,"TEMP")||strstr(lbuf,"ATTRIB")||
                            strstr(lbuf,"MAD")||strstr(lbuf,"MUL")||strstr(lbuf,"ADD"))) {
                            __builtin_snprintf(b,sizeof b,"[vp%d] %.*s\n",
                                s_bound_vp, ll<140?ll:140, p);
                            sceKernelDebugOutText(0,b);
                        }
                        if (!nl) break; p = nl+1;
                    }
                }
                if (gls3) { float e[4]; char b[96];
                    for (int i=0;i<6;i++){ gls3(0x8620u,i,e);
                        __builtin_snprintf(b,sizeof b,"[vp%d] loc[%d]=%.3f %.3f %.3f %.3f\n",
                            s_bound_vp,i,e[0],e[1],e[2],e[3]); sceKernelDebugOutText(0,b);} }
                if (ges3) { float e[4]; char b[96];
                    for (int i=0;i<6;i++){ ges3(0x8620u,i,e);
                        __builtin_snprintf(b,sizeof b,"[vp%d] env[%d]=%.3f %.3f %.3f %.3f\n",
                            s_bound_vp,i,e[0],e[1],e[2],e[3]); sceKernelDebugOutText(0,b);} }
            }
        } else if (strcmp(name,"glBindProgramARB")==0) {
            uint32_t target = A32(0), prog = A32(1);
            if (target == 0x8620u) s_bound_vp = prog;        /* GL_VERTEX_PROGRAM_ARB   */
            else if (target == 0x8804u) s_bound_fp = prog;   /* GL_FRAGMENT_PROGRAM_ARB */
        } else if (strcmp(name,"glCallList")==0 || strcmp(name,"glCallLists")==0 ||
                   strcmp(name,"glNewList")==0 || strcmp(name,"glEndList")==0 ||
                   strcmp(name,"glGenLists")==0) {
            s_dl++;
            if (s_dl <= 10 || (s_dl % 200) == 0) {
                char b[64]; __builtin_snprintf(b,sizeof b,"[dlist] %s #%d a=%#x\n",
                        name, s_dl, A32(0));
                sceKernelDebugOutText(0,b);
            }
        } else if (strcmp(name,"glBindTexture")==0) {
            if (A32(0) == 0x0DE1u) s_bound_tex = (int32_t)A32(1);
        } else if (strncmp(name,"glBindFramebuffer",17)==0 ||
                   strncmp(name,"glFramebufferTexture",20)==0 ||
                   strncmp(name,"glGenFramebuffers",17)==0 ||
                   strcmp(name,"glCheckFramebufferStatusEXT")==0) {
            if (strncmp(name,"glBindFramebuffer",17)==0) s_cur_fb = (int32_t)A32(1);
            s_fbo++;
            if (s_fbo <= 14 || (s_fbo % 60) == 0) {
                char b[96]; __builtin_snprintf(b,sizeof b,"[fbo] %s #%d a=%#x b=%#x\n",
                        name, s_fbo, A32(0), A32(1));
                sceKernelDebugOutText(0,b);
            }
        }
    }
    if (!g_gl_fn[idx]) g_gl_fn[idx] = gl_resolve(name);
    g_gl_calls[idx]++;
    if (gl_trace_on()) gl_trace(name);
    uint32_t args[GL_TOKEN_ARGS];
    for (int i = 0; i < GL_TOKEN_ARGS; i++) args[i] = A32(i);

    /* ---- FF immediate-mode shim ---- */
    /* glBegin: record prim type, enter accumulation mode. */
    if (strcmp(name, "glBegin") == 0) {
        ff_init();
        s_ff_active = 1;
        s_ff_prim   = args[0];
        s_ff_nv     = 0;
        RET(0); return;
    }
    /* Track current color regardless of begin/end so initial vertex color is
     * correct even when glColor* is called before glBegin. */
    if (strcmp(name, "glColor4ub") == 0) {
        s_ff_cr = args[0]/255.0f; s_ff_cg = args[1]/255.0f;
        s_ff_cb = args[2]/255.0f; s_ff_ca = args[3]/255.0f;
        if (!s_ff_active) { RET(gl_forward(g_gl_fn[idx],args,9,name)); return; }
        RET(0); return;
    }
    if (strcmp(name, "glColor4f") == 0) {
        s_ff_cr=*(float*)&args[0]; s_ff_cg=*(float*)&args[1];
        s_ff_cb=*(float*)&args[2]; s_ff_ca=*(float*)&args[3];
        if (!s_ff_active) { RET(gl_forward(g_gl_fn[idx],args,9,name)); return; }
        RET(0); return;
    }
    if (strcmp(name, "glColor3f") == 0) {
        s_ff_cr=*(float*)&args[0]; s_ff_cg=*(float*)&args[1];
        s_ff_cb=*(float*)&args[2]; s_ff_ca=1.0f;
        if (!s_ff_active) { RET(gl_forward(g_gl_fn[idx],args,9,name)); return; }
        RET(0); return;
    }
    if (strcmp(name, "glColor4fv") == 0) {
        const float *p = (const float*)(uintptr_t)args[0];
        if (p) { s_ff_cr=p[0]; s_ff_cg=p[1]; s_ff_cb=p[2]; s_ff_ca=p[3]; }
        if (!s_ff_active) { RET(gl_forward(g_gl_fn[idx],args,9,name)); return; }
        RET(0); return;
    }
    if (strcmp(name, "glColor3fv") == 0) {
        const float *p = (const float*)(uintptr_t)args[0];
        if (p) { s_ff_cr=p[0]; s_ff_cg=p[1]; s_ff_cb=p[2]; s_ff_ca=1.0f; }
        if (!s_ff_active) { RET(gl_forward(g_gl_fn[idx],args,9,name)); return; }
        RET(0); return;
    }
    if (strcmp(name, "glTexCoord2f") == 0) {
        s_ff_ts=*(float*)&args[0]; s_ff_tt=*(float*)&args[1];
        if (!s_ff_active) { RET(gl_forward(g_gl_fn[idx],args,9,name)); return; }
        RET(0); return;
    }
    if (strcmp(name, "glTexCoord1f") == 0) {
        s_ff_ts=*(float*)&args[0]; s_ff_tt=0.0f;
        if (!s_ff_active) { RET(gl_forward(g_gl_fn[idx],args,9,name)); return; }
        RET(0); return;
    }
    /* glVertex* — only meaningful inside begin/end; accumulate. */
    if (s_ff_active) {
        if (strcmp(name, "glVertex3f") == 0) {
            if (s_ff_nv < FF_MAX_VERTS) {
                FfVert *v = &s_ff_verts[s_ff_nv++];
                v->x=*(float*)&args[0]; v->y=*(float*)&args[1];
                v->z=*(float*)&args[2]; v->w=1.0f;
                v->r=s_ff_cr; v->g=s_ff_cg; v->b=s_ff_cb; v->a=s_ff_ca;
                v->s=s_ff_ts; v->t=s_ff_tt;
            }
            RET(0); return;
        }
        if (strcmp(name, "glVertex2f") == 0) {
            if (s_ff_nv < FF_MAX_VERTS) {
                FfVert *v = &s_ff_verts[s_ff_nv++];
                v->x=*(float*)&args[0]; v->y=*(float*)&args[1];
                v->z=0.0f; v->w=1.0f;
                v->r=s_ff_cr; v->g=s_ff_cg; v->b=s_ff_cb; v->a=s_ff_ca;
                v->s=s_ff_ts; v->t=s_ff_tt;
            }
            RET(0); return;
        }
        if (strcmp(name, "glVertex4f") == 0) {
            if (s_ff_nv < FF_MAX_VERTS) {
                FfVert *v = &s_ff_verts[s_ff_nv++];
                v->x=*(float*)&args[0]; v->y=*(float*)&args[1];
                v->z=*(float*)&args[2]; v->w=*(float*)&args[3];
                v->r=s_ff_cr; v->g=s_ff_cg; v->b=s_ff_cb; v->a=s_ff_ca;
                v->s=s_ff_ts; v->t=s_ff_tt;
            }
            RET(0); return;
        }
        if (strcmp(name, "glNormal3f") == 0)  { RET(0); return; }
        if (strcmp(name, "glTexCoord3f") == 0) {
            s_ff_ts=*(float*)&args[0]; s_ff_tt=*(float*)&args[1];
            RET(0); return;
        }
        if (strcmp(name, "glEnd") == 0) {
            s_ff_active = 0;
            ff_emit();
            RET(0); return;
        }
        /* Any other call inside begin/end is silently eaten */
        RET(0); return;
    }
    /* glEnd outside begin → no-op */
    if (strcmp(name, "glEnd") == 0) { RET(0); return; }
    /* lindbergh-loader: glProgramParameters4fvNV does not exist/work on Mesa —
     * route to the EXT env-parameters entry, same signature. Without this all
     * program uniforms (matrices, colors) the game uploads are dropped. */
    if (strcmp(name, "glProgramParameters4fvNV") == 0) {
        typedef void (*PFPP)(uint32_t,uint32_t,int32_t,const float*);
        static PFPP pp;
        static int logged;
        if (!pp) pp = (PFPP)gl_resolve("glProgramEnvParameters4fvEXT");
        if (!pp) pp = (PFPP)gl_resolve("glProgramEnvParameters4fvARB");
        if (!logged) { logged=1;
            extern int sceKernelDebugOutText(int,const char*);
            char b[96]; __builtin_snprintf(b,sizeof b,
                "[pp] params4fvNV pp=%p tgt=%#x idx=%d cnt=%d params=%#x\n",
                (void*)pp, (unsigned)args[0], (int)args[1], (int)args[2],
                (unsigned)args[3]);
            sceKernelDebugOutText(0,b);
        }
        /* The game's params pointer is a guest VA — mapped here. idx=-1 (a
         * game-side marker for "no binding") must not reach Mesa — a negative
         * index reads out of bounds and kills the linker/validate path. */
        if (pp && args[3] && (int32_t)args[1] >= 0 && (int32_t)args[2] > 0)
            pp((uint32_t)args[0], (uint32_t)args[1],
               (int32_t)args[2], (const float *)(uintptr_t)args[3]);
        RET(0); return;
    }
    if (strcmp(name, "glProgramParameters4fvNVARB") == 0 ||
        strcmp(name, "glProgramEnvParameters4fvNVARB") == 0 ||
        strcmp(name, "glProgramEnvParameters4fvARB") == 0) {
        typedef void (*PFPP)(uint32_t,uint32_t,int32_t,const float*);
        static PFPP pp;
        if (!pp) pp = (PFPP)gl_resolve("glProgramEnvParameters4fvEXT");
        if (pp && args[3] && (int32_t)args[1] >= 0 && (int32_t)args[2] > 0)
            pp((uint32_t)args[0], (uint32_t)args[1], (int32_t)args[2],
               (const float *)(uintptr_t)args[3]);
        RET(0); return;
    }
    if (strcmp(name, "glProgramLocalParameters4fvNVARB") == 0 ||
        strcmp(name, "glProgramLocalParameters4fvARB") == 0) {
        typedef void (*PFPP)(uint32_t,uint32_t,int32_t,const float*);
        static PFPP pp;
        if (!pp) pp = (PFPP)gl_resolve("glProgramLocalParameters4fvEXT");
        if (pp && args[3] && (int32_t)args[1] >= 0 && (int32_t)args[2] > 0)
            pp((uint32_t)args[0], (uint32_t)args[1], (int32_t)args[2],
               (const float *)(uintptr_t)args[3]);
        RET(0); return;
    }
    /* ---- end FF shim ---- */

    /* Diagnostic: /app0/redtest forces the clear colour to red — if the screen
     * turns red the present path works and the black frame is a draw/content
     * problem, not a swap problem. */
    if (strcmp(name, "glClear") == 0 && access("/app0/redtest",0) == 0) {
        typedef void (*PFCC)(float,float,float,float);
        static PFCC cc;
        if (!cc) cc = (PFCC)gl_resolve("glClearColor");
        if (cc) cc(1.0f, 0.0f, 0.0f, 1.0f);
    }

    /* glGetString is the one call a headless run should still answer: the game
     * checks GL_VERSION/GL_EXTENSIONS to decide which path to take. */
    if (strcmp(name, "glGetString") == 0 || strcmp(name, "glGetStringi") == 0) {
        /* String must live in GUEST memory — host .rodata addresses truncate.
         * GL_EXTENSIONS (0x1F03) gets strstr()'d per extension the game wants;
         * answer with the full set so every capability flag sets. */
        static const char ext[] =
            "GL_ARB_depth_texture GL_ARB_fragment_program "
            "GL_ARB_fragment_program_shadow GL_ARB_fragment_shader "
            "GL_ARB_half_float_pixel GL_ARB_pixel_buffer_object "
            "GL_ARB_point_sprite GL_ARB_shadow GL_ARB_texture_border_clamp "
            "GL_ARB_texture_cube_map GL_ARB_texture_env_add "
            "GL_ARB_texture_env_combine GL_ARB_texture_env_crossbar "
            "GL_ARB_texture_float GL_ARB_texture_mirrored_repeat "
            "GL_ARB_texture_non_power_of_two GL_ARB_texture_rectangle "
            "GL_ARB_vertex_program GL_ATI_texture_env_combine3 "
            "GL_ATI_texture_float GL_EXT_abgr GL_EXT_packed_depth_stencil "
            "GL_EXT_pixel_buffer_object GL_EXT_secondary_color "
            "GL_EXT_separate_specular_color GL_EXT_shadow "
            "GL_EXT_texture_cube_map GL_EXT_texture_env_add "
            "GL_EXT_texture_env_combine GL_EXT_texture_env_crossbar "
            "GL_EXT_texture_filter_anisotropic GL_NV_gpu_program_parameters "
            "GL_NV_multisample_filter_hint GL_NV_primitive_restart "
            "GL_NV_texture_env_combine4 GL_NVX_volatile_texture "
            "GL_SGIS_texture_lod GL_EXT_framebuffer_object "
            "GL_EXT_framebuffer_blit GL_EXT_framebuffer_multisample "
            "GL_ARB_vertex_buffer_object GL_ARB_shader_objects "
            "GL_ARB_vertex_shader GL_EXT_draw_range_elements "
            "GL_EXT_blend_func_separate GL_NV_texture_rectangle "
            "GL_EXT_stencil_wrap GL_NV_fog_distance GL_EXT_fog_coord "
            "GL_ARB_multitexture GL_SGIS_generate_mipmap "
            "GL_EXT_texture_lod_bias GL_ARB_occlusion_query "
            "GL_NV_occlusion_query GL_EXT_texture_compression_s3tc "
            "GL_ARB_texture_compression GL_EXT_texture_filter_anisotropic "
            "GL_ARB_fragment_program GL_ARB_texture_rectangle "
            "GL_EXT_framebuffer_object GL_EXT_packed_depth_stencil "
            "GL_ARB_half_float_pixel GL_EXT_texture_sRGB";
        static uint32_t s_gva[4];
        unsigned which = args[0];
        unsigned slot = which == 0x1F03 ? 0 : which == 0x1F01 ? 1 : which == 0x1F02 ? 2 : 3;
        if (!s_gva[slot]) {
            extern void *gmalloc(size_t, size_t);
            const char *v = slot == 0 ? ext :
                            slot == 1 ? "NVIDIA 7800GTX Lindbergh" :
                            slot == 2 ? "2.1.0 NVIDIA Lindbergh" :
                                        "NVIDIA Corporation";
            size_t n = strlen(v) + 1;
            void *p = gmalloc(n, 16);
            memcpy(p, v, n);
            s_gva[slot] = (uint32_t)(uintptr_t)p;
        }
        RET(s_gva[slot]); return;
    }
    /* The shader-object path (glCreateShaderObjectARB etc.) is broken in this
     * Mesa build — objects come back in a namespace the driver can't resolve
     * and the link step crashes inside gl_nir_link_varyings on a NULL. Fake
     * the WHOLE path: creation returns an opaque id, source/compile/attach/
     * link/use are no-op success, queries report success. The game's real
     * shaders go through the ARB-program path (glProgramStringARB) anyway. */
    if (strcmp(name, "glCreateShaderObjectARB") == 0 ||
        strcmp(name, "glCreateShader") == 0) {
        static uint32_t s_fake_shader; s_fake_shader++; RET(0x7100 + s_fake_shader); return;
    }
    if (strcmp(name, "glCreateProgramObjectARB") == 0 ||
        strcmp(name, "glCreateProgram") == 0) {
        static uint32_t s_fake_prog; s_fake_prog++; RET(0x7200 + s_fake_prog); return;
    }
    if (strcmp(name, "glShaderSourceARB") == 0 || strcmp(name, "glShaderSource") == 0 ||
        strcmp(name, "glCompileShaderARB") == 0 || strcmp(name, "glCompileShader") == 0 ||
        strcmp(name, "glAttachObjectARB") == 0 || strcmp(name, "glAttachShader") == 0 ||
        strcmp(name, "glLinkProgramARB") == 0 || strcmp(name, "glLinkProgram") == 0 ||
        strcmp(name, "glUseProgramObjectARB") == 0 || strcmp(name, "glUseProgram") == 0 ||
        strcmp(name, "glDeleteObjectARB") == 0 || strcmp(name, "glDeleteShader") == 0 ||
        strcmp(name, "glDeleteProgram") == 0 || strcmp(name, "glDetachObjectARB") == 0 ||
        strcmp(name, "glValidateProgram") == 0) {
        RET(0); return;
    }
    /* Uniform/attrib calls into a faked program would forward to Mesa with
     * an id it can't resolve — same crash. GetUniformLocation returns a real
     * -1 "not found", the rest are no-ops. */
    if (strcmp(name, "glGetUniformLocationARB") == 0 ||
        strcmp(name, "glGetUniformLocation") == 0 ||
        strcmp(name, "glGetAttribLocationARB") == 0 ||
        strcmp(name, "glGetAttribLocation") == 0) {
        RET(0xFFFFFFFFu); return;
    }
    if (strncmp(name, "glUniform", 9) == 0 ||
        strcmp(name, "glBindAttribLocationARB") == 0 ||
        strcmp(name, "glBindAttribLocation") == 0 ||
        strcmp(name, "glGetUniformfvARB") == 0 || strcmp(name, "glGetUniformivARB") == 0 ||
        strcmp(name, "glGetUniformfv") == 0 || strcmp(name, "glGetUniformiv") == 0 ||
        strcmp(name, "glGetActiveUniformARB") == 0 || strcmp(name, "glGetActiveAttribARB") == 0 ||
        strcmp(name, "glGetShaderSourceARB") == 0 || strcmp(name, "glGetAttachedObjectsARB") == 0 ||
        strcmp(name, "glGetAttachedShaders") == 0) {
        RET(0); return;
    }
    if (strcmp(name, "glIsShader") == 0 || strcmp(name, "glIsProgram") == 0 ||
        strcmp(name, "glGetObjectParameterivARB") == 0 ||
        strcmp(name, "glGetInfoLogARB") == 0) {
        /* shader-object queries — the objects live in a namespace the driver
         * can't look up, so fake success and let the game proceed to the
         * ARB-program path it actually uses. */
        if (strcmp(name, "glGetObjectParameterivARB") == 0) {
            if (args[2]) wr32(args[2], 1);
        } else if (strcmp(name, "glGetInfoLogARB") == 0) {
            if (args[2]) wr32(args[2], 0);
            if (args[3]) *(char *)(uintptr_t)args[3] = 0;
        } else {
            RET(1); return;
        }
        RET(0); return;
    }
    /* one-shot: count texture uploads so we can see if the texdata arrives */
    if (strcmp(name, "glTexImage2D") == 0 ||
        strcmp(name, "glTexSubImage2D") == 0 ||
        strcmp(name, "glCompressedTexImage2DARB") == 0 ||
        strcmp(name, "glCopyTexImage2D") == 0 ||
        strcmp(name, "glCopyTexSubImage2D") == 0) {
        static int s_tex_n;
        s_tex_n++;
        if (s_tex_n <= 20 || (s_tex_n % 200) == 0) {
            extern int sceKernelDebugOutText(int,const char*);
            char b[96];
            __builtin_snprintf(b,sizeof b,
                "[tex] %s #%d a3=%d a4=%d a5=%d a8=%#x\n", name, s_tex_n,
                (int)args[3], (int)args[4], (int)args[5], (unsigned)args[8]);
            sceKernelDebugOutText(0,b);
        }
    }
    /* glProgramStringARB: apply AMD-required patches before upload.
     * linuxloader does the same for all Lindbergh ID4 revisions on ATI/AMD.
     * PS5 GPU is AMD — without these the driver rejects the shaders.
     *   1. OPTION ARB_fragment_program_shadow → commented out (AMD ignores it
     *      in ARB but still parses and rejects the directive).
     *   2. SHADOW2D sampler → 2D  (shadow samplers not supported in ARB on AMD).
     *   3. Skip any shader using vertex.attrib[1] entirely — AMD crashes on it. */
    if (strcmp(name, "glProgramStringARB") == 0) {
        uint32_t target = args[0];
        const char *src  = (const char *)(uintptr_t)args[3];
        int         len  = (int)args[2];
        { extern int sceKernelDebugOutText(int, const char*);
          sceKernelDebugOutText(0, "[gPSA] enter\n"); }
        /* opt-in: which program source just got uploaded — Mesa compiles
         * lazily at first use, so the last upload before a GPU-side crash
         * is the prime suspect. */
        if (src && len > 0 && prog_trace_on()) {
            /* the #var comment names the program — grab the first one */
            const char *tag = strstr(src, "#var");
            char b[96]; int n = 0;
            n += snprintf(b + n, sizeof(b) - n, "[prog] %s tgt=%#x ", 
                          target == 0x8804 ? "fs" : "vs", target);
            if (tag) {
                const char *e = strchr(tag, '\n'); if (!e) e = tag + 60;
                while (tag < e && n < (int)sizeof(b) - 2) b[n++] = *tag++;
            }
            b[n++] = '\n'; b[n] = 0;
            extern int sceKernelDebugOutText(int, const char*);
            sceKernelDebugOutText(0, b);
        }
        if (gl_patch_arb_prog(args) < 0) { RET(0); return; }
    }
    /* VBO-map fault probe — the game fastMemCopys into a glMapBufferARB result
     * unchecked; when the map returns NULL the copy segfaults at 0
     * (deterministic, ~frame 950, eip=0x86AC76A). Track which buffer id gets
     * storage so a NULL map can be blamed on a missing undersized alloc. */
    if (strcmp(name, "glBindBufferARB") == 0 ||
        strcmp(name, "glBindBuffer") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        static uint32_t s_bound[8];
        s_bound[(args[0] == 0x8892u) ? 0 : 1] = args[1];
        (void)s_bound;
        char b[80]; __builtin_snprintf(b, sizeof b,
            "[vbo] bind tgt=%#x id=%u\n", (unsigned)args[0], (unsigned)args[1]);
        static int s_bn; if (s_bn++ < 40)
            sceKernelDebugOutText(0, b);
    }
    if (strcmp(name, "glBufferDataARB") == 0 ||
        strcmp(name, "glBufferData") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        char b[96]; __builtin_snprintf(b, sizeof b,
            "[vbo] data tgt=%#x size=%u usage=%#x\n",
            (unsigned)args[0], (unsigned)args[1], (unsigned)args[3]);
        static int s_dn; if (s_dn++ < 60) sceKernelDebugOutText(0, b);
    }
    if (strcmp(name, "glMapBufferARB") == 0 ||
        strcmp(name, "glMapBuffer") == 0) {
        uint32_t r = gl_forward(g_gl_fn[idx], args, 9, name);
        extern int sceKernelDebugOutText(int, const char*);
        if (!r) {
            /* Mesa refuses to map the default buffer (id 0). The game binds 0
             * and maps WRITE_ONLY as scratch space (ATI tolerated it), then
             * fastMemCopys in unchecked — returning a real guest-arena shadow
             * turns the guaranteed segv into a valid write that draws never
             * read anyway (id 0 = client arrays). */
            extern uint32_t guest_brk(uint32_t);
            extern void guest_heap_lock(void), guest_heap_unlock(void);
            static uint32_t s_sh_gva;
            if (!s_sh_gva) {
                guest_heap_lock();
                if (!s_sh_gva) {
                    s_sh_gva = (guest_brk(0) + 0xfffu) & ~0xfffu;
                    guest_brk(s_sh_gva + 4u * 1024 * 1024);
                }
                guest_heap_unlock();
            }
            r = s_sh_gva;
            sceKernelDebugOutText(0, "[vbo] map NULL -> shadow gva\n");
        }
        RET(r); g_gl_last_call = NULL; return;
    }
    /* bind calls are where Mesa lazily compiles — the last [bind] line before
     * an ACO crash names the guilty program. Dedup by program id — the game
     * rebinds the same set every frame and floods the ring otherwise. */
    if ((strcmp(name, "glBindProgramARB") == 0 ||
         strcmp(name, "glUseProgramObjectARB") == 0) &&
        prog_trace_on()) {
        static int n;
        n++;
        if (n <= 30 || (n % 200) == 0) {
            char b[64]; snprintf(b, sizeof(b), "[bind] %s %#x %#x #%d\n",
                                 name, args[0], args[1], n);
            extern int sceKernelDebugOutText(int, const char*);
            sceKernelDebugOutText(0, b);
        }
    }

    g_gl_last_call = name;
    /* Per-call error probe on the texture upload path — a persistent
     * INVALID_ENUM (=0x500) means the texture never materialises, which is
     * what makes the menu quads sample black. */
    if (strcmp(name, "glTexImage2D") == 0 ||
        strcmp(name, "glTexSubImage2D") == 0 ||
        strcmp(name, "glBindTexture") == 0 ||
        strcmp(name, "glCompressedTexImage2DARB") == 0 ||
        strcmp(name, "glCompressedTexSubImage2DARB") == 0 ||
        strcmp(name, "glTexParameteri") == 0 ||
        strcmp(name, "glGenTextures") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        typedef uint32_t (*PFGE)(void);
        static PFGE ge;
        if (!ge) ge = (PFGE)gl_resolve("glGetError");
        uint32_t e = ge ? ge() : 0;
        if (e) {
            char b[160];
            if (!strcmp(name, "glBindTexture") || !strcmp(name, "glGenTextures"))
                __builtin_snprintf(b, sizeof(b),
                    "[texerr] %s -> %#x tgt=%#x id=%u\n",
                    name, e, (unsigned)args[0], (unsigned)args[1]);
            else if (!strcmp(name, "glTexParameteri"))
                __builtin_snprintf(b, sizeof(b),
                    "[texerr] %s -> %#x tgt=%#x pname=%#x val=%#x\n",
                    name, e, (unsigned)args[0], (unsigned)args[1],
                    (unsigned)args[2]);
            else
                __builtin_snprintf(b, sizeof(b),
                    "[texerr] %s -> %#x ifmt=%#x fmt=%#x type=%#x tgt=%#x\n",
                    name, e, (unsigned)args[2], (unsigned)args[6], (unsigned)args[7],
                    (unsigned)args[0]);
            sceKernelDebugOutText(0, b);
        }
    }
    /* texImage2D crash-diagnostic on the PLT path — dump args + ctx-owner so
     * the klog shows whether Mesa's CurrentTex is NULL / the size is garbage. */
    if (strcmp(name, "glTexImage2D") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        char b[160];
        __builtin_snprintf(b, sizeof(b),
            "[tex] glTexImage2D tgt=%#x lvl=%d ifmt=%#x w=%d h=%d bord=%d fmt=%#x type=%#x px=%#x ctx=%d own=%d\n",
            (unsigned)args[0], (int)args[1], (unsigned)args[2], (int)args[3],
            (int)args[4], (int)args[5], (unsigned)args[6], (unsigned)args[7],
            (unsigned)args[8], s_ctx_tls,
            (int)pthread_equal(s_ctx_owner, pthread_self()));
        sceKernelDebugOutText(0, b);
    }
    /* RGB565 video frames (the Sofdec decoded output) — the PS5 driver's
     * format table has no PIPE_FORMAT_B5G6R5_UNORM, so the resource alloc
     * fails and Mesa reads texImage->pt NULL → segv.  Convert 565→RGBA8 in a
     * bounce buffer and hand Mesa a format it supports. */
    /* RGB565 Sofdec frames — AND any NPOT texture — the ps5 driver's
     * resource_layout rejects NPOT strides, so ps5_resource_create returns
     * NULL and Mesa reads texImage->pt NULL → segv.  Convert the pixels to
     * RGBA8 AND pad the dims up to the next power of two so the alloc path
     * is one the driver accepts.  The game maps UV 0..1 so the frame just
     * occupies the lower-left of a larger texture — visible, slightly offset. */
    /* Depth/stencil textures must NOT hit the RGBA8 rewrite: a color texture
     * attached to DEPTH/STENCIL_ATTACHMENT makes the FBO permanently
     * INCOMPLETE_ATTACHMENT (the game's 962x768 DEPTH24_STENCIL8 targets are
     * NPOT, so they were being converted and every scene FBO died).  The
     * driver allocates NPOT depth-stencil fine — the 1920x1080 window DS is
     * the same path — so let these through untouched. */
    /* Depth-format remap — the ps5 screen supports only PIPE_FORMAT_Z32_FLOAT
     * for depth (and Z32F_S8X24 when PACKED_DEPTH_STENCIL is on); the game's
     * DEPTH24_STENCIL8 (0x88f0) and DEPTH_COMPONENT16/24 (0x81a5/0x81a6)
     * requests produce textures the driver can't attach, so every game FBO
     * came back INCOMPLETE_ATTACHMENT and the offscreen pipeline rendered
     * to nothing.  Upgrade them to the formats the driver actually has. */
    if (strcmp(name, "glTexImage2D") == 0) {
        switch ((unsigned)args[2]) {
        case 0x88f0u:                        /* GL_DEPTH24_STENCIL8   */
            args[2] = 0x8cadu;               /* -> DEPTH32F_STENCIL8  */
            if ((unsigned)args[6] == 0x84f9u &&
                (unsigned)args[7] == 0x84fau)
                args[7] = 0x8dadu;           /* UINT_24_8 -> F32_UINT_24_8_REV */
            break;
        case 0x81a5u:                        /* DEPTH_COMPONENT16     */
        case 0x81a6u:                        /* DEPTH_COMPONENT24     */
        case 0x81a7u:                        /* DEPTH_COMPONENT32     */
            args[2] = 0x8cacu;               /* -> DEPTH_COMPONENT32F */
            if ((unsigned)args[6] == 0x1902u &&
                ((unsigned)args[7] >= 0x1401u && (unsigned)args[7] <= 0x1407u))
                args[7] = 0x1406u;           /* int type -> GL_FLOAT  */
            break;
        }
    }
    int is_depth_tex =
        (unsigned)args[6] == 0x84f9u ||   /* GL_DEPTH_STENCIL   */
        (unsigned)args[6] == 0x1902u ||   /* GL_DEPTH_COMPONENT */
        (unsigned)args[2] == 0x88f0u ||   /* GL_DEPTH24_STENCIL8 */
        (unsigned)args[2] == 0x8cacu ||   /* GL_DEPTH32F_STENCIL8 */
        (unsigned)args[2] == 0x8cadu ||   /* GL_DEPTH32F_STENCIL8 */
        ((unsigned)args[2] >= 0x81a5u &&
         (unsigned)args[2] <= 0x81a7u);   /* DEPTH_COMPONENT16/24/32 */
    if (strcmp(name, "glTexImage2D") == 0 &&
        args[3] && args[4] && !is_depth_tex &&
        ((args[3] & (args[3]-1)) || (args[4] & (args[4]-1)) ||
         ((unsigned)args[7] == 0x8363u) ||
         /* GL_RGB / GL_LUMINANCE / GL_ALPHA internalformat — the ps5 driver
          * only allocates RGBA-able formats; a 3-byte/1-byte src expands to
          * RGBA8 here the same way NPOT does. */
         ((unsigned)args[2] == 0x1907u) ||   /* GL_RGB internalformat */
         ((unsigned)args[6] == 0x1907u) ||   /* GL_RGB format           */
         ((unsigned)args[6] == 0x1906u) ||   /* GL_LUMINANCE */
         ((unsigned)args[6] == 0x1909u))) {  /* GL_ALPHA  */
        uint32_t w = args[3], h = args[4];
        uint32_t pw = 1, ph = 1;
        while (pw < w) pw <<= 1;
        while (ph < h) ph <<= 1;
        uint8_t *dst = calloc(1, (size_t)pw * ph * 4);
        if (!dst) {
            extern int sceKernelDebugOutText(int, const char*);
            char b[96];
            __builtin_snprintf(b, sizeof(b),
                "[tex565] calloc FAILED %ux%u -> upload skipped\n", pw, ph);
            sceKernelDebugOutText(0, b);
            RET(0); return;
        }
        {
            const uint8_t *src = (const uint8_t *)(uintptr_t)args[8];
            int is565 = ((unsigned)args[6] == 0x1907u &&
                         (unsigned)args[7] == 0x8363u);
            int isrgb = (((unsigned)args[6] == 0x1907u ||
                          (unsigned)args[2] == 0x1907u) &&
                         (unsigned)args[7] == 0x1401u);   /* 3-byte RGB */
            int islum = ((unsigned)args[6] == 0x1906u);   /* GL_LUMINANCE */
            int isalp = ((unsigned)args[6] == 0x1909u);   /* GL_ALPHA */
            for (uint32_t y = 0; y < h && src; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t *d = dst + (y * pw + x) * 4;
                    if (is565) {
                        uint16_t v = ((const uint16_t *)src)[y*w + x];
                        d[0] = (uint8_t)((((v >> 11) & 0x1f) * 255 + 15) / 31);
                        d[1] = (uint8_t)((((v >> 5)  & 0x3f) * 255 + 31) / 63);
                        d[2] = (uint8_t)((((v >> 0)  & 0x1f) * 255 + 15) / 31);
                        d[3] = 0xff;
                    } else if (isrgb) {               /* 3-byte RGB -> RGBA */
                        const uint8_t *s3 = src + (y*w + x) * 3;
                        d[0]=s3[0]; d[1]=s3[1]; d[2]=s3[2]; d[3]=0xff;
                    } else if (islum) {               /* L -> RGBA grey */
                        uint8_t v = src[y*w + x];
                        d[0]=v; d[1]=v; d[2]=v; d[3]=0xff;
                    } else if (isalp) {               /* A -> white*A */
                        uint8_t v = src[y*w + x];
                        d[0]=d[1]=d[2]=0xff; d[3]=v;
                    } else {                          /* RGBA8 passthrough */
                        const uint8_t *s4 = src;
                        d[0]=s4[(y*w+x)*4]; d[1]=s4[(y*w+x)*4+1];
                        d[2]=s4[(y*w+x)*4+2]; d[3]=s4[(y*w+x)*4+3];
                    }
                }
            typedef void (*PTI)(uint32_t,int32_t,uint32_t,int32_t,int32_t,
                                int32_t,uint32_t,uint32_t,void*);
            static PTI real_tex;
            if (!real_tex) real_tex = (PTI)gl_resolve("glTexImage2D");
            /* Probe Mesa state — bound texture name + current error — the
             * crash is NULL+0x30 inside texImage so the object's state tells
             * us whether glBindTexture created a real target. */
            {
                extern int sceKernelDebugOutText(int, const char*);
                typedef uint32_t (*PFGE)(void);
                typedef void    (*PFGEI)(uint32_t, int32_t*);
                typedef void    (*PFBT)(uint32_t, uint32_t);
                static PFGEI geti; static PFBT bindt; static PFGE gerr;
                if (!geti)  geti  = (PFGEI)gl_resolve("glGetIntegerv");
                if (!bindt) bindt = (PFBT)gl_resolve("glBindTexture");
                if (!gerr)  gerr  = (PFGE)gl_resolve("glGetError");
                int32_t bound = -1;
                if (geti) geti(0x8069u /*GL_TEXTURE_BINDING_2D*/, &bound);
                char b[128];
                __builtin_snprintf(b, sizeof(b),
                    "[tex565] bound=%d err=%d real_tex=%p\n",
                    (int)bound, gerr ? (int)gerr() : -1, (void*)real_tex);
                sceKernelDebugOutText(0, b);
            }
            /* Is the decoded content actually dark? Hash + sample the
             * converted frame — the attract movie spends nearly all its
             * time on screen, so near-black decode output IS the dark
             * screen. Sample every 60th converted upload. */
            {
                static int s_texprobe;
                if ((++s_texprobe % 60) == 0) {
                    extern int sceKernelDebugOutText(int, const char*);
                    uint64_t h = 0xcbf29ce484222325ull;
                    size_t nb = (size_t)pw * ph * 4;
                    for (size_t i = 0; i < nb; i += 4096)
                        { h ^= dst[i]; h *= 0x100000001b3ull; }
                    size_t mid = (size_t)(ph/2) * pw * 4 + (pw/2) * 4;
                    char b[160];
                    __builtin_snprintf(b, sizeof b,
                        "[texpx] bound err? h=%016llx c=%02x%02x%02x%02x"
                        " tl=%02x%02x%02x%02x w=%u h=%u pw=%u\n",
                        (unsigned long long)h,
                        dst[mid], dst[mid+1], dst[mid+2], dst[mid+3],
                        dst[0], dst[1], dst[2], dst[3],
                        (unsigned)w, (unsigned)h, (unsigned)pw);
                    sceKernelDebugOutText(0, b);
                }
            }
            if (real_tex)
                real_tex(args[0], (int32_t)args[1], 0x1908u /*GL_RGBA*/,
                         (int32_t)pw, (int32_t)ph, (int32_t)args[5],
                         0x1908u, 0x1401u /*GL_UNSIGNED_BYTE*/, dst);
            free(dst);
            RET(0); return;
        }
    }
    /* glCompressedTexImage2DARB — the .efo textures are DXT; the PS5 driver
     * has no compressed-tex path (hits unreachable → SIGILL).  Decompress
     * DXT1/DXT5 to RGBA8 in a bounce buffer and upload as glTexImage2D. */
    if (strcmp(name, "glCompressedTexImage2DARB") == 0 &&
        args[3] && args[4] && args[7]) {
        /* args: tgt, level, ifmt, w, h, border, imageSize, data */
        uint32_t ifmt = args[2], w = args[3], h = args[4];
        const uint8_t *src = (const uint8_t *)(uintptr_t)args[7];
        uint8_t *dst = calloc(1, (size_t)w * h * 4);
        if (dst) {
            unsigned bx = (w + 3) / 4, by = (h + 3) / 4;
            int dxt5 = (ifmt == 0x83F3u);          /* DXT5; else DXT1/0x83f1 */
            for (unsigned byi = 0; byi < by; byi++)
            for (unsigned bxi = 0; bxi < bx; bxi++) {
                const uint8_t *blk = src + (byi * bx + bxi) * (dxt5 ? 16 : 8);
                const uint8_t *cblk = blk + (dxt5 ? 8 : 0);
                uint16_t c0 = cblk[0] | (cblk[1] << 8);
                uint16_t c1 = cblk[2] | (cblk[3] << 8);
                uint8_t col[4][4];
                for (int i = 0; i < 2; i++) {
                    uint16_t v = i ? c1 : c0;
                    col[i][0] = (uint8_t)((((v >> 11) & 0x1f) * 255 + 15) / 31);
                    col[i][1] = (uint8_t)((((v >> 5)  & 0x3f) * 255 + 31) / 63);
                    col[i][2] = (uint8_t)((((v >> 0)  & 0x1f) * 255 + 15) / 31);
                    col[i][3] = 0xff;
                }
                if (c0 > c1 || dxt5) {
                    for (int i = 0; i < 4; i++) {
                        col[2][i] = (uint8_t)((2 * col[0][i] + col[1][i]) / 3);
                        col[3][i] = (uint8_t)((col[0][i] + 2 * col[1][i]) / 3);
                    }
                    col[2][3] = 0xff; col[3][3] = 0xff;
                } else { /* DXT1 transparent mode */
                    for (int i = 0; i < 4; i++) col[2][i] = (uint8_t)((col[0][i] + col[1][i]) / 2);
                    col[3][0] = col[3][1] = col[3][2] = col[3][3] = 0;
                }
                uint32_t idx = (uint32_t)cblk[4] | ((uint32_t)cblk[5] << 8) |
                               ((uint32_t)cblk[6] << 16) | ((uint32_t)cblk[7] << 24);
                uint8_t al[16]; memset(al, 0xff, sizeof al);
                if (dxt5) {
                    uint8_t a0 = blk[0], a1 = blk[1];
                    uint8_t aramp[8] = {a0, a1,
                        a0>a1?(uint8_t)((6*a0+a1)/7):a0>a1?(uint8_t)((5*a0+2*a1)/7):0,
                        a0>a1?(uint8_t)((4*a0+3*a1)/7):0,
                        a0>a1?(uint8_t)((3*a0+4*a1)/7):0,
                        a0>a1?(uint8_t)((2*a0+5*a1)/7):0,
                        a0>a1?(uint8_t)((a0+6*a1)/7):0, 0xff};
                    uint64_t ab = 0;
                    for (int i = 0; i < 6; i++) ab |= (uint64_t)blk[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++) al[i] = aramp[(ab >> (3 * i)) & 7];
                }
                for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    unsigned x = bxi * 4 + px, y = byi * 4 + py;
                    if (x >= w || y >= h) continue;
                    unsigned ci = (idx >> (2 * (py * 4 + px))) & 3;
                    uint8_t *d = dst + (y * w + x) * 4;
                    d[0] = col[ci][0]; d[1] = col[ci][1];
                    d[2] = col[ci][2];
                    d[3] = dxt5 ? al[py * 4 + px] : col[ci][3];
                }
            }
            typedef void (*PTI)(uint32_t,int32_t,uint32_t,int32_t,int32_t,
                                int32_t,uint32_t,uint32_t,void*);
            static PTI real_tex;
            if (!real_tex) real_tex = (PTI)gl_resolve("glTexImage2D");
            if (real_tex)
                real_tex(args[0], (int32_t)args[1], 0x1908u /*GL_RGBA*/,
                         (int32_t)w, (int32_t)h, (int32_t)args[5],
                         0x1908u, 0x1401u /*GL_UNSIGNED_BYTE*/, dst);
            free(dst);
            RET(0); return;
        }
    }
    /* LINDBERGH_FP_SOLID: every fragment program replaced with solid green —
     * the LGJ instrument that split "nothing rasterises" from "wrong colour".
     * If the screen goes green, geometry+rasterisation reach the target and
     * the loss is in program/texture content. */
    static int s_fp_solid = -1;
    if (s_fp_solid < 0) { extern char *getenv(const char *); s_fp_solid = getenv("LINDBERGH_FP_SOLID") ? 1 : 0; }
    if (s_fp_solid && strcmp(name, "glProgramStringARB") == 0 &&
        (unsigned)args[0] == 0x8804u /*GL_FRAGMENT_PROGRAM_ARB*/) {
        static const char fp_green[] =
            "!!ARBfp1.0\n"
            "MOV result.color, { 0.0, 1.0, 0.0, 1.0 };\n"
            "END\n";
        args[2] = (uint64_t)(sizeof(fp_green) - 1);
        args[3] = (uint64_t)(uintptr_t)fp_green;
    }
    /* LINDBERGH_NO_FBO: force every FBO bind to the default framebuffer. */
    static int s_no_fbo = -1;
    if (s_no_fbo < 0) { extern char *getenv(const char *); s_no_fbo = getenv("LINDBERGH_NO_FBO") ? 1 : 0; }
    if (s_no_fbo && strcmp(name, "glBindFramebufferEXT") == 0)
        args[1] = 0;
    /* glProgramEnvParameter4fvARB — the vertex constants upload; the LGJ
     * black frame was env[0] all zeros (matrix first column lost). Log the
     * vector so we can see whether the guest hands real matrices. */
    if (strncmp(name, "glProgramEnvParameter", 21) == 0 ||
        strncmp(name, "glProgramLocalParameter", 23) == 0 ||
        strcmp(name, "glProgramParameters4fvNV") == 0) {
        static int epv;
        if (epv++ < 40) {
            extern int sceKernelDebugOutText(int, const char*);
            /* upload-path flags in the game's data segment — which of the
             * four setMatrix44 paths (cg-indirect / local / env / nv-batch)
             * the engine armed itself with. */
            extern uint32_t g_image_delta;
            uintptr_t d = g_image_delta;
            uint8_t fenv = *(volatile uint8_t *)(uintptr_t)(0x89ec8f4u + d);
            uint8_t fcg  = *(volatile uint8_t *)(uintptr_t)(0x89ec93cu + d);
            uint32_t pfcg = *(volatile uint32_t *)(uintptr_t)(0x89ec940u + d);
            char b[160];
            __builtin_snprintf(b, sizeof(b),
                "[envp] %s a0=%#x a1=%#x a2=%#x fn=%p fenv=%d fcg=%d pfcg=%#x\n",
                name, (unsigned)args[0], (unsigned)args[1], (unsigned)args[2],
                (void *)g_gl_fn[idx], fenv, fcg, pfcg);
            sceKernelDebugOutText(0, b);
        }
    }
    if (strcmp(name, "glProgramEnvParameter4fvARB") == 0) {
        static int ev;
        const float *fv = (const float *)(uintptr_t)args[2];
        if (ev++ < 80) {
            extern int sceKernelDebugOutText(int, const char*);
            char b[160];
            __builtin_snprintf(b, sizeof(b),
                "[env] p4v tgt=%#x idx=%d f=%.4g %.4g %.4g %.4g\n",
                (unsigned)args[0], (int)args[1],
                fv ? fv[0] : -1.f, fv ? fv[1] : -1.f, fv ? fv[2] : -1.f, fv ? fv[3] : -1.f);
            sceKernelDebugOutText(0, b);
        }
    }
    /* For glProgramStringARB: log fn ptr + string addr + first bytes before
     * calling Mesa — rip=0x3 crashes leave no context, this shows the cause. */
    if (strcmp(name, "glProgramStringARB") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        char _bd[128];
        const char *_hdr = args[3] ? (const char *)(uintptr_t)args[3] : "";
        int _hl = 0;
        while (_hl < 40 && _hdr[_hl] && _hdr[_hl] != '\n') _hl++;
        __builtin_snprintf(_bd, sizeof(_bd),
            "[ps] fn=%p str=%#x len=%d hdr=%.40s\n",
            (void *)g_gl_fn[idx], (unsigned)args[3], (int)args[2], _hdr);
        sceKernelDebugOutText(0, _bd);
    }
    /* glFramebufferTexture2DEXT — the texture it attaches + completeness.
     * If the offscreen FBOs the scene renders into never become complete,
     * the 3D content draws to nowhere and the composite samples black. */
    if (strcmp(name, "glFramebufferTexture2DEXT") == 0 ||
        strcmp(name, "glCheckFramebufferStatusEXT") == 0 ||
        strcmp(name, "glBindFramebufferEXT") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        if (strcmp(name, "glCheckFramebufferStatusEXT") == 0) {
            uint32_t r = gl_forward(g_gl_fn[idx], args, 9, name);
            static int cfs;
            if (r != 0x8CD5u && cfs++ < 40) {
                char b[120];
                __builtin_snprintf(b, sizeof(b),
                    "[fbo] CheckFramebufferStatus -> %#x (incomplete!)\n", r);
                sceKernelDebugOutText(0, b);
            }
            RET(r); g_gl_last_call = NULL; return;
        }
        if (strcmp(name, "glFramebufferTexture2DEXT") == 0) {
            static int ft;
            if (ft++ < 60) {
                char b[160];
                __builtin_snprintf(b, sizeof(b),
                    "[fbo] FbTex2D tgt=%#x attach=%#x texgt=%#x tex=%d lvl=%d\n",
                    (unsigned)args[0], (unsigned)args[1], (unsigned)args[2],
                    (int)args[3], (int)args[4]);
                sceKernelDebugOutText(0, b);
            }
        }
        /* fb0 — the window drawable: log its completeness status so we know
         * whether Mesa considers the default framebuffer renderable. */
        if (strcmp(name, "glBindFramebufferEXT") == 0 && (int)args[1] == 0) {
            int cfsi = -1;
            for (int i = 0; i < (int)GL_COUNT; i++)
                if (g_gl_names[i] && strcmp(g_gl_names[i], "glCheckFramebufferStatusEXT") == 0) { cfsi = i; break; }
            if (cfsi >= 0) {
                if (!g_gl_fn[cfsi]) g_gl_fn[cfsi] = gl_resolve("glCheckFramebufferStatusEXT");
                if (g_gl_fn[cfsi]) {
                    uint32_t r = gl_forward(g_gl_fn[idx], args, 9, name);
                    uint32_t s = ((uint32_t(*)(uint32_t))g_gl_fn[cfsi])((uint32_t)args[0]);
                    static int bs0;
                    if (bs0 < 20 || (bs0 & 0xff) == 0) {
                        char b[96];
                        __builtin_snprintf(b, sizeof(b),
                            "[fbo] fb0 status %#x n=%d\n", s, bs0);
                        sceKernelDebugOutText(0, b);
                    }
                    bs0++;
                    RET(r); g_gl_last_call = NULL; return;
                }
            }
        }
        /* glBindFramebufferEXT — after binding an FBO, check completeness so
         * we can see which offscreen target fails (the 0x506 spam). */
        if (strcmp(name, "glBindFramebufferEXT") == 0 && (int)args[1] != 0) {
            int cfsi = -1;
            for (int i = 0; i < (int)GL_COUNT; i++)
                if (g_gl_names[i] && strcmp(g_gl_names[i], "glCheckFramebufferStatusEXT") == 0) { cfsi = i; break; }
            if (cfsi >= 0) {
                if (!g_gl_fn[cfsi]) g_gl_fn[cfsi] = gl_resolve("glCheckFramebufferStatusEXT");
                if (g_gl_fn[cfsi]) {
                    uint32_t r = gl_forward(g_gl_fn[idx], args, 9, name);
                    uint32_t s = ((uint32_t(*)(uint32_t))g_gl_fn[cfsi])((uint32_t)args[0]);
                    static int bs;
                    if (s != 0x8CD5u && bs++ < 40) {
                        char b[120];
                        __builtin_snprintf(b, sizeof(b),
                            "[fbo] bind fb=%d -> INCOMPLETE %#x\n", (int)args[1], s);
                        sceKernelDebugOutText(0, b);
                    }
                    RET(r); g_gl_last_call = NULL; return;
                }
            }
        }
    }
    /* A/B5: after uploading a (possibly rewritten) program, pull the compile
     * verdict — Mesa defers to first draw, but a text it can't parse fails
     * NOW at glProgramStringARB with PROGRAM_ERROR_POSITION. Cheap check so
     * a rejected fp2 rewrite shows up as a log line, not a silent black draw. */
    if (strcmp(name, "glProgramStringARB") == 0) {
        /* Same patch pipeline the other upload paths take — fp2/vp3
         * downgrades, wrong-target skips, H<n> decl injection — plus the
         * in-place strip of BRA/BBn/OPTION that s_patched doesn't do. */
        int _pk = gl_patch_arb_prog(args);
        if (_pk < 0) { RET(0); return; }
        extern void hle_gl_arb_rewrite(char *src, uint32_t slen);
        char *psrc = (char *)(uintptr_t)args[3];
        uint32_t slen = (uint32_t)args[2];
        if (psrc && slen > 0 && slen < 262144u) hle_gl_arb_rewrite(psrc, slen);
        uint32_t r = gl_forward(g_gl_fn[idx], args, 9, name);
        typedef uint32_t (*PFGE)(void);
        typedef void (*PFGI)(uint32_t, int32_t *);
        static PFGE s_ge; static PFGI s_gi;
        if (!s_ge) s_ge = (PFGE)gl_resolve("glGetError");
        if (!s_gi) s_gi = (PFGI)gl_resolve("glGetIntegerv");
        int32_t errpos = -1;
        if (s_gi) s_gi(0x864Bu /*GL_PROGRAM_ERROR_POSITION_ARB*/, &errpos);
        uint32_t ge = s_ge ? s_ge() : 0;
        static int s_pr_n;
        s_pr_n++;
        if (errpos >= 0 || ge || s_pr_n <= 8) {
            extern int sceKernelDebugOutText(int, const char*);
            char _b[128];
            __builtin_snprintf(_b, sizeof _b,
                "[progck] #%d tgt=%#x err=%#x errpos=%d\n",
                s_pr_n, (unsigned)args[0], ge, errpos);
            sceKernelDebugOutText(0, _b);
            /* On first N failures, dump the ARB driver's error string plus a
             * source-context window around errpos — this is what pins down
             * why Mesa rejected the program. */
            static int s_dumped;
            if (errpos >= 0 && s_dumped < 6) {
                s_dumped++;
                typedef const char * (*PFGS)(uint32_t);
                static PFGS s_gs;
                if (!s_gs) s_gs = (PFGS)gl_resolve("glGetString");
                if (s_gs) {
                    const char *es = s_gs(0x8874u /*GL_PROGRAM_ERROR_STRING_ARB*/);
                    if (es) {
                        char eb[256];
                        __builtin_snprintf(eb, sizeof eb,
                            "[progck] #%d errstr: %.220s\n", s_pr_n, es);
                        sceKernelDebugOutText(0, eb);
                    }
                }
                const char *src = (const char *)(uintptr_t)args[3];
                uint32_t slen = (uint32_t)args[2];
                if (src) {
                    int lo = errpos - 60; if (lo < 0) lo = 0;
                    int hi = errpos + 60;
                    if (slen && (uint32_t)hi > slen) hi = (int)slen;
                    char nb[256]; int o = 0;
                    o += __builtin_snprintf(nb+o, sizeof nb - o,
                            "[progck] #%d near[%d..%d]: ", s_pr_n, lo, hi);
                    for (int i = lo; i < hi && o < (int)sizeof nb - 2; i++) {
                        char ch = src[i];
                        if (ch == '\n' || ch == '\r') ch = ' ';
                        else if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '?';
                        nb[o++] = ch;
                    }
                    nb[o++] = '\n'; nb[o] = 0;
                    sceKernelDebugOutText(0, nb);
                    /* Header dump — the very first bytes identify the dialect
                     * (!!ARBvp1.0 vs !!NVvp2.0) and reveal any OPTION clauses
                     * that Mesa may not support. */
                    int hhi = slen && slen < 200u ? (int)slen : 200;
                    char hb[256]; int ho = 0;
                    ho += __builtin_snprintf(hb+ho, sizeof hb - ho,
                            "[progck] #%d head[0..%d]: ", s_pr_n, hhi);
                    for (int i = 0; i < hhi && ho < (int)sizeof hb - 2; i++) {
                        char ch = src[i];
                        if (ch == '\n' || ch == '\r') ch = ' ';
                        else if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '?';
                        hb[ho++] = ch;
                    }
                    hb[ho++] = '\n'; hb[ho] = 0;
                    sceKernelDebugOutText(0, hb);
                    /* Full failing source to disk — the near/head windows are
                     * not enough to debug multi-statement-line rewrites. */
                    {
                        char pf[64];
                        FILE *f = NULL;
                        snprintf(pf, sizeof pf, "/tmp/progfail_%d.txt", s_pr_n);
                        f = fopen(pf, "w");
                        if (!f) {
                            snprintf(pf, sizeof pf,
                                     "/data/homebrew/progfail_%d.txt", s_pr_n);
                            f = fopen(pf, "w");
                        }
                        if (f) {
                            if (slen) fwrite(src, 1, slen, f);
                            fclose(f);
                            char fb[96];
                            __builtin_snprintf(fb, sizeof fb,
                                "[progck] #%d dumped %s\n", s_pr_n, pf);
                            sceKernelDebugOutText(0, fb);
                        }
                    }
                }
            }
        }
        RET(r); g_gl_last_call = NULL; return;
    }
    RET(gl_forward(g_gl_fn[idx], args, 9, name));
    g_gl_last_call = NULL;
}

/* trampolines — one per name, the guest-callable address. */
#define GL_T(i) static void gl_t##i(CPU *c){ gl_dispatch(c, i); }
GL_T(0) GL_T(1) GL_T(2) GL_T(3) GL_T(4) GL_T(5) GL_T(6) GL_T(7) GL_T(8) GL_T(9)
GL_T(10) GL_T(11) GL_T(12) GL_T(13) GL_T(14) GL_T(15) GL_T(16) GL_T(17) GL_T(18) GL_T(19)
GL_T(20) GL_T(21) GL_T(22) GL_T(23) GL_T(24) GL_T(25) GL_T(26) GL_T(27) GL_T(28) GL_T(29)
GL_T(30) GL_T(31) GL_T(32) GL_T(33) GL_T(34) GL_T(35) GL_T(36) GL_T(37) GL_T(38) GL_T(39)
GL_T(40) GL_T(41) GL_T(42) GL_T(43) GL_T(44) GL_T(45) GL_T(46) GL_T(47) GL_T(48) GL_T(49)
GL_T(50) GL_T(51) GL_T(52) GL_T(53) GL_T(54) GL_T(55) GL_T(56) GL_T(57) GL_T(58) GL_T(59)
GL_T(60) GL_T(61) GL_T(62) GL_T(63) GL_T(64) GL_T(65) GL_T(66) GL_T(67) GL_T(68) GL_T(69)
GL_T(70) GL_T(71) GL_T(72) GL_T(73) GL_T(74) GL_T(75) GL_T(76) GL_T(77) GL_T(78) GL_T(79)
GL_T(80) GL_T(81) GL_T(82) GL_T(83) GL_T(84) GL_T(85) GL_T(86) GL_T(87) GL_T(88) GL_T(89)
GL_T(90) GL_T(91) GL_T(92) GL_T(93) GL_T(94) GL_T(95) GL_T(96) GL_T(97) GL_T(98) GL_T(99)
GL_T(100) GL_T(101) GL_T(102) GL_T(103) GL_T(104) GL_T(105) GL_T(106) GL_T(107) GL_T(108) GL_T(109)
GL_T(110) GL_T(111) GL_T(112) GL_T(113) GL_T(114) GL_T(115) GL_T(116) GL_T(117) GL_T(118) GL_T(119)
GL_T(120) GL_T(121) GL_T(122) GL_T(123) GL_T(124) GL_T(125) GL_T(126) GL_T(127) GL_T(128) GL_T(129)
GL_T(130) GL_T(131) GL_T(132) GL_T(133) GL_T(134) GL_T(135) GL_T(136) GL_T(137) GL_T(138) GL_T(139)
GL_T(140) GL_T(141) GL_T(142) GL_T(143) GL_T(144) GL_T(145) GL_T(146) GL_T(147) GL_T(148) GL_T(149)
GL_T(150) GL_T(151) GL_T(152) GL_T(153) GL_T(154) GL_T(155) GL_T(156) GL_T(157) GL_T(158) GL_T(159)
GL_T(160) GL_T(161) GL_T(162) GL_T(163) GL_T(164) GL_T(165) GL_T(166) GL_T(167) GL_T(168) GL_T(169)
GL_T(170) GL_T(171) GL_T(172) GL_T(173) GL_T(174) GL_T(175) GL_T(176) GL_T(177) GL_T(178) GL_T(179)
GL_T(180) GL_T(181) GL_T(182) GL_T(183) GL_T(184) GL_T(185) GL_T(186) GL_T(187) GL_T(188) GL_T(189)
GL_T(190) GL_T(191) GL_T(192) GL_T(193) GL_T(194) GL_T(195) GL_T(196) GL_T(197) GL_T(198) GL_T(199)
GL_T(200) GL_T(201) GL_T(202) GL_T(203) GL_T(204) GL_T(205) GL_T(206) GL_T(207) GL_T(208) GL_T(209)
GL_T(210) GL_T(211) GL_T(212) GL_T(213) GL_T(214) GL_T(215) GL_T(216) GL_T(217) GL_T(218) GL_T(219)
GL_T(220) GL_T(221) GL_T(222) GL_T(223) GL_T(224) GL_T(225) GL_T(226) GL_T(227) GL_T(228) GL_T(229)
GL_T(230) GL_T(231) GL_T(232) GL_T(233) GL_T(234) GL_T(235) GL_T(236) GL_T(237) GL_T(238) GL_T(239)
GL_T(240) GL_T(241) GL_T(242) GL_T(243) GL_T(244) GL_T(245) GL_T(246) GL_T(247) GL_T(248) GL_T(249)
GL_T(250) GL_T(251) GL_T(252) GL_T(253) GL_T(254) GL_T(255) GL_T(256) GL_T(257) GL_T(258) GL_T(259)
GL_T(260) GL_T(261) GL_T(262) GL_T(263) GL_T(264) GL_T(265) GL_T(266) GL_T(267) GL_T(268) GL_T(269)
GL_T(270) GL_T(271) GL_T(272) GL_T(273) GL_T(274) GL_T(275) GL_T(276) GL_T(277) GL_T(278) GL_T(279)
GL_T(280) GL_T(281) GL_T(282)

uint32_t gl_token_for(const char *name)
{
    void *fn = gl_resolve(name);
    for (int i = 0; i < g_tok_n; i++)
        if (strcmp(g_tok[i].name, name) == 0)
            return GL_TOKEN_BASE + (uint32_t)i;
    if (g_tok_n >= GL_TOKEN_MAX) return 0;
    /* Headless/unknown names still get a token: gl_token_call treats a NULL
     * fn as a silent 0, and the game's "did this resolve?" checks only test
     * for non-NULL — returning 0 here would make it gate whole features off
     * (e.g. the GLSL/ARB_program flags that initializeGraph requires). */
    /* Copy the name NOW — the pointer lives in guest memory and the game's
     * extension walker reuses one buffer for every name it asks about.
     * Storing the pointer meant every ARB name strcmp-matched the first ARB
     * entry (all aliased to whatever the buffer holds today): every
     * glXGetProcAddressARB("glFooARB") collapsed to token 0x60000026, and a
     * call through it dispatched on stale text — e.g. setMatrix44's resolved
     * upload pointer silently no-opped, leaving program.env all zeros. */
    {
        static char s_names[GL_TOKEN_MAX][96];
        size_t L = strlen(name);
        void *fn2 = fn;
        if (!fn2 && L > 5 && !strncmp(name, "gl", 2) &&
            !strcmp(name + L - 3, "ARB") && L - 3 < sizeof s_names[0]) {
            /* NVIDIA-alias name with no real export — resolve the BASE name
             * and store it, so call-time name checks (doubles arity, FBO
             * special-cases) see the function the call really lands on. */
            memcpy(s_names[g_tok_n], name, L - 3); s_names[g_tok_n][L - 3] = 0;
            fn2 = gl_resolve(s_names[g_tok_n]);
            if (fn2) { fn = fn2; } else { s_names[g_tok_n][0] = 0; }
        }
        if (!s_names[g_tok_n][0]) {
            if (L >= sizeof s_names[0]) L = sizeof s_names[0] - 1;
            memcpy(s_names[g_tok_n], name, L); s_names[g_tok_n][L] = 0;
        }
        g_tok[g_tok_n].name = s_names[g_tok_n];
    }
    g_tok[g_tok_n].fn = fn;
    return GL_TOKEN_BASE + (uint32_t)(g_tok_n++);
}

int gl_token_call(CPU *c, uint32_t va)
{
    if (va < GL_TOKEN_BASE || va >= GL_TOKEN_BASE + (uint32_t)g_tok_n) return 0;
    gl_ensure_ctx();
    const char *tname = g_tok[va - GL_TOKEN_BASE].name;
    /* Same shader-object stub as gl_dispatch — resolved pointers for the GLSL
     * path would otherwise hit Mesa's broken linker and crash. */
    if (tname) {
        if (strcmp(tname,"glCreateShaderObjectARB")==0 || strcmp(tname,"glCreateShader")==0 ||
            strcmp(tname,"glCreateProgramObjectARB")==0 || strcmp(tname,"glCreateProgram")==0) {
            static uint32_t s_ftok; s_ftok++; RET(0x7300 + s_ftok); return 1;
        }
        if (strcmp(tname,"glShaderSourceARB")==0 || strcmp(tname,"glShaderSource")==0 ||
            strcmp(tname,"glCompileShaderARB")==0 || strcmp(tname,"glCompileShader")==0 ||
            strcmp(tname,"glAttachObjectARB")==0 || strcmp(tname,"glAttachShader")==0 ||
            strcmp(tname,"glLinkProgramARB")==0 || strcmp(tname,"glLinkProgram")==0 ||
            strcmp(tname,"glUseProgramObjectARB")==0 || strcmp(tname,"glUseProgram")==0 ||
            strcmp(tname,"glValidateProgram")==0 ||
            strncmp(tname,"glUniform",9)==0 ||
            strcmp(tname,"glGetUniformLocationARB")==0 || strcmp(tname,"glGetUniformLocation")==0 ||
            strcmp(tname,"glGetAttribLocationARB")==0 || strcmp(tname,"glGetAttribLocation")==0 ||
            strcmp(tname,"glBindAttribLocationARB")==0 || strcmp(tname,"glBindAttribLocation")==0) {
            RET(strcmp(tname,"glGetUniformLocationARB")==0 || strcmp(tname,"glGetUniformLocation")==0 ||
                strcmp(tname,"glGetAttribLocationARB")==0 || strcmp(tname,"glGetAttribLocation")==0
                ? 0xFFFFFFFFu : 0);
            return 1;
        }
    }
    {
        /* One-shot markers — the game's real draws land here via function
         * pointers from glXGetProcAddressARB. */
        static uint8_t seen_draw, seen_progstr;
        extern int sceKernelDebugOutText(int, const char*);
        if (!seen_draw && (strcmp(tname,"glDrawRangeElements")==0 ||
                           strcmp(tname,"glDrawElements")==0 ||
                           strcmp(tname,"glDrawArrays")==0)) {
            seen_draw = 1; sceKernelDebugOutText(0,"[gl] first draw (tok)\n");
        }
        if (!seen_progstr && strcmp(tname,"glProgramStringARB")==0) {
            seen_progstr = 1; sceKernelDebugOutText(0,"[gl] first progstr (tok)\n");
        }
        static int s_tex_tok;
        if (strncmp(tname,"glTexImage",10)==0 || strncmp(tname,"glTexSubImage",13)==0 ||
            strncmp(tname,"glCompressedTex",15)==0 || strncmp(tname,"glCopyTex",9)==0) {
            s_tex_tok++;
            if (s_tex_tok <= 20 || (s_tex_tok % 200) == 0) {
                char b[80]; __builtin_snprintf(b,sizeof b,"[tex-tok] %s #%d\n",tname,s_tex_tok);
                sceKernelDebugOutText(0,b);
            }
        }
        /* Vertex-setup call family — vsrc showed arr=0000 attr=000000 at FBO
         * draw time, so trace whether the array/attrib setup calls ever
         * arrive and with what arguments. */
        {
            static const char *const vs_names[] = {
                "glVertexPointer","glColorPointer","glNormalPointer",
                "glTexCoordPointer","glInterleavedArrays",
                "glEnableClientState","glDisableClientState",
                "glClientActiveTexture",
                "glVertexAttribPointerARB","glEnableVertexAttribArrayARB",
                "glDisableVertexAttribArrayARB","glVertexAttribPointer",
                "glEnableVertexAttribArray","glDisableVertexAttribArray",
                "glBindBufferARB","glBindBuffer","glBufferDataARB",
                "glBufferData","glBufferSubDataARB","glBufferSubData",
                "glPushClientAttrib","glPopClientAttrib","glLockArraysEXT",0 };
            static int vs_seen[64];
            char tbase[96]; size_t tl = strlen(tname);
            if (tl > 3 && !strcmp(tname + tl - 3, "ARB")) tl -= 3;
            if (tl >= sizeof tbase) tl = sizeof tbase - 1;
            memcpy(tbase, tname, tl); tbase[tl] = 0;
            for (int i = 0; vs_names[i]; i++)
                if (!strcmp(tbase, vs_names[i]) || !strcmp(tname, vs_names[i])) {
                    if (vs_seen[i] < 4) {
                        vs_seen[i]++;
                        char b[144];
                        __builtin_snprintf(b,sizeof b,
                            "[vsetup] %s a0=%#x a1=%#x a2=%#x a3=%#x a4=%#x\n",
                            tname,A32(0),A32(1),A32(2),A32(3),A32(4));
                        sceKernelDebugOutText(0,b);
                    }
                    break;
                }
        }
        /* Draw + FBO activity — is the 3D scene actually submitting geometry
         * and rendering into the offscreen target the composite reads? */
        static int s_dr, s_fbo;
        if (strcmp(tname,"glDrawElements")==0 || strcmp(tname,"glDrawRangeElements")==0 ||
            strcmp(tname,"glDrawArrays")==0 || strcmp(tname,"glDrawArraysInstancedARB")==0) {
            s_dr++;
            s_frame_draws++;
            if (s_bound_vp) s_frame_draws_with_vp++;
            if (s_bound_fp) s_frame_draws_with_fp++;
            if (s_dr <= 10 || (s_dr % 300) == 0) {
                char b[128]; __builtin_snprintf(b,sizeof b,"[draw] %s #%d mode=%#x n=%d vp=%u fp=%u\n",
                        tname, s_dr, A32(0), (int)A32(strcmp(tname,"glDrawArrays")==0||strcmp(tname,"glDrawArraysInstancedARB")==0 ? 2 : 1),
                        s_bound_vp, s_bound_fp);
                sceKernelDebugOutText(0,b);
            }
            /* Per-program one-shot: dump the bound VP's source PARAM lines +
             * local/env banks — settles which constant bank 3D shaders read. */
            static int s_vpd[10]; int slot3 = -1;
            if (s_bound_vp) {
                for (int i = 0; i < 10; i++)
                    if (s_vpd[i] == (int)s_bound_vp) { slot3 = -2; break; }
                if (slot3 != -2)
                    for (int i = 0; i < 10; i++)
                        if (!s_vpd[i]) { slot3 = i; break; }
            }
            if (slot3 >= 0) {
                s_vpd[slot3] = (int)s_bound_vp;
                typedef void (*PFGPS3)(uint32_t,uint32_t,void*);
                typedef void (*PFGPV)(uint32_t,uint32_t,float*);
                static PFGPS3 gps3; static PFGPV gls3, ges3;
                if (!gps3) gps3 = (PFGPS3)gl_resolve("glGetProgramStringARB");
                if (!gls3) gls3 = (PFGPV)gl_resolve("glGetProgramLocalParameterfvARB");
                if (!ges3) ges3 = (PFGPV)gl_resolve("glGetProgramEnvParameterfvARB");
                static char pbuf3[16384];
                if (gps3) {
                    gps3(0x8620u, 0x8628u, pbuf3); pbuf3[16383] = 0;
                    char b[160];
                    __builtin_snprintf(b,sizeof b,"[vp%d-src] ----\n", s_bound_vp);
                    sceKernelDebugOutText(0,b);
                    int li = 0; char *p = pbuf3;
                    while (*p && li++ < 200) {
                        char *nl = strchr(p,'\n');
                        int ll = nl ? (int)(nl-p) : (int)strlen(p);
                        /* skip blank pad lines */
                        int nb = 0; for (int k=0;k<ll;k++) if (p[k]!=' '&&p[k]!='\t') nb=1;
                        char lbuf[160]; int lc = ll<159?ll:159;
                        memcpy(lbuf,p,lc); lbuf[lc]=0;
                        if (nb && (strstr(lbuf,"PARAM")||strstr(lbuf,"program.")||strstr(lbuf,"DP4")||
                            strstr(lbuf,"MOV")||strstr(lbuf,"result.position")||li<4||strstr(lbuf,"END")||
                            strstr(lbuf,"ARBvp")||strstr(lbuf,"ARBfp")||strstr(lbuf,"TEMP")||strstr(lbuf,"ATTRIB"))) {
                            __builtin_snprintf(b,sizeof b,"[vp%d] %.*s\n",
                                s_bound_vp, ll<140?ll:140, p);
                            sceKernelDebugOutText(0,b);
                        }
                        if (!nl) break; p = nl+1;
                    }
                }
                if (gls3) { float e[4]; char b[96];
                    for (int i=0;i<6;i++){ gls3(0x8620u,i,e);
                        __builtin_snprintf(b,sizeof b,"[vp%d] loc[%d]=%.3f %.3f %.3f %.3f\n",
                            s_bound_vp,i,e[0],e[1],e[2],e[3]); sceKernelDebugOutText(0,b);} }
                if (ges3) { float e[4]; char b[96];
                    for (int i=0;i<6;i++){ ges3(0x8620u,i,e);
                        __builtin_snprintf(b,sizeof b,"[vp%d] env[%d]=%.3f %.3f %.3f %.3f\n",
                            s_bound_vp,i,e[0],e[1],e[2],e[3]); sceKernelDebugOutText(0,b);} }
            }
        } else if (strcmp(tname,"glBindProgramARB")==0) {
            uint32_t target = A32(0), prog = A32(1);
            if (target == 0x8620u) s_bound_vp = prog;        /* GL_VERTEX_PROGRAM_ARB   */
            else if (target == 0x8804u) s_bound_fp = prog;   /* GL_FRAGMENT_PROGRAM_ARB */
        } else if (strcmp(tname,"glBindFramebufferEXT")==0 || strcmp(tname,"glBindFramebuffer")==0 ||
                   strcmp(tname,"glFramebufferTexture2DEXT")==0 || strcmp(tname,"glGenFramebuffersEXT")==0) {
            s_fbo++;
            if (s_fbo <= 12 || (s_fbo % 60) == 0) {
                char b[96]; __builtin_snprintf(b,sizeof b,"[fbo] %s #%d a=%#x b=%#x\n",
                        tname, s_fbo, A32(0), A32(1));
                sceKernelDebugOutText(0,b);
            }
        }
    }
    if (gl_trace_on()) gl_trace(tname);
    uint32_t args[GL_TOKEN_ARGS];
    for (int i = 0; i < GL_TOKEN_ARGS; i++) args[i] = A32(i);
    if (prog_trace_on()) {
        if (strcmp(tname, "glProgramStringARB") == 0) {
            const char *src = (const char *)(uintptr_t)args[3];
            const char *tag = src ? strstr(src, "#var") : NULL;
            char b[128]; int n = snprintf(b, sizeof(b), "[prog] tok tgt=%#x ", args[0]);
            if (tag) { const char *e = strchr(tag, '\n'); if (!e) e = tag + 60;
                while (tag < e && n < (int)sizeof(b) - 2) b[n++] = *tag++; }
            b[n++] = '\n'; b[n] = 0;
            extern int sceKernelDebugOutText(int, const char*);
            sceKernelDebugOutText(0, b);
        } else if (strcmp(tname, "glBindProgramARB") == 0 ||
                   strcmp(tname, "glUseProgramObjectARB") == 0 ||
                   strcmp(tname, "glUseProgram") == 0) {
            static int n;
            n++;
            if (n <= 30 || (n % 200) == 0) {
                char b[64]; snprintf(b, sizeof(b), "[bind] %s %#x %#x #%d\n", tname, args[0], args[1], n);
                extern int sceKernelDebugOutText(int, const char*);
                sceKernelDebugOutText(0, b);
            }
        }
    }
    /* glProgramStringARB through the resolved path compiles the game's ARB
     * text inside Mesa (prog_to_nir → nir link) — a bad program kills a
     * background compile thread. Dump its first #var comment + header line so
     * the klog names the offender. */
    if (strcmp(tname, "glProgramStringARB") == 0) {
        const char *src = (const char *)(uintptr_t)args[3];
        int len = (int)args[2];
        if (src && len > 0) {
            extern int sceKernelDebugOutText(int, const char*);
            char b[160]; int n = 0;
            /* skip vertex.attrib[1] shaders — same AMD crash as dispatch path */
            if (strstr(src, "vertex.attrib[1]")) { RET(0); return 1; }
            n += __builtin_snprintf(b + n, sizeof(b) - n, "[progstr] tgt=%#x len=%d\n",
                                    (unsigned)args[0], len);
            const char *nl = memchr(src, '\n', len < 80 ? len : 80);
            int hl = nl ? (int)(nl - src) : (len < 79 ? len : 79);
            if (hl > 0) {
                n += __builtin_snprintf(b + n, sizeof(b) - n, "  hdr: %.*s\n", hl < 78 ? hl : 78, src);
            }
            const char *tag = strstr(src, "#var");
            if (tag && n < (int)sizeof(b) - 4) {
                b[n++] = ' '; b[n++] = ' ';
                while (*tag && *tag != '\n' && n < (int)sizeof(b) - 3) b[n++] = *tag++;
                b[n++] = '\n';
            }
            b[n] = 0;
            sceKernelDebugOutText(0, b);
            /* Same AMD patch as the dispatch path — OPTION shadows, SHADOW2D
             * samplers, and NV_fragment_program2 (this is the path the .asm_lbg
             * fp40 shaders take via cgCreateProgram — they must not reach Mesa
             * with the OPTION or H-opcodes intact, or the upload is rejected and
             * the game throws). */
            if (gl_patch_arb_prog(args) < 0) { RET(0); return 1; }
        }
        /* LINDBERGH_FP_SOLID on the token path too — dispatch-side patch
         * alone left FP uploads here unpatched, making the green test
         * inconclusive. */
        { static int s_fp_solid2 = -1;
          if (s_fp_solid2 < 0) { extern char *getenv(const char *);
              s_fp_solid2 = getenv("LINDBERGH_FP_SOLID") ? 1 : 0; }
          if (s_fp_solid2 && (unsigned)args[0] == 0x8804u) {
              static const char fp_green[] =
                  "!!ARBfp1.0\nMOV result.color, { 0.0, 1.0, 0.0, 1.0 };\nEND\n";
              args[2] = (uint64_t)(sizeof(fp_green) - 1);
              args[3] = (uint64_t)(uintptr_t)fp_green;
          } }
    }
    g_gl_last_call = tname;
    if (strncmp(tname, "glProgramEnvParameter", 21) == 0 ||
        strncmp(tname, "glProgramLocalParameter", 23) == 0 ||
        strncmp(tname, "glProgramParameters", 19) == 0) {
        static int epv2;
        if (epv2++ < 60) {
            extern int sceKernelDebugOutText(int, const char*);
            char b[150];
            __builtin_snprintf(b, sizeof(b),
                "[envp-tok] %s a0=%#x a1=%#x a2=%#x a3=%#x fn=%p\n", tname,
                (unsigned)args[0], (unsigned)args[1],
                (unsigned)args[2], (unsigned)args[3],
                g_tok[va - GL_TOKEN_BASE].fn);
            sceKernelDebugOutText(0, b);
        }
    }
    /* texImage2D crash-diagnostic — dump args + the bound texture name so the
     * klog shows whether Mesa's CurrentTex is NULL / the size is garbage. */
    if (strcmp(tname, "glTexImage2D") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        char b[160];
        __builtin_snprintf(b, sizeof(b),
            "[tex] glTexImage2D tgt=%#x lvl=%d ifmt=%#x w=%d h=%d bord=%d fmt=%#x type=%#x px=%#x ctx=%d\n",
            (unsigned)A32(0), (int)A32(1), (unsigned)A32(2), (int)A32(3),
            (int)A32(4), (int)A32(5), (unsigned)A32(6), (unsigned)A32(7),
            (unsigned)A32(8), s_ctx_tls);
        sceKernelDebugOutText(0, b);
    }
    if (strcmp(tname, "glProgramStringARB") == 0) {
        extern int sceKernelDebugOutText(int, const char*);
        char _bd[128];
        const char *_hdr = args[3] ? (const char *)(uintptr_t)args[3] : "";
        __builtin_snprintf(_bd, sizeof(_bd),
            "[ps-tok] fn=%p str=%#x len=%d hdr=%.40s\n",
            g_tok[va - GL_TOKEN_BASE].fn, (unsigned)args[3], (int)args[2], _hdr);
        sceKernelDebugOutText(0, _bd);
    }
    if (strcmp(tname, "glProgramStringARB") == 0) {
        uint32_t r = gl_forward((GlFn)g_tok[va - GL_TOKEN_BASE].fn, args, GL_TOKEN_ARGS, tname);
        typedef uint32_t (*PFGE)(void);
        typedef void (*PFGI)(uint32_t, int32_t *);
        typedef const uint8_t *(*PFGSTR)(uint32_t);
        static PFGE s_ge2; static PFGI s_gi2; static PFGSTR s_gs2;
        if (!s_ge2) s_ge2 = (PFGE)gl_resolve("glGetError");
        if (!s_gi2) s_gi2 = (PFGI)gl_resolve("glGetIntegerv");
        if (!s_gs2) s_gs2 = (PFGSTR)gl_resolve("glGetString");
        int32_t errpos = -1;
        if (s_gi2) s_gi2(0x864Bu /*GL_PROGRAM_ERROR_POSITION_ARB*/, &errpos);
        uint32_t ge = s_ge2 ? s_ge2() : 0;
        static int s_tok_n;
        s_tok_n++;
        if (errpos >= 0 || ge || s_tok_n <= 8) {
            extern int sceKernelDebugOutText(int, const char*);
            char _b[128];
            __builtin_snprintf(_b, sizeof _b,
                "[progck-tok] #%d tgt=%#x err=%#x errpos=%d\n",
                s_tok_n, (unsigned)args[0], ge, errpos);
            sceKernelDebugOutText(0, _b);
            /* On failure, dump the driver's error-string + the offending region
             * of source so we know *why* the compile rejected it (OPTION name,
             * bad opcode, unsupported sampler, ...). This is the one piece the
             * old instrumentation missed and the reason we're flying blind. */
            if (errpos >= 0) {
                const uint8_t *estr = s_gs2 ? s_gs2(0x8874u /*GL_PROGRAM_ERROR_STRING_ARB*/) : NULL;
                if (estr) {
                    char _e[192];
                    __builtin_snprintf(_e, sizeof _e, "[progck-tok] errstr: %.180s\n", (const char *)estr);
                    sceKernelDebugOutText(0, _e);
                }
                const char *src = (const char *)(uintptr_t)args[3];
                int srclen = (int)args[2];
                if (src && errpos < srclen) {
                    int from = errpos > 32 ? errpos - 32 : 0;
                    int span = srclen - from; if (span > 96) span = 96;
                    char _c[160];
                    __builtin_snprintf(_c, sizeof _c, "[progck-tok] near: %.*s\n", span, src + from);
                    sceKernelDebugOutText(0, _c);
                }
            }
        }
        RET(r); g_gl_last_call = NULL; return 1;
    }
    /* Cg NVARB batched parameter uploads — no such GL symbols exist; Mesa's
     * permissive eglGetProcAddress returns a thunk that does nothing, so the
     * game's matrix uploads silently vanish (env reads back all zeros).
     * Emulate the batch with the scalar ARB entry Mesa really exports. */
    if (strcmp(tname, "glProgramEnvParameters4fvNVARB") == 0 ||
        strcmp(tname, "glProgramParameters4fvNVARB") == 0 ||
        strcmp(tname, "glProgramEnvParameters4fvARB") == 0 ||
        strcmp(tname, "glProgramLocalParameters4fvNVARB") == 0 ||
        strcmp(tname, "glProgramLocalParameters4fvARB") == 0 ||
        strcmp(tname, "glProgramParameters4fvNV") == 0) {
        int is_local = (strstr(tname, "Local") != NULL);
        typedef void (*PFEP)(uint32_t,uint32_t,const float*);
        static PFEP ep, lp;
        if (!ep) ep = (PFEP)gl_resolve("glProgramEnvParameter4fvARB");
        if (!lp) lp = (PFEP)gl_resolve("glProgramLocalParameter4fvARB");
        const float *src = (const float *)(uintptr_t)args[3];
        int32_t idx = (int32_t)args[1], cnt = (int32_t)args[2];
        if (src && idx >= 0 && cnt > 0 && cnt <= 4096) {
            PFEP fn = is_local ? lp : ep;
            if (fn) for (int32_t i = 0; i < cnt; i++)
                fn((uint32_t)args[0], (uint32_t)(idx + i), src + i*4);
        }
        RET(0); g_gl_last_call = NULL; return 1;
    }
    RET(gl_forward((GlFn)g_tok[va - GL_TOKEN_BASE].fn, args, GL_TOKEN_ARGS, tname));
    g_gl_last_call = NULL;
    return 1;
}

int gl_token_count(void) { return g_tok_n; }

static void h_glXGetProcAddressARB(CPU *c)
{
    const char *name = ASTR(0);
    uint32_t tok = gl_token_for(name);
    if (!tok) {
        /* Headless: gl_resolve returned NULL but the game's PLT already has a
         * trampoline for every standard GL name — return that so the game gets
         * a callable address instead of 0. */
        tok = hle_plt_address(name);
    }
    fprintf(stderr, "[glx] getProcAddress(%s) -> %#x\n", name, tok);
    RET(tok);
}
static void h_glXGetProcAddress(CPU *c) { h_glXGetProcAddressARB(c); }

void hle_register_gl(void)
{
    /* Bind the whole GL name table to the trampolines — one hle_bind per
     * entry, the guest calls the trampoline which forwards to the real GL. */
    static void (*const t[])(CPU *) = {
        gl_t0,gl_t1,gl_t2,gl_t3,gl_t4,gl_t5,gl_t6,gl_t7,gl_t8,gl_t9,
        gl_t10,gl_t11,gl_t12,gl_t13,gl_t14,gl_t15,gl_t16,gl_t17,gl_t18,gl_t19,
        gl_t20,gl_t21,gl_t22,gl_t23,gl_t24,gl_t25,gl_t26,gl_t27,gl_t28,gl_t29,
        gl_t30,gl_t31,gl_t32,gl_t33,gl_t34,gl_t35,gl_t36,gl_t37,gl_t38,gl_t39,
        gl_t40,gl_t41,gl_t42,gl_t43,gl_t44,gl_t45,gl_t46,gl_t47,gl_t48,gl_t49,
        gl_t50,gl_t51,gl_t52,gl_t53,gl_t54,gl_t55,gl_t56,gl_t57,gl_t58,gl_t59,
        gl_t60,gl_t61,gl_t62,gl_t63,gl_t64,gl_t65,gl_t66,gl_t67,gl_t68,gl_t69,
        gl_t70,gl_t71,gl_t72,gl_t73,gl_t74,gl_t75,gl_t76,gl_t77,gl_t78,gl_t79,
        gl_t80,gl_t81,gl_t82,gl_t83,gl_t84,gl_t85,gl_t86,gl_t87,gl_t88,gl_t89,
        gl_t90,gl_t91,gl_t92,gl_t93,gl_t94,gl_t95,gl_t96,gl_t97,gl_t98,gl_t99,
        gl_t100,gl_t101,gl_t102,gl_t103,gl_t104,gl_t105,gl_t106,gl_t107,gl_t108,gl_t109,
        gl_t110,gl_t111,gl_t112,gl_t113,gl_t114,gl_t115,gl_t116,gl_t117,gl_t118,gl_t119,
        gl_t120,gl_t121,gl_t122,gl_t123,gl_t124,gl_t125,gl_t126,gl_t127,gl_t128,gl_t129,
        gl_t130,gl_t131,gl_t132,gl_t133,gl_t134,gl_t135,gl_t136,gl_t137,gl_t138,gl_t139,
        gl_t140,gl_t141,gl_t142,gl_t143,gl_t144,gl_t145,gl_t146,gl_t147,gl_t148,gl_t149,
        gl_t150,gl_t151,gl_t152,gl_t153,gl_t154,gl_t155,gl_t156,gl_t157,gl_t158,gl_t159,
        gl_t160,gl_t161,gl_t162,gl_t163,gl_t164,gl_t165,gl_t166,gl_t167,gl_t168,gl_t169,
        gl_t170,gl_t171,gl_t172,gl_t173,gl_t174,gl_t175,gl_t176,gl_t177,gl_t178,gl_t179,
        gl_t180,gl_t181,gl_t182,gl_t183,gl_t184,gl_t185,gl_t186,gl_t187,gl_t188,gl_t189,
        gl_t190,gl_t191,gl_t192,gl_t193,gl_t194,gl_t195,gl_t196,gl_t197,gl_t198,gl_t199,
        gl_t200,gl_t201,gl_t202,gl_t203,gl_t204,gl_t205,gl_t206,gl_t207,gl_t208,gl_t209,
        gl_t210,gl_t211,gl_t212,gl_t213,gl_t214,gl_t215,gl_t216,gl_t217,gl_t218,gl_t219,
        gl_t220,gl_t221,gl_t222,gl_t223,gl_t224,gl_t225,gl_t226,gl_t227,gl_t228,gl_t229,
        gl_t230,gl_t231,gl_t232,gl_t233,gl_t234,gl_t235,gl_t236,gl_t237,gl_t238,gl_t239,
        gl_t240,gl_t241,gl_t242,gl_t243,gl_t244,gl_t245,gl_t246,gl_t247,gl_t248,gl_t249,
        gl_t250,gl_t251,gl_t252,gl_t253,gl_t254,gl_t255,gl_t256,gl_t257,gl_t258,gl_t259,
        gl_t260,gl_t261,gl_t262,gl_t263,gl_t264,gl_t265,gl_t266,gl_t267,gl_t268,gl_t269,
        gl_t270,gl_t271,gl_t272,gl_t273,gl_t274,gl_t275,gl_t276,gl_t277,gl_t278,gl_t279,
        gl_t280,gl_t281,gl_t282
    };
    for (unsigned i = 0; g_gl_names[i] && i < GL_COUNT && i < (sizeof t / sizeof t[0]); i++)
        hle_bind(g_gl_names[i], t[i]);
    hle_bind("glXGetProcAddressARB", h_glXGetProcAddressARB);
    hle_bind("glXGetProcAddress", h_glXGetProcAddress);

}

/* ---- GLX ----
 *
 * The Lindbergh binds GL through GLX: XOpenDisplay for the Display, then
 * glXChooseVisual -> glXCreateContext -> glXMakeCurrent, glXSwapBuffers to
 * present. On Orbis there is no X server, so every GLX object is a
 * guest-allocated handle the game can read - the real rendering goes through
 * the ps5-opengl/Zink backend, and the frame counter goes through host_swap.
 */

extern void *gmalloc(size_t, size_t);
extern int host_gl_context(void);
extern void host_swap(void);
extern void host_pump(void);
extern int host_window_open(const char *);

static uint32_t glx_alloc(unsigned bytes)
{
    void *p = gmalloc(bytes, 16);
    if (p) memset(p, 0, bytes);
    return (uint32_t)(uintptr_t)p;
}

static uint32_t g_glx_ctx;         /* the "current" GLXContext */
static uint32_t g_glx_drawable = 0x5000001;
static uint32_t g_glx_window_created;
static uint32_t g_glx_config[16];  /* a GLXFBConfig array */

static void h_glXChooseVisual(CPU *c)
{
    /* glXChooseVisual(display, screen, attrib_list) -> XVisualInfo* */
    uint32_t vi = glx_alloc(0x40);
    /* XVisualInfo: visual, visualid, screen, depth, class, red/green/blue
     * masks, colormap_size, bits_per_rgb. A 24-bit TrueColor visual. */
    wr32(vi + 0x00, 0x21);          /* visual */
    wr32(vi + 0x04, 0x21);          /* visualid */
    wr32(vi + 0x08, 0);             /* screen */
    wr32(vi + 0x0c, 24);            /* depth */
    wr32(vi + 0x10, 4);             /* class = TrueColor */
    wr32(vi + 0x14, 0xFF0000);      /* red_mask */
    wr32(vi + 0x18, 0x00FF00);      /* green_mask */
    wr32(vi + 0x1c, 0x0000FF);      /* blue_mask */
    wr32(vi + 0x20, 256);           /* colormap_size */
    wr32(vi + 0x24, 8);             /* bits_per_rgb */
    RET(vi);
}

static void h_glXCreateContext(CPU *c)
{
    if (!g_glx_ctx) g_glx_ctx = glx_alloc(0x100);
    RET(g_glx_ctx);
}
static void h_glXCreateNewContext(CPU *c) { h_glXCreateContext(c); }
static void h_glXCopyContext(CPU *c)   { (void)c; }
static void h_glXDestroyContext(CPU *c) { (void)c; }

/* The render thread proves itself by calling glXMakeCurrent/glXSwapBuffers —
 * claim the shared context for it.  If a worker thread (Sofdec uploader)
 * grabbed it first via gl_ensure_ctx, hand ownership back so the render
 * thread's calls aren't dropped. */
static void gl_claim_ctx(void)
{
    extern int orbis_egl_make_current(void);
    extern int host_gl_context(void);
    extern int host_window_open(const char *);
    if (!host_gl_context()) host_window_open("id4");
    s_ctx_owner = pthread_self();
    s_ctx_owned = 1;
    if (orbis_egl_make_current()) s_ctx_tls = 1;
}

static void h_glXMakeCurrent(CPU *c)
{
    /* glXMakeCurrent(display, drawable, ctx) - make the context current so
     * the gl* calls land on the real backend. Lazily open the window/GL the
     * first time. */
    if (!g_glx_window_created) { g_glx_window_created = 1; host_window_open("id4"); }
    g_glx_drawable = A32(1);
    g_glx_ctx      = A32(2);
    gl_claim_ctx();
    RET(1);
}
static void h_glXMakeContextCurrent(CPU *c) { h_glXMakeCurrent(c); }
static void h_glXIsDirect(CPU *c)       { (void)c; RET(1); }

static void h_glXSwapBuffers(CPU *c)
{
    (void)c;
    if (gl_trace_on()) gl_trace("glXSwapBuffers");
    host_swap();
    host_pump();
    RET(0);
}

static void h_glXGetCurrentContext(CPU *c)  { (void)c; RET(g_glx_ctx); }
static void h_glXGetCurrentDrawable(CPU *c) { (void)c; RET(g_glx_drawable); }
static void h_glXGetCurrentReadDrawable(CPU *c) { (void)c; RET(g_glx_drawable); }
static void h_glXGetCurrentDisplay(CPU *c)
{
    /* The framebuffer code reads Display fields off this - NULL faults. */
    extern uint32_t orbis_x_display(void);
    RET(orbis_x_display());
}

static void h_glXQueryExtension(CPU *c) { (void)c; RET(1); }
static void h_glXQueryVersion(CPU *c)
{
    if (A32(0)) wr32(A32(0), 1);
    if (A32(1)) wr32(A32(1), 4);
    RET(1);
}
static void h_glXQueryVersion_2(CPU *c)
{
    /* glXQueryVersion(display, *major, *minor) */
    if (A32(1)) wr32(A32(1), 1);
    if (A32(2)) wr32(A32(2), 4);
    RET(1);
}

static void h_glXGetFBConfigs(CPU *c)
{
    /* glXGetFBConfigs(display, screen, *nelem) -> GLXFBConfig* */
    uint32_t arr = glx_alloc(16 * 4);
    for (int i = 0; i < 16; i++) wr32(arr + i * 4, 0xA000000 + i);
    if (A32(2)) wr32(A32(2), 16);
    RET(arr);
}
static void h_glXChooseFBConfig(CPU *c) { h_glXGetFBConfigs(c); }

static void h_glXGetFBConfigAttrib(CPU *c)
{
    if (A32(3)) wr32(A32(3), 1);
    RET(0);
}
static void h_glXGetVisualFromFBConfig(CPU *c)
{
    /* GLXFBConfig -> XVisualInfo* - reuse the visual the game already read */
    uint32_t vi = glx_alloc(0x40);
    wr32(vi + 0x00, 0x21); wr32(vi + 0x04, 0x21); wr32(vi + 0x08, 0);
    wr32(vi + 0x0c, 24);   wr32(vi + 0x10, 4);
    wr32(vi + 0x14, 0xFF0000); wr32(vi + 0x18, 0x00FF00); wr32(vi + 0x1c, 0x0000FF);
    wr32(vi + 0x20, 256);  wr32(vi + 0x24, 8);
    RET(vi);
}

static void h_glXCreateWindow(CPU *c)    { (void)c; RET(g_glx_drawable); }
static void h_glXDestroyWindow(CPU *c)   { (void)c; }
static void h_glXCreatePixmap(CPU *c)    { (void)c; RET(0x6000001); }
static void h_glXDestroyPixmap(CPU *c)   { (void)c; }
static void h_glXCreatePbuffer(CPU *c)   { (void)c; RET(0x6000002); }
static void h_glXDestroyPbuffer(CPU *c)  { (void)c; }
static void h_glXCreateGLXPixmap(CPU *c) { (void)c; RET(0x6000003); }
static void h_glXDestroyGLXPixmap(CPU *c){ (void)c; }
static void h_glXQueryDrawable(CPU *c)
{
    /* GLX_WIDTH/HEIGHT queries get the window's size — same value the
     * VidMode entry reports, so every consumer agrees. */
    extern void lindbergh_window_size(int *, int *);
    int w, h;
    lindbergh_window_size(&w, &h);
    if (A32(3)) {
        /* GLX_WIDTH 0x22D3 / GLX_HEIGHT 0x22D4 — other attrs get 0. */
        wr32(A32(3), A32(2) == 0x22D3 ? (uint32_t)w :
                     A32(2) == 0x22D4 ? (uint32_t)h : 0u);
    }
    RET(0);
}
static void h_glXSelectEvent(CPU *c)     { (void)c; }
static void h_glXGetSelectedEvent(CPU *c){ (void)c; }
static void h_glXGetConfig(CPU *c)
{
    if (A32(3)) wr32(A32(3), 1);
    RET(0);
}
static void h_glXWaitGL(CPU *c)          { (void)c; }
static void h_glXWaitX(CPU *c)           { (void)c; }
static void h_glXQueryContext(CPU *c)
{
    if (A32(3)) wr32(A32(3), 1);
    RET(0);
}
/* glh_helper_glx::init_glx_extensions parses the GLX extension string and
 * calls init_extension() per name -> glXGetProcAddressARB -> g_glh's func
 * table.  An empty string leaves g_glh's members NULL -> createTextureFromMem
 * derefs NULL+0x30.  Report the standard GLX extension set so the table fills.
 * name: 1=VENDOR 2=VERSION 3=EXTENSIONS. */
static const char k_glx_exts[] =
    "GLX_ARB_create_context GLX_ARB_create_context_profile"
    " GLX_ARB_fbconfig_float GLX_ARB_get_proc_address GLX_ARB_multisample"
    " GLX_EXT_import_context GLX_EXT_texture_from_pixmap GLX_EXT_visual_info"
    " GLX_EXT_visual_rating GLX_MESA_copy_sub_buffer GLX_OML_swap_method"
    " GLX_SGIS_multisample GLX_SGIX_fbconfig GLX_SGIX_pbuffer"
    " GLX_SGIX_visual_select_group GLX_SGI_make_current_read"
    " GLX_SGI_swap_control GLX_SGI_video_sync";
static void h_glXGetClientString(CPU *c)
{
    extern uint32_t cg_gstr(const char *);
    int name = AI32(1);
    RET(cg_gstr(name == 1 ? "Cognition Orbis" :
                name == 2 ? "1.4" : k_glx_exts));
}
static void h_glXQueryServerString(CPU *c)
{
    extern uint32_t cg_gstr(const char *);
    int name = AI32(2);
    RET(cg_gstr(name == 1 ? "Cognition Orbis" :
                name == 2 ? "1.4" : k_glx_exts));
}
static void h_glXQueryExtensionsString(CPU *c)
{   extern uint32_t cg_gstr(const char *); RET(cg_gstr(k_glx_exts)); }

/* SGIX / SGI extensions the game imports directly */
static void h_glXChooseFBConfigSGIX(CPU *c)        { h_glXGetFBConfigs(c); }
static void h_glXCreateContextWithConfigSGIX(CPU *c){ h_glXCreateContext(c); }
static void h_glXCreateGLXPbufferSGIX(CPU *c)      { (void)c; RET(0x6000010); }
static void h_glXDestroyGLXPbufferSGIX(CPU *c)     { (void)c; }
static void h_glXGetFBConfigAttribSGIX(CPU *c)      { h_glXGetFBConfigAttrib(c); }
static void h_glXSwapIntervalSGI(CPU *c)            { (void)c; RET(0); }
static void h_glXGetVideoSyncSGI(CPU *c)           { static unsigned int fc; uint32_t p=A32(0); if(p)wr32(p,fc++); RET(0); }
static void h_glXGetRefreshRateSGI(CPU *c)         { uint32_t p=A32(0); if(p)wr32(p,60); RET(0); }
/* NV fence objects — fake incrementing handles, always-ready */
static uint32_t s_nv_fence = 1;
static void h_glGenFencesNV(CPU *c)    { int n=AI32(0); uint32_t f=A32(1); while(n-->0){wr32(f,s_nv_fence++);f+=4;} }
static void h_glDeleteFencesNV(CPU *c) { (void)c; }
static void h_glFinishFenceNV(CPU *c)  { (void)c; }
static void h_glSetFenceNV(CPU *c)     { (void)c; }
static void h_glTestFenceNV(CPU *c)    { (void)c; RET(1); }

void hle_register_glx(void)
{
    hle_bind("glXChooseVisual", h_glXChooseVisual);
    hle_bind("glXCreateContext", h_glXCreateContext);
    hle_bind("glXCreateNewContext", h_glXCreateNewContext);
    hle_bind("glXCopyContext", h_glXCopyContext);
    hle_bind("glXDestroyContext", h_glXDestroyContext);
    hle_bind("glXMakeCurrent", h_glXMakeCurrent);
    hle_bind("glXMakeContextCurrent", h_glXMakeContextCurrent);
    hle_bind("glXIsDirect", h_glXIsDirect);
    hle_bind("glXSwapBuffers", h_glXSwapBuffers);
    hle_bind("glXGetCurrentContext", h_glXGetCurrentContext);
    hle_bind("glXGetCurrentDrawable", h_glXGetCurrentDrawable);
    hle_bind("glXGetCurrentReadDrawable", h_glXGetCurrentReadDrawable);
    hle_bind("glXGetCurrentDisplay", h_glXGetCurrentDisplay);
    hle_bind("glXQueryExtension", h_glXQueryExtension);
    hle_bind("glXQueryVersion", h_glXQueryVersion_2);
    hle_bind("glXGetFBConfigs", h_glXGetFBConfigs);
    hle_bind("glXChooseFBConfig", h_glXChooseFBConfig);
    hle_bind("glXGetFBConfigAttrib", h_glXGetFBConfigAttrib);
    hle_bind("glXGetVisualFromFBConfig", h_glXGetVisualFromFBConfig);
    hle_bind("glXCreateWindow", h_glXCreateWindow);
    hle_bind("glXDestroyWindow", h_glXDestroyWindow);
    hle_bind("glXCreatePixmap", h_glXCreatePixmap);
    hle_bind("glXDestroyPixmap", h_glXDestroyPixmap);
    hle_bind("glXCreatePbuffer", h_glXCreatePbuffer);
    hle_bind("glXDestroyPbuffer", h_glXDestroyPbuffer);
    hle_bind("glXCreateGLXPixmap", h_glXCreateGLXPixmap);
    hle_bind("glXDestroyGLXPixmap", h_glXDestroyGLXPixmap);
    hle_bind("glXQueryDrawable", h_glXQueryDrawable);
    hle_bind("glXSelectEvent", h_glXSelectEvent);
    hle_bind("glXGetSelectedEvent", h_glXGetSelectedEvent);
    hle_bind("glXGetConfig", h_glXGetConfig);
    hle_bind("glXWaitGL", h_glXWaitGL);
    hle_bind("glXWaitX", h_glXWaitX);
    hle_bind("glXQueryContext", h_glXQueryContext);
    hle_bind("glXGetClientString", h_glXGetClientString);
    hle_bind("glXQueryServerString", h_glXQueryServerString);
    hle_bind("glXQueryExtensionsString", h_glXQueryExtensionsString);
    hle_bind("glXChooseFBConfigSGIX", h_glXChooseFBConfigSGIX);
    hle_bind("glXCreateContextWithConfigSGIX", h_glXCreateContextWithConfigSGIX);
    hle_bind("glXCreateGLXPbufferSGIX", h_glXCreateGLXPbufferSGIX);
    hle_bind("glXDestroyGLXPbufferSGIX", h_glXDestroyGLXPbufferSGIX);
    hle_bind("glXGetFBConfigAttribSGIX", h_glXGetFBConfigAttribSGIX);
    hle_bind("glXSwapIntervalSGI", h_glXSwapIntervalSGI);
    hle_bind("glXGetVideoSyncSGI", h_glXGetVideoSyncSGI);
    hle_bind("glXGetRefreshRateSGI", h_glXGetRefreshRateSGI);
    hle_bind("glGenFencesNV", h_glGenFencesNV);
    hle_bind("glDeleteFencesNV", h_glDeleteFencesNV);
    hle_bind("glFinishFenceNV", h_glFinishFenceNV);
    hle_bind("glSetFenceNV", h_glSetFenceNV);
    hle_bind("glTestFenceNV", h_glTestFenceNV);
}
