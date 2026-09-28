/* player.c — libmpv wrapper. */
#include "player.h"
#include <mpv/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Player {
    mpv_handle *h;
    int pending;      /* 1 = EOF, 2 = error, waiting for main loop */
    int meta_dirty;   /* metadata property changed */
};

struct TagReader {
    mpv_handle *h;
};

static void setopt(mpv_handle *h, const char *name, const char *val)
{
    if (mpv_set_option_string(h, name, val) < 0)
        fprintf(stderr, "marimo: mpv option '%s' failed\n", name);
}

Player *player_create(const char *aout_override)
{
    Player *p = (Player *)calloc(1, sizeof(Player));
    p->h = mpv_create();
    if (!p->h) { free(p); return NULL; }
    /* the gapless trio: */
    setopt(p->h, "gapless-audio", "yes");
    setopt(p->h, "audio-display", "no");
    setopt(p->h, "vo", "null");
    setopt(p->h, "idle", "yes");
    setopt(p->h, "keep-open", "no");
    setopt(p->h, "load-scripts", "no");
    setopt(p->h, "osc", "no");
    setopt(p->h, "osd-level", "0");
    setopt(p->h, "volume-max", "100");
    setopt(p->h, "audio-client-name", "marimo");
    if (aout_override)
        setopt(p->h, "ao", aout_override);
    if (mpv_initialize(p->h) < 0) {
        mpv_terminate_destroy(p->h);
        free(p);
        return NULL;
    }
    mpv_observe_property(p->h, 0, "metadata", MPV_FORMAT_NODE);
    return p;
}

void player_destroy(Player *p)
{
    if (!p) return;
    mpv_terminate_destroy(p->h);
    free(p);
}

int player_play_with_next(Player *p, const char *path, const char *next_path)
{
    const char *clear[] = { "playlist-clear", NULL };
    const char *load[] = { "loadfile", path, NULL };
    const char *app[] = { "loadfile", next_path, "append-play", NULL };
    int loaded = 0;
    if (mpv_command(p->h, clear) < 0) return -1;
    if (mpv_command(p->h, load) < 0) return -1;
    if (next_path)
        mpv_command(p->h, app);   /* preload the gapless next entry */
    /* wait briefly for FILE_LOADED so metadata is readable.
     * END_FILE with ERROR = the new file failed; STOP/EOF = old file being
     * replaced — keep waiting in that case. */
    for (int i = 0; i < 100; i++) {
        mpv_event *ev = mpv_wait_event(p->h, 10);
        if (ev->event_id == MPV_EVENT_NONE) continue;
        if (ev->event_id == MPV_EVENT_FILE_LOADED) { loaded = 1; break; }
        if (ev->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *e = (mpv_event_end_file *)ev->data;
            if (e->reason == MPV_END_FILE_REASON_ERROR) return -1;
        }
    }
    return loaded ? 0 : 1;
}

/* after an EOF auto-advance the finished entry is always playlist index 0 —
 * remove it (playlist-remove takes an INDEX, not the entry id!) and preload
 * the new following entry. */
void player_gapless_shift(Player *p, const char *next_path)
{
    const char *rm[] = { "playlist-remove", "0", NULL };
    const char *app[] = { "loadfile", next_path, "append-play", NULL };
    mpv_command(p->h, rm);
    if (next_path)
        mpv_command(p->h, app);
}

int player_playlist_count(Player *p)
{
    int64_t n = 0;
    mpv_get_property(p->h, "playlist-count", MPV_FORMAT_INT64, &n);
    return (int)n;
}

/* See player.h: the queue is not mirrored in mpv, so after a reorder or a removal
 * the only thing to fix is which entry plays next. */
void player_set_next(Player *p, const char *path)
{
    const char *rm[] = { "playlist-remove", "1", NULL };
    const char *app[] = { "loadfile", path, "append-play", NULL };
    if (player_playlist_count(p) > 1) mpv_command(p->h, rm);
    if (path) mpv_command(p->h, app);
}

