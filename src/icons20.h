/* icons20.h - 20x20 icon masks, generated from marimo-android's drawables.
 *
 * The phone's icons are Feather-style stroked vectors on a 24/26-unit grid with
 * 1.7-unit strokes. At 16px that stroke is about 1.05px, and thresholding it to
 * 1 bit gives dotted, broken shapes; at 20px the same vectors rasterise cleanly.
 * So the transport row and the window buttons (24px wide, room for 20) use these,
 * while the 18x16 shuffle/repeat and 16x16 volume controls keep the hand-drawn
 * 16px masks in icons.c.
 *
 * Regenerate with the recipe in the marimo-desktop-icon-port skill. Each mask is
 * 20 rows of 20 characters. */
#ifndef MIKA_ICONS20_H
#define MIKA_ICONS20_H

enum {
    ICON20_PREV,
    ICON20_NEXT,
    ICON20_PLAY,
    ICON20_PAUSE,
    ICON20_STOP,
    ICON20_MINUS,
    ICON20_TRI_UP,
    ICON20_TRI_DOWN,
    ICON20_GEAR,
    ICON20_X,
    N_ICONS20
};

extern const char *icon_art20[N_ICONS20][20];

#endif
