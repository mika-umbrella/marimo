/* bg.c — see bg.h. Where this file and BgManager.java disagree, BgManager.java
 * is right and this is the bug; every rounding and every float/int choice below
 * was copied deliberately so the two can be compared byte for byte. */
#ifndef _GNU_SOURCE          /* gio-2.0's pkg-config cflags already define this */
#define _GNU_SOURCE
#endif
#include "bg.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- java.util.Random, faithfully ---------------- */

/* The phone's grid comes from `new Random(0xC0FFEE)`, so matching it needs the
 * real LCG (48-bit, 0x5DEECE66D, 0xB) and Java's nextFloat (next(24)/2^24), not
 * any reasonable-looking PRNG. */
static uint64_t jr_seed;
#define JR_MASK ((1ull << 48) - 1)

static void jr_init(uint64_t seed)
{
    jr_seed = (seed ^ 0x5DEECE66Dull) & JR_MASK;
}

static int32_t jr_next(int bits)
{
    jr_seed = (jr_seed * 0x5DEECE66Dull + 0xBull) & JR_MASK;
    return (int32_t)(jr_seed >> (48 - bits));
}

static float jr_next_float(void)
{
    return (float)(jr_next(24) / (float)(1 << 24));
}

/* ---------------- the noise grid ---------------- */

static float *g_grid;
static int g_gw, g_gh;
static float *g_field;
static int g_fw, g_fh;

static void build_grid(int w, int h)
{
    int gw = BG_CELLS + 2;
    int gh = (int)floorf(BG_CELLS * (float)h / w + 0.5f) + 2;   /* Math.round */
    int gx, gy;

    if (gh < 2) gh = 2;
    free(g_grid);
    free(g_field);
    g_grid = malloc((size_t)gw * gh * sizeof(float));
    g_field = malloc((size_t)w * h * sizeof(float));
    if (!g_grid || !g_field) {
        free(g_grid); g_grid = NULL;
        free(g_field); g_field = NULL;
        g_gw = g_gh = g_fw = g_fh = 0;
        return;
    }
    jr_init(0xC0FFEE);
    for (gy = 0; gy < gh; gy++)
        for (gx = 0; gx < gw; gx++)
            g_grid[gy * gw + gx] = jr_next_float() * 2.0f - 1.0f;
    g_gw = gw; g_gh = gh; g_fw = w; g_fh = h;
}

