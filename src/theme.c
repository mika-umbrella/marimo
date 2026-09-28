/* theme.c — see theme.h. The arithmetic here mirrors BgManager.java; if the two
 * ever disagree, the phone is right and this file is the bug. */
#include "theme.h"

#include <SDL_image.h>
#include <math.h>
#include <string.h>

#define PAL_SAMPLES 4000.0     /* BgManager samples about this many pixels */
#define PAL_LIGHT   0.60f      /* luma > this  -> the light bucket */
#define PAL_DARK    0.38f      /* luma < this  -> the dark bucket */

static SDL_Color rgb(int r, int g, int b)
{
    SDL_Color c;
    c.r = (Uint8)(r < 0 ? 0 : r > 255 ? 255 : r);
    c.g = (Uint8)(g < 0 ? 0 : g > 255 ? 255 : g);
    c.b = (Uint8)(b < 0 ? 0 : b > 255 ? 255 : b);
    c.a = 255;
    return c;
}

/* channels of a and b mixed by t in [0,1] — Java's ColorUtils-style blend,
 * rounded the same way (+0.5 before the cast) */
static SDL_Color mix(SDL_Color a, SDL_Color b, double t)
{
    return rgb((int)(a.r + (b.r - a.r) * t + 0.5),
               (int)(a.g + (b.g - a.g) * t + 0.5),
               (int)(a.b + (b.b - a.b) * t + 0.5));
}

static Palette fallback(void)
{
    Palette p;
    p.light = rgb(0x3A, 0x42, 0x58);
    p.mid   = rgb(0x23, 0x28, 0x38);
    p.dark  = rgb(0x0D, 0x0F, 0x14);
    p.from_art = 0;
    return p;
}

Palette palette_from_cover(const char *path)
{
    SDL_Surface *raw;
    Palette p;

    if (!path || !*path) return fallback();
    raw = IMG_Load(path);
    if (!raw) return fallback();
    p = palette_from_surface(raw);
    SDL_FreeSurface(raw);
    return p;
}

Palette palette_from_surface(SDL_Surface *in)
{
    Palette p;
    SDL_Surface *s = in;
    int converted = 0;
    long lr = 0, lg = 0, lb = 0, lcnt = 0;
    long mr = 0, mg = 0, mb = 0, mcnt = 0;
    long dr = 0, dg = 0, db = 0, dcnt = 0;
    int stride, x, y;

    if (!s || s->w <= 0 || s->h <= 0) return fallback();
    if (s->format->format != SDL_PIXELFORMAT_ARGB8888) {
        s = SDL_ConvertSurfaceFormat(in, SDL_PIXELFORMAT_ARGB8888, 0);
        if (!s) return fallback();
        converted = 1;
    }

    stride = (int)sqrt((double)s->w * (double)s->h / PAL_SAMPLES);
    if (stride < 1) stride = 1;

    for (y = 0; y < s->h; y += stride) {
        const Uint32 *row = (const Uint32 *)((const Uint8 *)s->pixels + (size_t)y * s->pitch);
        for (x = 0; x < s->w; x += stride) {
            Uint32 px = row[x];
            int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
            float lum = (r * 0.3f + g * 0.59f + b * 0.11f) / 255.0f;
            if (lum > PAL_LIGHT)      { lr += r; lg += g; lb += b; lcnt++; }
            else if (lum < PAL_DARK)  { dr += r; dg += g; db += b; dcnt++; }
            else                      { mr += r; mg += g; mb += b; mcnt++; }
        }
    }
    if (converted) SDL_FreeSurface(s);

    p.light = lcnt > 0 ? rgb((int)(lr / lcnt), (int)(lg / lcnt), (int)(lb / lcnt))
                       : rgb(0x9F, 0xC4, 0xD4);
    p.dark  = dcnt > 0 ? rgb((int)(dr / dcnt), (int)(dg / dcnt), (int)(db / dcnt))
                       : mix(p.light, rgb(0x20, 0x24, 0x2E), 0.7);
    p.mid   = mcnt > 0 ? rgb((int)(mr / mcnt), (int)(mg / mcnt), (int)(mb / mcnt))
                       : mix(p.light, p.dark, 0.5);
    p.from_art = 1;
    return p;
}

SDL_Color palette_scrim(const Palette *p, int dark)
{
    /* the mid tone, scrimmed the way the phone scrims its gradient: 0x99 black
     * in dark mode, 0x66 white in light. The mid tone is the right one to stand
     * in for a flat backdrop — the phone's noise wanders between all three, so
     * anything else would read as lighter or darker than the real thing. */
    if (dark) return mix(p->mid, rgb(0, 0, 0), 0x99 / 255.0);
    return mix(p->mid, rgb(255, 255, 255), 0x66 / 255.0);
}
