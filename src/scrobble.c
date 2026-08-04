/* scrobble.c — Last.fm + ListenBrainz via libcurl, worker thread + auth thread.
 * All network work happens on background threads; the UI never blocks. */
#include "scrobble.h"
#include "config.h"
#include "md5.h"
#include <curl/curl.h>
#include "cJSON.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#ifndef sleep
#define sleep(s) Sleep((s) * 1000)
#endif
#endif

#define LF_ENDPOINT "https://ws.audioscrobbler.com/2.0/"
#define LB_ENDPOINT "https://api.listenbrainz.org/1/submit-listens"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static pthread_t worker, auth;
static int worker_run = 1;

typedef struct {
    int type;         /* 0 = now playing, 1 = submit */
    int played;
    Meta meta;
} Job;

static Job *jobs = NULL;
static int n_jobs = 0, cap_jobs = 0;

static volatile int auth_state = 0;   /* 0 idle, 1 waiting, 2 done, -1 failed */
static char auth_msg[256] = "";

/* ---------------- helpers ---------------- */

static void set_msg(const char *fmt, const char *a)
{
    snprintf(auth_msg, sizeof auth_msg, fmt, a);
}

typedef struct { char *data; size_t len; } Buf;

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
    Buf *b = (Buf *)ud;
    size_t n = sz * nm;
    b->data = (char *)realloc(b->data, b->len + n + 1);
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

static int http_post(const char *url, const char *body, const char *auth_hdr, Buf *out)
{
    CURL *c = curl_easy_init();
    struct curl_slist *hdrs = NULL;
    CURLcode res;
    if (!c) return -1;
    memset(out, 0, sizeof(*out));
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "marimo/1.1");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    if (auth_hdr) {
        hdrs = curl_slist_append(hdrs, auth_hdr);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    }
    res = curl_easy_perform(c);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if (res != CURLE_OK) {
        fprintf(stderr, "marimo: scrobble http error: %s\n", curl_easy_strerror(res));
        free(out->data);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

static void urlenc(const char *s, char *out, size_t outsz)
{
    static const char *hex = "0123456789ABCDEF";
    size_t i = 0;
    while (*s && i + 3 < outsz - 1) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[i++] = (char)c;
        } else {
            out[i++] = '%';
            out[i++] = hex[c >> 4];
            out[i++] = hex[c & 15];
        }
        s++;
    }
    out[i] = 0;
}

/* ---------------- last.fm ---------------- */

typedef struct { const char *k; const char *v; } LFParam;

static int lf_cmp(const void *a, const void *b)
{
    return strcmp(((const LFParam *)a)->k, ((const LFParam *)b)->k);
}

/* POST an API call; returns 0 + JSON in out on success */
static int lf_api(LFParam *params, int n, Buf *out)
{
    char body[16384], enc[4096];
    char sigstr[16384];
    char sig[33];
    int pos = 0, i;
    /* signature: sorted key+value concatenation + secret (no format param) */
    qsort(params, n, sizeof(LFParam), lf_cmp);
    sigstr[0] = 0;
    for (i = 0; i < n; i++) {
        pos += snprintf(sigstr + pos, sizeof sigstr - pos, "%s%s", params[i].k, params[i].v);
        if (pos >= (int)sizeof sigstr - 64) return -1;
    }
    pos += snprintf(sigstr + pos, sizeof sigstr - pos, "%s", cfg.lf_secret);
    md5_hex(sigstr, pos, sig);

    pos = 0;
    for (i = 0; i < n; i++) {
        urlenc(params[i].v, enc, sizeof enc);
        pos += snprintf(body + pos, sizeof body - pos, "%s=%s&", params[i].k, enc);
    }
    snprintf(body + pos, sizeof body - pos, "format=json&api_sig=%s", sig);
    if (http_post(LF_ENDPOINT, body, NULL, out) < 0) return -1;
    return 0;
}

static void lf_add_common(LFParam *p, int *n, const char *method)
{
    p[(*n)++] = (LFParam){ "method", method };
    p[(*n)++] = (LFParam){ "api_key", cfg.lf_key };
    if (cfg.lf_session[0]) p[(*n)++] = (LFParam){ "sk", cfg.lf_session };
}