static float smoothf(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static float lerp_smooth(float a, float b, float t)
{
    return a + (b - a) * smoothf(t);
}

void bg_field(int w, int h, double dx, double dy)
{
    float gwx, ghy, ox, oy;
    int x, y;

    if (w <= 1 || h <= 1) return;
    if (w != g_fw || h != g_fh || !g_grid) build_grid(w, h);
    if (!g_grid) return;

    gwx = (float)g_gw;
    ghy = (float)g_gh;
    /* wrap at the field's real period: sliding by exactly one period must land
     * on the identical pattern, and it is the whole reason this does not jump */
    ox = (float)((dx - floor(dx)) * g_gw);
    oy = (float)((dy - floor(dy)) * g_gh);

    for (y = 0; y < h; y++) {
        float fy = (float)y / (h - 1) * ghy + oy;
        int gy = (int)fy;
        float ty = fy - gy;
        int gy0 = gy % g_gh, gy1 = (gy + 1) % g_gh;
        float *outrow = g_field + (size_t)y * w;
        for (x = 0; x < w; x++) {
            float fx = (float)x / (w - 1) * gwx + ox;
            int gx = (int)fx;
            float tx = fx - gx;
            int gx0 = gx % g_gw, gx1 = (gx + 1) % g_gw;
            float v00 = g_grid[gy0 * g_gw + gx0], v10 = g_grid[gy0 * g_gw + gx1];
            float v01 = g_grid[gy1 * g_gw + gx0], v11 = g_grid[gy1 * g_gw + gx1];
            outrow[x] = lerp_smooth(lerp_smooth(v00, v10, tx),
                                    lerp_smooth(v01, v11, tx), ty);
        }
    }
}

const float *bg_field_data(int *w, int *h)
{
    if (w) *w = g_fw;
    if (h) *h = g_fh;
    return g_field;
}

const float *bg_grid_data(int *gw, int *gh)
{
    if (gw) *gw = g_gw;
    if (gh) *gh = g_gh;
    return g_grid;
}

/* ---------------- colour ---------------- */

/* Java: Math.round(255 * (1f - t)). Math.round(float) is floor(x + 0.5). */
static int jround(float v)
{
    return (int)floorf(v + 0.5f);
}

static Uint32 rgb_px(int r, int g, int b)
{
    return 0xFF000000u | ((Uint32)(r & 0xFF) << 16) | ((Uint32)(g & 0xFF) << 8)
         | (Uint32)(b & 0xFF);
}

/* blend(base, over, t) — the Java version's integer arithmetic, including the
 * truncating /255, which is what makes the output land on the same bytes */
static Uint32 blend_t(Uint32 base, Uint32 over, float t)
{
    int ia, a, r, g, b;
    if (t <= 0.0f) return base;
    if (t >= 1.0f) return over;
    ia = jround(255.0f * (1.0f - t));
    a = 255 - ia;
    r = ((int)((over >> 16) & 0xFF) * a + (int)((base >> 16) & 0xFF) * ia) / 255;
    g = ((int)((over >> 8) & 0xFF) * a + (int)((base >> 8) & 0xFF) * ia) / 255;
    b = ((int)(over & 0xFF) * a + (int)(base & 0xFF) * ia) / 255;
    return rgb_px(r, g, b);
}

/* blend(base, over) — over's alpha decides, as the Android scrim does */
static Uint32 blend_alpha(Uint32 base, Uint32 over)
{
    int a = (int)((over >> 24) & 0xFF);
    int ia, r, g, b;
    if (a <= 0) return base;
    ia = 255 - a;
    r = ((int)((over >> 16) & 0xFF) * a + (int)((base >> 16) & 0xFF) * ia) / 255;
    g = ((int)((over >> 8) & 0xFF) * a + (int)((base >> 8) & 0xFF) * ia) / 255;
    b = ((int)(over & 0xFF) * a + (int)(base & 0xFF) * ia) / 255;
    return rgb_px(r, g, b);
}

static Uint32 tri_lerp(Uint32 light, Uint32 mid, Uint32 dark, float t)
{
    if (t < 0.5f) return blend_t(light, mid, t * 2.0f);
    return blend_t(mid, dark, (t - 0.5f) * 2.0f);
}

static Uint32 pal_px(const Palette *p, int which)
{
    SDL_Color c = which == 0 ? p->light : which == 1 ? p->mid : p->dark;
    return rgb_px(c.r, c.g, c.b);
}

void bg_render_px(const Palette *pal, int dark, double t, Uint32 *out, int w, int h)
{
    Uint32 light, mid, darkc, scrim;
    float dx, dy;
    int x, y;

    if (w <= 1 || h <= 1 || !out) return;
    light = pal_px(pal, 0);
    mid   = pal_px(pal, 1);
    darkc = pal_px(pal, 2);
    scrim = dark ? 0x99000000u : 0x66FFFFFFu;

    dx = (float)(BG_DX * t);
    dy = (float)(BG_DY * t);
    bg_field(w, h, dx, dy);
    if (!g_field) return;

    for (y = 0; y < h; y++) {
        float v = (float)y / (h - 1);              /* 0 top .. 1 bottom */
        float dark_bias = 0.30f + 0.70f * v;       /* more dark near the bottom */
        const float *frow = g_field + (size_t)y * w;
        Uint32 *orow = out + (size_t)y * w;
        for (x = 0; x < w; x++) {
            float n = frow[x];                     /* ~ -1..1, coarse */
            float tt = n * 0.5f + 0.5f;            /* 0..1 */
            tt = tt * (1.0f + 0.7f * v) * dark_bias;
            if (tt < 0.0f) tt = 0.0f;
            if (tt > 1.0f) tt = 1.0f;
            orow[x] = blend_alpha(tri_lerp(light, mid, darkc, tt), scrim);
        }
    }
}

/* ---------------- the app's texture ---------------- */

static SDL_Texture *g_tex;
static Uint32 *g_px;
static int g_tw, g_th;
static double g_last_t;
static Uint64 g_next_ms;
static int g_have_last;
static Palette g_last_pal;
static int g_last_dark;

void bg_size_for(int win_w, int win_h, int *rw, int *rh)
{
    int w = win_w / 2, h = win_h / 2;
    if (w < 180) w = 180;
    if (h < 240) h = 240;
    *rw = w;
    *rh = h;
}

void bg_init(SDL_Renderer *ren, int win_w, int win_h)
{
    int w, h;
    bg_size_for(win_w, win_h, &w, &h);
    if (g_tex && (w != g_tw || h != g_th)) {
        SDL_DestroyTexture(g_tex);
        g_tex = NULL;
        free(g_px);
        g_px = NULL;
    }
    if (!g_tex) {
        /* Smooth stretch — the blobs are enormous, so a half-res field scaled up
         * is visually identical to a full-res one and costs a quarter of the work.
         * The quality has to be set BEFORE the texture is created (it is baked in
         * at creation), and is put back afterwards so nothing else that scales —
         * album art — quietly changes behaviour on the way past. */
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        g_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        if (!g_tex) return;
        g_px = malloc((size_t)w * h * sizeof(Uint32));
        if (!g_px) { SDL_DestroyTexture(g_tex); g_tex = NULL; return; }
        g_tw = w;
        g_th = h;
        g_last_t = -1.0;
    }
}

void bg_frame(SDL_Renderer *ren, const Palette *pal, int dark, double t,
              int win_w, int win_h)
{
    Uint64 now = SDL_GetTicks64();
    if (!g_tex) bg_init(ren, win_w, win_h);
    if (!g_tex || !g_px) return;
    if (g_have_last && t == g_last_t) return;
    /* 20 field updates a second is plenty for something this slow, but a new
     * cover must never wait for the next tick — and in capture mode t is frozen,
     * so the palette is the only thing that can make it redraw at all */
    if (g_have_last && g_last_dark == dark && now < g_next_ms &&
        memcmp(&g_last_pal, pal, sizeof *pal) == 0)
        return;
    bg_render_px(pal, dark, t, g_px, g_tw, g_th);
    SDL_UpdateTexture(g_tex, NULL, g_px, g_tw * (int)sizeof(Uint32));
    g_last_t = t;
    g_last_pal = *pal;
    g_last_dark = dark;
    g_have_last = 1;
    g_next_ms = now + (Uint64)(1000.0 / BG_DFPS);
}

void bg_draw(SDL_Renderer *ren, int win_w, int win_h)
{
    SDL_Rect dst = { 0, 0, win_w, win_h };
    if (!g_tex) return;
    SDL_RenderCopy(ren, g_tex, NULL, &dst);
}

void bg_free(void)
{
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = NULL;
    free(g_px);
    g_px = NULL;
    free(g_grid);
    g_grid = NULL;
    free(g_field);
    g_field = NULL;
    g_gw = g_gh = g_fw = g_fh = g_tw = g_th = 0;
}
