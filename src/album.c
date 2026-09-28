/* album.c — folder-name metadata and cover thumbnails.
 *
 * The year parser is a port of Album.java (marimo-android), and the ORDER of the
 * four passes below is the entire value of it. Real folders look like
 * "23.exe - (2020) WALK [FLAC] {2025 13433-8873443}" and every naive reading of
 * that gets 2025 (a catalogue id) or 13433. If you change the order, re-run the
 * selftest vectors, which are the cases that were checked against the Java
 * implementation itself. */
#ifndef _GNU_SOURCE          /* gio-2.0's pkg-config cflags already define this */
#define _GNU_SOURCE
#endif
#include "album.h"
#include "library.h"
#include "libcache.h"
#include "tags.h"

#include <pthread.h>
#include <stdint.h>

#include <SDL_image.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------------- year ---------------- */

/* pass 1: {barcode-ish} groups are release ids, never years. Removing them FIRST
 * is what stops "2025 13433-8873443" from being read as 2025. */
static void strip_braces(const char *in, char *out, size_t n)
{
    size_t o = 0;
    int depth = 0;
    for (; *in && o + 1 < n; in++) {
        if (*in == '{') { depth++; continue; }
        if (*in == '}') { if (depth > 0) depth--; continue; }
        if (depth == 0) out[o++] = *in;
    }
    out[o] = 0;
}

static int plausible(int y)
{
    return y >= 1900 && y <= 2099;
}

/* first 4-digit run in `s` that is plausible, or 0. Used both for bracket
 * contents (dates like "2012-01-01" or "01.01.2025") and for the bare pass. */
static int first_year_in(const char *s, int require_delimited)
{
    int best = 0;
    const char *p = s;
    while (*p) {
        if (!isdigit((unsigned char)*p)) { p++; continue; }
        {
            const char *start = p;
            while (isdigit((unsigned char)*p)) p++;
            if (p - start == 4) {
                int y = (start[0] - '0') * 1000 + (start[1] - '0') * 100
                      + (start[2] - '0') * 10 + (start[3] - '0');
                /* a bare year must stand alone: not glued to more digits, and not
                 * part of a range like "1988-1991" */
                int clean = 1;
                if (require_delimited) {
                    if (p - start > 4) clean = 0;
                    if (start > s && (start[-1] == '-' || isdigit((unsigned char)start[-1]))) clean = 0;
                    if (*p == '-' || isdigit((unsigned char)*p)) clean = 0;
                }
                if (clean && plausible(y) && (best == 0 || y < best)) best = y;
            }
        }
    }
    return best;
}

int album_year(const char *folder)
{
    char work[1024], *p;
    int best = 0;

    if (!folder) return 0;
    strip_braces(folder, work, sizeof work);

    /* pass 2: a bracketed year is authoritative. Round and square brackets are
     * scanned independently — a round open must not be closed by a square — which
     * is what keeps "(2023) ... (2026 Remaster)" from crossing. Content with
     * letters is not a year ("2026 Remaster"), and for a list like "(1999, 2008)"
     * the earliest wins, i.e. the original release. */
    for (p = work; *p; p++) {
        char close = *p == '(' ? ')' : *p == '[' ? ']' : 0;
        char *end;
        if (!close) continue;
        end = strchr(p + 1, close);
        if (!end) continue;
        {
            char content[256];
            size_t len = (size_t)(end - p - 1);
            int has_letter = 0, y, i;
            if (len >= sizeof content) len = sizeof content - 1;
            memcpy(content, p + 1, len);
            content[len] = 0;
            for (i = 0; content[i]; i++)
                if (isalpha((unsigned char)content[i])) { has_letter = 1; break; }
            if (!has_letter) {
                y = first_year_in(content, 0);
                if (y && (best == 0 || y < best)) best = y;
            }
        }
        p = end;   /* continue after the group, so the next bracket is scanned */
    }
    if (best) return best;

    /* pass 3/4: nothing bracketed won, so a bare standalone year may, gated to a
     * plausible range — "9801" is a number, not a year. */
    return first_year_in(work, 1);
}

/* ---------------- artist / title ---------------- */

/* does this bracket group hold a year rather than part of the name? */
static int bracket_is_year(const char *open, const char *close)
{
    char content[256];
    size_t len = (size_t)(close - open - 1);
    int i;
    if (len >= sizeof content) return 0;
    memcpy(content, open + 1, len);
    content[len] = 0;
    for (i = 0; content[i]; i++)
        if (isalpha((unsigned char)content[i])) return 0;
    return first_year_in(content, 0) != 0;
}

