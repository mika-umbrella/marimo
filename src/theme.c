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

/* ---------------- the mode table ---------------- */

SDL_Color C_BG0   = { 0x0D, 0x0D, 0x0F, 255 };
SDL_Color C_BG1   = { 0x15, 0x15, 0x18, 255 };
SDL_Color C_BG2   = { 0x1E, 0x1E, 0x23, 255 };
SDL_Color C_ROW   = { 0x2A, 0x2A, 0x30, 255 };
SDL_Color C_BD    = { 0x2C, 0x2C, 0x33, 255 };
SDL_Color C_TXT   = { 0xC9, 0xC9, 0xD1, 255 };
SDL_Color C_DIM   = { 0x6B, 0x6B, 0x76, 255 };
SDL_Color C_ACC   = { 0x7D, 0xFF, 0x7D, 255 };
SDL_Color C_ACC2  = { 0xB7, 0x8C, 0xFF, 255 };
SDL_Color C_ERR   = { 0xFF, 0x6B, 0x6B, 255 };
SDL_Color C_SELBG = { 0x14, 0x5C, 0x14, 255 };
SDL_Color C_AMBER = { 0xE8, 0xC3, 0x6A, 255 };

static int g_dark = 1;
static int g_a_panel = 0xCC, g_a_row = 0x66, g_a_sel = 0x8C;

static void setc(SDL_Color *c, int r, int g, int b)
{
    c->r = (Uint8)r;
    c->g = (Uint8)g;
    c->b = (Uint8)b;
    c->a = 255;
}

void theme_apply(int dark)
{
    g_dark = dark ? 1 : 0;
    if (g_dark) {
        /* the dark set is what this port has always drawn with — C_BG1/BG2 are
         * these exact values, so flipping to light and back cannot drift, and
         * txt/dim/acc agree with the phone byte for byte */
        setc(&C_BG0, 0x0D, 0x0D, 0x0F);    /* opaque: the modal's field boxes */
        setc(&C_BG1, 0x15, 0x15, 0x18);    /* Theme.panel()   dark 0xCC141418 (+1 unit) */
        setc(&C_BG2, 0x1E, 0x1E, 0x23);    /* Theme.panelHi() dark 0xE0232329 (+1 unit) */
        setc(&C_ROW, 0x2A, 0x2A, 0x30);    /* Theme.rowBg()   dark 0x662A2A30 */
        /* borders stay opaque: the phone's dividers are 0x33 alpha, which a 1px
         * line cannot afford — it would simply vanish */
        setc(&C_BD,  0x2C, 0x2C, 0x33);
        setc(&C_TXT, 0xC9, 0xC9, 0xD1);    /* = Theme.sub()  dark */
        setc(&C_DIM, 0x6B, 0x6B, 0x76);    /* = Theme.dim()  dark */
        setc(&C_ACC, 0x7D, 0xFF, 0x7D);    /* = Theme.ACC_DARK, winamp green */
        setc(&C_ACC2, 0xB7, 0x8C, 0xFF);
        setc(&C_ERR, 0xFF, 0x6B, 0x6B);
        setc(&C_SELBG, 0x14, 0x5C, 0x14);  /* Theme.rowSel() dark 0x8C145C14 */
        setc(&C_AMBER, 0xE8, 0xC3, 0x6A);
        g_a_panel = 0xCC; g_a_row = 0x66; g_a_sel = 0x8C;
    } else {
        setc(&C_BG0, 0xFF, 0xFF, 0xFF);    /* a white field on a light panel */
        setc(&C_BG1, 0xF2, 0xF0, 0xEE);    /* Theme.panel()   light 0xD9F2F0EE */
        setc(&C_BG2, 0xE3, 0xE1, 0xDE);    /* Theme.panelHi() light 0xFFE3E1DE */
        setc(&C_ROW, 0xFF, 0xFF, 0xFF);    /* Theme.rowBg()   light 0x80FFFFFF */
        setc(&C_BD,  0xC9, 0xC7, 0xC4);
        setc(&C_TXT, 0x1B, 0x1B, 0x20);    /* = Theme.txt()  light, ~10:1 on a row */
        /* The four below are NOT the phone's light values. The reason is measured:
         * Theme.ACC_LIGHT (0x1E8A1E) renders about 1.4:1 as filename text on a
         * light row, and amber about 1.6:1 — mid-luminance colours, which work as
         * a button fill or a tab underline but not as filenames.
         *
         * The first attempt at this over-corrected into 0x0A520A and nova called
         * it: a green that dark is nearly indistinguishable from the grey text,
         * because luminance contrast is the wrong yardstick for an *accent*.
         * Chroma is — how far the colour sits from grey — so these keep a real
         * hue (green 95, amber 138) and take what contrast the lighter row
         * surface can give them (~2:1, i.e. legible while still obviously green).
         * Do not simply darken these again. */
        setc(&C_DIM, 0x4A, 0x4A, 0x52);    /* neutral grey, on purpose: it must not
                                            * read as an accent. Theme.dim() light
                                            * (0x6E6E76) measures only 2:1 here. */
        setc(&C_ACC, 0x1B, 0x7A, 0x1B);    /* chroma 95 */
        setc(&C_ACC2, 0x5B, 0x2F, 0xA8);   /* chroma 121 */
        setc(&C_ERR, 0xC0, 0x39, 0x2B);
        setc(&C_SELBG, 0xB9, 0xE8, 0xB9);  /* a pale wash: dark green text on the
                                            * phone's own 0x9928B928 would vanish */
        setc(&C_AMBER, 0x8A, 0x56, 0x00);  /* chroma 138 */
        /* rows a shade lighter than the phone's 0x80 white: in light mode the
         * surface is what gives the accents their contrast, and a row that reads
         * as a surface is the whole point of the ambient fill */
        g_a_panel = 0xD9; g_a_row = 0xC0; g_a_sel = 0x99;
    }
}

int theme_is_dark(void)     { return g_dark; }
int theme_alpha_panel(void) { return g_a_panel; }
int theme_alpha_row(void)   { return g_a_row; }
int theme_alpha_sel(void)   { return g_a_sel; }
