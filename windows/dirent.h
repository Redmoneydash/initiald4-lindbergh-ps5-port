/* dirent.h - minimal POSIX dirent over MSVC _findfirst/_findnext.
 * Only what hle_cg.c's non-Windows shader-index path uses. */
#ifndef LIND_DIRENT_SHIM_H
#define LIND_DIRENT_SHIM_H

#include <io.h>
#include <string.h>
#include <stdlib.h>

typedef struct dirent {
    char d_name[260];
} dirent;

typedef struct {
    intptr_t fh;                 /* _findfirst handle; -1 once exhausted */
    struct _finddata_t fd;       /* always holds the NEXT unread entry */
} DIR;

static DIR *opendir(const char *path)
{
    if (!path || !*path) return NULL;
    size_t n = strlen(path);
    char *pat = (char *)malloc(n + 3);
    if (!pat) return NULL;
    while (n && (path[n-1] == '/' || path[n-1] == '\\')) n--;
    memcpy(pat, path, n);
    pat[n] = '\\'; pat[n+1] = '*'; pat[n+2] = 0;
    DIR *d = (DIR *)calloc(1, sizeof *d);
    if (!d) { free(pat); return NULL; }
    d->fh = _findfirst(pat, &d->fd);
    free(pat);
    if (d->fh == -1) { free(d); return NULL; }
    return d;
}

static struct dirent *readdir(DIR *d)
{
    static __declspec(thread) dirent e;
    if (!d || d->fh == -1) return NULL;
    strncpy(e.d_name, d->fd.name, sizeof e.d_name - 1);
    e.d_name[sizeof e.d_name - 1] = 0;
    if (_findnext(d->fh, &d->fd) == -1)
        d->fh = -1;
    return &e;
}

static int closedir(DIR *d)
{
    if (!d) return -1;
    if (d->fh != -1) _findclose(d->fh);
    free(d);
    return 0;
}

#endif
