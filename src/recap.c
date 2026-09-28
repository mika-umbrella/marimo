/* recap.c — the aggregation behind the recap screen. See recap.h.
 *
 * Ported from marimo-android's Recap.java. Deliberately faithful to a few
 * integer/calendar quirks of the original, because the android unit tests are
 * reproduced in this repo's selftest and must agree:
 *   - the 50% rule is integer: sec*1000 >= dur/2, not a float compare;
 *   - a "week" is exactly 7*86400*1000 ms back from this Monday's midnight
 *     (Java does the same, so it is not DST-aware either);
 *   - months/years step through the calendar, so those *are* DST/month aware. */
#include "recap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- the counts-as-a-play rule ---------------- */

int recap_counts_as_play(const HistoryEntry *e)
{
    if (e->sec <= 30) return 0;
    if (e->dur <= 0) return e->sec >= 240;
    return e->sec * 1000 >= e->dur / 2;
}

/* ---------------- local calendar helpers ---------------- */

void recap_local_tm(long long ms, struct tm *out)
{
    time_t t = (time_t)(ms / 1000);
#ifdef _WIN32
    struct tm *p = localtime(&t);
    if (p) *out = *p; else memset(out, 0, sizeof *out);
#else
    if (!localtime_r(&t, out)) memset(out, 0, sizeof *out);
#endif
}

static long long mk_local(struct tm *tm)
{
    tm->tm_isdst = -1;
    return (long long)mktime(tm) * 1000;
}

static long long midnight_of(long long ms)
{
    struct tm tmv;
    recap_local_tm(ms, &tmv);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    return mk_local(&tmv);
}

void recap_windows(int mode, long long now, long long w[4])
{
    struct tm tmv;
    recap_local_tm(now, &tmv);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;

    long long cur_start, cur_end, prev_start;
    if (mode == RECAP_WEEK) {
        /* Monday of this week (weeks are Monday-first here) */
        int back = (tmv.tm_wday + 6) % 7;
        tmv.tm_mday -= back;
        cur_end = mk_local(&tmv);
        cur_start = cur_end - 7LL * 86400 * 1000;
        prev_start = cur_start - 7LL * 86400 * 1000;
    } else if (mode == RECAP_MONTH) {
        tmv.tm_mday = 1;
        cur_end = mk_local(&tmv);                 /* 1st of this month */
        struct tm s = tmv;
        s.tm_mon -= 1;
        cur_start = mk_local(&s);                 /* 1st of previous month */
        s.tm_mon -= 1;
        prev_start = mk_local(&s);
    } else {
        tmv.tm_mon = 0;
        tmv.tm_mday = 1;
        cur_end = mk_local(&tmv);                 /* Jan 1 this year */
        struct tm s = tmv;
        s.tm_year -= 1;
        cur_start = mk_local(&s);
        s.tm_year -= 1;
        prev_start = mk_local(&s);
    }
    w[0] = cur_start;
    w[1] = cur_end;
    w[2] = prev_start;
    w[3] = cur_start;                             /* prev_end == cur_start */
}

/* ---------------- small string -> count map ---------------- */

typedef struct {
    char key[RECAP_NAME_MAX];
    long long count;
} Kv;

typedef struct {
    Kv *v;
    int n, cap;
} Map;

static void map_free(Map *m)
{
    free(m->v);
    m->v = NULL;
    m->n = m->cap = 0;
}

static void map_bump(Map *m, const char *key)
{
    int i;
    if (!key || !*key) key = "?";                 /* empties collapse, as in Java */
    for (i = 0; i < m->n; i++)
        if (!strcmp(m->v[i].key, key)) { m->v[i].count++; return; }
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 32;
        Kv *nv = realloc(m->v, (size_t)cap * sizeof *nv);
        if (!nv) return;
        m->v = nv;
        m->cap = cap;
    }
    snprintf(m->v[m->n].key, sizeof m->v[m->n].key, "%s", key);
    m->v[m->n].count = 1;
    m->n++;
}

static int kv_cmp(const void *a, const void *b)
{
    const Kv *x = (const Kv *)a, *y = (const Kv *)b;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;   /* desc */
    return strcmp(x->key, y->key);                                   /* deterministic ties */
}

