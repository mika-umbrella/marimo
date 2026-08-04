/* player.h — libmpv playback engine wrapper.
 * mpv gives us: all formats, TRUE gapless (gapless-audio), accurate FLAC,
 * embedded album art (album-art property), metadata for scrobbling. */
#ifndef MIKA_PLAYER_H
#define MIKA_PLAYER_H

#include <stdint.h>
#include "queue.h"

typedef struct Player Player;

Player *player_create(const char *aout_override);  /* NULL = default audio out */
void    player_destroy(Player *p);

/* load + start playing a file, with next_path (may be NULL) preloaded as the
 * second playlist entry so mpv can switch to it gaplessly (demuxer is warmed
 * while the current track still plays). blocks up to ~1s for file-loaded. */
int  player_play_with_next(Player *p, const char *path, const char *next_path);
/* load + start playing a file, with next_path (may be NULL) preloaded as the
 * second playlist entry so mpv can switch to it gaplessly (demuxer is warmed
 * while the current track still plays). blocks up to ~1s for file-loaded. */
int  player_play_with_next(Player *p, const char *path, const char *next_path);
/* after an EOF auto-advance: drop the finished entry (index 0) and preload
 * the new following entry */
void player_gapless_shift(Player *p, const char *next_path);
int  player_playlist_count(Player *p);
void player_dbg_playlist(Player *p);
void player_set_pause(Player *p, int paused);
int  player_paused(Player *p);
void player_stop(Player *p);                       /* stops, keeps position state idle */
/* 0 stopped/idle, 1 playing, 2 paused */
int  player_state(Player *p);
double player_time(Player *p);                     /* seconds, -1 if none */
double player_length(Player *p);                   /* seconds, -1 if unknown */
void player_seek(Player *p, double secs);
void player_set_volume(Player *p, int v);          /* 0..100 */
void player_set_mute(Player *p, int on);
int  player_get_volume(Player *p);
void player_set_repeat_one(Player *p, int on);     /* mpv loop-file */

/* read tags for the CURRENTLY LOADED file. returns 1 when meta present. */
int  player_meta(Player *p, Meta *meta);
/* embedded album art as base64 string (malloc'd, caller frees) or NULL */
char *player_embedded_art(Player *p);

/* call every frame. returns 1 = current track ended (EOF), 2 = error */
int  player_poll(Player *p);
/* 1 if mpv reported a metadata update since last check (clears the flag) */
int  player_meta_dirty(Player *p);

/* --- background tag reader (own thread, own mpv handle) --- */
typedef struct TagReader TagReader;
TagReader *tagreader_create(void);
void       tagreader_destroy(TagReader *t);
/* blocking; fills meta. returns 1 if any tag found. */
int        tagreader_read(TagReader *t, const char *path, Meta *meta);

#endif
