/*
 * hle_cg.c - NVIDIA Cg, answered from the disc.
 *
 * The game compiles its shaders through Cg at startup: hand cgCreateProgram
 * the Cg source, ask cgGetProgramString for the result, feed that to
 * glProgramStringARB. Implementing that honestly would mean shipping a Cg
 * compiler.
 *
 * It is not necessary, because the answers are already on the disc. Every
 * shader is there twice - shader/Cg/vs/vs.cg next to shader/Cg/vs/vs.asm_gl,
 * the second being the first as cgc 1.4 compiled it for profile vp40 in 2005.
 * So this matches the source it is given against the .cg files it can find and
 * returns the .asm_gl beside it: the same text the real Cg runtime would have
 * produced, produced by the same compiler, just earlier.
 *
 * If a source does not match anything, that is reported rather than papered
 * over - a silently empty shader draws nothing and says nothing.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(LINDBERGH_STALE_WIN32_CG)
#include <windows.h>
#endif

#include "lindbergh_rt.h"

#define CG_TOKEN_CTX  0x43470001u      /* "CG" */
/* The lbg index alone fills several hundred slots and every no-match source
 * consumes another — the game compiles past 500 programs during init, so a
 * 512-entry table silently fails late shaders (dof among them). */
#define MAX_SHADERS   2048

typedef struct {
    char *path;         /* which file this came from, for diagnosis */
    char *body;         /* the .cg text, comments and whitespace stripped */
    char *defines;      /* the #define block the .asm_gl records, stripped */
    char *compiled;     /* the .asm_gl itself */
    uint32_t prof;      /* cg profile the program was created for, 0 = n/a */
    uint32_t gstr;      /* cached cgGetProgramString result (guest ptr, never freed) */
} Shader;

/* Strip comments and all whitespace. Two texts that differ only in formatting
 * are the same shader, and the engine hands over a PREPROCESSED source - its
 * includes already pasted in and its comments likely gone - so a byte compare
 * against the file on disc never matches. Reducing both to bare code makes the
 * file's own body findable inside the preprocessed whole. */
static char *strip_code(const char *src, size_t len)
{
    if (len == 0) len = strlen(src);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i++;
            continue;
        }
        if ((unsigned char)src[i] <= ' ') continue;
        out[o++] = src[i];
    }
    out[o] = 0;
    return out;
}

static Shader g_sh[MAX_SHADERS];
static int    g_sh_n;
static int    g_indexed;
static int    g_match_body;      /* matched on shader body AND defines */
static int    g_match_defines;   /* matched on a defines set unique to one shader */

static char *read_all(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = (char *)malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) == (size_t)n) p[n] = 0;
    else { free(p); p = NULL; }
    fclose(f);
    if (p && len_out) *len_out = (size_t)n;
    return p;
}

#if defined(LINDBERGH_STALE_WIN32_CG)
/* Walk a directory tree for *.cg, and for each one remember the .asm_gl that
 * sits beside it. Depth-limited because the tree is shallow by construction
 * (shader/Cg/{vs,ps,inc}) and a runaway walk during startup is worse than a
 * missed shader. */
/* A shader's own code: everything after its last #include.
 *
 * Includes expand in place, so the preprocessed source the engine hands over
 * is [expanded includes][this file's own code]. That tail is what identifies
 * the shader - the part before it is shared by all of them, which is why
 * matching on the whole file found the wrong program nearly every time.
 *
 * Returns a pointer into src; strip_code is called on it with len 0 meaning
 * "to the terminator". */
static const char *own_code(const char *src, size_t len)
{
    const char *last = src;
    for (const char *p = src; p && (size_t)(p - src) < len; ) {
        const char *inc = strstr(p, "#include");
        if (!inc || (size_t)(inc - src) >= len) break;
        const char *nl = strchr(inc, '\n');
        if (!nl) break;
        last = nl + 1;
        p = last;
    }
    return last;
}

/* cgc stamps its invocation into the output:
 *
 *   # command line args: -q -profile vp40 -entry main #define MODE_GL (1) ...
 *
 * Everything from the first #define on is the variant key - the same block the
 * engine passes to cgCreateProgram as its args. Stripped of whitespace so the
 * two spellings compare equal. */
static char *asm_defines(const char *compiled)
{
    const char *line = strstr(compiled, "command line args:");
    if (!line) return strip_code("", 0);
    const char *hash = strstr(line, "#define");
    if (!hash) return strip_code("", 0);

    /* The block runs to the end of the comment run - every following line that
     * still begins with a '#' or whitespace before one. */
    const char *end = hash;
    while (*end) {
        const char *nl = strchr(end, '\n');
        if (!nl) { end += strlen(end); break; }
        const char *peek = nl + 1;
        while (*peek == ' ' || *peek == '	') peek++;
        if (*peek != '#') { end = nl; break; }
        end = nl + 1;
    }
    return strip_code(hash, (size_t)(end - hash));
}

/* The arguments the engine passes, reduced the same way. */
static char *args_defines(CPU *c, uint32_t argv)
{
    char buf[4096];
    size_t n = 0;
    for (int k = 0; k < 32 && argv; k++) {
        uint32_t p = rd32(argv + 4u * (uint32_t)k);
        if (!p) break;
        const char *a = (const char *)(uintptr_t)p;
        size_t l = strlen(a);
        if (n + l >= sizeof buf) break;
        memcpy(buf + n, a, l);
        n += l;
    }
    buf[n] = 0;
    return strip_code(buf, n);
}