static void lf_add_track(LFParam *p, int *n, const Meta *m)
{
    p[(*n)++] = (LFParam){ "artist", m->artist };
    p[(*n)++] = (LFParam){ "track", m->title };
    if (m->album[0]) p[(*n)++] = (LFParam){ "album", m->album };
}

static void lf_now_playing(const Meta *m)
{
    LFParam p[8];
    int n = 0;
    Buf out;
    lf_add_common(p, &n, "track.updateNowPlaying");
    lf_add_track(p, &n, m);
    if (lf_api(p, n, &out) == 0) free(out.data);
}

static void lf_scrobble(const Meta *m)
{
    LFParam p[10];
    int n = 0;
    char ts[32];
    Buf out;
    cJSON *j;
    lf_add_common(p, &n, "track.scrobble");
    lf_add_track(p, &n, m);
    snprintf(ts, sizeof ts, "%lld", (long long)time(NULL));
    p[n++] = (LFParam){ "timestamp", ts };
    if (lf_api(p, n, &out) == 0) {
        j = cJSON_Parse(out.data ? out.data : "");
        if (j) {
            cJSON *err = cJSON_GetObjectItem(j, "error");
            if (err && err->valueint)
                fprintf(stderr, "marimo: last.fm scrobble error %d\n", err->valueint);
            cJSON_Delete(j);
        }
        free(out.data);
    }
}

/* ---------------- listenbrainz ---------------- */

static void lb_send(const Meta *m, int listened_at)
{
    cJSON *root, *payload, *it, *tm, *ai;
    char *json;
    Buf out;
    char auth_hdr[256];
    if (!cfg.lb_token[0]) return;
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "listen_type", listened_at < 0 ? "playing_now" : "single");
    payload = cJSON_AddArrayToObject(root, "payload");
    it = cJSON_CreateObject();
    if (listened_at >= 0) cJSON_AddNumberToObject(it, "listened_at", listened_at);
    tm = cJSON_CreateObject();
    cJSON_AddStringToObject(tm, "track_name", m->title);
    cJSON_AddStringToObject(tm, "artist_name", m->artist);
    if (m->album[0]) cJSON_AddStringToObject(tm, "album_name", m->album);
    ai = cJSON_CreateObject();
    if (m->duration_ms > 0) cJSON_AddNumberToObject(ai, "duration", m->duration_ms / 1000);
    cJSON_AddItemToObject(tm, "additional_info", ai);
    cJSON_AddItemToObject(it, "track_metadata", tm);
    cJSON_AddItemToArray(payload, it);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Token %.*s",
             (int)sizeof auth_hdr - 32, cfg.lb_token);
    if (http_post(LB_ENDPOINT, json, auth_hdr, &out) == 0) free(out.data);
    free(json);
}

/* ---------------- job queue ---------------- */

static void enqueue(int type, const Meta *m, int played)
{
    pthread_mutex_lock(&lock);
    if (n_jobs == cap_jobs) {
        cap_jobs = cap_jobs ? cap_jobs * 2 : 16;
        jobs = (Job *)realloc(jobs, cap_jobs * sizeof(Job));
    }
    jobs[n_jobs].type = type;
    jobs[n_jobs].played = played;
    jobs[n_jobs].meta = *m;
    n_jobs++;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
}

static void *worker_thread(void *x)
{
    (void)x;
    for (;;) {
        Job j;
        pthread_mutex_lock(&lock);
        while (worker_run && n_jobs == 0)
            pthread_cond_wait(&cond, &lock);
        if (!worker_run) { pthread_mutex_unlock(&lock); break; }
        j = jobs[0];
        memmove(jobs, jobs + 1, (n_jobs - 1) * sizeof(Job));
        n_jobs--;
        pthread_mutex_unlock(&lock);

        if (!j.meta.artist[0] || !j.meta.title[0]) continue;  /* can't scrobble */
        if (j.type == 0) {
            if (cfg.lf_key[0] && cfg.lf_secret[0] && cfg.lf_session[0]) lf_now_playing(&j.meta);
            if (cfg.lb_token[0]) lb_send(&j.meta, -1);
        } else {
            if (cfg.lf_key[0] && cfg.lf_secret[0] && cfg.lf_session[0]) lf_scrobble(&j.meta);
            if (cfg.lb_token[0]) lb_send(&j.meta, (int)time(NULL));
        }
    }
    return NULL;
}

