/* library.c — dirent-based browsing. Names are passed through as raw UTF-8
 * bytes; we never touch their contents, so weird characters just work. */
#include "library.h"
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static const char *audio_exts[] = {
    ".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wav",
    ".wma", ".ape", ".mpc", ".wv", ".aiff", ".aif", ".mka", ".ac3",
    ".tta", ".dsf", ".dff", ".tak", ".m4b", NULL
};

int lib_is_audio(const char *name)
{
    size_t l = strlen(name);
    int i;
    for (i = 0; audio_exts[i]; i++) {
        size_t e = strlen(audio_exts[i]);
        if (l > e && !strcasecmp(name + l - e, audio_exts[i])) return 1;
    }
    return 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const LibEntry *x = (const LibEntry *)a;
    const LibEntry *y = (const LibEntry *)b;
    if (x->kind != y->kind) return x->kind - y->kind;   /* UP, DIR, FILE */
    return strcasecmp(x->name, y->name);
}

static void add_entry(LibEntry **ents, int *n, int *cap, int kind,
                      const char *name, const char *path, long long size)
{
    LibEntry *e;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *ents = (LibEntry *)realloc(*ents, *cap * sizeof(LibEntry));
    }
    e = &(*ents)[(*n)++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->path, sizeof e->path, "%s", path);
    e->size = size;
}

int lib_scan(const char *dir, LibEntry **out)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    LibEntry *ents = NULL;
    int n = 0, cap = 0;
    char parent[L_PATH_MAX];
    size_t dl;
    *out = NULL;
    if (!d) return -1;

    /* ".." entry */
    snprintf(parent, sizeof parent, "%s", dir);
    dl = strlen(parent);
    while (dl > 1 && parent[dl - 1] == '/') parent[--dl] = 0;
    {
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) *slash = 0;
        else if (slash == parent) parent[1] = 0;
    }
    add_entry(&ents, &n, &cap, L_UP, "..", parent, 0);

    while ((de = readdir(d))) {
        char path[L_PATH_MAX];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode))
            add_entry(&ents, &n, &cap, L_DIR, de->d_name, path, 0);
        else if (S_ISREG(st.st_mode) && lib_is_audio(de->d_name))
            add_entry(&ents, &n, &cap, L_FILE, de->d_name, path, (long long)st.st_size);
    }
    closedir(d);
    qsort(ents, n, sizeof(LibEntry), ent_cmp);
    *out = ents;
    return n;
}

void lib_free_entries(LibEntry *e)
{
    free(e);
}

static int has_img_ext(const char *name)
{
    static const char *exts[] = { ".jpg", ".jpeg", ".png", ".webp", ".bmp", NULL };
    size_t l = strlen(name);
    for (int i = 0; exts[i]; i++) {
        size_t e = strlen(exts[i]);
        if (l > e && !strcasecmp(name + l - e, exts[i])) return 1;
    }
    return 0;
}

/* find the best cover image in dir.
 * order: known names (cover/folder/front/album/art...) > "album - artist" style
 * names > a lone image. ambiguous multi-image folders with no obvious cover → -1 */
int lib_find_cover(const char *dir, char *out, int outsz)
{
    static const char *known[] = { "cover", "front", "frontcover", "albumart",
                                   "folder", "album", "art", "front_cover", NULL };
    DIR *d = opendir(dir);
    struct dirent *de;
    char best[L_PATH_MAX] = "";
    int best_score = -1;
    int count = 0;
    if (!d) return -1;
    while ((de = readdir(d))) {
        char stem[256];
        size_t l;
        if (de->d_name[0] == '.') continue;
        if (!has_img_ext(de->d_name)) continue;
        count++;
        /* stem, lowercased */
        snprintf(stem, sizeof stem, "%s", de->d_name);
        l = strlen(stem);
        while (l && stem[l - 1] != '.') stem[--l] = 0;
        if (l) stem[l - 1] = 0;
        for (char *p = stem; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        int score = 10;
        for (int k = 0; known[k]; k++)
            if (!strcmp(stem, known[k])) { score = 100; break; }
        if (score == 10 && strstr(stem, " - "))
            score = 50;
        if (score > best_score) {
            best_score = score;
            snprintf(best, sizeof best, "%s", de->d_name);
        }
    }
    closedir(d);
    if (best_score < 0) return -1;
    if (best_score == 10 && count > 1) return -1;   /* ambiguous: back.jpg + tray.jpg etc */
    snprintf(out, outsz, "%s/%s", dir, best);
    return 0;
}