static void map_top(const Map *m, RecapRow *out, int *n_out)
{
    int i, n;
    Kv *tmp;
    if (m->n == 0) { *n_out = 0; return; }
    tmp = malloc((size_t)m->n * sizeof *tmp);
    if (!tmp) { *n_out = 0; return; }
    memcpy(tmp, m->v, (size_t)m->n * sizeof *tmp);
    qsort(tmp, (size_t)m->n, sizeof *tmp, kv_cmp);
    n = m->n > RECAP_TOP ? RECAP_TOP : m->n;
    for (i = 0; i < n; i++) {
        snprintf(out[i].name, sizeof out[i].name, "%s", tmp[i].key);
        out[i].count = tmp[i].count;
    }
    *n_out = n;
    free(tmp);
}

/* ---------------- bar bucketing ---------------- */

static void compute_bars(const HistoryEntry *cur, size_t n, int mode, RecapResult *r)
{
    size_t i;
    int nbars = mode == RECAP_WEEK ? 7 : mode == RECAP_MONTH ? 5 : 12;
    memset(r->bars, 0, sizeof r->bars);
    r->n_bars = nbars;
    for (i = 0; i < n; i++) {
        struct tm tmv;
        int idx;
        if (!recap_counts_as_play(&cur[i])) continue;
        recap_local_tm(cur[i].ts, &tmv);
        if (mode == RECAP_WEEK) {
            idx = (tmv.tm_wday + 6) % 7;                  /* Mon..Sun */
        } else if (mode == RECAP_MONTH) {
            idx = (tmv.tm_mday - 1) / 7;
            if (idx > 4) idx = 4;
        } else {
            idx = tmv.tm_mon;                             /* 0..11 */
        }
        if (idx >= 0 && idx < nbars) r->bars[idx]++;
    }
}

/* ---------------- listening behaviour ---------------- */

struct PlayRef {
    long long ts, sec;
    const char *title;
};

static int playref_cmp(const void *a, const void *b)
{
    const struct PlayRef *x = (const struct PlayRef *)a, *y = (const struct PlayRef *)b;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    return 0;
}

