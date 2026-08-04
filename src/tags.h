/* tags.h — fast tag reader for FLAC + MP3 (ID3v2).
 * Microseconds per file, no mpv needed — used for folder queue ordering
 * (disc/track numbers) and as the fast path for queue metadata. */
#ifndef MIKA_TAGS_H
#define MIKA_TAGS_H

#include "queue.h"

/* full tag read. returns 0 + have_meta on success (title/artist/album/duration) */
int tag_read_meta(const char *path, Meta *meta);
/* just disc/track numbers (for sorting). returns 0 if either was found */
int tag_trackinfo(const char *path, int *track, int *disc);

#endif