/* Does this bracket group name a disc or volume, rather than a format? "[FLAC]"
 * and "[MP3 320]" describe the file and the row shows them elsewhere; "[Disc 1
 * 2009]" names which disc this is and has to stay in the title. */
static int bracket_is_disc(const char *open, const char *close)
{
    char content[256];
    size_t len = (size_t)(close - open - 1), i;
    if (len >= sizeof content) return 0;
    memcpy(content, open + 1, len);
    content[len] = 0;
    for (i = 0; content[i]; i++) content[i] = (char)tolower((unsigned char)content[i]);
    return strstr(content, "disc") || strstr(content, "disk") || strstr(content, "vol")
           ? 1 : 0;
}

/* Where the artist/title separator sits, or NULL. Any dash character counts —
 * ascii '-', the en and em dashes, the horizontal bar, the minus sign — as long as
 * it has a space on either side; how many spaces follow does not matter.
 *
 * Her folders mix all of them: 618 use '-', but 21 use an em or en dash, and
 * looking only for the literal " - " meant those 21 showed the whole folder name as
 * the title with the artist column reduced to a bare year. */
static const char *split_sep(const char *s)
{
    const char *p;
    for (p = s; *p; p++) {
        int len = 0;
        if (*p == '-') {
            len = 1;
        } else if ((unsigned char)p[0] == 0xE2 && p[1] && p[2]) {
            unsigned char b = (unsigned char)p[1], c = (unsigned char)p[2];
            if ((b == 0x80 && c >= 0x90 && c <= 0x95) || (b == 0x88 && c == 0x92)) len = 3;
        }
        if (!len) continue;
        if (p == s || p[-1] != ' ') continue;          /* needs space before */
        if (p[len] != ' ') continue;                   /* ...and after */
        return p;
    }
    return NULL;
}

void album_split(const char *folder, char *artist, size_t asz,
                 char *title, size_t tsz)
{
    char work[1024], out[1024];
    const char *dash;
    size_t o = 0, len, i;

    if (artist && asz) artist[0] = 0;
    if (title && tsz) title[0] = 0;
    if (!folder) return;
    strip_braces(folder, work, sizeof work);

    /* the artist is whatever sits before the separator */
    dash = split_sep(work);
    if (dash && artist && asz) {
        size_t alen = (size_t)(dash - work);
        const char *rest = dash;
        char tail[1024];
        if (alen >= asz) alen = asz - 1;
        memcpy(artist, work, alen);
        artist[alen] = 0;
        while (alen && artist[alen - 1] == ' ') artist[--alen] = 0;
        /* past the separator itself, then past whatever spaces it carries — one in
         * most folders, two in one of hers, and the old code assumed exactly one */
        rest += (*rest == '-') ? 1 : 3;
        while (*rest == ' ') rest++;
        snprintf(tail, sizeof tail, "%s", rest);
        snprintf(work, sizeof work, "%s", tail);
    }

    /* the title keeps everything except bracket groups that are year/date, and
     * any trailing [FORMAT] — the row shows those separately */
    for (i = 0; work[i] && o + 1 < sizeof out; i++) {
        if (work[i] == '(' || work[i] == '[') {
            char close = work[i] == '(' ? ')' : ']';
            const char *end = strchr(work + i + 1, close);
            if (end) {
                if (bracket_is_year(work + i, end)) { i = (size_t)(end - work); continue; }
                /* A disc/volume marker is part of the name, not a format tag. Both
                 * discs of TRAIL are named "TRAIL [Disc N YYYY]", so stripping the
                 * group as if it were [FLAC] gave two rows both called "TRAIL" —
                 * the year survived the bare pass, the disc marker did not. The
                 * files' own album tag reads "TRAIL [Disc 1 2009]", so keeping the
                 * group is also what the tags say. */
                if (work[i] == '[' && !bracket_is_disc(work + i, end)) {
                    i = (size_t)(end - work);
                    continue;  /* [FLAC] etc */
                }
            }
        }
        out[o++] = work[i];
    }
    out[o] = 0;
    /* tidy the whitespace the removals left behind */
    {
        char *s = out;
        while (*s == ' ') s++;
        len = strlen(s);
        while (len && s[len - 1] == ' ') s[--len] = 0;
        if (title && tsz) snprintf(title, tsz, "%s", s);
    }
}