static int ll_cmp(const void *a, const void *b)
{
    long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void compute_behaviour(RecapResult *r, const HistoryEntry *cur, size_t n_cur,
                              const HistoryEntry *prev, size_t n_prev)
{
    size_t i;
    long long *days = NULL;
    size_t n_days = 0;
    struct PlayRef *qual = NULL;
    size_t n_qual = 0;
    Map track_count = {0}, skip_count = {0}, prev_artists = {0};

    /* persona: dominant time-of-day bucket over qualifying plays */
    {
        int bucket[4] = {0, 0, 0, 0}, best = 0;
        static const char *names[4] = { "midnight creature", "morning person",
                                        "afternoon drifter", "evening listener" };
        for (i = 0; i < n_cur; i++) {
            struct tm tmv;
            int h;
            if (!recap_counts_as_play(&cur[i])) continue;
            recap_local_tm(cur[i].ts, &tmv);
            h = tmv.tm_hour;
            bucket[h < 6 ? 0 : h < 12 ? 1 : h < 18 ? 2 : 3]++;
        }
        for (i = 1; i < 4; i++) if (bucket[i] > bucket[best]) best = (int)i;
        snprintf(r->persona, sizeof r->persona, "%s", names[best]);
    }

    /* streak: longest run of consecutive days with a qualifying play */
    days = malloc((n_cur ? n_cur : 1) * sizeof *days);
    if (days) {
        long long run = 0, best_run = 0, last = 0;
        for (i = 0; i < n_cur; i++)
            if (recap_counts_as_play(&cur[i])) days[n_days++] = midnight_of(cur[i].ts);
        qsort(days, n_days, sizeof *days, ll_cmp);
        for (i = 0; i < n_days; i++) {
            if (i > 0 && days[i] == days[i - 1]) continue;        /* dedupe */
            run = (last != 0 && days[i] == last + 86400000LL) ? run + 1 : 1;
            if (run > best_run) best_run = run;
            last = days[i];
        }
        r->streak_days = (int)best_run;
        free(days);
    }

    /* longest session: consecutive qualifying plays <= 1h apart count as one */
    qual = malloc((n_cur ? n_cur : 1) * sizeof *qual);
    if (qual) {
        long long best = 0, sess = 0, prev_ts = -1;
        for (i = 0; i < n_cur; i++)
            if (recap_counts_as_play(&cur[i])) {
                qual[n_qual].ts = cur[i].ts;
                qual[n_qual].sec = cur[i].sec;
                qual[n_qual].title = cur[i].title;
                n_qual++;
            }
        qsort(qual, n_qual, sizeof *qual, playref_cmp);
        for (i = 0; i < n_qual; i++) {
            int same = prev_ts >= 0 && (qual[i].ts - prev_ts) <= 3600000LL;
            sess = same ? sess + qual[i].sec : qual[i].sec;
            if (sess > best) best = sess;
            prev_ts = qual[i].ts;
        }
        r->longest_session_sec = best;
        free(qual);
    }

    /* skip rate + replay king + most-skipped, over plays longer than 5s */
    {
        long long play_n = 0, skip_n = 0;
        for (i = 0; i < n_cur; i++) {
            if (cur[i].sec <= 5) continue;
            if (recap_counts_as_play(&cur[i])) {
                play_n++;
                map_bump(&track_count, cur[i].title);
            } else {
                skip_n++;
                map_bump(&skip_count, cur[i].title);
            }
        }
        r->skip_rate = (int)(skip_n * 100 / (play_n + skip_n > 0 ? play_n + skip_n : 1));
        for (i = 0; i < (size_t)track_count.n; i++) {
            const char *k = track_count.v[i].key;
            if (!*k || !strcmp(k, "?")) continue;
            if (track_count.v[i].count > r->replay_king_plays) {
                r->replay_king_plays = track_count.v[i].count;
                snprintf(r->replay_king, sizeof r->replay_king, "%s", k);
            }
        }
        for (i = 0; i < (size_t)skip_count.n; i++) {
            const char *k = skip_count.v[i].key;
            if (!*k || !strcmp(k, "?")) continue;
            if (skip_count.v[i].count > r->most_skipped_plays) {
                r->most_skipped_plays = skip_count.v[i].count;
                snprintf(r->most_skipped, sizeof r->most_skipped, "%s", k);
            }
        }
    }

    /* discovery vs comfort: artists unseen in the previous period */
    {
        long long discovered = 0, comfort = 0;
        for (i = 0; i < n_prev; i++)
            if (prev[i].artist[0]) map_bump(&prev_artists, prev[i].artist);
        for (i = 0; i < n_cur; i++) {
            int j, seen = 0;
            if (!recap_counts_as_play(&cur[i]) || !cur[i].artist[0]) continue;
            for (j = 0; j < prev_artists.n && !seen; j++)
                if (!strcmp(prev_artists.v[j].key, cur[i].artist)) seen = 1;
            if (seen) comfort++; else discovered++;
        }
        r->discovery_pct =
          (int)(discovered * 100 / (discovered + comfort > 0 ? discovered + comfort : 1));
    }

    map_free(&track_count);
    map_free(&skip_count);
    map_free(&prev_artists);
}

/* ---------------- the aggregation ---------------- */

static long long count_plays(const HistoryEntry *e, size_t n)
{
    size_t i;
    long long c = 0;
    for (i = 0; i < n; i++) if (recap_counts_as_play(&e[i])) c++;
    return c;
}

void recap_compute(const HistoryEntry *cur, size_t n_cur,
                   const HistoryEntry *prev, size_t n_prev,
                   int mode, RecapResult *r)
{
    size_t i;
    long long secs = 0, psecs = 0;
    Map artists = {0}, albums = {0}, tracks = {0};

    memset(r, 0, sizeof *r);

    for (i = 0; i < n_cur; i++) {
        if (!recap_counts_as_play(&cur[i])) continue;
        secs += cur[i].sec;
        map_bump(&artists, cur[i].artist);
        map_bump(&albums, cur[i].album);
        map_bump(&tracks, cur[i].title);
    }
    r->total_sec = secs;
    r->hours = secs / 3600;
    r->tracks = count_plays(cur, n_cur);
    r->artists = artists.n;
    r->albums = albums.n;
    r->empty = r->tracks == 0;

    for (i = 0; i < n_prev; i++)
        if (recap_counts_as_play(&prev[i])) psecs += prev[i].sec;
    r->total_sec_prev = psecs;
    r->hours_prev = psecs / 3600;
    r->tracks_prev = count_plays(prev, n_prev);

    map_top(&artists, r->top_artists, &r->n_top_artists);
    map_top(&albums, r->top_albums, &r->n_top_albums);
    map_top(&tracks, r->top_tracks, &r->n_top_tracks);

    compute_bars(cur, n_cur, mode, r);
    compute_behaviour(r, cur, n_cur, prev, n_prev);

    map_free(&artists);
    map_free(&albums);
    map_free(&tracks);
}

/* ---------------- formatting ---------------- */

void recap_fmt_hours(long long total_sec, int zero_ok, char *out, size_t n)
{
    long long h = total_sec / 3600, m = (total_sec % 3600) / 60;
    if (h == 0 && m == 0) snprintf(out, n, "%s", zero_ok ? "0m" : "\xe2\x80\x94");
    else if (h == 0) snprintf(out, n, "%lldm", m);
    else snprintf(out, n, "%lldh %02lldm", h, m);
}

void recap_delta(long long prev, long long cur, const char *unit, char *out, size_t n)
{
    long long d = cur - prev;
    if (d == 0) { snprintf(out, n, "\xe2\x80\x94"); return; }        /* — */
    snprintf(out, n, "%s%lld %s",
             d > 0 ? "\xe2\x96\xb2 +" : "\xe2\x96\xbc \xe2\x88\x92",   /* ▲ + / ▼ − */
             d > 0 ? d : -d, unit);
}

const char *recap_a_an(const char *s)
{
    char c;
    if (!s || !*s) return "a";
    c = (char)((s[0] >= 'A' && s[0] <= 'Z') ? s[0] - 'A' + 'a' : s[0]);
    return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') ? "an" : "a";
}

void recap_share_card(int mode, const long long w[4], const RecapResult *r,
                      char *out, size_t n)
{
    char buf[8192], hours[32];
    size_t used;
    const char *pname = mode == RECAP_WEEK ? "week" : mode == RECAP_MONTH ? "month" : "year";

    snprintf(buf, sizeof buf, "my %s in marimo", pname);
    if (mode == RECAP_MONTH || mode == RECAP_YEAR) {
        struct tm tmv;
        char stamp[64];
        recap_local_tm(w[0], &tmv);
        strftime(stamp, sizeof stamp, mode == RECAP_MONTH ? "%B %Y" : "%Y", &tmv);
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " (%s)", stamp);
    }
    recap_fmt_hours(r->total_sec, 1, hours, sizeof hours);
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
             "\n%lld tracks \xc2\xb7 %s \xc2\xb7 %lld artists \xc2\xb7 %lld albums",
             r->tracks, hours, r->artists, r->albums);
    if (r->persona[0])
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "\nmostly %s %s",
                 recap_a_an(r->persona), r->persona);
    if (r->streak_days > 0)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                 "\nstreak: %d days in a row", r->streak_days);
    if (r->longest_session_sec > 0) {
        recap_fmt_hours(r->longest_session_sec, 1, hours, sizeof hours);
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                 "\nlongest session: %s", hours);
    }
    if (r->skip_rate > 0)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                 "\nskipped %d%% of starts", r->skip_rate);
    if (r->replay_king_plays >= 2)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                 "\nlooped \"%s\" \xc3\x97%lld", r->replay_king, r->replay_king_plays);
    if (r->most_skipped_plays > 0)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                 "\nmost-skipped \"%s\"", r->most_skipped);
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
             "\n%d%% new artists", r->discovery_pct);
    if (r->n_top_artists > 0)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                 "\ntop artist: %s", r->top_artists[0].name);

    used = strlen(buf);
    snprintf(out, n, "%s", used < n ? buf : "");
}
