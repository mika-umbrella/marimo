/* recap.h — "your week/month/year in marimo", aggregated from the listening
 * diary. Ported from marimo-android's Recap.java (2026-09-28) and kept
 * number-for-number identical: the android unit tests (RecapTest.java) are
 * reproduced in this repo's --selftest, so the two implementations agree.
 *
 * "Counts as a play": heard > 30s AND (duration unknown ? heard >= 4 min
 * : heard >= 50% of the track). Half-skipped tracks don't pad the numbers. */
#ifndef MARIMO_RECAP_H
#define MARIMO_RECAP_H

#include <stddef.h>
#include <time.h>
#include "history.h"

#define RECAP_TOP 5
#define RECAP_NAME_MAX 256

#define RECAP_WEEK  0
#define RECAP_MONTH 1
#define RECAP_YEAR  2

typedef struct {
    char name[RECAP_NAME_MAX];
    long long count;
} RecapRow;

typedef struct {
    long long hours, total_sec, tracks, artists, albums;
    long long hours_prev, total_sec_prev, tracks_prev;
    RecapRow top_artists[RECAP_TOP];
    int n_top_artists;
    RecapRow top_albums[RECAP_TOP];
    int n_top_albums;
    RecapRow top_tracks[RECAP_TOP];
    int n_top_tracks;
    long long bars[12];              /* per-day / per-week / per-month */
    int n_bars;
    int empty;                       /* nothing qualified in the window */

    /* listening-behaviour stats — local only, last.fm can never see these */
    char persona[32];
    int streak_days;
    long long longest_session_sec;
    int skip_rate;                   /* % of >5s plays that were skips */
    char replay_king[RECAP_NAME_MAX];
    long long replay_king_plays;
    char most_skipped[RECAP_NAME_MAX];
    long long most_skipped_plays;
    int discovery_pct;               /* % of qualified plays to new artists */
} RecapResult;

int recap_counts_as_play(const HistoryEntry *e);

/* local calendar breakdown of an epoch-ms timestamp. Exported because both the
 * recap screen (period labels) and the selftest need local day/hour parts, and
 * the callers must not have to redo the localtime_r dance. */
void recap_local_tm(long long ms, struct tm *out);

/* calendar-anchored boundaries for a period ending at or before `now`:
 * w = {cur_start, cur_end, prev_start, prev_end} */
void recap_windows(int mode, long long now, long long w[4]);

/* aggregate `cur` for the window and `prev` for the deltas (both already
 * filtered to their windows by history_window) */
void recap_compute(const HistoryEntry *cur, size_t n_cur,
                   const HistoryEntry *prev, size_t n_prev,
                   int mode, RecapResult *r);

void recap_fmt_hours(long long total_sec, int zero_ok, char *out, size_t n);
void recap_delta(long long prev, long long cur, const char *unit, char *out, size_t n);
const char *recap_a_an(const char *s);
/* the share card nova actually sends friends */
void recap_share_card(int mode, const long long w[4], const RecapResult *r,
                      char *out, size_t n);

#endif /* MARIMO_RECAP_H */
