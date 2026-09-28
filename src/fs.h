/* fs.h — UTF-8-safe file access.
 * marimo carries all paths as UTF-8. On windows the CRT's fopen/stat/
 * access use the ANSI codepage and mangle non-ascii names, so those go
 * through the *W APIs with UTF-8 <-> UTF-16 conversion here.
 * On POSIX these are plain wrappers. Use them for any path that may
 * contain non-ascii (music files, covers, drag&drop paths, queue). */
#ifndef MIKA_FS_H
#define MIKA_FS_H

#include <stdio.h>
#include <sys/stat.h>

FILE *fs_fopen(const char *path, const char *mode);
int   fs_stat(const char *path, struct stat *st);
int   fs_access(const char *path, int mode);

#ifdef _WIN32
/* malloc'd wide copy of a UTF-8 string (caller frees), NULL on failure */
wchar_t *fs_utf8_to_wide(const char *s);
/* malloc'd UTF-8 copy of a wide string (caller frees), NULL on failure */
char *fs_wide_to_utf8(const wchar_t *w);
#endif

#endif
