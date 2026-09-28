/* waveform.c — see waveform.h for the why and the sidecar layout. */
#ifndef _GNU_SOURCE          /* gio-2.0's pkg-config cflags already define this */
#define _GNU_SOURCE          /* ...we want strdup, and want it declared */
#endif
#include "waveform.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WAVE_RATE   8000        /* must match make-waveforms.py RATE */
#define WAVE_STRIDE 4           /* ...and STRIDE */
#define WAVE_SRC_MAX (400u * 1024u * 1024u)   /* refuse to buffer more than this */
#define WAVE_SIDECAR_MAX (16u * 1024u * 1024u)

/* ---------------- sidecar ---------------- */

int wave_sidecar_parse(const unsigned char *blob, size_t n, const char *name,
                       unsigned char out[WAVE_BUCKETS])
{
    static const char magic[8] = { 'M', 'W', 'A', 'V', 'S', '0', '0', '1' };
    uint32_t count;
    size_t off = 12;
    uint32_t i;

    if (!blob || n < 12 || !name) return 0;
    if (memcmp(blob, magic, 8) != 0) return 0;
    count = (uint32_t)blob[8] | ((uint32_t)blob[9] << 8)
          | ((uint32_t)blob[10] << 16) | ((uint32_t)blob[11] << 24);
    if (count == 0) return 0;

    for (i = 0; i < count; i++) {
        uint32_t len;
        if (off + 2 > n) break;
        len = (uint32_t)blob[off] | ((uint32_t)blob[off + 1] << 8);
        off += 2;
        if (len == 0 || off + len + WAVE_BUCKETS > n) break;
        /* Exact byte compare. The phone NFC-normalises both sides because SAF
         * hands back the same Japanese name in different normal forms depending
         * on where it travelled. Here both the sidecar and the file name come
         * from one readdir of one filesystem, so they are already the same
         * bytes — normalising would need ICU for no gain. */
        if (strlen(name) == len && memcmp(blob + off, name, len) == 0) {
            memcpy(out, blob + off + len, WAVE_BUCKETS);
            return 1;
        }
        off += len + WAVE_BUCKETS;
    }
    return 0;
}

int wave_sidecar_read(const char *album_dir, const char *track_name,
                      unsigned char out[WAVE_BUCKETS])
{
    char path[4096];
    unsigned char *blob;
    struct stat st;
    size_t got;
    int fd, ok;

    if (!album_dir || !track_name) return 0;
    snprintf(path, sizeof path, "%s/%s", album_dir, WAVE_SIDECAR);
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0 || st.st_size < 12 || (size_t)st.st_size > WAVE_SIDECAR_MAX) {
        close(fd);
        return 0;
    }
    blob = malloc((size_t)st.st_size);
    if (!blob) { close(fd); return 0; }
    got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, blob + got, (size_t)st.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    ok = wave_sidecar_parse(blob, got, track_name, out);
    free(blob);
    return ok;
}

/* ---------------- decode ---------------- */

