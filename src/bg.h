/* bg.h — the drifting art-derived backdrop.
 *
 * Ported from marimo-android's BgManager.java (2026-09-28), semantics and all:
 * a coarse bilinear value-noise field over a 4xN random grid, sampled at half
 * screen resolution and stretched, with a dark bias toward the bottom so text
 * and controls stay readable. The grid is seeded with the same constant as the
 * phone and filled from a faithful java.util.Random, so for one cover the two
 * render the same pattern rather than merely a similar one.
 *
 * Two things here are load-bearing and were learned the hard way on the phone:
 *
 * 1. `CELLS = 2` means giant, sparse blobs. Raising it makes it look like fabric
 *    rather than terrain.
 * 2. The drift offset must wrap at the field's REAL period (`gw`, `gh` cells),
 *    never at 1.0. Wrapping at 1.0 slides the whole field one cell and then snaps
 *    it back — the ~15s visible jump that took a screen recording to catch.
 *    `bg_field()` is periodic in time with period 1/0.015 s, exactly.
 *
 * It runs on the main thread, which is only defensible because the render is a
 * quarter of a megapixel of trivial arithmetic and every buffer is allocated
 * once. Do not add per-frame allocation here — that is what made the phone's
 * version stutter before it was fixed.
 */
#ifndef MARIMO_BG_H
#define MARIMO_BG_H

#include <SDL.h>
#include "theme.h"

#define BG_CELLS  2
#define BG_DFPS   20.0                 /* field updates per second */
#define BG_DX     0.015                /* cells per second, x */
#define BG_DY     0.010                /* cells per second, y */

/* halves the window, with the phone's floors so a tiny window is still smooth */
void bg_size_for(int win_w, int win_h, int *rw, int *rh);

/* the field itself: build (once) then slide (every call), in place */
void bg_field(int w, int h, double dx, double dy);
const float *bg_field_data(int *w, int *h);
const float *bg_grid_data(int *gw, int *gh);

/* palette -> ARGB pixels. `t` is seconds; the drift comes from it. Pure: writes
 * only into `out`, which must hold w*h Uint32. */
void bg_render_px(const Palette *pal, int dark, double t, Uint32 *out, int w, int h);

/* the texture path used by the app */
void bg_init(SDL_Renderer *ren, int win_w, int win_h);
void bg_frame(SDL_Renderer *ren, const Palette *pal, int dark, double t,
              int win_w, int win_h);
void bg_draw(SDL_Renderer *ren, int win_w, int win_h);
void bg_free(void);

#endif /* MARIMO_BG_H */
