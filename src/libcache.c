/* libcache.c — see libcache.h. */
#ifndef _GNU_SOURCE          /* gio-2.0's pkg-config cflags already define this */
#define _GNU_SOURCE
#endif
#include "libcache.h"
#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char dir[1024];
    long long mtime;
    int tracks;
} Row;

static Row *rows;
static int n_rows, cap_rows;

static char walk_root[1024];
static LibEntry *walk_entries;
static int walk_n, walk_i, walk_done;
static int last_scanned, last_reused;

static Row *row_for(const char *dir)
{
    int i;
    for (i = 0; i < n_rows; i++)
        if (!strcmp(rows[i].dir, dir)) return &rows[i];
    return NULL;
}

static Row *row_add(const char *dir)
{
    if (n_rows == cap_rows) {
        int ncap = cap_rows ? cap_rows * 2 : 512;
        Row *nr = realloc(rows, (size_t)ncap * sizeof *rows);
        if (!nr) return NULL;
        rows = nr;
        cap_rows = ncap;
    }
    snprintf(rows[n_rows].dir, sizeof rows[n_rows].dir, "%s", dir);
    rows[n_rows].mtime = -1;
    rows[n_rows].tracks = -1;
    return &rows[n_rows++];
}

static int count_audio(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (lib_is_audio(de->d_name)) n++;
    }
    closedir(d);
    return n;
}

int libcache_tracks(const char *dir)
{
    struct stat st;
    Row *r;

    if (!dir || !*dir) return -1;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    r = row_for(dir);
    /* unchanged since we counted it: that is the whole point of the cache */
    if (r && r->mtime == (long long)st.st_mtime && r->tracks >= 0) {
        last_reused++;
        return r->tracks;
    }
    {
        int n = count_audio(dir);
        if (n < 0) return -1;
        if (!r) r = row_add(dir);
        if (r) {
            r->mtime = (long long)st.st_mtime;
            r->tracks = n;
            last_scanned++;
        }
        return n;
    }
}

/* ---------------- the incremental walk ---------------- */

int libcache_step(const char *root, int budget)
{
    int i;

    if (!root || !*root) return 0;
    if (walk_done && !strcmp(walk_root, root)) return 0;
    if (strcmp(walk_root, root) != 0) {
        /* a different root: start over, snapshotting its entries once. Counting
         * the whole library from scratch is a readdir per album, which is fine
         * spread over frames and never happens twice for the same folder. */
        lib_free_entries(walk_entries);
        walk_entries = NULL;
        walk_n = lib_scan(root, &walk_entries);
        if (walk_n < 0) { walk_n = 0; walk_done = 1; snprintf(walk_root, sizeof walk_root, "%s", root); return 0; }
        walk_i = 0;
        walk_done = 0;
        last_scanned = last_reused = 0;
        snprintf(walk_root, sizeof walk_root, "%s", root);
    }
    if (walk_done) return 0;

    for (i = 0; i < budget && walk_i < walk_n; i++, walk_i++) {
        LibEntry *e = &walk_entries[walk_i];
        if (e->kind != L_DIR) continue;
        libcache_tracks(e->path);
    }
    if (walk_i >= walk_n) {
        walk_done = 1;
        /* persist as soon as it is complete, so the next launch is instant */
        libcache_save();
        return 0;
    }
    return 1;
}

int libcache_cached(const char *root)
{
    return walk_done && !strcmp(walk_root, root);
}

void libcache_last_walk(int *scanned, int *reused)
{
    if (scanned) *scanned = last_scanned;
    if (reused) *reused = last_reused;
}

/* is `dir` a folder directly inside `root`? */
static int direct_child(const char *dir, const char *root)
{
    size_t rl = strlen(root);
    return strncmp(dir, root, rl) == 0 && dir[rl] == '/' && !strchr(dir + rl + 1, '/');
}

long long libcache_total_tracks(const char *root)
{
    long long t = 0;
    int i;
    for (i = 0; i < n_rows; i++)
        if (rows[i].tracks > 0 && direct_child(rows[i].dir, root)) t += rows[i].tracks;
    return t;
}

int libcache_albums(const char *root)
{
    int n = 0, i;
    for (i = 0; i < n_rows; i++)
        if (rows[i].tracks > 0 && direct_child(rows[i].dir, root)) n++;
    return n;
}

/* ---------------- persistence ---------------- */

static int cache_file(char *out, size_t n)
{
    const char *base = getenv("XDG_CACHE_HOME");
    char dir[1024];
    if (base && *base) snprintf(dir, sizeof dir, "%s/marimo", base);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return 0;
        snprintf(dir, sizeof dir, "%s/.cache/marimo", home);
    }
    mkdir(dir, 0755);   /* EEXIST is the normal case */
    snprintf(out, n, "%s/library.txt", dir);
    return 1;
}

void libcache_load(void)
{
    char path[1200];
    FILE *f;
    char line[1400];

    if (!cache_file(path, sizeof path)) return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *t1, *t2;
        Row *r;
        t1 = strchr(line, '\t');
        if (!t1) continue;                       /* junk line: skip, do not fail */
        *t1 = 0;
        t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = 0;
        {
            char *dir = t2 + 1;
            size_t l = strlen(dir);
            while (l && (dir[l - 1] == '\n' || dir[l - 1] == '\r')) dir[--l] = 0;
            if (!dir[0] || dir[0] != '/') continue;
            r = row_for(dir);
            if (!r) r = row_add(dir);
            if (!r) continue;
            r->mtime = atoll(line);
            r->tracks = atoi(t1 + 1);
        }
    }
    fclose(f);
}

void libcache_save(void)
{
    char path[1200];
    FILE *f;
    int i;

    if (!cache_file(path, sizeof path)) return;
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# marimo library cache — album mtime, track count, folder\n");
    for (i = 0; i < n_rows; i++) {
        if (rows[i].tracks < 0) continue;
        fprintf(f, "%lld\t%d\t%s\n", rows[i].mtime, rows[i].tracks, rows[i].dir);
    }
    fclose(f);
}

void libcache_free(void)
{
    free(rows);
    rows = NULL;
    n_rows = cap_rows = 0;
    lib_free_entries(walk_entries);
    walk_entries = NULL;
    walk_n = walk_i = walk_done = 0;
    walk_root[0] = 0;
}