/* read() the child's stdout into one buffer. */
static unsigned char *slurp_fd(int fd, size_t *out_len)
{
    size_t cap = 1u << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        ssize_t r;
        if (len == cap) {
            unsigned char *nb;
            if (cap >= WAVE_SRC_MAX) { free(buf); return NULL; }
            cap *= 2;
            nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return NULL;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    *out_len = len;
    return buf;
}

/* fork/exec rather than popen(): no shell means no quoting rules to get wrong,
 * and her library is full of Japanese names, '&', and apostrophes. */
static int ffmpeg_pcm(const char *path, unsigned char **buf, size_t *len)
{
    int fd[2];
    pid_t pid;

    *buf = NULL;
    *len = 0;
    if (pipe(fd) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        int devnull;
        close(fd[0]);
        if (dup2(fd[1], STDOUT_FILENO) < 0) _exit(127);
        if (fd[1] != STDOUT_FILENO) close(fd[1]);
        /* ffmpeg's complaints must never land on our terminal or the TUI */
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        {
            char *argv[] = {
                (char *)"ffmpeg", (char *)"-v", (char *)"error", (char *)"-nostdin",
                (char *)"-i", (char *)path, (char *)"-map", (char *)"0:a:0",
                (char *)"-ac", (char *)"1", (char *)"-ar", (char *)"8000",
                (char *)"-f", (char *)"s16le", (char *)"-", NULL
            };
            execvp(argv[0], argv);
        }
        _exit(127);
    }
    close(fd[1]);
    *buf = slurp_fd(fd[0], len);
    close(fd[0]);
    {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            free(*buf);
            *buf = NULL;
            return -1;
        }
    }
    if (!*buf || *len < 2) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    return 0;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

int wave_decode(const char *path, unsigned char out[WAVE_BUCKETS])
{
    unsigned char *raw = NULL;
    size_t bytes = 0, n, i;
    double sumsq[WAVE_BUCKETS], rms[WAVE_BUCKETS], sorted[WAVE_BUCKETS], ref;
    long counts[WAVE_BUCKETS];
    int k;

    if (!path) return 0;
    if (ffmpeg_pcm(path, &raw, &bytes) != 0) return 0;
    n = bytes / 2;                       /* int16 mono */
    if (n == 0) { free(raw); return 0; }

    for (k = 0; k < WAVE_BUCKETS; k++) { sumsq[k] = 0.0; counts[k] = 0; }
    for (i = 0; i < n; i += WAVE_STRIDE) {
        int16_t s;
        size_t b;
        /* the buffer is little-endian PCM: x86 and ARM both are, and ffmpeg was
         * told s16le rather than native, so this is the same on either */
        memcpy(&s, raw + i * 2, sizeof s);
        b = (i * WAVE_BUCKETS) / n;
        if (b >= WAVE_BUCKETS) b = WAVE_BUCKETS - 1;
        sumsq[b] += (double)s * (double)s;
        counts[b]++;
    }
    free(raw);

    for (k = 0; k < WAVE_BUCKETS; k++)
        rms[k] = counts[k] ? sqrt(sumsq[k] / counts[k]) : 0.0;
    memcpy(sorted, rms, sizeof rms);
    qsort(sorted, WAVE_BUCKETS, sizeof sorted[0], cmp_double);
    ref = sorted[(int)(WAVE_BUCKETS * 0.95)];      /* index 91, as the generator */
    if (ref == 0.0) ref = 1.0;

    for (k = 0; k < WAVE_BUCKETS; k++) {
        int v;
        if (!counts[k]) { out[k] = 0; continue; }
        v = (int)(100.0 * sqrt(rms[k] / ref));
        if (v < 4) v = 4;
        if (v > 100) v = 100;
        out[k] = (unsigned char)v;
    }
    return 1;
}

/* ---------------- cache ---------------- */

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ull;
    }
    return h;
}

static int cache_path_for(const char *path, char *out, size_t n)
{
    const char *base = getenv("XDG_CACHE_HOME");
    char key[1024];
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || !*home) return 0;
        snprintf(key, sizeof key, "%s/.cache/marimo/waves", home);
    } else {
        snprintf(key, sizeof key, "%s/marimo/waves", base);
    }
    /* a truncated path would silently mean "no cache" for every file that long,
     * so treat it as no cache rather than guessing */
    if ((size_t)snprintf(out, n, "%s/%016llx-%016llx-%016llx.w", key,
                         (unsigned long long)fnv1a(path),
                         (unsigned long long)st.st_size,
                         (unsigned long long)st.st_mtim.tv_nsec
                             + (unsigned long long)st.st_mtime * 1000000000ull) >= n)
        return 0;
    return 1;
}

static void cache_dir_make(const char *file)
{
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", file);
    {   /* mkdir -p on the cache root: ~/.cache/marimo/waves */
        char *s;
        for (s = dir + 1; *s; s++) {
            if (*s != '/') continue;
            *s = 0;
            mkdir(dir, 0755);          /* EEXIST is fine and expected */
            *s = '/';
        }
    }
}

int wave_decode_cached(const char *path, unsigned char out[WAVE_BUCKETS])
{
    char cache[4096];
    int have_cache = cache_path_for(path, cache, sizeof cache);

    if (have_cache) {
        struct stat st;
        int fd = open(cache, O_RDONLY);
        if (fd >= 0) {
            size_t got = 0;
            int ok = 1;
            if (fstat(fd, &st) != 0 || st.st_size != WAVE_BUCKETS) ok = 0;
            while (ok && got < WAVE_BUCKETS) {
                ssize_t r = read(fd, out + got, WAVE_BUCKETS - got);
                if (r <= 0) { ok = 0; break; }
                got += (size_t)r;
            }
            close(fd);
            if (ok) return 1;
        }
    }

    if (!wave_decode(path, out)) return 0;
    if (have_cache) {
        int fd;
        cache_dir_make(cache);
        fd = open(cache, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            size_t put = 0;
            while (put < WAVE_BUCKETS) {
                ssize_t w = write(fd, out + put, WAVE_BUCKETS - put);
                if (w <= 0) break;
                put += (size_t)w;
            }
            close(fd);
        }
    }
    return 1;
}

/* ---------------- front door ---------------- */

int wave_peaks(const char *path, unsigned char out[WAVE_BUCKETS])
{
    char dir[4096], *slash, *name;
    if (!path) return 0;

    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        name = (char *)path + (slash - dir) + 1;
        if (wave_sidecar_read(dir, name, out)) return 1;
    }
    return wave_decode_cached(path, out);
}