/* ---------------- track counts ---------------- */

/* The count itself lives in libcache: same number, but kept on disk, so it
 * survives a restart and only folders whose mtime changed are ever opened again.
 * The cache was written for the album rows asking for it every frame — over CIFS
 * that was a readdir per row, per view. */
int album_tracks(const char *dir)
{
    return libcache_tracks(dir);
}

/* ---------------- cover thumbnails ---------------- */

#define THUMBS 128
#define THUMB_MAX 64           /* nothing asks for a bigger square than this */

typedef struct {
    char dir[1024];
    int size;
    SDL_Texture *tex;
    int state;                 /* 0 empty, 1 loaded, 2 no cover (remembered) */
} Thumb;

static Thumb thumbs[THUMBS];
static int thumbs_next;
static int thumb_decoded, thumb_from_disk;

void album_thumb_stats(int *decoded, int *cached)
{
    if (decoded) *decoded = thumb_decoded;
    if (cached) *cached = thumb_from_disk;
}

/* Decoding happens here, never on the render thread. Some of these covers are
 * 25 MB JPEGs (that is not a typo — the census found one at 25,251,998 bytes) and
 * IMG_Load expands them fully, to be shrunk to a 16px square. Doing that inline,
 * twice a frame, was the scrolling stutter nova felt.
 *
 * One worker and one job at a time on purpose: a cold pass fills in over about a
 * second without blocking a frame, and with the disk cache warm it is instant, so
 * a second worker would buy nothing but bookkeeping. */
static struct {
    pthread_mutex_t lock;
    pthread_t thr;
    int running;
    char job_dir[1024];
    int job_size;
    int have_result;           /* the main thread has not collected it yet */
    char done_dir[1024];
    int done_size;
    int done_ok;
    unsigned char px[THUMB_MAX * THUMB_MAX * 4];
} tw;

static uint64_t thumb_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ull;
    }
    return h;
}

/* ~/.cache/marimo/thumbs/<hash>-<size>.raw — exactly size*size*4 bytes of ARGB,
 * so a short or stale file is rejected by its length and never needs a header. */
static int thumb_cache_path(const char *dir, int size, char *out, size_t n)
{
    const char *base = getenv("XDG_CACHE_HOME");
    char d[1024];
    if (base && *base) snprintf(d, sizeof d, "%s/marimo/thumbs", base);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return 0;
        snprintf(d, sizeof d, "%s/.cache/marimo/thumbs", home);
    }
    mkdir(d, 0755);      /* EEXIST is the normal case */
    snprintf(out, n, "%s/%016llx-%d.raw", d, (unsigned long long)thumb_hash(dir), size);
    return 1;
}

/* the whole decode + shrink, with no SDL renderer anywhere in it, so it is safe
 * on the worker */
static int thumb_decode(const char *dir, int size, unsigned char *out)
{
    char cov[1024];
    SDL_Surface *raw, *conv, *small;
    int ok = 0;

    if (lib_find_cover(dir, cov, sizeof cov) != 0 || !cov[0]) return 0;
    raw = IMG_Load(cov);
    if (!raw) return 0;
    conv = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(raw);
    if (!conv) return 0;
    small = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (small) {
        /* letterbox, not crop: a non-square cover should still be recognisable */
        SDL_Rect dst = { 0, 0, size, size };
        double ar = (double)conv->w / (double)conv->h;
        if (conv->w > conv->h) {
            dst.h = (int)(size / ar);
            dst.y = (size - dst.h) / 2;
        } else if (conv->h > conv->w) {
            dst.w = (int)(size * ar);
            dst.x = (size - dst.w) / 2;
        }
        SDL_FillRect(small, NULL, 0);
        SDL_BlitScaled(conv, NULL, small, &dst);
        {
            int y;
            for (y = 0; y < size; y++)
                memcpy(out + (size_t)y * size * 4,
                       (unsigned char *)small->pixels + (size_t)y * small->pitch,
                       (size_t)size * 4);
        }
        ok = 1;
        SDL_FreeSurface(small);
    }
    SDL_FreeSurface(conv);
    return ok;
}