static void index_dir(const char *dir, int depth)
{
    if (depth > 4 || g_sh_n >= MAX_SHADERS) return;

    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s/*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (depth == 0) fprintf(stderr, "[cg] cannot open %s\n", dir);
        return;
    }

    do {
        if (fd.cFileName[0] == '.') continue;

        char full[MAX_PATH];
        snprintf(full, sizeof full, "%s/%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            index_dir(full, depth + 1);
            continue;
        }
        size_t n = strlen(fd.cFileName);
        if (n < 4 || strcmp(fd.cFileName + n - 3, ".cg") != 0) continue;

        char asm_path[MAX_PATH];
        snprintf(asm_path, sizeof asm_path, "%.*s.asm_gl",
                 (int)(strlen(full) - 3), full);

        size_t slen = 0, clen = 0;
        char *src = read_all(full, &slen);
        char *cmp = read_all(asm_path, &clen);
        if (src && cmp && g_sh_n < MAX_SHADERS) {
            g_sh[g_sh_n].path     = _strdup(full);
            g_sh[g_sh_n].body     = strip_code(own_code(src, slen), 0);
            g_sh[g_sh_n].defines  = asm_defines(cmp);
            g_sh[g_sh_n].compiled = cmp;
            g_sh_n++;
            free(src);                 /* only the stripped body is kept */
        } else {
            free(src);
            free(cmp);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

static void index_shaders(void)
{
    if (g_indexed) return;
    g_indexed = 1;

    const char *tea = getenv("TEA_DIR");
    if (!tea || !*tea) {
        fprintf(stderr, "[cg] TEA_DIR is unset, so there is nowhere to look for\n"
                        "     precompiled shaders. Set it to the game directory.\n");
        return;
    }
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s/shader", tea);
    index_dir(dir, 0);
    snprintf(dir, sizeof dir, "%s/extraShader", tea);
    index_dir(dir, 0);
    fprintf(stderr, "[cg] indexed %d precompiled shaders\n", g_sh_n);
    {
        /* How discriminating is the defines key actually? If most shaders
         * share one value it is not a key at all, and the match degenerates to
         * "first body that appears" - which for shaders sharing includes picks
         * the wrong program and transforms every vertex off screen. */
        int uniq = 0;
        for (int i = 0; i < g_sh_n; i++) {
            int seen = 0;
            for (int j = 0; j < i; j++)
                if (g_sh[j].defines && g_sh[i].defines &&
                    strcmp(g_sh[i].defines, g_sh[j].defines) == 0) { seen = 1; break; }
            if (!seen) uniq++;
        }
        fprintf(stderr, "[cg] %d distinct define sets across %d shaders\n", uniq, g_sh_n);
        for (int i = 0; i < 2 && i < g_sh_n; i++)
            fprintf(stderr, "[cg]   defines[%d] = %.90s\n", i,
                    g_sh[i].defines ? g_sh[i].defines : "(none)");
    }
}

#else
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/* POSIX/Orbis: the same answers live on the disc, in two shapes:
 *
 *  shader/Cg/**          : *.cg source beside its cgc-compiled *.asm_gl
 *  data/Shader/          : <variant>_inc.fx define blocks, car_v.fx / car_p.fx
 *                          shared sources, and v<variant>.asm_lbg /
 *                          p<variant>.asm_lbg precompiled output.
 *
 * For the data/Shader pipeline the game prepends the variant's _inc.fx to the
 * shared source before calling cgCreateProgram - so the source buffer's
 * stripped prefix IS the _inc file's stripped text, which names the variant,
 * and "v"/"p" + variant names the compiled file to hand back. */

static const char *own_code(const char *src, size_t len)
{
    const char *last = src;
    for (const char *p = src; p && (size_t)(p - src) < len; ) {
        const char *inc = strstr(p, "#include");
        if (!inc || (size_t)(inc - src) >= len) break;
        const char *nl = strchr(inc, '\n');
        if (!nl) break;
        last = nl + 1;
        p = last;
    }
    return last;
}

static char *asm_defines(const char *compiled)
{
    const char *line = strstr(compiled, "command line args:");
    if (!line) return strip_code("", 0);
    const char *hash = strstr(line, "#define");
    if (!hash) return strip_code("", 0);
    const char *end = hash;
    while (*end) {
        const char *nl = strchr(end, '\n');
        if (!nl) { end += strlen(end); break; }
        const char *peek = nl + 1;
        while (*peek == ' ' || *peek == '\t') peek++;
        if (*peek != '#') { end = nl; break; }
        end = nl + 1;
    }
    return strip_code(hash, (size_t)(end - hash));
}

static char *args_defines(CPU *c, uint32_t argv)
{
    char buf[4096];
    size_t n = 0;
    for (int k = 0; k < 32 && argv; k++) {
        uint32_t p = rd32(argv + 4u * (uint32_t)k);
        if (!p) break;
        const char *a = (const char *)(uintptr_t)p;
        size_t l = strlen(a);
        if (n + l >= sizeof buf) break;
        memcpy(buf + n, a, l);
        n += l;
    }
    buf[n] = 0;
    return strip_code(buf, n);
}

#define MAX_INC  512
#define MAX_LBG  1024
static struct { char stem[128]; char *defs; }        g_inc[MAX_INC];
static int   g_inc_n;
static struct { char stem[132]; char *compiled; }    g_lbg[MAX_LBG];
static int   g_lbg_n;

/* The game fopen()s each variant's _inc.fx right before compiling; h_fopen
 * feeds it here so the match below can name the variant without a directory
 * listing (opendir is EPERM'd inside the managed sandbox). */
void cg_note_inc(const char *stem, const char *text, size_t len)
{
    extern void klogf(const char*, ...);
    for (int i = 0; i < g_inc_n; i++)
        if (strcmp(g_inc[i].stem, stem) == 0) return;
    if (g_inc_n >= MAX_INC) return;
    snprintf(g_inc[g_inc_n].stem, sizeof g_inc[0].stem, "%s", stem);
    g_inc[g_inc_n].defs = strip_code(text, len);
    klogf("[cg] inc variant %s (%zu defs bytes)\n", stem, len);
    g_inc_n++;
}

static const char *lbg_lookup_fx(const char *stem, int vertex);
static const char *lbg_try_names(const char *const *names, int n);

/* Index a .fx body the game opened — combiner/post-process shaders carry no
 * _inc template, but the game prepends generated defines to the verbatim .fx
 * body, so the stripped tail still equals the file. Resolve its compiled
 * .asm_lbg by probing candidate names (combiner_p -> pSpecularCombiner etc). */
void cg_note_fx(const char *name, const char *text, size_t len)
{
    extern void klogf(const char*, ...);
    if (g_sh_n >= MAX_SHADERS || !name || !text || !len) return;
    const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
    char stem[128]; snprintf(stem, sizeof stem, "%s", b);
    size_t sl = strlen(stem);
    if (sl < 4 || strcmp(stem + sl - 3, ".fx") != 0) return;
    stem[sl - 3] = 0; sl -= 3;
    for (int i = 0; i < g_sh_n; i++)
        if (g_sh[i].path && strcmp(g_sh[i].path, stem) == 0) return;
    /* combiner_p/combiner_v carry the stage as a suffix — strip it */
    int vertex = sl >= 2 && stem[sl - 1] == 'v' && stem[sl - 2] == '_';
    char core[128]; snprintf(core, sizeof core, "%s", stem);
    size_t cl = strlen(core);
    if (cl >= 2 && core[cl - 2] == '_' && (core[cl - 1] == 'p' || core[cl - 1] == 'v'))
        core[cl - 2] = 0;
    /* .asm_lbg names: mostly {v,p} + Capitalised core (blur→pBlur), but
     * dof→pDOF, combiner→pSpecularCombiner, morphImage→morphImage_{ps,vs},
     * occlusionCuller stays verbatim. Probe the whole family. */
    char cap[128]; snprintf(cap, sizeof cap, "%s", core);
    if (cap[0]) cap[0] = (char)toupper((unsigned char)cap[0]);
    char up[128]; snprintf(up, sizeof up, "%s", core);
    for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
    char n1[160], n2[160], n3[160], n4[160], n5[160], n6[160], n7[160], n8[160];
    snprintf(n1, sizeof n1, "%c%s",   "vp"[vertex], cap);         /* pBlur */
    snprintf(n2, sizeof n2, "%c%s",   "vp"[vertex], up);          /* pDOF  */
    snprintf(n3, sizeof n3, "%cSpecular%s", "vp"[vertex], cap);   /* pSpecularCombiner */
    snprintf(n4, sizeof n4, "%s_%cs", core, "vp"[vertex]);        /* morphImage_ps */
    snprintf(n5, sizeof n5, "%s_%c",  core, "vp"[vertex]);        /* occlusionCuller_p */
    snprintf(n6, sizeof n6, "%c%s",   "vp"[vertex], core);        /* pblur */
    snprintf(n7, sizeof n7, "%s",     core);                      /* core */
    snprintf(n8, sizeof n8, "%s",     cap);                       /* Blur */
    const char *names[] = { n1, n2, n3, n4, n5, n6, n7, n8 };
    const char *cmp = lbg_try_names(names, 8);
    if (!cmp) cmp = lbg_lookup_fx(core, vertex);
    g_sh[g_sh_n].path     = strdup(stem);
    g_sh[g_sh_n].body     = strip_code(text, len);
    g_sh[g_sh_n].defines  = NULL;
    g_sh[g_sh_n].compiled = (char *)cmp;
    g_sh_n++;
    klogf("[cg] fx variant %s vertex=%d -> %s\n", stem, vertex, cmp ? "hit" : "miss");
}

/* Collapse a stripped define block to its `#define NAME` sequence — drops
 * the `(value)` groups so two permutations of the same variant compare equal
 * even when their enable flags differ. Stops at the first non-#define token. */
static char *defseq(const char *s)
{
    size_t cap = strlen(s) + 1, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    const char *p = s;
    while (strncmp(p, "#define", 7) == 0) {
        memcpy(out + n, "#define", 7); n += 7; p += 7;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) out[n++] = *p++;
        if (*p == '(') { int d = 1; p++; while (*p && d) { if (*p == '(') d++; if (*p == ')') d--; p++; } }
        else while (*p && !(p[0] == '#' && strncmp(p, "#define", 7) == 0) &&
                    !isspace((unsigned char)*p)) p++;
    }
    out[n] = 0;
    return out;
}

