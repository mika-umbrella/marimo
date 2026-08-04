/* font.c — GNU Unifont hex parser + SDL texture cache.
 * Parses the standard unifont .hex format (handles 4- and 5-digit
 * codepoints, optional '*' checksum prefix, 8x16 and 16x16 glyphs).
 * Glyphs are rendered white and tinted per-draw via color mod. */
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decode one UTF-8 sequence, advance pointer */
static int utf8_dec(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    unsigned char c = p[0];
    int cp, n, i;
    if (c < 0x80) { (*pp)++; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { (*pp)++; return 0xFFFD; }
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { (*pp)++; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp = p + n + 1;
    return cp;
}

static int glyph_cmp(const void *a, const void *b)
{
    return ((const FGlyph *)a)->cp - ((const FGlyph *)b)->cp;
}

static FGlyph *find_glyph(Font *f, int cp)
{
    int lo = 0, hi = f->ng - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static SDL_Texture *bits_to_tex(SDL_Renderer *ren, const unsigned char *bits, int w, int h)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Texture *t;
    Uint32 *px;
    int stride = (w + 7) / 8;
    int x, y;
    if (!s) return NULL;
    SDL_LockSurface(s);
    px = (Uint32 *)s->pixels;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (bits[y * stride + (x >> 3)] & (0x80 >> (x & 7)))
                px[y * w + x] = 0xFFFFFFFF;
    SDL_UnlockSurface(s);
    t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    return t;
}

int font_load(Font *f, SDL_Renderer *ren, const char *hexpath)
{
    FILE *fp;
    char line[1024];
    memset(f, 0, sizeof(*f));
    f->ren = ren;
    f->tcap = 1 << 14;
    f->tcp = (int *)calloc(f->tcap, sizeof(int));
    f->tt = (SDL_Texture **)calloc(f->tcap, sizeof(SDL_Texture *));
    fp = fopen(hexpath, "rb");
    if (!fp) { free(f->tcp); free(f->tt); return -1; }
    while (fgets(line, sizeof line, fp)) {
        char *p = line, *colon, *b;
        int cp = 0, n, gw, i;
        if (*p == '*') p++;
        if (!isxdigit((unsigned char)*p)) continue;
        colon = strchr(p, ':');
        if (!colon) continue;
        for (char *q = p; q < colon; q++) {
            int v = hexv(*q);
            if (v < 0) goto badline;
            cp = cp * 16 + v;
        }
        b = colon + 1;
        n = 0;
        while (b[n] && b[n] != '\n' && b[n] != '\r') n++;
        while (n && !isxdigit((unsigned char)b[n - 1])) n--; /* trailing crud */
        /* hex chars: 16x16 glyph = 32 bytes = 64 chars; 8x16 = 16 bytes = 32 chars */
        if (n != 32 && n != 64) goto badline;
        gw = n / 4;
        if (f->ng == f->gcap) {
            f->gcap = f->gcap ? f->gcap * 2 : 4096;
            f->glyphs = (FGlyph *)realloc(f->glyphs, f->gcap * sizeof(FGlyph));
        }
        {
            FGlyph *gl = &f->glyphs[f->ng++];
            gl->cp = cp;
            gl->w = gw;
            memset(gl->bits, 0, 32);
            for (i = 0; i + 1 < n; i += 2) {
                int hv = hexv(b[i]);
                int lv = hexv(b[i + 1]);
                int v, row, col;
                if (hv < 0 || lv < 0) continue;
                v = hv * 16 + lv;
                row = (i / 2) / (gw / 8);
                col = (i / 2) % (gw / 8);
                for (int bit = 0; bit < 8; bit++) {
                    if (v & (1 << (7 - bit))) {
                        int x = col * 8 + bit;
                        gl->bits[row * (gw / 8) + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                    }
                }
            }
        }
    badline: ;
    }
    fclose(fp);
    qsort(f->glyphs, f->ng, sizeof(FGlyph), glyph_cmp);
    return f->ng;
}

void font_free(Font *f)
{
    int i;
    for (i = 0; i < f->tcap; i++)
        if (f->tt[i]) SDL_DestroyTexture(f->tt[i]);
    for (i = 0; i < N_ICONS; i++)
        if (f->icon_tex[i]) SDL_DestroyTexture(f->icon_tex[i]);
    free(f->tcp);
    free(f->tt);
    free(f->glyphs);
    memset(f, 0, sizeof(*f));
}

/* lazily build + return the texture for cp; *w gets glyph width */
static SDL_Texture *glyph_tex(Font *f, int cp, int *w)
{
    FGlyph *g = find_glyph(f, cp);
    int slot, i;
    if (!g) {
        g = find_glyph(f, 0xFFFD);
        if (!g) { *w = 16; return NULL; }
    }
    *w = g->w;
    slot = (int)((unsigned)(cp * 2654435761u) & (f->tcap - 1));
    for (i = 0; i < f->tcap; i++) {
        int idx = (slot + i) & (f->tcap - 1);
        if (f->tcp[idx] == 0) {
            f->tcp[idx] = cp;
            f->tt[idx] = bits_to_tex(f->ren, g->bits, g->w, 16);
            return f->tt[idx];
        }
        if (f->tcp[idx] == cp) return f->tt[idx];
    }
    return NULL;
}

int font_char_w(Font *f, int cp)
{
    FGlyph *g = find_glyph(f, cp);
    return g ? g->w : 16;
}

int font_has(Font *f, int cp)
{
    return find_glyph(f, cp) != NULL;
}

int font_w(Font *f, const char *s, int scale)
{
    const unsigned char *p = (const unsigned char *)s;
    int w = 0;
    while (*p) {
        int cp = utf8_dec(&p);
        FGlyph *g = find_glyph(f, cp);
        w += (g ? g->w : 16) * scale;
    }
    return w;
}

int font_draw(Font *f, int x, int y, const char *s, int scale, Uint8 r, Uint8 g, Uint8 b)
{
    const unsigned char *p = (const unsigned char *)s;
    int xx = x;
    while (*p) {
        int cp = utf8_dec(&p);
        int gw;
        SDL_Texture *tex = glyph_tex(f, cp, &gw);
        SDL_Rect dst;
        if (!tex) { xx += gw * scale; continue; }
        SDL_SetTextureColorMod(tex, r, g, b);
        dst.x = xx; dst.y = y; dst.w = gw * scale; dst.h = 16 * scale;
        SDL_RenderCopy(f->ren, tex, NULL, &dst);
        xx += gw * scale;
    }
    return xx - x;
}

SDL_Texture *font_icon_tex(Font *f, int idx)
{
    SDL_Surface *s;
    SDL_Texture *t;
    Uint32 *px;
    if (f->icon_tex[idx]) return f->icon_tex[idx];
    s = SDL_CreateRGBSurfaceWithFormat(0, 16, 16, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    SDL_LockSurface(s);
    px = (Uint32 *)s->pixels;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (icon_art[idx][y][x] == '#')
                px[y * 16 + x] = 0xFFFFFFFF;
    SDL_UnlockSurface(s);
    t = SDL_CreateTextureFromSurface(f->ren, s);
    SDL_FreeSurface(s);
    f->icon_tex[idx] = t;
    return t;
}

void font_draw_icon(Font *f, int x, int y, int idx, int scale, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_Texture *t = font_icon_tex(f, idx);
    SDL_Rect dst;
    if (!t) return;
    SDL_SetTextureColorMod(t, r, g, b);
    dst.x = x; dst.y = y; dst.w = 16 * scale; dst.h = 16 * scale;
    SDL_RenderCopy(f->ren, t, NULL, &dst);
}