/* ---------------- last.fm auth ---------------- */

static void *auth_thread(void *x)
{
    (void)x;
    LFParam p[4];
    Buf out;
    cJSON *j, *tok;
    char url[512];
    char *token = NULL;
    int tries;

    p[0] = (LFParam){ "method", "auth.gettoken" };
    p[1] = (LFParam){ "api_key", cfg.lf_key };
    if (lf_api(p, 2, &out) < 0) {
        set_msg("auth failed: network error", "");
        auth_state = -1;
        return NULL;
    }
    j = cJSON_Parse(out.data ? out.data : "");
    free(out.data);
    if (!j) { auth_state = -1; set_msg("auth failed: bad response", ""); return NULL; }
    tok = cJSON_GetObjectItem(j, "token");
    if (!tok || !tok->valuestring) {
        cJSON_Delete(j);
        auth_state = -1;
        set_msg("auth failed: invalid api key", "");
        return NULL;
    }
    token = strdup(tok->valuestring ? tok->valuestring : "");
    snprintf(url, sizeof url, "https://www.last.fm/api/auth/?api_key=%.*s&token=%.*s",
             200, cfg.lf_key, 200, token);
    cJSON_Delete(j);

    {
        char cmd[600];
#ifdef _WIN32
        snprintf(cmd, sizeof cmd, "start \"\" \"%s\"", url);
#else
        snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", url);
#endif
        if (system(cmd) != 0)
            fprintf(stderr, "marimo: could not open browser for last.fm auth\n");
    }
    auth_state = 1;
    set_msg("approve the request in your browser...", "");

    for (tries = 0; tries < 90; tries++) {
        sleep(2);
        p[0] = (LFParam){ "method", "auth.getsession" };
        p[1] = (LFParam){ "api_key", cfg.lf_key };
        p[2] = (LFParam){ "token", token };
        if (lf_api(p, 3, &out) < 0) continue;
        j = cJSON_Parse(out.data ? out.data : "");
        free(out.data);
        if (!j) continue;
        {
            cJSON *sess = cJSON_GetObjectItem(j, "session");
            cJSON *key = sess ? cJSON_GetObjectItem(sess, "key") : NULL;
            cJSON *name = sess ? cJSON_GetObjectItem(sess, "name") : NULL;
            if (key && key->valuestring) {
                snprintf(cfg.lf_session, sizeof cfg.lf_session, "%s", key->valuestring);
                snprintf(cfg.lf_user, sizeof cfg.lf_user, "%s",
                         name && name->valuestring ? name->valuestring : "");
                config_save();
                set_msg("authorized as %s", cfg.lf_user[0] ? cfg.lf_user : "last.fm user");
                auth_state = 2;
                cJSON_Delete(j);
                free(token);
                return NULL;
            }
        }
        cJSON_Delete(j);
    }
    auth_state = -1;
    set_msg("auth timed out — try again", "");
    free(token);
    return NULL;
}

/* ---------------- public ---------------- */

void scrobble_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_create(&worker, NULL, worker_thread, NULL);
}

void scrobble_shutdown(void)
{
    pthread_mutex_lock(&lock);
    worker_run = 0;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    if (auth_state == 1) {
        pthread_cancel(auth);   /* still waiting on browser — give up quietly */
        pthread_join(auth, NULL);
    }
    curl_global_cleanup();
}

void scrobble_now_playing(const Meta *m)
{
    enqueue(0, m, 0);
}

void scrobble_submit(const Meta *m, int played_secs)
{
    (void)played_secs;
    enqueue(1, m, played_secs);
}

void scrobble_auth_start(void)
{
    if (auth_state == 1) return;          /* already running */
    if (!cfg.lf_key[0] || !cfg.lf_secret[0]) {
        set_msg("need api key + secret in settings first", "");
        auth_state = -1;
        return;
    }
    auth_state = 0;
    set_msg("contacting last.fm...", "");
    pthread_create(&auth, NULL, auth_thread, NULL);
}

int scrobble_auth_state(void)
{
    return auth_state;
}

const char *scrobble_auth_msg(void)
{
    return auth_msg;
}