/* 'v' = vertex program text, 'f' = fragment, '?' = unrecognised. The profile
 * argument to cgCreateProgram says which GL target the game will upload the
 * result to; a "match" that hands back the other kind is guaranteed to be
 * rejected by the driver with "invalid program header". */
static char prog_kind(const char *t)
{
    if (!t) return '?';
    if (!strncmp(t, "!!ARBvp", 7) || !strncmp(t, "!!NVvp", 6) ||
        strstr(t, "vertex_program")) return 'v';
    if (!strncmp(t, "!!ARBfp", 7) || !strncmp(t, "!!NVfp", 6) ||
        strstr(t, "fragment_program")) return 'f';
    return '?';
}

static const char *lbg_lookup(const char *stem, int vertex)
{
    /* find /app0/data/Shader/{v,p}<stem>.asm_lbg by name - opendir is dead
     * but fopen on a known name works fine. */
    static char path[512];
    const char *tea = getenv("TEA_DIR");
    if (!tea || !*tea) { extern const char *app0_root(void); tea = app0_root(); }
    const char *body0 = (stem[0] == 'p' || stem[0] == 'v') ? stem + 1 : stem;
    for (int i = 0; i < g_lbg_n; i++)
        if (strcmp(g_lbg[i].stem + 1, body0) == 0 &&
            g_lbg[i].stem[0] == (vertex ? 'v' : 'p'))
            return g_lbg[i].compiled;
    /* The _inc stem carries the fragment prefix already — padd_inc.fx →
     * padd.asm_lbg (fragment) and vadd.asm_lbg (vertex). Build the name as
     * {v,p} + stem-without-its-leading-p, not {v,p} + stem (which produced
     * vpadd / ppadd and never matched). */
    const char *body = (stem[0] == 'p' || stem[0] == 'v') ? stem + 1 : stem;
    for (int pass = 0; pass < 2; pass++) {
        /* Hand-translated ARBfp1.0/ARBvp1.0 replacements live in
         * data/Shader_fp1 — prefer them; anything missing falls through to
         * the cgc NV_fp2/vp3 original, which the upload path still rewrites
         * for Mesa. */
        snprintf(path, sizeof path, "%s/data/Shader_fp1/%c%s.asm_lbg",
                 tea, "vp"[pass == 0 ? vertex : !vertex], body);
        FILE *f = fopen(path, "rb");
        if (!f) {
            snprintf(path, sizeof path, "%s/data/Shader/%c%s.asm_lbg",
                     tea, "vp"[pass == 0 ? vertex : !vertex], body);
            f = fopen(path, "rb");
        }
        if (!f) continue;
        if (pass == 1) {
            extern void klogf(const char*, ...);
            klogf("[cg] lbg cross-fallback: no %c%s.asm_lbg, serving %c variant\n",
                  "vp"[vertex], body, "vp"[!vertex]);
        }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *t = malloc((size_t)sz + 1);
        int ok = t && fread(t, 1, (size_t)sz, f) == (size_t)sz;
        fclose(f);
        if (!ok) { free(t); continue; }
        t[sz] = 0;
        if (g_lbg_n < MAX_LBG) {
            const char *b = strrchr(path, '/');
            snprintf(g_lbg[g_lbg_n].stem, sizeof g_lbg[0].stem, "%.*s",
                     (int)(strlen(b ? b + 1 : path) - 8), b ? b + 1 : path);
            g_lbg[g_lbg_n].compiled = t;
            g_lbg_n++;
        }
        return t;
    }
    return NULL;
}

/* Resolve an .fx stem to a compiled program in the g_lbg index. The .fx
 * names and the .asm_lbg names do not line up exactly — blur→Blur,
 * dof→DOF, combiner→SpecularCombiner — so try exact, case-insensitive,
 * then a case-insensitive suffix (the asm name carries a prefix the .fx
 * does not). */
static const char *lbg_lookup_fx(const char *stem, int vertex)
{
    char want = vertex ? 'v' : 'p';
    size_t sl = strlen(stem);
    for (int i = 0; i < g_lbg_n; i++)
        if (g_lbg[i].stem[0] == want && strcmp(g_lbg[i].stem + 1, stem) == 0)
            return g_lbg[i].compiled;
    for (int i = 0; i < g_lbg_n; i++)
        if (g_lbg[i].stem[0] == want && strcasecmp(g_lbg[i].stem + 1, stem) == 0)
            return g_lbg[i].compiled;
    for (int i = 0; i < g_lbg_n; i++) {
        const char *g = g_lbg[i].stem + 1;
        size_t gl = strlen(g);
        if (g_lbg[i].stem[0] == want && gl > sl &&
            strcasecmp(g + gl - sl, stem) == 0)
            return g_lbg[i].compiled;
    }
    return NULL;
}

/* fopen-based probe for a set of candidate .asm_lbg names — fills g_lbg and
 * returns the first hit's compiled text. */
