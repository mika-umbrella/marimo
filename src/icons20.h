/* icons20.h - 20x20 icon masks, generated from marimo-android's drawables.
 *
 * The phone's icons are Feather-style stroked vectors on a 24/26-unit grid with
 * 1.7-unit strokes. At 16px that stroke is about 1.05px, and thresholding it to
 * 1 bit gives dotted, broken shapes; at 20px the same vectors rasterise cleanly.
 * So the transport row and the window buttons (24px wide, room for 20) use these.
 * The shuffle/repeat/volume controls joined them on 2026-10-09: their shapes are
 * fiddly enough (L-heads, an open loop) that 16px could not hold them without
 * either blobbing or breaking up, so their buttons widened to 24px too. The
 * 16px ICON_SHUFFLE/ICON_REPEAT/ICON_VOLUME masks in icons.c are now unused.
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
    ICON20_SHUFFLE,
    ICON20_REPEAT,
    ICON20_REPEAT_ONE,
    ICON20_VOLUME,
    N_ICONS20
};

extern const char *icon_art20[N_ICONS20][20];

#endif
