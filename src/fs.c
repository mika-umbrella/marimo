/* fs.c — UTF-8-safe file access (see fs.h). */
#include "fs.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <io.h>

wchar_t *fs_utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *fs_wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

FILE *fs_fopen(const char *path, const char *mode)
{
    wchar_t *wp = fs_utf8_to_wide(path);
    wchar_t wm[8];
    FILE *f;
    int i;
    if (!wp) return NULL;
    for (i = 0; mode[i] && i < 7; i++) wm[i] = (wchar_t)(unsigned char)mode[i];
    wm[i] = 0;
    f = _wfopen(wp, wm);
    free(wp);
    return f;
}

int fs_stat(const char *path, struct stat *st)
{
    wchar_t *wp = fs_utf8_to_wide(path);
    struct _stat wst;
    int r;
    if (!wp) return -1;
    r = _wstat(wp, &wst);
    free(wp);
    if (r == 0) {
        memset(st, 0, sizeof(*st));
        st->st_mode = wst.st_mode;
        st->st_size = wst.st_size;
        st->st_mtime = wst.st_mtime;
    }
    return r;
}

int fs_access(const char *path, int mode)
{
    wchar_t *wp = fs_utf8_to_wide(path);
    int r;
    if (!wp) return -1;
    r = _waccess(wp, mode);
    free(wp);
    return r;
}

#else /* POSIX */

FILE *fs_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int   fs_stat(const char *path, struct stat *st)   { return stat(path, st); }
int   fs_access(const char *path, int mode)        { return access(path, mode); }

#endif
