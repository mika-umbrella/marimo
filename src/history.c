/* history.c — listening diary: append-only jsonl, gzip year archives, tolerant
 * reads. See history.h for the format and the reasoning behind the fields. */
#include "history.h"
#include "cJSON.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>

static char h_dir[1024];
/* h_dir + "/" + a <=22 char archive name; this must be bigger than that or gcc
 * rightly complains that the snprintf might truncate */
#define H_PATH_MAX 1152
static pthread_mutex_t h_lock = PTHREAD_MUTEX_INITIALIZER;

static void dir_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", h_dir, name);
}

static int year_of(long long ms)
{
    time_t secs = (time_t)(ms / 1000);
    struct tm tmv;
#ifdef _WIN32
    struct tm *p = localtime(&secs);
    if (!p) return 1970;
    tmv = *p;
#else
    if (!localtime_r(&secs, &tmv)) return 1970;
#endif
    return tmv.tm_year + 1900;
}

/* ---------------- writing ---------------- */

/* append a JSON string literal, escaped the way HistoryDiary.line() does */
static void put_q(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (c < 0x20) fprintf(fp, "\\u%04x", c);
            else fputc(c, fp);
        }
    }
    fputc('"', fp);
}

char *history_line(const char *artist, const char *album, const char *title,
                   long long played_ms, long long dur_ms, long long ts)
{
    char *buf = NULL;
    size_t cap = 0;
    FILE *fp = open_memstream(&buf, &cap);
    if (!fp) return NULL;
    fprintf(fp, "{\"ts\":%lld,\"artist\":", ts);
    put_q(fp, artist ? artist : "");
    fputs(",\"album\":", fp);
    put_q(fp, album ? album : "");
    fputs(",\"title\":", fp);
    put_q(fp, title ? title : "");
    fprintf(fp, ",\"sec\":%lld,\"dur\":%lld}\n", played_ms / 1000,
            dur_ms > 0 ? dur_ms : 0);
    fclose(fp);
    return buf;                       /* caller frees */
}

static int append_file(const char *path, const char *line)
{
    FILE *fp = fopen(path, "ab");
    if (!fp) return -1;
    fputs(line, fp);
    fclose(fp);
    return 0;
}

/* gzip src into dst (whole file), then unlink src — the year rollover */
static int gzip_aside(const char *src, const char *dst)
{
    gzFile out = gzopen(dst, "wb6");
    if (!out) return -1;
    FILE *in = fopen(src, "rb");
    if (!in) { gzclose(out); return -1; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) gzwrite(out, buf, (unsigned)n);
    fclose(in);
    gzclose(out);
    remove(src);
    return 0;
}

/* drop archives older than the previous year: the vs-last-year report needs one
 * full previous year, nothing older ever gets read */
static void prune(int current_year)
{
    char name[32];
    int y;
    for (y = current_year - 2; y > current_year - 30; y--) {
        snprintf(name, sizeof name, "history.%d.jsonl.gz", y);
        char full[H_PATH_MAX];
        dir_path(full, sizeof full, name);
        remove(full);                 /* harmless if it isn't there */
    }
}

void history_log(const char *artist, const char *album, const char *title,
                 long long played_ms, long long dur_ms)
{
    char *line;
    char cur[H_PATH_MAX], arch[H_PATH_MAX];
    long long now;

    if (!h_dir[0]) return;
    now = (long long)time(NULL) * 1000;
    line = history_line(artist, album, title, played_ms, dur_ms, now);
    if (!line) return;

    pthread_mutex_lock(&h_lock);
    dir_path(cur, sizeof cur, "history.jsonl");

    struct stat st;
    if (stat(cur, &st) == 0 && st.st_size > 0) {
        /* the file on disk belongs to a different year (live rollover, or a
         * cold start after Jan 1) -> archive it and start fresh */
        int file_year = year_of((long long)st.st_mtime * 1000);
        int this_year = year_of(now);
        if (file_year != this_year) {
            char name[32];
            snprintf(name, sizeof name, "history.%d.jsonl.gz", file_year);
            dir_path(arch, sizeof arch, name);
            gzip_aside(cur, arch);
        }
    }
    append_file(cur, line);
    prune(year_of(now));
    pthread_mutex_unlock(&h_lock);
    free(line);
}

