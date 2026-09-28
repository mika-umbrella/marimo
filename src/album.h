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

/* A square cover thumbnail of `size` pixels, or NULL. Loading is spread across
 * frames on purpose (a few per frame, never a synchronous burst) so scrolling
 * into the library cannot hitch, and misses are remembered so a coverless album
 * is not re-opened sixty times a second. Call album_frame() once per frame to
 * release the budget. Textures belong to the cache — never free them. */
void album_frame(void);
SDL_Texture *album_thumb(SDL_Renderer *ren, const char *dir, int size);

void album_free(void);

#endif /* MARIMO_ALBUM_H */