static void *thumb_thread(void *unused)
{
    char dir[1024];
    int size;
    unsigned char px[THUMB_MAX * THUMB_MAX * 4];
    int ok;
    char path[1200];
    (void)unused;

    pthread_mutex_lock(&tw.lock);
    snprintf(dir, sizeof dir, "%s", tw.job_dir);
    size = tw.job_size;
    pthread_mutex_unlock(&tw.lock);

    ok = thumb_decode(dir, size, px);
    if (ok) {
        /* a successful thumbnail is worth keeping; a missing cover is not (she
         * may drop one in later, and a cached miss would never notice) */
        if (thumb_cache_path(dir, size, path, sizeof path)) {
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(px, 1, (size_t)size * size * 4, f);
                fclose(f);
            }
        }
    }
    pthread_mutex_lock(&tw.lock);
    if (ok) memcpy(tw.px, px, (size_t)size * size * 4);
    tw.done_ok = ok;
    tw.done_size = size;
    snprintf(tw.done_dir, sizeof tw.done_dir, "%s", dir);
    tw.have_result = 1;
    tw.running = 0;
    pthread_mutex_unlock(&tw.lock);
    /* counted here, not where the texture is collected: the collection site can
     * also be reached via the disk path, which made a cold run report "0 decoded,
     * 14 from disk" — its own freshly written files read straight back. */
    if (ok) thumb_decoded++;
    return NULL;
}

static SDL_Texture *tex_from_px(SDL_Renderer *ren, const unsigned char *px, int size)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)px, size, size, 32,
                                                        size * 4, SDL_PIXELFORMAT_ARGB8888);
    SDL_Texture *t = NULL;
    if (s) {
        t = SDL_CreateTextureFromSurface(ren, s);
        SDL_FreeSurface(s);
    }
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

SDL_Texture *album_thumb(SDL_Renderer *ren, const char *dir, int size)
{
    Thumb *slot = NULL;
    char path[1200];
    int i;

    if (!ren || !dir || !*dir) return NULL;
    if (size > THUMB_MAX) size = THUMB_MAX;
    if (size < 1) return NULL;

    for (i = 0; i < THUMBS; i++) {
        if (thumbs[i].dir[0] && !strcmp(thumbs[i].dir, dir) && thumbs[i].size == size) {
            if (thumbs[i].state == 1) return thumbs[i].tex;
            return NULL;            /* known to have no cover */
        }
    }

    /* 1. the disk cache: one small read, no decode at all */
    if (thumb_cache_path(dir, size, path, sizeof path)) {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size == (long long)size * size * 4) {
            FILE *f = fopen(path, "rb");
            unsigned char *buf = malloc((size_t)size * size * 4);
            if (f && buf && fread(buf, 1, (size_t)size * size * 4, f) == (size_t)size * size * 4) {
                SDL_Texture *t = tex_from_px(ren, buf, size);
                fclose(f);
                free(buf);
                if (t) {
                    thumb_from_disk++;
                    slot = &thumbs[thumbs_next++ % THUMBS];
                    if (slot->tex) SDL_DestroyTexture(slot->tex);
                    snprintf(slot->dir, sizeof slot->dir, "%s", dir);
                    slot->size = size;
                    slot->state = 1;
                    slot->tex = t;
                    return t;
                }
            } else {
                if (f) fclose(f);
                free(buf);
            }
        }
    }

    /* 2. a finished decode waiting to be turned into a texture */
    pthread_mutex_lock(&tw.lock);
    if (tw.have_result && tw.done_size == size && !strcmp(tw.done_dir, dir)) {
        int ok = tw.done_ok;
        tw.have_result = 0;
        if (ok) {
            SDL_Texture *t = tex_from_px(ren, tw.px, size);
            pthread_mutex_unlock(&tw.lock);
            if (t) {
                slot = &thumbs[thumbs_next++ % THUMBS];
                if (slot->tex) SDL_DestroyTexture(slot->tex);
                snprintf(slot->dir, sizeof slot->dir, "%s", dir);
                slot->size = size;
                slot->state = 1;
                slot->tex = t;
                return t;
            }
            return NULL;
        }
        pthread_mutex_unlock(&tw.lock);
        /* remembered as artless, so this folder is not asked about every frame */
        slot = &thumbs[thumbs_next++ % THUMBS];
        if (slot->tex) SDL_DestroyTexture(slot->tex);
        slot->tex = NULL;
        snprintf(slot->dir, sizeof slot->dir, "%s", dir);
        slot->size = size;
        slot->state = 2;
        return NULL;
    }
    /* 3. nothing ready: hand it to the worker and draw the icon this frame */
    if (!tw.running) {
        snprintf(tw.job_dir, sizeof tw.job_dir, "%s", dir);
        tw.job_size = size;
        tw.running = 1;
        if (pthread_create(&tw.thr, NULL, thumb_thread, NULL) == 0) {
            pthread_detach(tw.thr);
        } else {
            tw.running = 0;
        }
    }
    pthread_mutex_unlock(&tw.lock);
    return NULL;
}

