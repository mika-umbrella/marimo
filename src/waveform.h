/* waveform.h — 96-bucket RMS envelopes for the player's seekbar.
 *
 * The phone cannot decode cheaply: Android runs the codec in another process and
 * charges ~300us per buffer hand-off, so a track costs ~19s there. That is why
 * `scripts/make-waveforms.py` precomputes the peaks on the workstation and ships
 * them as a `waves.marimo` sidecar next to cover.jpg, and why the phone's player
 * draws a fake PRNG skyline when no sidecar covers a track.
 *
 * The desktop has the opposite economics: it links libmpv and can decode a track
 * in a couple of hundred milliseconds. So the sidecar is read when it is there
 * (it is, on the compressed library) and *made* when it is not — both paths run
 * the exact same arithmetic as the generator, so a peak set is a peak set whoever
 * produced it, and the phone can read anything this writes.
 *
 * Sidecar layout, byte for byte (see WaveSidecar.java, which parses the same file
 * on the phone):
 *     "MWAVS001" | uint32 LE count | (uint16 LE nameLen | name UTF-8 | 96 peaks)*
 *
 * Peaks: RMS per bucket, sqrt curve, normalised by the 95th percentile, clamped
 * to 4..100, from a mono decode at 8 kHz decimated 4:1. Do NOT lower that rate —
 * 1 kHz collapses the tail of a track (measured max bar error 66 vs 1 at 8 kHz).
 */
#ifndef MARIMO_WAVEFORM_H
#define MARIMO_WAVEFORM_H

#include <stddef.h>

#define WAVE_BUCKETS 96
#define WAVE_SIDECAR "waves.marimo"

/* Parse an in-memory sidecar and pull out one track's peaks.
 * Returns 1 if `name` was found, 0 otherwise. Tolerant on purpose: a foreign
 * magic, a truncated blob or a zero count must degrade to "no waveform", never
 * to a crash — same contract as the phone's parser. A truncated blob yields
 * whatever complete entries it held. */
int wave_sidecar_parse(const unsigned char *blob, size_t n, const char *name,
                       unsigned char out[WAVE_BUCKETS]);

/* Read <album_dir>/waves.marimo and look up one file name (no directory part). */
int wave_sidecar_read(const char *album_dir, const char *track_name,
                      unsigned char out[WAVE_BUCKETS]);

/* Decode `path` with ffmpeg and compute the envelope. 1 on success.
 * Ignore this and call wave_peaks() unless you specifically want to bypass both
 * the sidecar and the cache. */
int wave_decode(const char *path, unsigned char out[WAVE_BUCKETS]);

/* Decode with a per-file on-disk cache so a track is decoded once, ever — the
 * desktop's equivalent of the phone's "decode once, keep it" behaviour. */
int wave_decode_cached(const char *path, unsigned char out[WAVE_BUCKETS]);

/* The front door: album sidecar first (free), then cache, then decode.
 * Returns 1 and fills `out`, or 0 when the track has no waveform. */
int wave_peaks(const char *path, unsigned char out[WAVE_BUCKETS]);

#endif /* MARIMO_WAVEFORM_H */
