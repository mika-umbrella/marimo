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

#include <SDL_image.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* the artist is whatever sits before the first " - " */
    dash = strstr(work, " - ");
    if (dash && artist && asz) {
        size_t alen = (size_t)(dash - work);
        if (alen >= asz) alen = asz - 1;
        memcpy(artist, work, alen);
        artist[alen] = 0;
        while (alen && artist[alen - 1] == ' ') artist[--alen] = 0;
        work[0] = 0;
        snprintf(work, sizeof work, "%s", dash + 3);
    }

    /* the title keeps everything except bracket groups that are year/date, and
     * any trailing [FORMAT] — the row shows those separately */
    for (i = 0; work[i] && o + 1 < sizeof out; i++) {
        if (work[i] == '(' || work[i] == '[') {
            char close = work[i] == '(' ? ')' : ']';
            const char *end = strchr(work + i + 1, close);
            if (end) {
                if (bracket_is_year(work + i, end)) { i = (size_t)(end - work); continue; }
                if (work[i] == '[') { i = (size_t)(end - work); continue; }  /* [FLAC] etc */
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

#define THUMBS 96
#define THUMB_PER_FRAME 2      /* enough to feel instant, never enough to hitch */

typedef struct {
    char dir[1024];
    int size;
    SDL_Texture *tex;
    int state;                 /* 0 empty, 1 loaded, 2 no cover / give up */
} Thumb;

static Thumb thumbs[THUMBS];
static int thumbs_next;
static int thumb_budget;

void album_frame(void)
{
    thumb_budget = THUMB_PER_FRAME;
}

SDL_Texture *album_thumb(SDL_Renderer *ren, const char *dir, int size)
{
    Thumb *slot = NULL;
    int i;
    char cov[1024];

    if (!ren || !dir || !*dir) return NULL;
    for (i = 0; i < THUMBS; i++) {
        if (thumbs[i].dir[0] && !strcmp(thumbs[i].dir, dir) && thumbs[i].size == size) {
            if (thumbs[i].state == 1) return thumbs[i].tex;
            /* a pending or failed slot: try again only if this frame still has
             * budget, otherwise leave it and draw without art for now */
            if (thumbs[i].state == 2) return NULL;
            slot = &thumbs[i];
            break;
        }
    }
    if (!slot) {
        for (i = 0; i < THUMBS; i++) {
            if (thumbs[i].dir[0]) continue;
            slot = &thumbs[i];
            break;
        }
        if (!slot) {   /* cache full: evict in creation order */
            slot = &thumbs[thumbs_next++ % THUMBS];
            if (slot->tex) SDL_DestroyTexture(slot->tex);
            slot->tex = NULL;
        }
        snprintf(slot->dir, sizeof slot->dir, "%s", dir);
        slot->size = size;
        slot->state = 0;
    }
    if (thumb_budget <= 0) return NULL;   /* come back next frame */
    thumb_budget--;

    if (lib_find_cover(dir, cov, sizeof cov) != 0 || !cov[0]) {
        slot->state = 2;
        return NULL;
    }
    {
        SDL_Surface *raw = IMG_Load(cov);
        SDL_Surface *small, *conv;
        if (!raw) { slot->state = 2; return NULL; }
        small = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!small) { SDL_FreeSurface(raw); slot->state = 2; return NULL; }
        /* letterbox rather than crop: a non-square cover should still be
         * recognisable, and the background is drawn under it anyway */
        {
            SDL_Rect dst = { 0, 0, size, size };
            double ar = (double)raw->w / (double)raw->h;
            if (raw->w > raw->h) {
                dst.h = (int)(size / ar);
                dst.y = (size - dst.h) / 2;
            } else if (raw->h > raw->w) {
                dst.w = (int)(size * ar);
                dst.x = (size - dst.w) / 2;
            }
            SDL_FillRect(small, NULL, SDL_MapRGBA(small->format, 0, 0, 0, 0));
            conv = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
            if (conv) {
                SDL_BlitScaled(conv, NULL, small, &dst);
                SDL_FreeSurface(conv);
            }
        }
        SDL_FreeSurface(raw);
        slot->tex = SDL_CreateTextureFromSurface(ren, small);
        SDL_FreeSurface(small);
        if (!slot->tex) { slot->state = 2; return NULL; }
        SDL_SetTextureBlendMode(slot->tex, SDL_BLENDMODE_BLEND);
        slot->state = 1;
        return slot->tex;
    }
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
}