/* ---------------- track tags, from a cache ---------------- */

#define TAG_CACHE 512
static struct {
    char path[1024];
    int ok;
    char title[256];
    char artist[256];
    int secs;
    int track, disc;       /* shown as the row's number, queue style */
} tagcache[TAG_CACHE];
static int tagcache_next;

int album_track_tags(const char *path, char *title, size_t tn, char *artist, size_t an,
                     int *secs, int *track, int *disc)
{
    Meta m;
    int i;

    if (title && tn) title[0] = 0;
    if (artist && an) artist[0] = 0;
    if (secs) *secs = 0;
    if (track) *track = 0;
    if (disc) *disc = 0;
    if (!path || !*path) return 0;

    for (i = 0; i < TAG_CACHE; i++) {
        if (tagcache[i].path[0] && !strcmp(tagcache[i].path, path)) {
            if (!tagcache[i].ok) return 0;
            if (title) snprintf(title, tn, "%s", tagcache[i].title);
            if (artist) snprintf(artist, an, "%s", tagcache[i].artist);
            if (secs) *secs = tagcache[i].secs;
            if (track) *track = tagcache[i].track;
            if (disc) *disc = tagcache[i].disc;
            return 1;
        }
    }

    /* absent: read it once. The FLAC/MP3 path is the fast deterministic one; the
     * rows ask about every visible file on every frame, which is why this is
     * cached at all. */
    memset(&m, 0, sizeof m);
    {
        int ok = (tag_read_meta(path, &m) == 0) && (m.title[0] || m.artist[0] || m.duration_ms > 0);
        i = tagcache_next++ % TAG_CACHE;
        snprintf(tagcache[i].path, sizeof tagcache[i].path, "%s", path);
        tagcache[i].ok = ok;
        snprintf(tagcache[i].title, sizeof tagcache[i].title, "%.240s", ok ? m.title : "");
        snprintf(tagcache[i].artist, sizeof tagcache[i].artist, "%.240s", ok ? m.artist : "");
        tagcache[i].secs = (ok && m.duration_ms > 0) ? (int)(m.duration_ms / 1000) : 0;
        tagcache[i].track = (ok && m.track > 0) ? m.track : 0;
        tagcache[i].disc = (ok && m.disc > 0) ? m.disc : 0;
        if (!ok) return 0;
        if (title) snprintf(title, tn, "%s", tagcache[i].title);
        if (artist) snprintf(artist, an, "%s", tagcache[i].artist);
        if (secs) *secs = tagcache[i].secs;
        if (track) *track = tagcache[i].track;
        if (disc) *disc = tagcache[i].disc;
        return 1;
    }
}

/* ---------------- display dashes ---------------- */

void album_dashes(const char *in, char *out, size_t n)
{
    size_t o = 0;
    const char *p = in ? in : "";
    while (*p && o + 1 < n) {
        unsigned char c = (unsigned char)*p;
        /* U+2010..U+2015 and U+2212: the dashes her folders mix in freely. Display
         * only — the name on disk is what queue.dat, the scrobbles and the cover
         * matcher key off, so this must never write back. */
        if (c == 0xE2 && p[1] && p[2] &&
            (((unsigned char)p[1] == 0x80 && (unsigned char)p[2] >= 0x90 &&
              (unsigned char)p[2] <= 0x95) ||
             ((unsigned char)p[1] == 0x88 && (unsigned char)p[2] == 0x92))) {
            out[o++] = '-';
            p += 3;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

void album_free(void)
{
    int i;
    for (i = 0; i < THUMBS; i++) {
        if (thumbs[i].tex) SDL_DestroyTexture(thumbs[i].tex);
        thumbs[i].tex = NULL;
        thumbs[i].dir[0] = 0;
        thumbs[i].state = 0;
    }
    for (i = 0; i < TAG_CACHE; i++) tagcache[i].path[0] = 0;
}
