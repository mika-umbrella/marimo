/* theme.h — the cover-derived palette, and the surfaces that come from it.
 *
 * Ported from marimo-android's BgManager.palette() (2026-09-28) and kept
 * number-for-number identical: same ~4000-sample stride, same luma buckets, same
 * fallbacks.
 *
 * Note what the art does and does not drive. On the phone the accent stays
 * winamp green and the panels stay near-neutral — the *backdrop* is the only
 * thing wearing the cover's colours. That is deliberate, and it is the property
 * that keeps text readable over a bright album cover, so the port keeps it.
 *
 * The fallback triple is strictly blue (r < g < b). That is not decoration: it
 * is the probe that answers "is art loading at all?" from a screenshot alone,
 * with no code inspection — if the background is neutral blue, nothing was
 * sampled. Do not "fix" it to grey.
 */
#ifndef MARIMO_THEME_H
#define MARIMO_THEME_H

#include <SDL.h>

typedef struct {
    SDL_Color light, mid, dark;
    int from_art;          /* 0 = the blue fallback, i.e. no art was sampled */
} Palette;

/* Sample a cover's light/mid/dark representatives. A missing or unreadable file
 * (NULL/empty path included) returns the fallback palette rather than failing. */
Palette palette_from_cover(const char *path);

/* The same sampling over an in-memory surface. Exists so the buckets can be
 * tested exactly — a surface of three solid bands has three known averages,
 * which a real JPEG never does. Handles (and does not free) any format. */
Palette palette_from_surface(SDL_Surface *s);

/* The backdrop colour for a palette: what the phone passes to the gradient
 * before the perlin is layered on. Dark scrim 0x99 black, light 0x66 white. */
SDL_Color palette_scrim(const Palette *p, int dark);

#endif /* MARIMO_THEME_H */