void player_dbg_playlist(Player *p)
{
    mpv_node node;
    if (mpv_get_property(p->h, "playlist", MPV_FORMAT_NODE, &node) == 0) {
        printf("  [pl] ");
        if (node.format == MPV_FORMAT_NODE_ARRAY) {
            for (int i = 0; i < node.u.list->num; i++) {
                mpv_node *e = &node.u.list->values[i];
                if (e->format != MPV_FORMAT_NODE_MAP) continue;
                int64_t id = 0;
                const char *fn = "?";
                for (int k = 0; k < e->u.list->num; k++) {
                    if (!strcmp(e->u.list->keys[k], "id"))
                        id = e->u.list->values[k].u.int64;
                    if (!strcmp(e->u.list->keys[k], "filename"))
                        fn = e->u.list->values[k].u.string;
                }
                const char *b = strrchr(fn, '/');
                printf("[%lld %s] ", (long long)id, b ? b + 1 : fn);
            }
        }
        printf("\n");
        mpv_free_node_contents(&node);
    }
}

void player_set_pause(Player *p, int paused)
{
    int v = paused ? 1 : 0;
    mpv_set_property(p->h, "pause", MPV_FORMAT_FLAG, &v);
}

int player_paused(Player *p)
{
    int v = 0;
    mpv_get_property(p->h, "pause", MPV_FORMAT_FLAG, &v);
    return v;
}

void player_stop(Player *p)
{
    const char *cmd[] = { "stop", NULL };
    mpv_command(p->h, cmd);
}

int player_state(Player *p)
{
    int paused = 0, idle = 0;
    mpv_get_property(p->h, "pause", MPV_FORMAT_FLAG, &paused);
    mpv_get_property(p->h, "core-idle", MPV_FORMAT_FLAG, &idle);
    if (paused) return 2;
    if (idle) return 0;
    return 1;
}

double player_time(Player *p)
{
    double t = -1;
    mpv_get_property(p->h, "time-pos", MPV_FORMAT_DOUBLE, &t);
    return t;
}

double player_length(Player *p)
{
    double d = -1;
    mpv_get_property(p->h, "duration", MPV_FORMAT_DOUBLE, &d);
    return d;
}

void player_seek(Player *p, double secs)
{
    mpv_set_property(p->h, "time-pos", MPV_FORMAT_DOUBLE, &secs);
}

void player_set_volume(Player *p, int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    mpv_set_property(p->h, "volume", MPV_FORMAT_INT64, &(int64_t){ v });
}

void player_set_mute(Player *p, int on)
{
    int v = on ? 1 : 0;
    mpv_set_property(p->h, "mute", MPV_FORMAT_FLAG, &v);
}

int player_get_volume(Player *p)
{
    int64_t v = 100;
    mpv_get_property(p->h, "volume", MPV_FORMAT_INT64, &v);
    return (int)v;
}

void player_set_repeat_one(Player *p, int on)
{
    const char *v = on ? "inf" : "no";
    mpv_set_property_string(p->h, "loop-file", v);
}

static int meta_from_handle(mpv_handle *h, Meta *meta);

static void node_str(mpv_node *n, char *dst, size_t dstsz)
{
    if (n->format == MPV_FORMAT_STRING)
        snprintf(dst, dstsz, "%s", n->u.string ? n->u.string : "");
    else if (n->format == MPV_FORMAT_NODE_ARRAY && n->u.list->num > 0
             && n->u.list->values[0].format == MPV_FORMAT_STRING)
        snprintf(dst, dstsz, "%s", n->u.list->values[0].u.string ? n->u.list->values[0].u.string : "");
}

int player_meta(Player *p, Meta *meta)
{
    return meta_from_handle(p->h, meta);
}

static int meta_from_handle(mpv_handle *h, Meta *meta)
{
    mpv_node node;
    int i;
    memset(meta, 0, sizeof(*meta));
    if (mpv_get_property(h, "metadata", MPV_FORMAT_NODE, &node) < 0)
        return 0;
    if (node.format != MPV_FORMAT_NODE_MAP) {
        mpv_free_node_contents(&node);
        return 0;
    }
    for (i = 0; i < node.u.list->num; i++) {
        const char *key = node.u.list->keys[i];
        mpv_node *val = &node.u.list->values[i];
        if (!strcmp(key, "title")) node_str(val, meta->title, sizeof meta->title);
        else if (!strcmp(key, "artist")) node_str(val, meta->artist, sizeof meta->artist);
        else if (!strcmp(key, "album")) node_str(val, meta->album, sizeof meta->album);
        else if (!strcmp(key, "track")) {
            if (val->format == MPV_FORMAT_INT64) meta->track = (int)val->u.int64;
            else if (val->format == MPV_FORMAT_STRING) meta->track = atoi(val->u.string ? val->u.string : "");
        }
        else if (!strcmp(key, "disc")) {
            if (val->format == MPV_FORMAT_INT64) meta->disc = (int)val->u.int64;
            else if (val->format == MPV_FORMAT_STRING) meta->disc = atoi(val->u.string ? val->u.string : "");
        }
    }
    mpv_free_node_contents(&node);
    {
        double d = -1;
        if (mpv_get_property(h, "duration", MPV_FORMAT_DOUBLE, &d) == 0 && d > 0)
            meta->duration_ms = (int)(d * 1000);
    }
    meta->have_meta = (meta->title[0] || meta->artist[0] || meta->album[0] || meta->duration_ms > 0);
    return meta->have_meta ? 1 : 0;
}

