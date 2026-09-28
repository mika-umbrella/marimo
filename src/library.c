/* library.c — dirent-based browsing. Names are passed through as raw UTF-8
 * bytes; we never touch their contents, so weird characters just work. */
#include "library.h"
#include "fs.h"
#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <unistd.h>
#ifdef _WIN32
/* minimal dirent shim over FindFirstFileW (folder browser only).
 * The ANSI (A) APIs use the system codepage and mangle non-ascii names;
 * the W APIs give UTF-16, which we convert to UTF-8 for the app. */
#include <windows.h>
#include "fs.h"
typedef struct DIR DIR;
struct dirent { char d_name[512]; };
struct DIR {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    struct dirent ent;
    int first;
};
static DIR *opendir(const char *path)
{
    wchar_t *wpat, *wp;
    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    size_t plen;
    if (!d) return NULL;
    plen = strlen(path);
    if (plen > L_PATH_MAX - 2) { free(d); return NULL; }
    wp = fs_utf8_to_wide(path);
    if (!wp) { free(d); return NULL; }
    wpat = (wchar_t *)malloc((plen + 3) * sizeof(wchar_t));
    if (!wpat) { free(wp); free(d); return NULL; }
    wcscpy(wpat, wp);
    wcscat(wpat, L"\\*");
    free(wp);
    d->h = FindFirstFileW(wpat, &d->fd);
    free(wpat);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}
static struct dirent *readdir(DIR *d)
{
    if (!d->first && !FindNextFileW(d->h, &d->fd)) return NULL;
    d->first = 0;
    {
        char *u = fs_wide_to_utf8(d->fd.cFileName);
        if (!u) return NULL;
        snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", u);
        free(u);
    }
    return &d->ent;
}
static int closedir(DIR *d)
{
    if (d) { FindClose(d->h); free(d); }
    return 0;
}
#else
#include <dirent.h>
#endif

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
        if (fs_stat(path, &st)) continue;
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

/* ---------------- cover selection (CoverName.java, ported) ---------------- */

/* Fold a folder name or a filename to a comparable form — the phone's
 * CoverName.key(). The one thing dropped is its NFC normalisation, which needs
 * ICU: the phone wants it because SAF can hand the same Japanese name back in
 * either normal form, whereas here both names come out of one readdir of one
 * filesystem (the same trade the waveform sidecar lookup makes). */
static void cover_key(const char *in, char *out, size_t n)
{
    size_t o = 0;
    int group = 0;
    const char *p = in;
    if (!n) return;
    if (!in) { out[0] = 0; return; }
    for (; *p && o + 2 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '(') { group = 1; continue; }          /* drop the group AND its text */
        if (c == '[') { group = 2; continue; }
        if (group == 1 && c == ')') { group = 0; out[o++] = ' '; continue; }
        if (group == 2 && c == ']') { group = 0; out[o++] = ' '; continue; }
        if (group) continue;
        /* the dash characters folders mix freely: U+2010..U+2015 and U+2212 */
        if (c == 0xE2 && p[1] && p[2] &&
            (((unsigned char)p[1] == 0x80 && (unsigned char)p[2] >= 0x90 && (unsigned char)p[2] <= 0x95) ||
             ((unsigned char)p[1] == 0x88 && (unsigned char)p[2] == 0x92))) {
            out[o++] = '-';
            p += 2;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        out[o++] = (char)c;
    }
    out[o] = 0;
    /* then: " - " for every dash, collapse runs of spaces, trim */
    {
        char tmp[512];
        size_t i = 0, j = 0, lim = n < sizeof tmp ? n : sizeof tmp;
        for (i = 0; out[i] && j + 2 < lim; i++) {
            if (out[i] == ' ') {
                size_t k = i;
                while (out[k] == ' ') k++;
                if (out[k] == '-') {                    /* spaces before a dash */
                    tmp[j++] = ' ';
                    i = k - 1;
                    continue;
                }
                if (j && tmp[j - 1] != ' ') tmp[j++] = ' ';
                i = k - 1;
                continue;
            }
            tmp[j++] = out[i];
        }
        tmp[j] = 0;
        {
            char *s = tmp;
            while (*s == ' ') s++;
            /* the precision keeps the copy inside `out` whatever n is — the
             * compiler is right that %s alone could overrun it */
            snprintf(out, n, "%.*s", (int)(lim ? lim - 1 : 0), s);
            o = strlen(out);
            while (o && out[o - 1] == ' ') out[--o] = 0;
        }
    }
}

int cover_rank(const char *name, const char *folder)
{
    const char *dot;
    char stem[512], ext[32], fk[512], sk[512];
    size_t l;
    int i;

    if (!name) return -1;
    dot = strrchr(name, '.');
    if (!dot || dot == name) return -1;
    l = (size_t)(dot - name);
    if (l >= sizeof stem) l = sizeof stem - 1;
    memcpy(stem, name, l);
    stem[l] = 0;
    snprintf(ext, sizeof ext, "%s", dot + 1);
    for (i = 0; stem[i]; i++) if (stem[i] >= 'A' && stem[i] <= 'Z') stem[i] = (char)(stem[i] + 32);
    for (i = 0; ext[i]; i++) if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] = (char)(ext[i] + 32);

    if (strcmp(ext, "jpg") && strcmp(ext, "jpeg") && strcmp(ext, "png") &&
        strcmp(ext, "bmp") && strcmp(ext, "webp")) return -1;
    if (!strcmp(stem, "cover"))  return 0;
    if (!strcmp(stem, "folder")) return 1;
    if (!strcmp(stem, "front"))  return 2;
    if (!strcmp(stem, "album"))  return 3;      /* EXACT: AlbumArtSmall.jpg is a thumbnail */
    if (!strncmp(stem, "cover", 5)) return 4;
    cover_key(folder, fk, sizeof fk);
    cover_key(stem, sk, sizeof sk);
    return (fk[0] && !strcmp(fk, sk)) ? 5 : -1;
}

/* find the best cover image in dir.
 * first the ranked names above — so the desktop and the phone agree on every
 * album that has a conventional one — and, only if none of those matched, a lone
 * image, which cannot be a mispick because there is nothing to pick between.
 * Enumerated across Nova's 633 albums: five reach that fallback and every one of
 * them holds exactly one image, while no album holds several unmatched images, so
 * this can never start choosing between back.jpg, a disc scan and a spectrogram.
 * Several unmatched images decline, as they always have. */
int lib_find_cover(const char *dir, char *out, int outsz)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char best[512] = "", only[512] = "";
    const char *folder;
    int best_rank = -1, n_img = 0;

    if (!d) return -1;
    folder = strrchr(dir, '/');
    folder = folder ? folder + 1 : dir;
    while ((de = readdir(d))) {
        int r;
        if (de->d_name[0] == '.') continue;
        if (!has_img_ext(de->d_name)) continue;
        n_img++;
        if (n_img == 1) snprintf(only, sizeof only, "%s", de->d_name);
        r = cover_rank(de->d_name, folder);
        if (r < 0) continue;
        /* best rank wins; ties break on the name so the answer cannot depend on
         * the order the filesystem happens to hand entries back */
        if (best_rank < 0 || r < best_rank ||
            (r == best_rank && strcmp(de->d_name, best) < 0)) {
            best_rank = r;
            snprintf(best, sizeof best, "%s", de->d_name);
        }
    }
    closedir(d);

    if (best_rank < 0) {
        if (n_img != 1 || !only[0]) return -1;
        snprintf(out, outsz, "%s/%s", dir, only);
        return 0;
    }
    snprintf(out, outsz, "%s/%s", dir, best);
    return 0;
}