/* ---------------- reading ---------------- */

int history_parse_line(const char *line, HistoryEntry *out)
{
    cJSON *j;
    const cJSON *v;
    if (!line || !out) return -1;
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
    if (*line != '{') return -1;

    j = cJSON_Parse(line);
    if (!j) return -1;
    memset(out, 0, sizeof *out);

    v = cJSON_GetObjectItemCaseSensitive(j, "ts");
    /* NB: cJSON only fills valuestring for strings — a number carries its value
     * in valuedouble, so reading ->valuestring here (as I first did) silently
     * rejects every line. */
    if (!cJSON_IsNumber(v)) { cJSON_Delete(j); return -1; }
    out->ts = (long long)v->valuedouble;
    if (out->ts < 0) { cJSON_Delete(j); return -1; }

    v = cJSON_GetObjectItemCaseSensitive(j, "sec");
    if (cJSON_IsNumber(v)) out->sec = (long long)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(j, "dur");
    if (cJSON_IsNumber(v)) out->dur = (long long)v->valuedouble;

    v = cJSON_GetObjectItemCaseSensitive(j, "artist");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(out->artist, sizeof out->artist, "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "album");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(out->album, sizeof out->album, "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "title");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(out->title, sizeof out->title, "%s", v->valuestring);

    cJSON_Delete(j);
    return 0;
}

static void collect_file(const char *path, int gz, HistoryEntry **arr, size_t *n,
                         size_t *cap, long long lo, long long hi)
{
    FILE *fp = NULL;
    gzFile gzfp = NULL;
    char line[8192];

    if (gz) {
        gzfp = gzopen(path, "rb");
        if (!gzfp) return;
    } else {
        fp = fopen(path, "r");
        if (!fp) return;
    }

    for (;;) {
        char *got;
        if (gz) got = gzgets(gzfp, line, (int)sizeof line);
        else got = fgets(line, (int)sizeof line, fp);
        if (!got) break;
        HistoryEntry e;
        if (history_parse_line(line, &e) != 0) continue;   /* skip junk */
        if (e.ts < lo || e.ts >= hi) continue;
        if (*n == *cap) {
            size_t ncap = *cap ? *cap * 2 : 64;
            HistoryEntry *na = realloc(*arr, ncap * sizeof *na);
            if (!na) break;
            *arr = na;
            *cap = ncap;
        }
        (*arr)[(*n)++] = e;
    }
    if (gz) gzclose(gzfp);
    else fclose(fp);
}

HistoryEntry *history_window(long long start_ms, long long end_ms, size_t *n)
{
    HistoryEntry *arr = NULL;
    size_t cnt = 0, cap = 0;
    int y, y0, y1, cur_year;
    char path[H_PATH_MAX];

    *n = 0;
    if (!h_dir[0] || end_ms <= start_ms) return NULL;

    cur_year = year_of((long long)time(NULL) * 1000);
    y0 = year_of(start_ms);
    y1 = year_of(end_ms - 1);
    if (y1 > cur_year) y1 = cur_year;

    pthread_mutex_lock(&h_lock);
    for (y = y0; y <= y1; y++) {
        if (y == cur_year) {
            dir_path(path, sizeof path, "history.jsonl");
            collect_file(path, 0, &arr, &cnt, &cap, start_ms, end_ms);
        } else {
            char name[32];
            snprintf(name, sizeof name, "history.%d.jsonl.gz", y);
            dir_path(path, sizeof path, name);
            collect_file(path, 1, &arr, &cnt, &cap, start_ms, end_ms);
        }
    }
    pthread_mutex_unlock(&h_lock);

    *n = cnt;
    return arr;
}

void history_free(HistoryEntry *e)
{
    free(e);
}

void history_init(const char *dir)
{
    if (!dir || !dir[0]) return;
    snprintf(h_dir, sizeof h_dir, "%s", dir);
#ifndef _WIN32
    mkdir(h_dir, 0755);               /* the app's config dir usually exists */
#else
    _mkdir(h_dir);
#endif
}

void history_shutdown(void)
{
    h_dir[0] = 0;
}