static const char *lbg_try_names(const char *const *names, int n)
{
    static char path[512];
    const char *tea = getenv("TEA_DIR");
    if (!tea || !*tea) { extern const char *app0_root(void); tea = app0_root(); }
    for (int i = 0; i < n; i++) {
        const char *nm = names[i];
        if (!nm || !*nm) continue;
        for (int j = 0; j < g_lbg_n; j++)
            if (strcmp(g_lbg[j].stem + 1, nm) == 0) return g_lbg[j].compiled;
        snprintf(path, sizeof path, "%s/data/Shader_fp1/%s.asm_lbg", tea, nm);
        FILE *f = fopen(path, "rb");
        if (!f) {
            snprintf(path, sizeof path, "%s/data/Shader/%s.asm_lbg", tea, nm);
            f = fopen(path, "rb");
        }
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *t = malloc((size_t)sz + 1);
        int ok = t && fread(t, 1, (size_t)sz, f) == (size_t)sz;
        fclose(f);
        if (!ok) { free(t); continue; }
        t[sz] = 0;
        if (g_lbg_n < MAX_LBG) {
            const char *bb = strrchr(path, '/');
            snprintf(g_lbg[g_lbg_n].stem, sizeof g_lbg[0].stem, "%.*s",
                     (int)(strlen(bb ? bb + 1 : path) - 8), bb ? bb + 1 : path);
            g_lbg[g_lbg_n].compiled = t;
            g_lbg_n++;
        }
        return t;
    }
    return NULL;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static void index_dir_p(const char *dir, int depth)
{
    if (depth > 6) return;
    extern void klogf(const char*, ...);
    DIR *d = opendir(dir);
    if (!d) { klogf("[cg] opendir %s fail errno=%d\n", dir, errno); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (is_dir(full)) { index_dir_p(full, depth + 1); continue; }

        size_t n = strlen(e->d_name);
        if (n > 7 && strcmp(e->d_name + n - 7, "_inc.fx") == 0) {
            /* variant define block: stem = name minus "_inc.fx" */
            if (g_inc_n < MAX_INC) {
                size_t l = 0;
                char *t = read_all(full, &l);
                if (t) {
                    snprintf(g_inc[g_inc_n].stem, sizeof g_inc[0].stem,
                             "%.*s", (int)(n - 7), e->d_name);
                    g_inc[g_inc_n].defs = strip_code(t, l);
                    free(t);
                    g_inc_n++;
                }
            }
            continue;
        }
        if (n > 8 && strcmp(e->d_name + n - 8, ".asm_lbg") == 0) {
            /* first index of a stem wins — data/Shader_fp1 is indexed
             * before data/Shader so a hand-translated file supersedes its
             * cgc original everywhere lbg_lookup* is consulted. */
            char st0[128];
            snprintf(st0, sizeof st0, "%.*s", (int)(n - 8), e->d_name);
            int dup = 0;
            for (int i = 0; i < g_lbg_n; i++)
                if (strcmp(g_lbg[i].stem, st0) == 0) { dup = 1; break; }
            if (!dup && g_lbg_n < MAX_LBG) {
                char *t = read_all(full, NULL);
                if (t) {
                    snprintf(g_lbg[g_lbg_n].stem, sizeof g_lbg[0].stem,
                             "%.*s", (int)(n - 8), e->d_name);
                    g_lbg[g_lbg_n].compiled = t;
                    g_lbg_n++;
                }
            }
            continue;
        }
        /* Non-inc .fx shaders — combiner/post-process programs with no inc
         * template. The game prepends a generated define block to the .fx
         * body, so index the body for tail-matching and resolve its compiled
         * .asm_lbg by stem. Gated: real post-process shaders sampling
         * unready textures panic the GPU — opt in with LINDBERGH_FX_SHADERS. */
        if (getenv("LINDBERGH_FX_SHADERS") &&
            n >= 6 && strcmp(e->d_name + n - 3, ".fx") == 0) {
            int vertex = e->d_name[n - 4] == 'v';
            char stem[128];
            snprintf(stem, sizeof stem, "%.*s", (int)(n - 5), e->d_name);
            size_t slen = 0;
            char *src = read_all(full, &slen);
            const char *cmp = src ? lbg_lookup_fx(stem, vertex) : NULL;
            if (src && cmp && g_sh_n < MAX_SHADERS) {
                g_sh[g_sh_n].path     = strdup(e->d_name);
                g_sh[g_sh_n].body     = strip_code(src, slen);
                g_sh[g_sh_n].defines  = NULL;
                g_sh[g_sh_n].compiled = (char *)cmp;
                g_sh_n++;
            } else free(src);
            continue;
        }
        if (n >= 3 && strcmp(e->d_name + n - 3, ".cg") == 0) {
            char asm_path[1024];
            snprintf(asm_path, sizeof asm_path, "%.*s.asm_gl",
                     (int)(strlen(full) - 3), full);
            size_t slen = 0;
            char *src = read_all(full, &slen);
            char *cmp = read_all(asm_path, NULL);
            if (src && cmp && g_sh_n < MAX_SHADERS) {
                g_sh[g_sh_n].path     = strdup(full);
                g_sh[g_sh_n].body     = strip_code(own_code(src, slen), 0);
                g_sh[g_sh_n].defines  = asm_defines(cmp);
                g_sh[g_sh_n].compiled = cmp;
                g_sh_n++;
            } else { free(src); free(cmp); }
            continue;
        }
    }
    closedir(d);
}

static void index_shaders(void)
{
    if (g_indexed) return;
    g_indexed = 1;
    const char *tea = getenv("TEA_DIR");
    if (!tea || !*tea) { extern const char *app0_root(void); tea = app0_root(); }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/shader", tea);     index_dir_p(dir, 0);
    snprintf(dir, sizeof dir, "%s/extraShader", tea); index_dir_p(dir, 0);
    snprintf(dir, sizeof dir, "%s/data/Shader_fp1", tea); index_dir_p(dir, 0);
    snprintf(dir, sizeof dir, "%s/data/Shader", tea); index_dir_p(dir, 0);
    { extern void klogf(const char*, ...);
      klogf("[cg] indexed %d cg shaders, %d inc variants, %d lbg\n",
            g_sh_n, g_inc_n, g_lbg_n); }
}
#endif

static void h_cgCreateContext(CPU *c)  { index_shaders(); RET(CG_TOKEN_CTX); }
static void h_cgIsContext(CPU *c)      { RET(A32(0) == CG_TOKEN_CTX); }
static void h_cgDestroyContext(CPU *c)
{
    (void)c;
    fprintf(stderr, "[cg] matches: %d by body+defines, %d by defines alone\n",
            g_match_body, g_match_defines);
}
static void h_cgSetErrorCallback(CPU *c) { (void)c; }
/* Compare two program texts ignoring '#' comment lines, whitespace, and the
 * engine's program.local -> program.env rewrite. */
static int prog_norm_eq(const char *a, const char *b)
{
    /* token-compare: skip ws and comment lines; map "local"->"env" */
    for (;;) {
        while (*a==' '||*a=='\t'||*a=='\r'||*a=='\n') a++;
        while (*b==' '||*b=='\t'||*b=='\r'||*b=='\n') b++;
        if (*a=='#') { while(*a&&*a!='\n')a++; continue; }
        if (*b=='#') { while(*b&&*b!='\n')b++; continue; }
        if (!*a || !*b) return *a==*b;
        if (!strncmp(a,"local",5) && (a[5]=='['||a[5]==' ')) {
            if (strncmp(b,"env",3)) return 0;
            a+=5; b+=3; continue;
        }
        if (!strncmp(b,"local",5) && (b[5]=='['||b[5]==' ')) {
            if (strncmp(a,"env",3)) return 0;
            b+=5; a+=3; continue;
        }
        if (*a!=*b) return 0;
        a++; b++;
    }
}
static int prog_same_text_norm(const char *a, const char *b)
{
    /* a is host text (compiled), b is guest text - same bytes either way */
    return prog_norm_eq(a, b);
}

/* Which shader table entry owns a cgGetProgramString result pointer - used by
 * the GL upload path to name a program the driver rejected. */
const char *cg_gstr_owner(uint32_t gstr)
{
    /* The engine rewrites program.local -> program.env and strips cgc's
     * comment lines before uploading, so compare on instruction text with
     * those normalisations applied. Only called on the rare cross-kind
     * upload, so a linear scan is fine. */
    static char buf[160];
    for (int i = 0; i < g_sh_n; i++) {
        if (!g_sh[i].compiled) continue;
        if (prog_same_text_norm(g_sh[i].compiled,
                                (const char *)(uintptr_t)gstr)) {
            snprintf(buf, sizeof buf, "g_sh[%d] %s prof=%#x", i,
                     g_sh[i].path ? g_sh[i].path : "?", g_sh[i].prof);
            return buf;
        }
    }
    return NULL;
}

uint32_t cg_gstr(const char *s)
{
    extern void *gmalloc(size_t, size_t);
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)gmalloc(n, 16);
    if (p) memcpy(p, s, n);
    return (uint32_t)(uintptr_t)p;
}