char *player_embedded_art(Player *p)
{
    mpv_node node;
    char *out = NULL;
    if (mpv_get_property(p->h, "album-art", MPV_FORMAT_NODE, &node) == 0
        && node.format == MPV_FORMAT_NODE_ARRAY && node.u.list->num > 0
        && node.u.list->values[0].format == MPV_FORMAT_STRING) {
        out = strdup(node.u.list->values[0].u.string ? node.u.list->values[0].u.string : "");
        mpv_free_node_contents(&node);
        if (out && out[0]) return out;
        free(out);
        out = NULL;
    } else if (mpv_get_property(p->h, "album-art", MPV_FORMAT_NODE, &node) == 0) {
        mpv_free_node_contents(&node);
    }
    /* older mpv: metadata cover key */
    {
        char *s = NULL;
        if (mpv_get_property(p->h, "metadata/by-key/cover", MPV_FORMAT_STRING, &s) == 0 && s) {
            out = strdup(s);
            mpv_free(s);
        }
    }
    return out;
}

int player_poll(Player *p)
{
    int result = 0;
    for (;;) {
        mpv_event *ev = mpv_wait_event(p->h, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *e = (mpv_event_end_file *)ev->data;
            /* reason STOP = manual stop or replace — never auto-advance */
            if (e->reason == MPV_END_FILE_REASON_EOF) {
                p->pending = 1;
            } else if (e->reason == MPV_END_FILE_REASON_ERROR) {
                p->pending = 2;
                fprintf(stderr, "marimo: playback error: %s\n",
                        mpv_error_string(e->error));
            }
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE:
            if (ev->reply_userdata == 0) p->meta_dirty = 1;
            break;
        default:
            break;
        }
    }
    if (p->pending) {
        result = p->pending;
        p->pending = 0;
    }
    return result;
}

int player_meta_dirty(Player *p)
{
    int d = p->meta_dirty;
    p->meta_dirty = 0;
    return d;
}

/* ---- background tag reader ---- */

TagReader *tagreader_create(void)
{
    TagReader *t = (TagReader *)calloc(1, sizeof(TagReader));
    t->h = mpv_create();
    if (!t->h) { free(t); return NULL; }
    setopt(t->h, "ao", "null");
    setopt(t->h, "vo", "null");
    setopt(t->h, "idle", "yes");
    setopt(t->h, "load-scripts", "no");
    if (mpv_initialize(t->h) < 0) {
        mpv_terminate_destroy(t->h);
        free(t);
        return NULL;
    }
    mpv_observe_property(t->h, 0, "metadata", MPV_FORMAT_NODE);
    return t;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void tagreader_destroy(TagReader *t)
{
    if (!t) return;
    mpv_terminate_destroy(t->h);
    free(t);
}

int tagreader_read(TagReader *t, const char *path, Meta *meta)
{
    const char *cmd[] = { "loadfile", path, "replace", NULL };
    int ok = 0, loaded = 0;
    long long start = now_ms();
    memset(meta, 0, sizeof(*meta));
    if (mpv_command(t->h, cmd) < 0) return 0;
    /* wait for tags up to 4s; metadata can arrive slightly after FILE_LOADED */
    while (now_ms() - start < 4000) {
        mpv_event *ev = mpv_wait_event(t->h, 50);
        if (ev->event_id == MPV_EVENT_FILE_LOADED) loaded = 1;
        else if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE && ev->reply_userdata == 0)
            loaded = 1;
        else if (ev->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *e = (mpv_event_end_file *)ev->data;
            if (e->reason == MPV_END_FILE_REASON_ERROR) break;
        }
        if (loaded && meta_from_handle(t->h, meta)) { ok = 1; break; }
    }
    if (!ok) ok = meta_from_handle(t->h, meta);
    {
        const char *stop[] = { "stop", NULL };
        mpv_command(t->h, stop);
    }
    return ok;
}
