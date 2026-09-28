/* history.h — the listening diary behind the recap screen.
 *
 * Ported from marimo-android's HistoryDiary.java (2026-09-28) and deliberately
 * format-compatible: one newline-delimited JSON line per *finished* listen,
 *
 *   {"ts":<epoch_ms>,"artist":"...","album":"...","title":"...",
 *    "sec":<seconds_heard>,"dur":<duration_ms>}
 *
 * `sec` is how much of the track was actually heard (raw truth, so skip-rate and
 * replay stats stay possible); `dur` is the known duration so the "counts as a
 * play" rule (heard > 30s AND > 50%) can be applied at *aggregation* time rather
 * than losing information at write time.
 *
 * The current calendar year lives in history.jsonl (append-only); when the year
 * rolls over the finished year is gzipped aside as history.YYYY.jsonl.gz, and
 * archives older than the previous year are pruned as they're written. Reads
 * tolerate corrupt/partial lines by skipping them.
 */
#ifndef MARIMO_HISTORY_H
#define MARIMO_HISTORY_H

#include <stddef.h>

#define H_ARTIST_MAX 256
#define H_ALBUM_MAX  256
#define H_TITLE_MAX  512

typedef struct {
    long long ts;                    /* epoch ms */
    long long sec;                   /* seconds heard */
    long long dur;                   /* duration ms, 0 = unknown */
    char artist[H_ARTIST_MAX];
    char album[H_ALBUM_MAX];
    char title[H_TITLE_MAX];
} HistoryEntry;

/* directory that holds history.jsonl (created if missing) */
void history_init(const char *dir);
void history_shutdown(void);

/* append one finished listen. best-effort: never fails loudly, never throws. */
void history_log(const char *artist, const char *album, const char *title,
                 long long played_ms, long long dur_ms);

/* every entry with ts in [start_ms, end_ms), across the current year and the
 * archived one, so a window crossing New Year still resolves. Caller frees the
 * array with history_free(). Returns NULL / *n = 0 when there is nothing. */
HistoryEntry *history_window(long long start_ms, long long end_ms, size_t *n);
void history_free(HistoryEntry *e);

/* exported for the selftest — the line builder and the tolerant parser */
char *history_line(const char *artist, const char *album, const char *title,
                   long long played_ms, long long dur_ms, long long ts);
int history_parse_line(const char *line, HistoryEntry *out);

#endif /* MARIMO_HISTORY_H */
