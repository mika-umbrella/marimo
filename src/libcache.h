/* libcache.h — per-album track counts, cached on disk.
 *
 * The phone needs a LibraryCache because a scan there takes seconds (SAF, a
 * separate permission model, tags per track). Here one folder is cheap — a
 * readdir — but 632 of them over CIFS are not, and both the album rows and the
 * breadcrumb's total want that number. So this is the desktop-shaped version of
 * the same idea: cache the cheap-to-store, expensive-to-fetch fact, and refresh
 * it incrementally.
 *
 * Incremental means keyed on the folder's mtime: a folder that has not changed
 * keeps its cached count, so the second run over the library is stats only and
 * opens nothing at all.
 */
#ifndef MARIMO_LIBCACHE_H
#define MARIMO_LIBCACHE_H

/* read/write ~/.cache/marimo/library.txt. Loading is tolerant: a truncated or
 * hand-edited file loses entries, it does not fail. */
void libcache_load(void);
void libcache_save(void);
void libcache_free(void);

/* Track count for one folder, from the cache when its mtime is unchanged, by
 * counting when it is not. -1 if the folder cannot be read. */
int libcache_tracks(const char *dir);

/* Refresh the folders directly under `root`, at most `budget` per call, so this
 * can be driven from the frame loop without stalling anything. Returns 1 while
 * there is more to do, 0 once that root is fully known (which is also what
 * libcache_cached() reports). */
int libcache_step(const char *root, int budget);

/* Meaningful once the walk has finished; 0 otherwise. Totals cover the folders
 * directly under `root`. */
long long libcache_total_tracks(const char *root);
int libcache_albums(const char *root);
int libcache_cached(const char *root);

/* Forget a root so the next libcache_step re-stats it. The counts themselves stay,
 * so this verifies rather than recounts: an unchanged folder is still answered
 * from the cache, and only what actually changed is opened. */
void libcache_forget(void);

/* a one-line summary for the status bar: scanned vs reused on the last walk */
void libcache_last_walk(int *scanned, int *reused);

#endif /* MARIMO_LIBCACHE_H */
