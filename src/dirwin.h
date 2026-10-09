/* dirwin.h — one way to read a directory, and on windows it is the wide one.
 *
 * mingw's own dirent is ANSI: opendir() hands the path to FindFirstFileA, which
 * reads those UTF-8 bytes as the system codepage. Every folder whose name is not
 * plain ASCII therefore fails to open, and it fails *silently* — the album just
 * looks like a folder with no tracks in it, so it draws as a plain folder row with
 * its raw name and no cover. That is the whole of "the Japanese albums are not
 * loading right" on the windows build (2026-09-28).
 *
 * The folder browser had its own FindFirstFileW shim from the start and was the
 * only thing that worked. This is that shim, shared: md_opendir/md_readdir/
 * md_rewinddir/md_closedir, with readdir handing back just the name.
 *
 * The windows side enumerates the directory once at open time rather than walking a
 * handle, which makes rewinddir() free. Listings here are at most a few hundred
 * names (the library root is 632), so the array is nothing.
 */
#ifndef MARIMO_DIRWIN_H
#define MARIMO_DIRWIN_H

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "fs.h"

typedef struct {
    char **name;
    int n, i;
} MD_DIR;

static inline MD_DIR *md_opendir(const char *path)
{
    MD_DIR *d;
    wchar_t *wp, *pat;
    size_t plen;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int cap = 0;

    if (!path || !*path) return NULL;
    plen = strlen(path);
    wp = fs_utf8_to_wide(path);
    if (!wp) return NULL;
    pat = (wchar_t *)malloc((plen + 3) * sizeof(wchar_t));
    if (!pat) { free(wp); return NULL; }
    wcscpy(pat, wp);
    wcscat(pat, L"\\*");
    free(wp);

    h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    d = (MD_DIR *)calloc(1, sizeof(MD_DIR));
    if (!d) { FindClose(h); return NULL; }
    do {
        char *u = fs_wide_to_utf8(fd.cFileName);
        if (!u) continue;
        if (d->n == cap) {
            char **nn;
            cap = cap ? cap * 2 : 64;
            nn = (char **)realloc(d->name, (size_t)cap * sizeof(char *));
            if (!nn) { free(u); break; }
            d->name = nn;
        }
        d->name[d->n++] = u;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return d;
}

static inline const char *md_readdir(MD_DIR *d)
{
    if (!d || d->i >= d->n) return NULL;
    return d->name[d->i++];
}

static inline void md_rewinddir(MD_DIR *d) { if (d) d->i = 0; }

static inline void md_closedir(MD_DIR *d)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->n; i++) free(d->name[i]);
    free(d->name);
    free(d);
}

#else /* POSIX — the real thing */

#include <dirent.h>

typedef DIR MD_DIR;

static inline MD_DIR *md_opendir(const char *path) { return opendir(path); }

static inline const char *md_readdir(MD_DIR *d)
{
    struct dirent *e;
    if (!d) return NULL;
    e = readdir(d);
    return e ? e->d_name : NULL;
}

static inline void md_rewinddir(MD_DIR *d) { if (d) rewinddir(d); }
static inline void md_closedir(MD_DIR *d) { if (d) closedir(d); }

#endif
#endif /* MARIMO_DIRWIN_H */
