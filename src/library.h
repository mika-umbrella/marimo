/* library.h — file system browsing + cover art discovery. */
#ifndef MIKA_LIBRARY_H
#define MIKA_LIBRARY_H

#define L_UP   0
#define L_DIR  1
#define L_FILE 2

#define L_PATH_MAX 2048

typedef struct {
    int kind;                  /* L_UP / L_DIR / L_FILE */
    char name[512];
    char path[L_PATH_MAX];
    long long size;
} LibEntry;

/* scan dir; returns count, or -1 on error. *out must be freed with lib_free_entries */
int lib_scan(const char *dir, LibEntry **out);
void lib_free_entries(LibEntry *e);
/* find a cover image in dir; 0 + out on success, -1 if none */
int lib_find_cover(const char *dir, char *out, int outsz);
int lib_is_audio(const char *name);

#endif
