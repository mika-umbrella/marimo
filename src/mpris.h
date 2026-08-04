/* mpris.h — MPRIS2 (D-Bus) integration so KDE/playerctl can control mikaplay.
 * The D-Bus thread only reads the published state and enqueues commands;
 * all mpv access stays on the main thread. */
#ifndef MIKA_MPRIS_H
#define MIKA_MPRIS_H

#include <stdint.h>

typedef struct {
    int status;            /* 0 stopped, 1 playing, 2 paused */
    double position;       /* seconds */
    int64_t duration_us;
    int volume;            /* 0..100 */
    int can_next, can_prev;
    char title[512];
    char artist[512];
    char album[512];
    char trackid[64];      /* mpris:trackid object path */
} MprisState;

typedef struct { int cmd; int64_t arg; } MprisCmd;

#define MPRIS_PLAYPAUSE 1
#define MPRIS_PLAY 2
#define MPRIS_PAUSE 3
#define MPRIS_STOP 4
#define MPRIS_NEXT 5
#define MPRIS_PREV 6
#define MPRIS_SEEK 7       /* arg = offset in microseconds */
#define MPRIS_SETPOS 8     /* arg = position in microseconds */
#define MPRIS_VOLUME 9     /* arg = volume * 100 */
#define MPRIS_VOLUMEDELTA 10 /* arg = delta * 100 (windows hotkeys) */
#define MPRIS_MUTE 11        /* toggle mute (windows hotkeys) */

void mpris_init(void);
void mpris_shutdown(void);
/* main thread: publish current state every frame (or on change) */
void mpris_publish(const MprisState *s);
/* main thread: drain queued commands; returns cmd 0 when empty */
MprisCmd mpris_take_command(void);

#endif
