/* album.h — album metadata read off the folder name, plus cover thumbnails.
 *
 * Nova's folders are named "ARTIST - (YEAR) ALBUM [FORMAT]", so almost everything
 * a library row wants is already in the directory name and costs nothing to read.
 * The year rules are a port of marimo-android's Album.java: see album.c for the
 * order, which is the whole trick, and the test vectors in --selftest, which are
 * the cases that were verified against the Java by running it.
 */
#ifndef MARIMO_ALBUM_H
#define MARIMO_ALBUM_H

#include <SDL.h>
#include <stddef.h>

/* Release year from an album folder name, 0 when there isn't one. */
int album_year(const char *folder);

/* Split "ARTIST - (YEAR) ALBUM [FORMAT]" into its parts. Either may come back
 * empty ("no dash" and "no year" are both ordinary). The year bracket and any
 * trailing [FORMAT] are stripped out of the title, because the row shows the
 * year separately and nobody needs to read "of 2016 [FLAC]" twice. */
void album_split(const char *folder, char *artist, size_t asz,
                 char *title, size_t tsz);

/* Number of audio files directly in `dir`, or -1 if it cannot be read. Cached —
 * the list asks for the same handful of folders every frame. */
int album_tracks(const char *dir);

/* A square cover thumbnail of `size` pixels, or NULL. The decode happens on a
 * worker thread and the result is cached on disk, because the render thread must
 * never decode a 25 MB JPEG to draw a 16px square — that was the scrolling
 * stutter. Returns NULL while a thumbnail is still coming: draw the icon and ask
 * again next frame. Textures belong to the cache — never free them. */
SDL_Texture *album_thumb(SDL_Renderer *ren, const char *dir, int size);

/* How many thumbnails were decoded this run versus served from the disk cache.
 * Exists so "the cache works" is a number rather than a claim — a warm run must
 * decode nothing. */
void album_thumb_stats(int *decoded, int *cached);

/* Tags for one track file, from a cache: title/artist and seconds (0 when the tag
 * reader could not say). Returns 1 when anything was read, so callers can fall
 * back to the filename. Reading is the fast FLAC/MP3 path, and it happens once per
 * file — the library rows ask for every visible file every frame. */
int album_track_tags(const char *path, char *title, size_t tn, char *artist, size_t an,
                     int *secs);

/* Display form of a folder or track name: the em/en dashes her folders mix in
 * become plain hyphens. Display only — the name on disk is left exactly alone,
 * since queue.dat, the scrobbles and the cover matching all key off it. */
void album_dashes(const char *in, char *out, size_t n);

void album_free(void);

#endif /* MARIMO_ALBUM_H */
