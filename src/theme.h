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

/* ---- the mode table ---- */

/* The rest of the colours live here rather than in main.c because they are
 * mode-dependent: theme_apply() swaps the whole set between the dark and the
 * light table, exactly as marimo-android's Theme.java does. (The phone splits
 * these across Theme.java and BgManager.java; same division, one file here.)
 *
 * They are deliberately *not* const: they are global current state, which is how
 * the phone treats them too, so every call site keeps naming its colour and
 * nothing has to know that a switch happened.
 *
 * The dark table is what this port has always drawn with — its C_TXT, C_DIM and
 * C_ACC are byte-identical to Theme.sub(), Theme.dim() and Theme.ACC_DARK, so the
 * two apps agree on the dark look and only the light one is new. */
extern SDL_Color C_BG0, C_BG1, C_BG2, C_BD, C_TXT, C_DIM, C_ACC, C_ACC2, C_ERR,
                 C_SELBG, C_AMBER, C_ROW;

void theme_apply(int dark);
int theme_is_dark(void);

/* alphas for the surfaces that sit directly on the art — the phone uses three
 * different ones and all three are visible: ambient surfaces, row hovers, and
 * the selected row. Everything else stays opaque. */
int theme_alpha_panel(void);
int theme_alpha_row(void);
int theme_alpha_sel(void);

#endif /* MARIMO_THEME_H */