/* ---- the real Cg compiler, for what the disc does not have ----
 *
 * 45 of Initial D 4's shaders - the whole post-process chain (combiner,
 * glare, DOF, blur, downsample, gamma) and some shadow-map permutations - were
 * compiled live on the cabinet and have no .asm_gl/.asm_lbg on disc. Without a
 * compiler they became a textured pass-through and the post chain blacked out
 * every cutscene. NVIDIA's own Cg runtime still runs on Windows, and this is a
 * 32-bit process, so its 32-bit cg.dll can compile them exactly as the board
 * did: same source, same profile (fp40/vp40), same entry point.
 *
 * Loaded on first need, never required: LINDBERGH_CG_DLL names the DLL, else
 * the toolkit's CG_BIN_PATH, else the default install folder, else PATH.
 * LINDBERGH_CG_REAL=0 turns it off entirely. By default it compiles EVERY
 * shader (see real_cg_all); LINDBERGH_CG_ALL=0 uses it only for shaders the
 * disc has no precompiled copy of. */
typedef void *(__cdecl *PFN_cgCreateContext)(void);
typedef void *(__cdecl *PFN_cgCreateProgram)(void *, int, const char *, int, const char *, const char **);
typedef const char *(__cdecl *PFN_cgGetProgramString)(void *, int);
typedef const char *(__cdecl *PFN_cgGetLastListing)(void *);
typedef int (__cdecl *PFN_cgGetError)(void);
typedef const char *(__cdecl *PFN_cgGetErrorString)(int);

#define RCG_SOURCE           4112   /* CG_SOURCE - verified against cg.h */
#define RCG_COMPILED_PROGRAM 4106   /* CG_COMPILED_PROGRAM */

static struct {
    int state;                       /* 0 untried, 1 ready, -1 unavailable */
    void *ctx;
    PFN_cgCreateProgram create;
    PFN_cgGetProgramString get;
    PFN_cgGetLastListing listing;
    PFN_cgGetError err;
    PFN_cgGetErrorString errstr;
} g_rcg;

static int real_cg_ready(void)
{
    if (g_rcg.state) return g_rcg.state > 0;
    g_rcg.state = -1;
#ifndef _WIN32
    return 0;        /* NVIDIA's Cg runtime is a Windows DLL here; PS5/Linux
                      * keep the precompiled-match path */
#else
    const char *off = getenv("LINDBERGH_CG_REAL");
    if (off && *off == '0') return 0;

    HMODULE h = NULL;
    char path[600];
    const char *env = getenv("LINDBERGH_CG_DLL");
    if (env && *env) h = LoadLibraryA(env);
    if (!h) {
        const char *bin = getenv("CG_BIN_PATH");
        if (bin && *bin) { snprintf(path, sizeof path, "%s\\cg.dll", bin); h = LoadLibraryA(path); }
    }
    if (!h) h = LoadLibraryA("C:\\Program Files (x86)\\NVIDIA Corporation\\Cg\\bin\\cg.dll");
    if (!h) h = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\Cg\\bin\\cg.dll");
    if (!h) h = LoadLibraryA("cg.dll");
    if (!h) {
        fprintf(stderr, "[cgreal] cg.dll not found - shaders with no precompiled copy "
                        "fall back to a texture copy (install NVIDIA Cg Toolkit 3.1, "
                        "or set LINDBERGH_CG_DLL)\n");
        return 0;
    }
    PFN_cgCreateContext mk = (PFN_cgCreateContext)GetProcAddress(h, "cgCreateContext");
    g_rcg.create  = (PFN_cgCreateProgram)GetProcAddress(h, "cgCreateProgram");
    g_rcg.get     = (PFN_cgGetProgramString)GetProcAddress(h, "cgGetProgramString");
    g_rcg.listing = (PFN_cgGetLastListing)GetProcAddress(h, "cgGetLastListing");
    g_rcg.err     = (PFN_cgGetError)GetProcAddress(h, "cgGetError");
    g_rcg.errstr  = (PFN_cgGetErrorString)GetProcAddress(h, "cgGetErrorString");
    if (!mk || !g_rcg.create || !g_rcg.get) {
        fprintf(stderr, "[cgreal] cg.dll is missing entry points - not using it\n");
        return 0;
    }
    g_rcg.ctx = mk();
    if (!g_rcg.ctx) { fprintf(stderr, "[cgreal] cgCreateContext failed\n"); return 0; }
    GetModuleFileNameA(h, path, sizeof path);
    fprintf(stderr, "[cgreal] using %s\n", path);
    g_rcg.state = 1;
    return 1;
#endif
}

/* Compile EVERY shader with the real runtime when it is present. The disc
 * matching picks one precompiled file for a whole family of #define
 * permutations (car_p.fx -> pCar.asm_lbg for every car variant, the course
 * shaders likewise), so its constant layout is wrong for most of them: car
 * bodies drew black and the Lake Akina terrain came out as a flat dark plane.
 * Compiling the actual source the game hands over has none of that. Matching
 * remains the fallback when cg.dll is absent or a compile fails;
 * LINDBERGH_CG_ALL=0 forces the old matching-first behaviour. */
