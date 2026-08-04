/* font.h — GNU Unifont hex-file renderer.
 * Loads unifont .hex (any plane count: 4- or 5-digit codepoints), renders
 * glyphs to cached SDL textures. Pixel-perfect, no smoothing, ever. */
#ifndef MIKA_FONT_H
#define MIKA_FONT_H

#include <SDL.h>
#include "icons.h"

typedef struct {
    int cp;
    unsigned char bits[32]; /* 1bpp, 8 or 16 px wide, 16 tall, row-major */
    int w;                  /* 8 or 16 */
} FGlyph;

typedef struct {
    SDL_Renderer *ren;
    FGlyph *glyphs;
    int ng, gcap;
    /* open-addressing texture cache, keyed by codepoint */
    int *tcp;
    SDL_Texture **tt;
    int tcap;
    SDL_Texture *icon_tex[N_ICONS];
} Font;

/* returns number of glyphs loaded, or -1 if the file can't be opened */
int  font_load(Font *f, SDL_Renderer *ren, const char *hexpath);
void font_free(Font *f);

/* pixel width of s at scale (no clipping) */
int  font_w(Font *f, const char *s, int scale);
/* draw s at x,y, scale 1 or 2; returns advance in px */
int  font_draw(Font *f, int x, int y, const char *s, int scale, Uint8 r, Uint8 g, Uint8 b);
/* single glyph width (for caret placement etc) */
int  font_char_w(Font *f, int cp);
/* does the font have this codepoint? */
int  font_has(Font *f, int cp);

/* icons */
SDL_Texture *font_icon_tex(Font *f, int idx);
void font_draw_icon(Font *f, int x, int y, int idx, int scale, Uint8 r, Uint8 g, Uint8 b);

#endif