static int real_cg_all(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("LINDBERGH_CG_ALL"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}

/* Compile a guest shader source with the real Cg runtime. Returns a malloc'd
 * copy of the compiled program text, or NULL (with the compiler's listing on
 * stderr) when it fails or the runtime is not there. */
static char *real_cg_compile(const char *src, int profile, const char *entry)
{
    if (!src || !real_cg_ready()) return NULL;
    /* fp30 compiles to NV_fragment_program "!!FP1.0" text, which cannot go
     * through glProgramStringARB the way the engine uploads everything; fp40
     * is a superset that emits "!!ARBfp1.0" + NV_fragment_program2. */
    if (profile == 6149 /* CG_PROFILE_FP30 */) profile = 6151 /* CG_PROFILE_FP40 */;
    void *prog = g_rcg.create(g_rcg.ctx, RCG_SOURCE, src, profile,
                              entry && *entry ? entry : "main", NULL);
    const char *out = prog ? g_rcg.get(prog, RCG_COMPILED_PROGRAM) : NULL;
    if (!out || !*out) {
        int e = g_rcg.err ? g_rcg.err() : -1;
        const char *lst = g_rcg.listing ? g_rcg.listing(g_rcg.ctx) : NULL;
        fprintf(stderr, "[cgreal] compile FAILED (profile %#x, err %d %s)\n%s\n",
                profile, e, (g_rcg.errstr && e > 0) ? g_rcg.errstr(e) : "",
                lst ? lst : "(no listing)");
        return NULL;
    }
    return strdup(out);
}

static void h_cgGetError(CPU *c)       { (void)c; RET(0); }           /* CG_NO_ERROR */
static void h_cgGetErrorString(CPU *c) { RET(cg_gstr("")); }
static void h_cgDestroyProgram(CPU *c) { (void)c; }

static void h_cgGLIsProfileSupported(CPU *c) { RET(1); }

/* cgCreateProgram(ctx, type, source, profile, entry, args). The returned
 * handle is an index into the shader table, offset so that zero stays the
 * failure value Cg uses. */
static void h_cgCreateProgram(CPU *c)
{
    { extern void klogf(const char*, ...);
      uint32_t argv = A32(5);
      const char *s = (const char *)(uintptr_t)A32(2);
      fprintf(stderr, "[cg] createProgram type=%#x src=%p argv=%#x inc=%d lbg=%d prof=%#x\n",
            A32(1), A32(2), argv, g_inc_n, g_lbg_n, A32(3));
      fflush(stderr);
      if (s) {
          char peek[160]; int i;
          for (i = 0; i < 150 && s[i]; i++) peek[i] = s[i] >= 32 ? s[i] : '.';
          peek[i] = 0;
          klogf("[cg]   src peek: %.150s\n", peek);
      }
      for (int k = 0; k < 24 && argv; k++) {
          uint32_t p = rd32(argv + 4u*(uint32_t)k);
          if (!p) break;
          klogf("[cg]   arg[%d]=%s\n", k, (const char*)(uintptr_t)p);
      } }
#if defined(LINDBERGH_STALE_WIN32_CG)
    index_shaders();
    const char *src = ASTR(2);
    if (src) {
        /* Two things identify a program: which shader it is, and which of its
         * #define permutations was asked for. The source says the first - the
         * file's own body survives preprocessing and can be found inside the
         * pasted-together whole - and the args say the second. Either alone is
         * ambiguous: many shaders share a define set, and one shader has many. */
        char *want_defs = args_defines(c, A32(5));
        char *whole = strip_code(src, strlen(src));
        int hit = -1;
        if (want_defs && whole) {
            for (int i = 0; i < g_sh_n; i++) {
                if (!g_sh[i].body || !g_sh[i].defines) continue;
                if (strcmp(g_sh[i].defines, want_defs) != 0) continue;
                size_t wl = strlen(whole), bl = strlen(g_sh[i].body);
                if (bl && bl <= wl && memcmp(whole + wl - bl, g_sh[i].body, bl) == 0) {
                    hit = i; g_match_body++;
                    if (getenv("LINDBERGH_FBSTATS") && g_match_body <= 14)
                        fprintf(stderr, "[cg] match %2d -> %s\n", g_match_body,
                                g_sh[i].path ? g_sh[i].path : "?");
                    break;      /* its own code, at the end */
                }
            }
            /* A define set that matches exactly one shader needs no second
             * opinion - and preprocessing can rewrite a body past recognition. */
            if (hit < 0) {
                int only = -1, seen = 0;
                for (int i = 0; i < g_sh_n; i++)
                    if (g_sh[i].defines && strcmp(g_sh[i].defines, want_defs) == 0) {
                        only = i; seen++;
                    }
                if (seen == 1) { hit = only; g_match_defines++; }
            }
        }
        free(want_defs);
        free(whole);
        if (hit >= 0) {
            static int reported;
            if (!reported && (g_match_body + g_match_defines) == 50) {
                reported = 1;
                fprintf(stderr, "[cg] first 50 matches: %d by body+defines, %d by defines alone\n",
                        g_match_body, g_match_defines);
            }
            { extern void klogf(const char*, ...);
              klogf("[cg] createProgram -> h=%d (g_sh[%d])\n", hit + 1, hit); }
            RET(hit + 1); return;
        }
    }
    fprintf(stderr, "[cg] no precompiled match for a %zu-byte shader source\n",
            src ? strlen(src) : 0);
#else
    index_shaders();
    const char *src = ASTR(2);
    if (src) {
        char *whole = strip_code(src, strlen(src));
        if (whole) {
            size_t wl = strlen(whole);
            int hit = -1;
            /* Which kind of program the game will upload this as. Every
             * match below must return text of that kind or the driver rejects
             * it outright. */
            uint32_t prof = A32(3);
            /* 0x1B59=ARBVP1, 0x1807=ARBFP1, 0x1805=FP40 - all fragment-side
             * profiles below compile to !!ARBfp text. */
            char want = prof == 0x1B59 ? 'v'
                     : (prof == 0x1807 || prof == 0x1805) ? 'f' : '?';

            /* Default when cg.dll is present: compile every source with the
             * real Cg runtime; a failed compile falls through to matching. */
            if (real_cg_all() && real_cg_ready()) {
                for (int i = 0; i < g_sh_n; i++)
                    if (g_sh[i].path && strcmp(g_sh[i].path, "(cg)") == 0 &&
                        g_sh[i].body && g_sh[i].prof == prof &&
                        strcmp(g_sh[i].body, whole) == 0) { hit = i; break; }
                if (hit < 0 && g_sh_n < MAX_SHADERS) {
                    const char *entry = A32(4) ? (const char *)(uintptr_t)A32(4) : "main";
                    char *real = real_cg_compile(src, (int)prof, entry);
                    if (real) {
                        hit = g_sh_n;
                        g_sh[g_sh_n].path = strdup("(cg)");
                        g_sh[g_sh_n].body = strdup(whole);
                        g_sh[g_sh_n].defines = NULL;
                        g_sh[g_sh_n].compiled = real;
                        g_sh[g_sh_n].prof = prof;
                        g_sh_n++;
                    }
                }
                if (hit >= 0) { free(whole); RET(hit + 1); return; }
            }

            /* data/Shader pipeline: the source begins with a variant's _inc
             * define block. The values differ per permutation, so compare the
             * #define-name sequence (values dropped) rather than the raw
             * text — same variant, different flags still names the same
             * shader. */
            const char *stem = NULL;
            char *src_seq = defseq(whole);
            for (int i = 0; src_seq && i < g_inc_n; i++) {
                char *inc_seq = g_inc[i].defs ? defseq(g_inc[i].defs) : NULL;
                size_t il = inc_seq ? strlen(inc_seq) : 0;
                if (il && strncmp(src_seq, inc_seq, il) == 0) {
                    stem = g_inc[i].stem; free(inc_seq); break;
                }
                free(inc_seq);
            }
            free(src_seq);
            if (stem) {
                /* The profile argument is authoritative: 0x1B59 is
                 * CG_PROFILE_ARBVP1, 0x1807/0x1805 the fragment profiles. The
                 * old strstr(src,"VS_INPUT"/"POSITION") heuristic fired on
                 * fragment sources too and handed vertex assembly to the
                 * fragment target - every such upload failed with "invalid
                 * fragment program header" and the material rendered wrong. */
                uint32_t prof = A32(3);
                int vertex = prof == 0x1B59 ? 1
                           : (prof == 0x1807 || prof == 0x1805) ? 0
                           : (strstr(src, "VS_INPUT") || strstr(src, "VS_OUTPUT")
                              || strstr(src, "POSITION") ? 1 : 0);
                const char *compiled = lbg_lookup(stem, vertex);
                { extern void klogf(const char*, ...);
                  klogf("[cg] inc match stem=%s vertex=%d -> %s\n",
                        stem, vertex, compiled ? "hit" : "miss"); }
                if (compiled && g_sh_n < MAX_SHADERS &&
                    (want == '?' || prog_kind(compiled) == want ||
                     prog_kind(compiled) == '?')) {
                    hit = g_sh_n;
                    g_sh[g_sh_n].path     = strdup(stem);
                    g_sh[g_sh_n].body     = NULL;
                    g_sh[g_sh_n].defines  = NULL;
                    g_sh[g_sh_n].compiled = (char *)compiled;
                    g_sh[g_sh_n].prof     = prof;
                    g_sh_n++;
                    g_match_body++;
                }
            }

            /* .fx body match: combiner/post-process shaders carry no inc
             * template, but the game prepends its generated defines to the
             * verbatim .fx body — the stripped tail still equals the file. */
            if (hit < 0) {
                for (int i = 0; i < g_sh_n; i++) {
                    if (!g_sh[i].body || g_sh[i].defines) continue;
                    if (want != '?' && prog_kind(g_sh[i].compiled) != want &&
                        prog_kind(g_sh[i].compiled) != '?') continue;
                    size_t bl = strlen(g_sh[i].body);
                    if (bl && bl <= wl &&
                        memcmp(whole + wl - bl, g_sh[i].body, bl) == 0) {
                        hit = i; g_match_body++;
                        { extern void klogf(const char*, ...);
                          klogf("[cg] fx body match -> %s\n",
                                g_sh[i].path ? g_sh[i].path : "?"); }
                        break;
                    }
                }
            }

            /* shader/Cg pipeline: defines + body-tail, as on Windows. The
             * lindbergh-loader zeroes the cgCreateProgram args pointer — the
             * game can pass a garbage argv that yields bogus -D defines and a
             * wrong shader pick. Force it out rather than trust it. */
            if (hit < 0) {
                char *want_defs = args_defines(c, 0);
                if (want_defs) {
                    for (int i = 0; i < g_sh_n; i++) {
                        if (!g_sh[i].body || !g_sh[i].defines) continue;
                        if (want != '?' && prog_kind(g_sh[i].compiled) != want &&
                            prog_kind(g_sh[i].compiled) != '?') continue;
                        if (strcmp(g_sh[i].defines, want_defs) != 0) continue;
                        size_t bl = strlen(g_sh[i].body);
                        if (bl && bl <= wl &&
                            memcmp(whole + wl - bl, g_sh[i].body, bl) == 0) {
                            hit = i; g_match_body++; break;
                        }
                    }
                    if (hit < 0) {
                        int only = -1, seen = 0;
                        for (int i = 0; i < g_sh_n; i++)
                            if (g_sh[i].defines &&
                                (want == '?' || prog_kind(g_sh[i].compiled) == want ||
                                 prog_kind(g_sh[i].compiled) == '?') &&
                                strcmp(g_sh[i].defines, want_defs) == 0) {
                                only = i; seen++;
                            }
                        if (seen == 1) { hit = only; g_match_defines++; }
                    }
                }
                free(want_defs);
            }

            /* Last resort: no precompiled answer exists for this permutation
             * (the combiner-family shaders were cgc'd live on real hardware).
             * Hand back a minimal legal ARB program so the shader registers
             * and the constant-table parse sees a well-formed text. A repeat
             * compile of the same source reuses its fallback slot — each new
             * slot otherwise pushes the table one closer to MAX_SHADERS. */
            if (hit < 0) {
                for (int i = 0; i < g_sh_n; i++)
                    if (g_sh[i].path && (strcmp(g_sh[i].path, "(fallback)") == 0 ||
                                         strcmp(g_sh[i].path, "(cg)") == 0) &&
                        g_sh[i].body && strcmp(g_sh[i].body, whole) == 0 &&
                        g_sh[i].prof == A32(3) &&
                        (want == '?' || prog_kind(g_sh[i].compiled) == want)) {
                        hit = i; break;
                    }
            }
            /* No precompiled answer: compile it for real, as the board did. */
            if (hit < 0 && g_sh_n < MAX_SHADERS) {
                const char *entry = A32(4) ? (const char *)(uintptr_t)A32(4) : "main";
                char *real = real_cg_compile(src, (int)A32(3), entry);
                if (real && (want == '?' || prog_kind(real) == want)) {
                    hit = g_sh_n;
                    g_sh[g_sh_n].path     = strdup("(cg)");
                    g_sh[g_sh_n].body     = strdup(whole);
                    g_sh[g_sh_n].defines  = NULL;
                    g_sh[g_sh_n].compiled = real;
                    g_sh[g_sh_n].prof     = A32(3);
                    g_sh_n++;
                    fprintf(stderr, "[cgreal] compiled %zu-byte %s source -> g_sh[%d] (%zu bytes)\n",
                            strlen(src), want == 'v' ? "vertex" : "fragment", hit, strlen(real));
                } else {
                    if (real)
                        fprintf(stderr, "[cgreal] compiled text is the wrong kind for profile %#x "
                                        "(head %.20s) - using fallback\n", A32(3), real);
                    free(real);
                }
            }
            if (hit < 0 && g_sh_n < MAX_SHADERS) {
                extern void klogf(const char*, ...);
                uint32_t prof = A32(3);
                int vertex = prof == 0x1B59 ? 1
                           : (prof == 0x1807 || prof == 0x1805) ? 0
                           : (strstr(src, "VS_INPUT") || strstr(src, "VS_OUTPUT") ? 1 : 0);
                /* The fallback pair is a textured pass-through: the fragment
                 * stub multiplies the texture by fragment.color, so the vertex
                 * stub MUST write result.color. An unwritten vertex result is
                 * undefined in ARB_vertex_program and current NVIDIA drivers
                 * read it back as zero - which turned every fallback post pass
                 * (the scene composite and the copy to the screen among them)
                 * into solid black while the 3D scene itself rendered fine.
                 * That is the black world in Initial D 4's cutscenes and races. */
                static const char vs_stub[] =
                    "!!ARBvp1.0\n"
                    "# cgc version 1.4.0000, lindberghrecomp fallback\n"
                    "ATTRIB ipos = vertex.position;\n"
                    "ATTRIB itex = vertex.texcoord[0];\n"
                    "MOV result.position, ipos;\n"
                    "MOV result.texcoord[0], itex;\n"
                    "MOV result.color, vertex.color;\n"
                    "END\n";
                static const char fp_stub[] =
                    "!!ARBfp1.0\n"
                    "# cgc version 1.4.0000, lindberghrecomp fallback\n"
                    "TEMP t;\n"
                    "TEX t, fragment.texcoord[0], texture[0], 2D;\n"
                    "MUL result.color, fragment.color, t;\n"
                    "END\n";
                hit = g_sh_n;
                g_sh[g_sh_n].path     = strdup("(fallback)");
                /* The stripped source is the dedupe key — repeats find this
                 * slot via the scan above rather than appending again. */
                g_sh[g_sh_n].body     = strdup(whole);
                g_sh[g_sh_n].defines  = NULL;
                g_sh[g_sh_n].compiled = (char *)(vertex ? vs_stub : fp_stub);
                g_sh[g_sh_n].prof     = prof;
                g_sh_n++;
                (void)klogf;
                /* fprintf, not klogf: klogf caps the [cg] prefix at 60 lines
                 * and the fallbacks come late in init, so they were hidden.
                 * Every fallback is a shader drawing as a plain texture copy. */
                {   char head[200]; int hi = 0;
                    for (const char *q = src; *q && hi < 190; q++)
                        head[hi++] = (*q == '\n' || *q == '\r') ? '|' : *q;
                    head[hi] = 0;
                    fprintf(stderr, "[cgfb] %s fallback #%d for %zu-byte source (stem=%s) g_sh[%d]: %.190s\n",
                            vertex ? "vertex" : "pixel", g_sh_n, strlen(src),
                            stem ? stem : "none", g_sh_n - 1, head); }
            }
            free(whole);
            if (hit >= 0) {
                fprintf(stderr, "[cg] createProgram -> h=%d (g_sh[%d] prof=%#x)\n",
                        hit + 1, hit, g_sh[hit].prof);
                fflush(stderr);
                RET(hit + 1); return;
            }
        }
    }
    fprintf(stderr, "[cg] no precompiled match for a %zu-byte shader source\n",
            src ? strlen(src) : 0);
#endif
    RET(0);
}

/* cgGetProgramString(program, string_type) - the game wants the compiled
 * output, which is the .asm_gl read at index time.
 *
 * The game reads the returned pointer once (to upload via glProgramStringARB)
 * then makes its own guest-heap copy in _sPixelShader/VertexShader::initialize.
 * It never holds two cgGetProgramString results simultaneously.
 *
 * We therefore keep exactly ONE guest-heap allocation alive at a time: free the
 * previous result before allocating the next.  Peak guest-heap cost drops from
 * N_shaders * text_len to 1 * text_len regardless of build bitness. */
static void h_cgGetProgramString(CPU *c)
{
    extern void gfree(void *);
    extern void guest_heap_lock(void), guest_heap_unlock(void);
    { static int s_n;
      if (s_n++ < 400) { char b[80];
          const char *kind = "?";
          int ii = (int)A32(0) - 1;
          if (ii >= 0 && ii < g_sh_n && g_sh[ii].compiled) {
              char k = prog_kind(g_sh[ii].compiled);
              kind = k == 'v' ? "vp" : k == 'f' ? "fp" : "??";
          }
          snprintf(b,sizeof b,"[cg] getProgramString #%d i=%d -> %s\n",
                   s_n, (int)A32(0), kind);
          sceKernelDebugOutText(0,b); } }
    guest_heap_lock();
    int i = (int)A32(0) - 1;
    /* cgGetProgramString returns a pointer valid for the program's lifetime,
     * and we never destroy programs (cgDestroyProgram is a no-op). Keep one
     * stable guest string per shader (gstr) instead of freeing the previous
     * result on each call: that was a use-after-free for anything holding the
     * pointer past the next call — including Mesa's lazy ARB source read,
     * which would parse freed/corrupted text and abort non-deterministically. */
    static int s_last_good_i = -1;
    if (i >= 0 && i < g_sh_n && g_sh[i].compiled && g_sh[i].compiled[0]) {
        s_last_good_i = i;
        if (!g_sh[i].gstr) g_sh[i].gstr = cg_gstr(g_sh[i].compiled);
        uint32_t r = g_sh[i].gstr;
        guest_heap_unlock();
        RET(r);
        return;
    }
    /* empty/unknown program — hand back the last-good shader's stable string */
    uint32_t r;
    if (s_last_good_i >= 0) {
        if (!g_sh[s_last_good_i].gstr) g_sh[s_last_good_i].gstr = cg_gstr(g_sh[s_last_good_i].compiled);
        r = g_sh[s_last_good_i].gstr;
    } else {
        static uint32_t s_empty;
        if (!s_empty) s_empty = cg_gstr("");
        r = s_empty;
    }
    guest_heap_unlock();
    RET(r);
}

void hle_register_cg(void)
{
    hle_bind("cgCreateContext", h_cgCreateContext);
    hle_bind("cgIsContext", h_cgIsContext);
    hle_bind("cgDestroyContext", h_cgDestroyContext);
    hle_bind("cgSetErrorCallback", h_cgSetErrorCallback);
    hle_bind("cgGetError", h_cgGetError);
    hle_bind("cgGetErrorString", h_cgGetErrorString);
    hle_bind("cgGLIsProfileSupported", h_cgGLIsProfileSupported);
    hle_bind("cgCreateProgram", h_cgCreateProgram);
    hle_bind("cgGetProgramString", h_cgGetProgramString);
    hle_bind("cgDestroyProgram", h_cgDestroyProgram);
}
