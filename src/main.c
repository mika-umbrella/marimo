/* marimo — a tiny retro pixel music player.
 * SDL2 + libmpv + GNU Unifont. No GTK, no CSS, no feelings of guilt.
 *
 *  browse    : file-structure browser (breadcrumb + folder list)
 *  queue     : double-click = add to queue + play now
 *              middle-click = replace queue + play now
 *  art       : folder covers first, then embedded tags
 *  scrobble  : Last.fm (api key + device auth) and ListenBrainz (token)
 *  gapless   : mpv gapless-audio; accurate FLAC
 *  font      : GNU Unifont from unifont_all.hex — every weird character
 *
 * modes: --selftest  (headless self checks)   --headless=DIR (play pipeline test)
 *        --smoke     (render N frames, exit)  --music DIR   --aout NAME
 */
#ifdef _WIN32
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_image.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "font.h"
#include "player.h"
#include "library.h"
#include "queue.h"
#include "scrobble.h"
#include "config.h"
#include "md5.h"
#include "tags.h"
#include "mpris.h"
#include "fs.h"
#include "history.h"
#include <mpv/client.h>

#define APP_VER "1.1"

/* ---------------- palette ---------------- */
static const SDL_Color C_BG0    = { 13, 13, 15, 255 };
static const SDL_Color C_BG1    = { 21, 21, 24, 255 };
static const SDL_Color C_BG2    = { 30, 30, 35, 255 };
static const SDL_Color C_BD     = { 44, 44, 51, 255 };
static const SDL_Color C_TXT    = { 201, 201, 209, 255 };
static const SDL_Color C_DIM    = { 107, 107, 118, 255 };
static const SDL_Color C_ACC    = { 125, 255, 125, 255 };  /* winamp green */
static const SDL_Color C_ACC2   = { 183, 140, 255, 255 };
static const SDL_Color C_ERR    = { 255, 107, 107, 255 };
static const SDL_Color C_SELBG  = { 20, 44, 20, 255 };
static const SDL_Color C_AMBER  = { 232, 195, 106, 255 };

/* ---------------- geometry ---------------- */
#define PAD 6
#define HEADER_H 26
#define BAR_W 480              /* player bar width (narrower than the window) */
#define ART_SZ 128
#define CTRL_H 22
#define TAB_H 24
#define STATUS_H 18
#define ROW_H 20
#define ALPHA_W 18               /* alphabet jump strip width */
#define MINI_H (HEADER_H + 8 + ART_SZ + 8)

typedef struct {
    SDL_Rect header;
    SDL_Rect bar, art, t1, t2, t3;
    SDL_Rect b_prev, b_play, b_stop, b_next;
    SDL_Rect seek, time;
    SDL_Rect b_shuf, b_rep, vol_icon, vol, vol_txt;
    SDL_Rect b_minimize, b_mini, b_gear, b_close;
    SDL_Rect tabs_lib, tabs_q;
    SDL_Rect breadcrumb;
    SDL_Rect alpha;
    SDL_Rect list, status;
} Layout;

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    int w, h, mini;
    Layout L;
    Font font;
    Player *pl;
    TagReader *tr;
    Queue q;
    char cur_dir[L_PATH_MAX];
    LibEntry *entries;
    int n_entries, scroll, sel, hover;
    int q_scroll, q_sel, q_hover;
    int tab;                       /* 0 library, 1 queue */
    SDL_Texture *art;
    int art_retry;
    int show_rem;                  /* time display: remaining */
    int vol, drag;
    int alpha_drag;                /* alphabet strip drag */
    int muted;
    char status[256];
    Uint64 status_at;
    int last_row, last_list;
    Uint32 last_down;
    int meta_running;
    pthread_t meta_thr;
    volatile int meta_stop;
    /* modal */
    int modal, m_focus;
    char m_fields[4][512];
    /* misc */
    int running;
    double last_time;
    Meta last_meta;         /* survives queue end so the bar keeps its info */
    char last_name[Q_NAME_MAX];
    int have_last;
    char tip_buf[1200];     /* hover tooltip for truncated rows */
    int tip;
} App;

static App A;

static const char *status_idle = "";

/* ---------------- small helpers ---------------- */

static int inr(SDL_Rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* borderless window: drag via the header strip; fixed size otherwise */
static SDL_HitTestResult hit_test(SDL_Window *win, const SDL_Point *pt, void *data)
{
    int w;
    (void)data;
    SDL_GetWindowSize(win, &w, NULL);
    if (pt->y < HEADER_H && pt->x < w - 102) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.status, sizeof A.status, fmt, ap);
    va_end(ap);
    A.status_at = SDL_GetTicks64();
}

/* is path inside the library root (or the root itself)? */
static int path_under(const char *path, const char *root)
{
    size_t rl = strlen(root);
    if (rl > 1 && (root[rl - 1] == '/' || root[rl - 1] == '\\')) rl--;
    return !strncmp(path, root, rl) &&
           (path[rl] == 0 || path[rl] == '/' || path[rl] == '\\');
}

/* drop the ".." entry when we're at the library root */
static void trim_up(void)
{
    if (A.n_entries > 0 && A.entries[0].kind == L_UP &&
        !strcmp(A.cur_dir, cfg.music_dir)) {
        memmove(&A.entries[0], &A.entries[1], (A.n_entries - 1) * sizeof(LibEntry));
        A.n_entries--;
    }
}

static void fmt_time(char *out, int secs)
{
    if (secs < 0) secs = 0;
    if (secs >= 3600)
        snprintf(out, 16, "%d:%02d:%02d", secs / 3600, (secs % 3600) / 60, secs % 60);
    else
        snprintf(out, 16, "%d:%02d", secs / 60, secs % 60);
}

/* copy up to maxw px of s into out; returns bytes consumed.
 * prefer_spaces: break at the last space within the limit instead of mid-word
 * (hard-breaks only when a single word is wider than the line). */
static size_t wrap_line(const char *s, int maxw, char *out, int outsz, int prefer_spaces)
{
    const unsigned char *p = (const unsigned char *)s;
    int w = 0;
    size_t o = 0;
    size_t space_out = 0;
    size_t space_consumed = 0;
    if (maxw < 24) maxw = 24;
    while (*p && o + 8 < (size_t)outsz) {
        const unsigned char *start = p;
        unsigned char c = *p;
        int cp, n, i, bad = 0;
        if (c < 0x80) { cp = c; n = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
        else { p++; continue; }
        for (i = 1; i <= n; i++) {
            if (!p[i] || (p[i] & 0xC0) != 0x80) { bad = 1; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (bad) { p++; continue; }
        {
            int cw = font_char_w(&A.font, cp);
            if (w + cw > maxw) {
                if (prefer_spaces && space_consumed > 0) {
                    out[space_out] = 0;          /* drop the trailing space */
                    return space_consumed;
                }
                break;
            }
            memcpy(out + o, start, n + 1);
            o += n + 1;
            w += cw;
            if (cp == ' ') {
                space_out = o;
                space_consumed = (size_t)(p - (const unsigned char *)s) + n + 1;
            }
        }
        p += n + 1;
    }
    out[o] = 0;
    return (size_t)(p - (const unsigned char *)s);
}

/* copy s into out, truncated to maxw px with a trailing … when needed.
 * returns 1 if truncated */
static int fit_text(const char *s, int maxw, char *out, int outsz)
{
    size_t used = wrap_line(s, maxw - 16, out, outsz, 0);
    if (s[used]) {
        strncat(out, "\xe2\x80\xa6", outsz - strlen(out) - 1);
        return 1;
    }
    return 0;
}

static void draw_text(int x, int y, const char *s, SDL_Color c, int scale);   /* defined below */

static void draw_tooltip(void)
{
    int mx, my, nlines = 0, maxw, boxw = 0, i;
    SDL_Rect box, clip;
    const char *rest;
    char lines[3][600];
    if (!A.tip || A.modal) return;
    SDL_GetMouseState(&mx, &my);
    maxw = A.w - 16;
    /* wrap the full text into up to 3 lines at window width */
    rest = A.tip_buf;
    while (nlines < 3 && *rest) {
        size_t used = wrap_line(rest, maxw, lines[nlines], sizeof lines[0], 1);
        int lw = font_w(&A.font, lines[nlines], 1);
        if (lw > boxw) boxw = lw;
        nlines++;
        rest += used;
    }
    if (nlines == 0) return;
    box.w = boxw + 12;
    box.h = nlines * 16 + 8;
    {
        int tx = mx + 14, ty = my + 20;
        if (tx + box.w > A.w) tx = mx - box.w - 4;
        if (ty + box.h > A.h) ty = my - box.h - 4;
        if (tx < 2) tx = 2;
        if (ty < 2) ty = 2;
        box.x = tx;
        box.y = ty;
    }
    SDL_SetRenderDrawColor(A.ren, C_BG0.r, C_BG0.g, C_BG0.b, 255);
    SDL_RenderFillRect(A.ren, &box);
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_RenderDrawRect(A.ren, &box);
    clip = box;
    clip.x += 4;
    clip.w -= 8;
    SDL_RenderSetClipRect(A.ren, &clip);
    for (i = 0; i < nlines; i++)
        draw_text(box.x + 6, box.y + 4 + i * 16, lines[i], C_TXT, 1);
    SDL_RenderSetClipRect(A.ren, NULL);
}

static void fmt_size(char *out, long long b)
{
    if (b >= 1024 * 1024) snprintf(out, 24, "%.1fM", b / (1024.0 * 1024.0));
    else if (b >= 1024) snprintf(out, 24, "%.0fK", b / 1024.0);
    else snprintf(out, 24, "%lld", b);
}

static void base_name(const char *path, char *out, int outsz)
{
    const char *s = strrchr(path, '/');
    snprintf(out, outsz, "%s", s ? s + 1 : path);
}

static void strip_ext(char *name)
{
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
}

/* ---------------- base64 (for embedded art) ---------------- */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *b64_decode(const char *s, int *outlen)
{
    int len = (int)strlen(s);
    int cap = len / 4 * 3 + 3;
    unsigned char *out = (unsigned char *)malloc(cap);
    int o = 0, acc = 0, bits = 0;
    for (int i = 0; i < len; i++) {
        int v = b64_val(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    *outlen = o;
    return out;
}

static SDL_Texture *load_tex_file(const char *path)
{
    SDL_Surface *s = IMG_Load(path);
    SDL_Texture *t;
    if (!s) return NULL;
    t = SDL_CreateTextureFromSurface(A.ren, s);
    SDL_FreeSurface(s);
    return t;
}

static SDL_Texture *load_tex_b64(const char *b64)
{
    int len;
    unsigned char *data = b64_decode(b64, &len);
    SDL_RWops *rw;
    SDL_Surface *s;
    SDL_Texture *t;
    if (!data || len < 8) { free(data); return NULL; }
    rw = SDL_RWFromMem(data, len);
    s = IMG_Load_RW(rw, 1);
    if (!s) { free(data); return NULL; }
    t = SDL_CreateTextureFromSurface(A.ren, s);
    SDL_FreeSurface(s);
    free(data);
    return t;
}

/* ---------------- font path ---------------- */

static const char *find_font(void)
{
    static const char *cands[] = {
        NULL, /* MARIMO_FONT / MIKAPLAY_FONT */
        "./assets/unifont_all.hex",
        "../assets/unifont_all.hex",
        "assets/unifont_all.hex",
        NULL, /* ~/.local/share/marimo */
        NULL, /* ~/.local/share/mikaplay (old name) */
        "/home/nova/mika/mikaplay/assets/unifont_all.hex",
        NULL
    };
    static char localpath[1024];
    static char oldpath[1024];
    const char *env = getenv("MARIMO_FONT");
    if (!(env && env[0])) env = getenv("MIKAPLAY_FONT");
    if (env && fs_access(env, R_OK) == 0) return env;
    snprintf(localpath, sizeof localpath, "%s/.local/share/marimo/unifont_all.hex",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    cands[4] = localpath;
    snprintf(oldpath, sizeof oldpath, "%s/.local/share/mikaplay/unifont_all.hex",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    cands[5] = oldpath;
    for (int i = 1; cands[i]; i++)
        if (fs_access(cands[i], R_OK) == 0) return cands[i];
    return NULL;
}

/* ---------------- layout ---------------- */

static void layout(void)
{
    Layout *L = &A.L;
    int w = A.w, h = A.h;
    int y0 = HEADER_H;
    memset(L, 0, sizeof(*L));

    L->header.x = 0; L->header.y = 0; L->header.w = w; L->header.h = HEADER_H;

    /* landscape bar: big art on the left, text + controls beside it */
    L->bar.x = 0; L->bar.y = y0; L->bar.w = BAR_W; L->bar.h = 8 + ART_SZ + 8;

    L->art.x = PAD; L->art.y = y0 + 8; L->art.w = ART_SZ; L->art.h = ART_SZ;
    {
        int rx = PAD + ART_SZ + 12;
        int rw = BAR_W - rx - 8;
        L->t1.x = rx; L->t1.y = y0 + 10; L->t1.w = rw; L->t1.h = 16;
        L->t2.x = rx; L->t2.y = y0 + 28; L->t2.w = rw; L->t2.h = 16;
        L->t3.x = rx; L->t3.y = y0 + 46; L->t3.w = rw; L->t3.h = 16;

        {
            int cy = y0 + 64;
            L->b_prev = (SDL_Rect){ rx, cy, 24, CTRL_H };
            L->b_play = (SDL_Rect){ rx + 28, cy, 24, CTRL_H };
            L->b_stop = (SDL_Rect){ rx + 56, cy, 24, CTRL_H };
            L->b_next = (SDL_Rect){ rx + 84, cy, 24, CTRL_H };
            L->time.x = rx + rw - 104; L->time.y = cy + 3; L->time.w = 104; L->time.h = 16;
            /* seek gets its own row, spanning the whole right column */
            L->seek.x = rx; L->seek.y = cy + 26; L->seek.w = rw; L->seek.h = 6;
        }

        {
            int sy = y0 + 100;
            L->b_shuf = (SDL_Rect){ rx, sy, 18, 16 };
            L->b_rep = (SDL_Rect){ rx + 20, sy, 18, 16 };
            L->vol_icon = (SDL_Rect){ rx + 44, sy, 16, 16 };
            L->vol.x = rx + 62; L->vol.y = sy + 5; L->vol.w = 58; L->vol.h = 6;
            L->vol_txt.x = rx + 126; L->vol_txt.y = sy; L->vol_txt.w = 34; L->vol_txt.h = 16;
        }
    }

    L->b_minimize = (SDL_Rect){ w - 100, 2, 24, HEADER_H - 4 };
    L->b_mini = (SDL_Rect){ w - 76, 2, 24, HEADER_H - 4 };
    L->b_gear = (SDL_Rect){ w - 50, 2, 24, HEADER_H - 4 };
    L->b_close = (SDL_Rect){ w - 24, 2, 24, HEADER_H - 4 };

    if (A.mini) return;

    {
        int ty = y0 + 8 + ART_SZ + 8;
        L->tabs_lib = (SDL_Rect){ PAD, ty, 84, TAB_H };
        L->tabs_q = (SDL_Rect){ PAD + 90, ty, 84, TAB_H };
        L->breadcrumb.x = PAD; L->breadcrumb.y = ty + TAB_H;
        L->breadcrumb.w = w - 2 * PAD; L->breadcrumb.h = 20;
        L->list.x = 0; L->list.y = ty + TAB_H + (A.tab == 0 ? 20 : 0);
        L->list.w = w - (A.tab == 0 ? ALPHA_W : 0);
        L->list.h = h - L->list.y - STATUS_H;
        L->alpha.x = w - ALPHA_W; L->alpha.y = L->list.y;
        L->alpha.w = A.tab == 0 ? ALPHA_W : 0; L->alpha.h = L->list.h;
        L->status.x = PAD; L->status.y = h - STATUS_H + 2;
        L->status.w = w - 2 * PAD; L->status.h = 16;
    }
}

/* ---------------- alphabet jump ---------------- */

/* strip order follows the sorted list order: '#' (digits/symbols) at top,
 * then A-Z, then あ = the non-ascii (japanese) section at the bottom.
 * slot: 0 = '#', 1..26 = A-Z, 27 = kana. */
#define ALPHA_N 28
static const char ALPHA_CHARS[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ\xe3\x81\x82"; /* #A-Zあ */

/* first entry at or after the bucket, case-insensitive; -1 = none */
static int alpha_target(const LibEntry *e, int n, int slot)
{
    int want;   /* -2 = non-alpha bucket, -3 = non-ascii bucket, else 0..25 */
    if (slot == 0) want = -2;
    else if (slot <= 26) want = slot - 1;
    else want = -3;
    for (int i = 0; i < n; i++) {
        const char *s;
        unsigned char c;
        int b;
        if (e[i].kind == L_UP) continue;
        s = e[i].name;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;
        c = (unsigned char)*s;
        b = (c >= 'a' && c <= 'z') ? c - 'a' : (c >= 'A' && c <= 'Z') ? c - 'A' : -1;
        if (want == -2) { if (b < 0) return i; }
        else if (want == -3) { if (c >= 0x80) return i; }
        else if (b >= want) return i;
    }
    return -1;
}

static void alpha_jump(int slot)
{
    int t = alpha_target(A.entries, A.n_entries, slot);
    int vis;
    if (t < 0) return;
    vis = A.L.list.h / ROW_H;
    A.sel = t;
    A.scroll = t;
    if (vis > 0 && A.scroll > A.n_entries - vis) A.scroll = A.n_entries - vis;
    if (A.scroll < 0) A.scroll = 0;
}

static void alpha_pick(int y)
{
    int nslot, slot;
    if (A.tab != 0 || A.mini || A.L.alpha.w <= 0) return;
    if (y < A.L.alpha.y || y >= A.L.alpha.y + A.L.alpha.h) return;
    nslot = A.L.alpha.h / ALPHA_N;
    if (nslot <= 0) nslot = 1;
    slot = (y - A.L.alpha.y) / nslot;
    if (slot < 0) slot = 0;
    if (slot > ALPHA_N - 1) slot = ALPHA_N - 1;
    alpha_jump(slot);
}

static void draw_alpha(void)
{
    int mx, my, hover_i = -1;
    int nslot;
    if (A.tab != 0 || A.mini || A.L.alpha.w <= 0) return;
    SDL_GetMouseState(&mx, &my);
    /* divider between rows and the strip */
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_RenderDrawLine(A.ren, A.L.alpha.x, A.L.alpha.y,
                       A.L.alpha.x, A.L.alpha.y + A.L.alpha.h - 1);
    nslot = A.L.alpha.h / ALPHA_N;
    if (nslot <= 0) return;
    if (inr(A.L.alpha, mx, my)) {
        hover_i = (my - A.L.alpha.y) / nslot;
        if (hover_i > ALPHA_N - 1) hover_i = ALPHA_N - 1;
    }
    for (int i = 0; i < ALPHA_N; i++) {
        int y = A.L.alpha.y + i * nslot;
        if (i == hover_i) {
            SDL_SetRenderDrawColor(A.ren, C_BG2.r, C_BG2.g, C_BG2.b, 255);
            SDL_Rect hl = { A.L.alpha.x + 1, y, A.L.alpha.w - 2, nslot };
            SDL_RenderFillRect(A.ren, &hl);
        }
        char c[4];
        if (i == ALPHA_N - 1)
            snprintf(c, sizeof c, "\xe3\x81\x82");   /* あ */
        else
            snprintf(c, sizeof c, "%c", ALPHA_CHARS[i]);
        draw_text(A.L.alpha.x + 1, y, c, i == hover_i ? C_TXT : C_DIM, 1);
    }
}

/* ---------------- drawing ---------------- */

static void draw_text(int x, int y, const char *s, SDL_Color c, int scale)
{
    font_draw(&A.font, x, y, s, scale, c.r, c.g, c.b);
}

static void marquee(SDL_Rect r, const char *s, SDL_Color c)
{
    int tw = font_w(&A.font, s, 1);
    SDL_RenderSetClipRect(A.ren, &r);
    if (tw <= r.w) {
        font_draw(&A.font, r.x, r.y, s, 1, c.r, c.g, c.b);
    } else {
        int off = (int)((SDL_GetTicks64() / 30) % (tw + r.w));
        int x = r.x + r.w - off;
        font_draw(&A.font, x, r.y, s, 1, c.r, c.g, c.b);
        font_draw(&A.font, x + tw + r.w, r.y, s, 1, c.r, c.g, c.b);
    }
    SDL_RenderSetClipRect(A.ren, NULL);
}

static void draw_slider(SDL_Rect r, double v, int enabled)
{
    SDL_SetRenderDrawColor(A.ren, C_BG2.r, C_BG2.g, C_BG2.b, 255);
    SDL_RenderFillRect(A.ren, &r);
    if (enabled && v > 0) {
        SDL_Rect fill = r;
        fill.w = (int)(r.w * v);
        if (fill.w < 1) fill.w = 1;
        SDL_SetRenderDrawColor(A.ren, C_ACC.r, C_ACC.g, C_ACC.b, 255);
        SDL_RenderFillRect(A.ren, &fill);
    }
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_Rect border = r;
    SDL_RenderDrawRect(A.ren, &border);
}

static void draw_header(void)
{
    SDL_Rect r = A.L.header;
    SDL_SetRenderDrawColor(A.ren, C_BG1.r, C_BG1.g, C_BG1.b, 255);
    SDL_RenderFillRect(A.ren, &r);
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_RenderDrawLine(A.ren, 0, r.y + r.h - 1, A.w, r.y + r.h - 1);

    char title[64];
    snprintf(title, sizeof title, "marimo %s", APP_VER);
    draw_text(8, r.y + 5, title, C_DIM, 1);

    /* window buttons */
    SDL_Rect bs[4] = { A.L.b_minimize, A.L.b_mini, A.L.b_gear, A.L.b_close };
    const char *glyphs[4] = { "\xe2\x80\x94",                              /* — */
                              A.mini ? "\xe2\x96\xb2" : "\xe2\x96\xbe",  /* ▲ ▼ */
                              "\xe2\x9a\x99",                              /* ⚙ */
                              "\xe2\x9c\x95" };                           /* ✕ */
    for (int i = 0; i < 4; i++) {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        if (inr(bs[i], mx, my)) {
            SDL_SetRenderDrawColor(A.ren, C_BG2.r, C_BG2.g, C_BG2.b, 255);
            SDL_RenderFillRect(A.ren, &bs[i]);
        }
        draw_text(bs[i].x + 4, bs[i].y + 2, glyphs[i], i == 3 ? C_ERR : C_TXT, 1);
    }
}

static void draw_art(void)
{
    SDL_Rect r = A.L.art;
    SDL_SetRenderDrawColor(A.ren, 0, 0, 0, 255);
    SDL_RenderFillRect(A.ren, &r);
    if (A.art) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   /* smooth art */
        SDL_RenderCopy(A.ren, A.art, NULL, &r);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    } else {
        draw_text(r.x + 8, r.y + 8, "\xe2\x99\xaa", C_DIM, 5);  /* ♪ */
    }
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_RenderDrawRect(A.ren, &r);
}

static void draw_playerbar(void)
{
    QItem *it = NULL;
    char title[512] = "", artist[512] = "", album[512] = "";
    SDL_Color titlec = C_TXT;
    int playing = player_state(A.pl) == 1;
    double t = player_time(A.pl), len = player_length(A.pl);
    if (t >= 0) A.last_time = t;

    /* the whole window is the grey surface; a full-width line separates
     * the player strip (480px compact block on the left) from the panes */
    SDL_SetRenderDrawColor(A.ren, C_BG1.r, C_BG1.g, C_BG1.b, 255);
    SDL_RenderDrawLine(A.ren, 0, A.L.bar.y + A.L.bar.h - 1,
                       A.w, A.L.bar.y + A.L.bar.h - 1);

    q_lock(&A.q);
    if (A.q.cur >= 0 && A.q.cur < A.q.n) {
        it = &A.q.items[A.q.cur];
        snprintf(title, sizeof title, "%s", it->meta.title[0] ? it->meta.title : it->name);
        snprintf(artist, sizeof artist, "%s", it->meta.artist[0] ? it->meta.artist : "");
        snprintf(album, sizeof album, "%s", it->meta.album[0] ? it->meta.album : "");
    } else if (A.have_last) {
        /* queue finished / stopped: keep the last track's info, dimmed */
        snprintf(title, sizeof title, "%s", A.last_meta.title[0] ? A.last_meta.title : A.last_name);
        snprintf(artist, sizeof artist, "%s", A.last_meta.artist[0] ? A.last_meta.artist : "");
        snprintf(album, sizeof album, "%s", A.last_meta.album[0] ? A.last_meta.album : "");
        titlec = C_DIM;
    }
    q_unlock(&A.q);

    draw_art();

    marquee(A.L.t1, title[0] ? title : "marimo", titlec);
    marquee(A.L.t2, artist, C_DIM);
    marquee(A.L.t3, album, C_DIM);

    /* transport buttons */
    SDL_Rect b[4] = { A.L.b_prev, A.L.b_play, A.L.b_stop, A.L.b_next };
    int icons[4] = { ICON_PREV, playing ? ICON_PAUSE : ICON_PLAY, ICON_STOP, ICON_NEXT };
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    for (int i = 0; i < 4; i++) {
        SDL_Color c = C_TXT;
        if (inr(b[i], mx, my)) {
            SDL_SetRenderDrawColor(A.ren, C_BG2.r, C_BG2.g, C_BG2.b, 255);
            SDL_RenderFillRect(A.ren, &b[i]);
            c = C_ACC;
        }
        font_draw_icon(&A.font, b[i].x + 4, b[i].y + 3, icons[i], 1, c.r, c.g, c.b);
    }

    /* seek */
    double v = (len > 0 && t >= 0) ? t / len : 0;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    draw_slider(A.L.seek, v, len > 0);
    {
        int tt = t > 0 ? (int)t : 0;
        int tl = len > 0 ? (int)len : 0;
        char tb[24], lb[32];
        if (A.show_rem && len > 0) {
            fmt_time(tb, tl - tt);
            snprintf(lb, sizeof lb, "-%s", tb);
        } else {
            fmt_time(lb, tt);
        }
        fmt_time(tb, tl);
        char line[64];
        snprintf(line, sizeof line, "%s / %s", lb, tb);
        {
            int lw = font_w(&A.font, line, 1);
            draw_text(A.L.time.x + A.L.time.w - lw, A.L.time.y, line, len > 0 ? C_TXT : C_DIM, 1);
        }
    }

    /* sub row: shuffle / repeat / volume */
    SDL_Color sc = A.q.shuffle ? C_ACC : C_DIM;
    font_draw_icon(&A.font, A.L.b_shuf.x, A.L.b_shuf.y, ICON_SHUFFLE, 1, sc.r, sc.g, sc.b);
    SDL_Color rc = A.q.repeat ? C_ACC : C_DIM;
    font_draw_icon(&A.font, A.L.b_rep.x, A.L.b_rep.y, ICON_REPEAT, 1, rc.r, rc.g, rc.b);
    if (A.q.repeat == 2)
        draw_text(A.L.b_rep.x + 12, A.L.b_rep.y + 8, "1", C_ACC, 1);
    SDL_Color vc = (A.vol > 0 && !A.muted) ? C_TXT : C_DIM;
    font_draw_icon(&A.font, A.L.vol_icon.x, A.L.vol_icon.y, ICON_VOLUME, 1, vc.r, vc.g, vc.b);
    draw_slider(A.L.vol, A.vol / 100.0, 1);
    {
        char vt[8];
        if (A.muted) snprintf(vt, sizeof vt, "MUTE");
        else snprintf(vt, sizeof vt, "%d%%", A.vol);
        draw_text(A.L.vol_txt.x, A.L.vol_txt.y, vt, C_DIM, 1);
    }
}

static void draw_tabs(void)
{
    SDL_Rect t[2] = { A.L.tabs_lib, A.L.tabs_q };
    const char *names[2] = { "Library", "Queue" };
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    for (int i = 0; i < 2; i++) {
        if (inr(t[i], mx, my)) {
            SDL_SetRenderDrawColor(A.ren, C_BG2.r, C_BG2.g, C_BG2.b, 255);
            SDL_RenderFillRect(A.ren, &t[i]);
        }
        SDL_Color c = A.tab == i ? C_ACC : C_TXT;
        int w = font_w(&A.font, names[i], 1);
        draw_text(t[i].x + (t[i].w - w) / 2, t[i].y + 4, names[i], c, 1);
        if (A.tab == i) {
            SDL_SetRenderDrawColor(A.ren, C_ACC.r, C_ACC.g, C_ACC.b, 255);
            SDL_RenderDrawLine(A.ren, t[i].x + 4, t[i].y + t[i].h - 2,
                               t[i].x + t[i].w - 4, t[i].y + t[i].h - 2);
        }
    }
}

static void draw_breadcrumb(void)
{
    SDL_Rect r = A.L.breadcrumb;
    SDL_SetRenderDrawColor(A.ren, C_BG1.r, C_BG1.g, C_BG1.b, 255);
    SDL_RenderFillRect(A.ren, &r);
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_RenderDrawLine(A.ren, 0, r.y + r.h - 1, A.w, r.y + r.h - 1);

    char *dup = strdup(A.cur_dir);
    char *parts[64];
    int n = 0;
    char *tok = strtok(dup, "/");
    while (tok && n < 63) {
        parts[n++] = tok;
        tok = strtok(NULL, "/");
    }
    int x = r.x;
    SDL_Rect clip = r;
    SDL_RenderSetClipRect(A.ren, &clip);
    for (int i = 0; i < n; i++) {
        draw_text(x, r.y + 2, "/", C_BD, 1);
        x += 8;
        int w = font_w(&A.font, parts[i], 1);
        draw_text(x, r.y + 2, parts[i], C_DIM, 1);
        x += w;
        if (x > r.x + r.w) break;
    }
    free(dup);
    SDL_RenderSetClipRect(A.ren, NULL);
}

static void draw_list_rows(int tab)
{
    int n = tab == 0 ? A.n_entries : A.q.n;
    int rows = A.L.list.h / ROW_H;
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    if (tab == 1) q_lock(&A.q);
    for (int i = 0; i < rows; i++) {
        int idx = (tab == 0 ? A.scroll : A.q_scroll) + i;
        if (idx >= n) break;
        int y = A.L.list.y + i * ROW_H;
        SDL_Rect row = { A.L.list.x, y, A.L.list.w, ROW_H };
        int sel = tab == 0 ? A.sel : A.q_sel;
        int hover = inr(row, mx, my);
        if (idx == sel) {
            SDL_SetRenderDrawColor(A.ren, C_SELBG.r, C_SELBG.g, C_SELBG.b, 255);
            SDL_RenderFillRect(A.ren, &row);
        } else if (hover) {
            SDL_SetRenderDrawColor(A.ren, C_BG2.r, C_BG2.g, C_BG2.b, 255);
            SDL_RenderFillRect(A.ren, &row);
        }
        SDL_Rect clip = row;
        clip.x += 2; clip.w -= 4;
        SDL_RenderSetClipRect(A.ren, &clip);

        if (tab == 0) {
            LibEntry *e = &A.entries[idx];
            SDL_Color icc = e->kind == L_DIR ? C_AMBER : C_ACC2;
            font_draw_icon(&A.font, row.x + 4, y + 2, e->kind == L_DIR ? ICON_FOLDER : ICON_NOTE,
                           1, icc.r, icc.g, icc.b);
            {
                char disp[600];
                int szw = 0;
                if (e->kind == L_FILE) {
                    char sz[24];
                    fmt_size(sz, e->size);
                    szw = font_w(&A.font, sz, 1) + 10;
                }
                int avail = row.w - 26 - 12 - szw;
                int trunc = fit_text(e->name, avail, disp, sizeof disp);
                draw_text(row.x + 24, y + 2, disp,
                          idx == sel ? C_ACC : (e->kind == L_DIR ? C_AMBER : C_TXT), 1);
                if (hover && trunc) {
                    snprintf(A.tip_buf, sizeof A.tip_buf, "%s", e->name);
                    A.tip = 1;
                }
                if (e->kind == L_FILE) {
                    char sz[24];
                    fmt_size(sz, e->size);
                    int w = font_w(&A.font, sz, 1);
                    draw_text(row.x + row.w - w - 10, y + 2, sz, C_DIM, 1);
                }
            }
        } else {
            QItem *it = &A.q.items[idx];
            int nx = row.x + 20;
            if (idx == A.q.cur) draw_text(row.x + 4, y + 2, "\xe2\x96\xb6", C_ACC, 1);
            if (it->meta.track > 0) {
                char num[24];
                if (it->meta.disc > 0)
                    snprintf(num, sizeof num, "%d.%d", it->meta.disc, it->meta.track);
                else
                    snprintf(num, sizeof num, "%d", it->meta.track);
                draw_text(nx, y + 2, num, C_DIM, 1);
                nx += font_w(&A.font, num, 1) + 8;
            }
            {
                char disp[1100], full[1100];
                int durw = 0;
                if (it->meta.duration_ms > 0) {
                    char tt[16];
                    fmt_time(tt, it->meta.duration_ms / 1000);
                    durw = font_w(&A.font, tt, 1) + 10;
                }
                if (it->meta.have_meta && it->meta.artist[0] && it->meta.title[0])
                    snprintf(full, sizeof full, "%s — %s", it->meta.artist, it->meta.title);
                else
                    snprintf(full, sizeof full, "%s", it->name);
                int avail = row.x + row.w - 10 - durw - nx;
                int trunc = fit_text(full, avail, disp, sizeof disp);
                draw_text(nx, y + 2, disp, idx == A.q.cur ? C_ACC : C_TXT, 1);
                if (hover && trunc) {
                    snprintf(A.tip_buf, sizeof A.tip_buf, "%s", full);
                    A.tip = 1;
                }
            }
            if (it->meta.duration_ms > 0) {
                char tt[16];
                fmt_time(tt, it->meta.duration_ms / 1000);
                int w = font_w(&A.font, tt, 1);
                draw_text(row.x + row.w - w - 10, y + 2, tt, C_DIM, 1);
            }
        }
        SDL_RenderSetClipRect(A.ren, NULL);
    }
    if (tab == 1) q_unlock(&A.q);
}

static void draw_status(void)
{
    SDL_Rect r = A.L.status;
    char msg[256];
    snprintf(msg, sizeof msg, "%s", A.status);
    if (A.status_at && SDL_GetTicks64() - A.status_at > 6000)
        msg[0] = 0;
    draw_text(r.x, r.y, msg, C_DIM, 1);

    /* scrobble indicators, right side */
    int x = r.x + r.w;
    const char *lf = cfg.lf_session[0] ? "lf \xe2\x9c\x93" : "lf \xe2\x80\x93";
    const char *lb = cfg.lb_token[0] ? "lb \xe2\x9c\x93" : "lb \xe2\x80\x93";
    SDL_Color lfc = cfg.lf_session[0] ? C_ACC : C_DIM;
    SDL_Color lbc = cfg.lb_token[0] ? C_ACC : C_DIM;
    {
        int w = font_w(&A.font, lb, 1);
        x -= w + 12;
        draw_text(x, r.y, lb, lbc, 1);
    }
    {
        int w = font_w(&A.font, lf, 1);
        x -= w + 12;
        draw_text(x, r.y, lf, lfc, 1);
    }
    int st = scrobble_auth_state();
    if (st == 1 || st == 2 || st == -1) {
        const char *m = scrobble_auth_msg();
        int w = font_w(&A.font, m, 1);
        x -= w + 12;
        draw_text(x, r.y, m, st == -1 ? C_ERR : C_DIM, 1);
    }
}

static void draw_modal(void)
{
    SDL_Rect ov = { 0, 0, A.w, A.h };
    SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(A.ren, 0, 0, 0, 180);
    SDL_RenderFillRect(A.ren, &ov);
    SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_NONE);

    int pw = A.w - 24 < 640 ? A.w - 24 : 640;
    SDL_Rect p = { (A.w - pw) / 2, 60, pw, 300 };
    SDL_SetRenderDrawColor(A.ren, C_BG1.r, C_BG1.g, C_BG1.b, 255);
    SDL_RenderFillRect(A.ren, &p);
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_RenderDrawRect(A.ren, &p);

    draw_text(p.x + 10, p.y + 8, "settings", C_ACC, 1);

    const char *labels[4] = { "library folder (root)", "last.fm api key", "last.fm api secret", "listenbrainz token" };
    for (int i = 0; i < 4; i++) {
        int fy = p.y + 32 + i * 42;
        draw_text(p.x + 10, fy, labels[i], C_DIM, 1);
        SDL_Rect f = { p.x + 10, fy + 16, pw - 20, 22 };
        SDL_SetRenderDrawColor(A.ren, C_BG0.r, C_BG0.g, C_BG0.b, 255);
        SDL_RenderFillRect(A.ren, &f);
        SDL_SetRenderDrawColor(A.ren, A.m_focus == i ? C_ACC.r : C_BD.r,
                               A.m_focus == i ? C_ACC.g : C_BD.g,
                               A.m_focus == i ? C_ACC.b : C_BD.b, 255);
        SDL_RenderDrawRect(A.ren, &f);
        SDL_Rect clip = f;
        clip.x += 3; clip.w -= 6;
        SDL_RenderSetClipRect(A.ren, &clip);
        draw_text(f.x + 3, f.y + 3, A.m_fields[i], A.m_fields[i][0] ? C_TXT : C_DIM, 1);
        if (A.m_focus == i && (SDL_GetTicks64() / 500) % 2 == 0) {
            int cw = font_w(&A.font, A.m_fields[i], 1);
            SDL_SetRenderDrawColor(A.ren, C_ACC.r, C_ACC.g, C_ACC.b, 255);
            SDL_Rect car = { f.x + 3 + cw, f.y + 4, 1, 14 };
            SDL_RenderFillRect(A.ren, &car);
        }
        SDL_RenderSetClipRect(A.ren, NULL);
    }

    /* buttons */
    SDL_Rect ba = { p.x + 10, p.y + 32 + 4 * 42 + 6, 150, 22 };
    SDL_Rect bs = { p.x + 170, p.y + 32 + 4 * 42 + 6, 120, 22 };
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    for (int i = 0; i < 2; i++) {
        SDL_Rect *b = i == 0 ? &ba : &bs;
        SDL_SetRenderDrawColor(A.ren, C_BG2.r, C_BG2.g, C_BG2.b, 255);
        SDL_RenderFillRect(A.ren, b);
        SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
        SDL_RenderDrawRect(A.ren, b);
        if (inr(*b, mx, my)) {
            SDL_SetRenderDrawColor(A.ren, C_ACC.r, C_ACC.g, C_ACC.b, 255);
            SDL_RenderDrawRect(A.ren, b);
        }
        const char *t = i == 0 ? "authorize last.fm" : "save";
        draw_text(b->x + 6, b->y + 3, t, C_TXT, 1);
    }
    {
        int st = scrobble_auth_state();
        const char *m = scrobble_auth_msg();
        draw_text(p.x + 10, ba.y + 28, m, st == -1 ? C_ERR : (st == 2 ? C_ACC : C_DIM), 1);
    }
    draw_text(p.x + 10, p.y + p.h - 16,
              "keys: last.fm/api/account/create   token: listenbrainz.org/profile   [esc] close",
              C_DIM, 1);
}

static void render(void)
{
    A.tip = 0;
    SDL_SetRenderDrawColor(A.ren, C_BG1.r, C_BG1.g, C_BG1.b, 255);
    SDL_RenderClear(A.ren);
    draw_header();
    draw_playerbar();
    if (!A.mini) {
        draw_tabs();
        if (A.tab == 0) draw_breadcrumb();
        draw_list_rows(A.tab);
        draw_alpha();
        draw_status();
    }
    if (A.modal) draw_modal();
    draw_tooltip();
    SDL_SetRenderDrawColor(A.ren, C_BD.r, C_BD.g, C_BD.b, 255);
    SDL_Rect frame = { 0, 0, A.w, A.h };
    SDL_RenderDrawRect(A.ren, &frame);
    SDL_RenderPresent(A.ren);
}

/* ---------------- playback ---------------- */

static void spawn_meta(void);   /* defined below in the meta thread section */

static void threshold_check(void)
{
    QItem *it;
    Meta m;
    int cur;
    q_lock(&A.q);
    if (A.q.cur < 0 || A.q.cur >= A.q.n) { q_unlock(&A.q); return; }
    it = &A.q.items[A.q.cur];
    if (it->scrobbled) { q_unlock(&A.q); return; }
    m = it->meta;
    cur = A.q.cur;
    q_unlock(&A.q);
    if (!m.have_meta || !m.artist[0] || !m.title[0]) return;
    double len = player_length(A.pl);
    double t = player_time(A.pl);
    if (t < 0) t = A.last_time;
    if (len > 30 && (t >= 240 || t * 2 >= len)) {
        scrobble_submit(&m, (int)t);
        q_lock(&A.q);
        if (A.q.cur == cur && A.q.items[cur].scrobbled == 0)
            A.q.items[cur].scrobbled = 1;
        q_unlock(&A.q);
    }
}

/* One diary line per finished listen — the raw material for the recap.
 *
 * Called at the *transitions* only (track change, end of track, stop, quit).
 * It is deliberately NOT called from the per-frame threshold_check(): the flag
 * would then be set on the first frame and every track would be logged at zero
 * seconds. The flag makes it idempotent, exactly like `scrobbled`, so a
 * transition that fires more than once still writes one line. */
static void diary_flush(void)
{
    QItem *it;
    Meta m;
    char nm[Q_NAME_MAX];
    int cur;
    double len, t;
    long long dur_ms;

    q_lock(&A.q);
    if (A.q.cur < 0 || A.q.cur >= A.q.n) { q_unlock(&A.q); return; }
    it = &A.q.items[A.q.cur];
    if (it->diary_logged) { q_unlock(&A.q); return; }
    m = it->meta;
    snprintf(nm, sizeof nm, "%s", it->name);   /* copied: the array can move */
    cur = A.q.cur;
    q_unlock(&A.q);

    /* At a gapless transition mpv has already moved on to the preloaded next
     * entry, so the *live* position/length at this moment belong to the wrong
     * track (that's what made the first run log sec=0 dur=0). Use the position
     * the app was last showing — A.last_time, refreshed every frame in render()
     * — and take the duration from the tags, which is what the phone does too. */
    len = player_length(A.pl);
    t = player_time(A.pl);
    if (t < 0) t = 0;
    if (A.last_time > t) t = A.last_time;
    dur_ms = m.duration_ms > 0 ? (long long)m.duration_ms
                               : (len > 0 ? (long long)(len * 1000.0) : 0);
    history_log(m.artist, m.album, m.title[0] ? m.title : nm,
                (long long)(t * 1000.0), dur_ms);

    q_lock(&A.q);
    if (A.q.cur == cur) A.q.items[cur].diary_logged = 1;
    q_unlock(&A.q);
}

static void load_art(void)
{
    char dir[L_PATH_MAX], cov[1024], path[Q_PATH_MAX];
    QItem *it;
    if (!A.ren) return;
    if (A.art) { SDL_DestroyTexture(A.art); A.art = NULL; }
    A.art_retry = 0;
    q_lock(&A.q);
    if (A.q.cur < 0 || A.q.cur >= A.q.n) { q_unlock(&A.q); return; }
    it = &A.q.items[A.q.cur];
    snprintf(path, sizeof path, "%s", it->path);
    q_unlock(&A.q);

    snprintf(dir, sizeof dir, "%s", path);
    {
        char *s = strrchr(dir, '/');
        if (s) *s = 0;
    }
    if (lib_find_cover(dir, cov, sizeof cov) == 0) {
        A.art = load_tex_file(cov);
        if (A.art) return;
    }
    char *b64 = player_embedded_art(A.pl);
    if (b64) {
        A.art = load_tex_b64(b64);
        free(b64);
        if (A.art) return;
    }
    A.art_retry = 40;   /* embedded art may lag metadata */
}

static void play_index(int i)
{
    char path[Q_PATH_MAX], name[Q_NAME_MAX];
    QItem *it;
    Meta meta;
    diary_flush();                    /* the outgoing listen, before the switch */
    q_lock(&A.q);
    if (i < 0 || i >= A.q.n) { q_unlock(&A.q); return; }
    A.q.cur = i;
    it = &A.q.items[i];
    snprintf(path, sizeof path, "%s", it->path);
    snprintf(name, sizeof name, "%s", it->name);
    q_unlock(&A.q);

    player_set_repeat_one(A.pl, A.q.repeat == 2);
    /* preload the gapless next entry before starting */
    {
        char next_path[Q_PATH_MAX] = "";
        int nxt;
        q_lock(&A.q);
        nxt = q_next(&A.q);
        if (nxt >= 0 && nxt < A.q.n)
            snprintf(next_path, sizeof next_path, "%s", A.q.items[nxt].path);
        q_unlock(&A.q);
        if (player_play_with_next(A.pl, path, next_path[0] ? next_path : NULL) < 0) {
            int n;
            set_status("could not play %s — skipping", name);
            threshold_check();
            n = q_next(&A.q);
            if (n >= 0) {
                play_index(n);
            } else {
                q_lock(&A.q);
                A.q.cur = -1;
                q_unlock(&A.q);
                set_status("queue finished");
            }
            return;
        }
    }
    /* fast tag parser first (FLAC/MP3 — instant, deterministic); mpv fallback */
    if (tag_read_meta(path, &meta) != 0 && !player_meta(A.pl, &meta)) {
        /* fall back to filename */
        memset(&meta, 0, sizeof meta);
        snprintf(meta.title, sizeof meta.title, "%s", name);
        strip_ext(meta.title);
        meta.have_meta = 1;
    }
    q_lock(&A.q);
    if (A.q.cur == i) {
        A.q.items[i].meta = meta;
        A.q.items[i].scrobbled = 0;
        A.q.items[i].np_sent = 0;
        A.q.items[i].diary_logged = 0;   /* a replay is a listen too */
    }
    q_unlock(&A.q);
    A.last_meta = meta;
    snprintf(A.last_name, sizeof A.last_name, "%s", meta.title[0] ? meta.title : name);
    A.have_last = 1;

    player_set_volume(A.pl, A.vol);
    load_art();
    A.last_time = 0;
    scrobble_now_playing(&meta);
    set_status("\xe2\x96\xb6 %s", meta.title[0] ? meta.title : name);
}

static void on_end(int err)
{
    int next;
    diary_flush();                    /* the track that just finished */
    threshold_check();
    if (err == 2) {
        /* the entry failed — skip it explicitly */
        next = q_next(&A.q);
        if (next >= 0) {
            play_index(next);
        } else {
            q_lock(&A.q);
            A.q.cur = -1;
            q_unlock(&A.q);
            set_status("playback error, queue finished");
        }
        return;
    }
    next = q_next(&A.q);
    if (next >= 0) {
        char nextnext_path[Q_PATH_MAX] = "";
        Meta meta;
        char name[Q_NAME_MAX] = "";
        q_lock(&A.q);
        A.q.cur = next;
        {
            int n2 = q_next(&A.q);
            if (n2 >= 0 && n2 < A.q.n)
                snprintf(nextnext_path, sizeof nextnext_path, "%s", A.q.items[n2].path);
        }
        {
            QItem *it = &A.q.items[next];
            snprintf(name, sizeof name, "%s", it->meta.title[0] ? it->meta.title : it->name);
            it->scrobbled = 0;
            it->np_sent = 0;
            it->diary_logged = 0;
            if (tag_read_meta(it->path, &meta) != 0)
                memset(&meta, 0, sizeof meta);
        }
        q_unlock(&A.q);
        if (!meta.have_meta)
            player_meta(A.pl, &meta);
        q_lock(&A.q);
        if (A.q.cur == next && meta.have_meta)
            A.q.items[next].meta = meta;
        q_unlock(&A.q);
        A.last_meta = meta;
        snprintf(A.last_name, sizeof A.last_name, "%s",
                 meta.title[0] ? meta.title : name);
        A.last_time = 0;
        /* mpv has already switched to the preloaded next entry — just drop
         * the finished one and warm the new following entry */
        player_gapless_shift(A.pl, nextnext_path[0] ? nextnext_path : NULL);
        load_art();
        scrobble_now_playing(&meta);
        set_status("\xe2\x96\xb6 %s", meta.title[0] ? meta.title : name);
    } else {
        q_lock(&A.q);
        A.q.cur = -1;
        q_unlock(&A.q);
        set_status(err == 2 ? "playback error, queue finished" : "queue finished");
    }
}

static void queue_play_file(const char *path, const char *name, long long size, int replace)
{
    int idx;
    if (replace) {
        /* middle-click: replace the queue and play now */
        q_lock(&A.q);
        q_clear(&A.q);
        idx = q_add(&A.q, path, name, size);
        q_unlock(&A.q);
        play_index(idx);
    } else {
        /* double-click: add to the end of the queue, don't interrupt */
        int was_new = 0;
        q_lock(&A.q);
        idx = q_find(&A.q, path);
        if (idx < 0) {
            idx = q_add(&A.q, path, name, size);
            was_new = 1;
        }
        q_unlock(&A.q);
        if (A.q.cur < 0) {
            play_index(idx);
        } else if (was_new) {
            set_status("added to queue: %s", name);
        } else {
            set_status("already in queue: %s", name);
        }
    }
    spawn_meta();
}

typedef struct { int pos, has, track, disc; } TagSort;

static int tagsort_cmp(const void *a, const void *b)
{
    const TagSort *x = (const TagSort *)a, *y = (const TagSort *)b;
    if (x->has != y->has) return y->has - x->has;
    if (x->has) {
        int xd = x->disc > 0 ? x->disc : 1, yd = y->disc > 0 ? y->disc : 1;
        if (xd != yd) return xd - yd;
        int xt = x->track > 0 ? x->track : 10000, yt = y->track > 0 ? y->track : 10000;
        if (xt != yt) return xt - yt;
    }
    return x->pos - y->pos;
}

static void play_dir(const char *path, int replace)
{
    LibEntry *e = NULL;
    int n = lib_scan(path, &e);
    int nfiles = 0, tagged = 0, added = 0, first = -1;
    TagSort *ts = NULL;
    for (int i = 0; i < n; i++) if (e[i].kind == L_FILE) nfiles++;
    if (nfiles == 0) {
        lib_free_entries(e);
        set_status("no audio files in that folder");
        return;
    }
    ts = (TagSort *)calloc(nfiles, sizeof(TagSort));
    {
        int ti = 0;
        for (int i = 0; i < n; i++) {
            if (e[i].kind == L_FILE) {
                ts[ti].pos = i;
                int tr = -1, dc = -1;
                if (tag_trackinfo(e[i].path, &tr, &dc) == 0 && (tr > 0 || dc > 0)) {
                    ts[ti].has = 1;
                    ts[ti].track = tr;
                    ts[ti].disc = dc;
                    tagged++;
                }
                ti++;
            }
        }
    }
    /* sort by disc/track tags when we have them, else keep filename order */
    if (tagged >= 2) qsort(ts, nfiles, sizeof(TagSort), tagsort_cmp);
    q_lock(&A.q);
    if (replace) q_clear(&A.q);
    for (int k = 0; k < nfiles; k++) {
        LibEntry *fe = &e[ts[k].pos];
        int idx = q_add(&A.q, fe->path, fe->name, fe->size);
        if (first < 0) first = idx;
        added++;
    }
    q_unlock(&A.q);
    free(ts);
    lib_free_entries(e);
    if (replace || A.q.cur < 0)
        play_index(first);
    else
        set_status("added %d tracks to queue", added);
    spawn_meta();
}

/* ---------------- meta thread ---------------- */

static void *meta_thread(void *x)
{
    TagReader *tr = (TagReader *)x;
    for (;;) {
        char path[Q_PATH_MAX];
        int idx = -1;
        if (A.meta_stop) break;
        q_lock(&A.q);
        for (int i = 0; i < A.q.n; i++) {
            if (!A.q.items[i].meta.have_meta) {
                idx = i;
                snprintf(path, sizeof path, "%s", A.q.items[i].path);
                break;
            }
        }
        q_unlock(&A.q);
        if (idx < 0) break;
        if (A.meta_stop) break;
        Meta m;
        if (tag_read_meta(path, &m) == 0 && m.have_meta) {
            q_lock(&A.q);
            if (idx < A.q.n && !A.q.items[idx].meta.have_meta &&
                !strcmp(A.q.items[idx].path, path))
                A.q.items[idx].meta = m;
            q_unlock(&A.q);
        } else if (tagreader_read(tr, path, &m)) {
            q_lock(&A.q);
            if (idx < A.q.n && !A.q.items[idx].meta.have_meta &&
                !strcmp(A.q.items[idx].path, path))
                A.q.items[idx].meta = m;
            q_unlock(&A.q);
        } else {
            q_lock(&A.q);
            if (idx < A.q.n && !A.q.items[idx].meta.have_meta &&
                !strcmp(A.q.items[idx].path, path))
                A.q.items[idx].meta.have_meta = 1;   /* parsed, nothing there */
            q_unlock(&A.q);
        }
    }
    A.meta_running = 0;
    return NULL;
}

static void spawn_meta(void)
{
    int need = 0;
    if (A.meta_running) return;
    q_lock(&A.q);
    for (int i = 0; i < A.q.n; i++)
        if (!A.q.items[i].meta.have_meta) { need = 1; break; }
    q_unlock(&A.q);
    if (!need) return;
    if (!A.tr) A.tr = tagreader_create();
    if (!A.tr) return;
    A.meta_running = 1;
    pthread_create(&A.meta_thr, NULL, meta_thread, A.tr);
}

/* ---------------- events ---------------- */

static void enter_dir(const char *path);   /* defined below */

static void lib_activate(int row, int btn)
{
    LibEntry *e;
    if (row < 0 || row >= A.n_entries) return;
    e = &A.entries[row];
    if (e->kind == L_UP) {
        enter_dir(e->path);
        return;
    }
    if (e->kind == L_DIR) {
        if (btn == SDL_BUTTON_RIGHT) {
            enter_dir(e->path);           /* peek inside the folder */
            return;
        }
        play_dir(e->path, btn == SDL_BUTTON_MIDDLE);   /* queue the album */
        return;
    }
    if (btn == SDL_BUTTON_RIGHT) return;  /* no action on files */
    queue_play_file(e->path, e->name, e->size, btn == SDL_BUTTON_MIDDLE);
}

static void queue_activate(int row)
{
    if (row < 0 || row >= A.q.n) return;
    play_index(row);
}

static void row_click(int list, int row, int btn)
{
    Uint32 now = SDL_GetTicks();
    if (list == 0) A.sel = row; else A.q_sel = row;
    if (btn == SDL_BUTTON_LEFT) {
        if (row == A.last_row && A.last_list == list && now - A.last_down < 400) {
            A.last_down = 0;
            if (list == 0) lib_activate(row, SDL_BUTTON_LEFT);
            else queue_activate(row);
            return;
        }
        A.last_row = row;
        A.last_list = list;
        A.last_down = now;
    } else if (btn == SDL_BUTTON_MIDDLE) {
        A.last_down = 0;
        if (list == 0) lib_activate(row, SDL_BUTTON_MIDDLE);
        else queue_activate(row);
    } else if (btn == SDL_BUTTON_RIGHT) {
        if (list == 1) {
            int was_cur;
            q_lock(&A.q);
            was_cur = (row == A.q.cur);
            q_remove(&A.q, row);
            if (A.q_sel >= A.q.n) A.q_sel = A.q.n - 1;
            q_unlock(&A.q);
            if (was_cur) {
                player_stop(A.pl);
                set_status("stopped");
            }
        } else {
            lib_activate(row, SDL_BUTTON_RIGHT);
        }
    }
}

/* browsing history: remember each visited folder's scroll position */
typedef struct { char path[L_PATH_MAX]; int scroll; } NavEntry;
static NavEntry nav[32];
static int nav_n;

static void enter_dir(const char *path)
{
    if (!path_under(path, cfg.music_dir)) {
        set_status("outside your library");
        return;
    }
    if (nav_n > 0 && !strcmp(nav[nav_n - 1].path, path)) {
        /* going back up — restore where we were */
        nav_n--;
        A.scroll = nav[nav_n].scroll;
    } else {
        /* going deeper — remember the current spot */
        if (nav_n < 32) {
            snprintf(nav[nav_n].path, sizeof nav[nav_n].path, "%s", A.cur_dir);
            nav[nav_n].scroll = A.scroll;
            nav_n++;
        }
        A.scroll = 0;
    }
    snprintf(A.cur_dir, sizeof A.cur_dir, "%s", path);
    A.sel = 0;
    lib_free_entries(A.entries);
    A.entries = NULL;
    A.n_entries = lib_scan(A.cur_dir, &A.entries);
    trim_up();
    if (A.scroll > A.n_entries) A.scroll = 0;
    set_status(A.cur_dir);
}

static void toggle_pause(void)
{
    int st = player_state(A.pl);
    if (st == 0) {
        if (A.q.n > 0) play_index(A.q.cur >= 0 ? A.q.cur : 0);
        return;
    }
    player_set_pause(A.pl, st != 2);
}

/* volume changes unmute */
static void set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    A.vol = v;
    if (A.muted) {
        A.muted = 0;
        player_set_mute(A.pl, 0);
    }
    player_set_volume(A.pl, A.vol);
}

/* ---------------- auto refresh ---------------- */

/* one stat per interval on the current library dir; rescan only when the
 * dir's mtime/size changed (new/renamed/deleted albums over CIFS).
 * selection is kept by name so the view doesn't jump around, and the scroll
 * is anchored on the entry the top row was showing — anchoring on the
 * selection instead snaps the list back up whenever you've been browsing
 * with the wheel (scrolling never moves the selection). */
static void poll_refresh(void)
{
    struct stat st;
    static struct stat last;
    static char last_dir[L_PATH_MAX];
    static int have_last;
    int had_sel, vis;

    if (A.mini || A.modal || A.tab != 0) return;
    if (strcmp(last_dir, A.cur_dir)) {          /* moved — re-baseline */
        snprintf(last_dir, sizeof last_dir, "%s", A.cur_dir);
        have_last = 0;
    }
    if (fs_stat(A.cur_dir, &st)) return;        /* NAS unreachable — keep list */
    if (have_last && st.st_mtime == last.st_mtime && st.st_size == last.st_size)
        return;
    last = st;
    have_last = 1;

    {
        char keep[512];       /* selected entry, matched by name */
        char anchor[512];     /* entry the top row was showing */
        int kind = -1, a_kind = -1, had_anchor = 0;
        int scroll = A.scroll;

        had_sel = A.sel >= 0 && A.sel < A.n_entries;
        if (had_sel) {
            snprintf(keep, sizeof keep, "%s", A.entries[A.sel].name);
            kind = A.entries[A.sel].kind;
        }
        if (scroll >= 0 && scroll < A.n_entries) {
            snprintf(anchor, sizeof anchor, "%s", A.entries[scroll].name);
            a_kind = A.entries[scroll].kind;
            had_anchor = 1;
        }
        lib_free_entries(A.entries);
        A.entries = NULL;
        A.n_entries = lib_scan(A.cur_dir, &A.entries);
        trim_up();
        A.sel = -1;
        if (had_sel)
            for (int i = 0; i < A.n_entries; i++)
                if (A.entries[i].kind == kind && !strcmp(A.entries[i].name, keep)) { A.sel = i; break; }
        if (A.sel < 0 && A.n_entries > 0) A.sel = 0;
        vis = A.L.list.h / ROW_H;
        /* hold the view still: the anchor entry's new index becomes the
         * scroll, so rows added above it shift nothing. only if it vanished
         * do we fall back to following the selection. */
        A.scroll = scroll;
        if (had_anchor) {
            for (int i = 0; i < A.n_entries; i++)
                if (A.entries[i].kind == a_kind && !strcmp(A.entries[i].name, anchor)) { A.scroll = i; break; }
        } else if (A.sel >= 0 && (A.sel < A.scroll || A.sel >= A.scroll + vis))
            A.scroll = A.sel;
        if (vis > 0 && A.scroll > A.n_entries - vis) A.scroll = A.n_entries - vis;
        if (A.scroll < 0) A.scroll = 0;
        set_status("library refreshed");
    }
}

static void next_track(void)
{
    int n;
    diary_flush();
    threshold_check();
    if (A.q.cur < 0) n = A.q.n > 0 ? 0 : -1;
    else n = q_next(&A.q);
    if (n >= 0) play_index(n);
}

static void prev_track(void)
{
    int n;
    diary_flush();
    threshold_check();
    n = q_prev(&A.q);
    if (n >= 0) play_index(n);
}

static void stop_playback(void)
{
    diary_flush();
    threshold_check();
    player_stop(A.pl);
    q_lock(&A.q);
    A.q.cur = -1;
    q_unlock(&A.q);
    set_status("stopped");
}

static void open_modal(void)
{
    A.modal = 1;
    A.m_focus = 0;
    snprintf(A.m_fields[0], sizeof A.m_fields[0], "%.*s",
             (int)sizeof A.m_fields[0] - 1, cfg.music_dir);
    snprintf(A.m_fields[1], sizeof A.m_fields[1], "%s", cfg.lf_key);
    snprintf(A.m_fields[2], sizeof A.m_fields[2], "%s", cfg.lf_secret);
    snprintf(A.m_fields[3], sizeof A.m_fields[3], "%s", cfg.lb_token);
    SDL_StartTextInput();
}

static void close_modal(int save)
{
    if (save) {
        char old_music[1024];
        snprintf(old_music, sizeof old_music, "%s", cfg.music_dir);
        snprintf(cfg.music_dir, sizeof cfg.music_dir, "%s", A.m_fields[0]);
        snprintf(cfg.lf_key, sizeof cfg.lf_key, "%s", A.m_fields[1]);
        snprintf(cfg.lf_secret, sizeof cfg.lf_secret, "%s", A.m_fields[2]);
        snprintf(cfg.lb_token, sizeof cfg.lb_token, "%s", A.m_fields[3]);
        config_save();
        if (strcmp(old_music, cfg.music_dir)) {
            /* music dir changed — jump there */
            enter_dir(cfg.music_dir);
            set_status("music folder: %s", cfg.music_dir);
        } else {
            set_status("settings saved");
        }
    }
    A.modal = 0;
    SDL_StopTextInput();
}

static void backspace_field(char *s)
{
    size_t l = strlen(s);
    if (l == 0) return;
    l--;
    while (l > 0 && ((unsigned char)s[l] & 0xC0) == 0x80) l--;
    s[l] = 0;
}

static void handle_key(SDL_Event *ev)
{
    if (A.modal) {
        if (ev->key.keysym.sym == SDLK_ESCAPE) { close_modal(0); return; }
        if (ev->key.keysym.sym == SDLK_TAB) {
            A.m_focus = (A.m_focus + 1) % 4;
            return;
        }
        if (ev->key.keysym.sym == SDLK_RETURN) {
            close_modal(1);
            return;
        }
        if (ev->key.keysym.sym == SDLK_BACKSPACE) {
            backspace_field(A.m_fields[A.m_focus]);
            return;
        }
        if (ev->key.keysym.sym == SDLK_v &&
            (ev->key.keysym.mod & KMOD_CTRL)) {
            char *clip = SDL_GetClipboardText();
            if (clip) {
                size_t l = strlen(A.m_fields[A.m_focus]);
                size_t cl = strlen(clip);
                if (l + cl < 1023) strcat(A.m_fields[A.m_focus], clip);
                SDL_free(clip);
            }
            return;
        }
        return;
    }
    switch (ev->key.keysym.sym) {
    case SDLK_SPACE:
        toggle_pause();
        break;
    case SDLK_LEFT: {
        double t = player_time(A.pl);
        if (t > 0) player_seek(A.pl, t - 5);
        break;
    }
    case SDLK_RIGHT: {
        double t = player_time(A.pl);
        if (t > 0) player_seek(A.pl, t + 5);
        break;
    }
    case SDLK_UP:
        set_volume(A.vol + 5);
        break;
    case SDLK_DOWN:
        set_volume(A.vol - 5);
        break;
    case SDLK_RETURN:
        if (A.tab == 0 && A.sel >= 0 && A.sel < A.n_entries) lib_activate(A.sel, SDL_BUTTON_LEFT);
        else if (A.tab == 1 && A.q_sel >= 0) queue_activate(A.q_sel);
        break;
    case SDLK_DELETE:
        if (A.tab == 1) {
            int was_cur;
            q_lock(&A.q);
            was_cur = (A.q_sel == A.q.cur);
            q_remove(&A.q, A.q_sel);
            if (A.q_sel >= A.q.n) A.q_sel = A.q.n - 1;
            q_unlock(&A.q);
            if (was_cur) {
                player_stop(A.pl);
                set_status("stopped");
            }
        }
        break;
    case SDLK_n:
        if (ev->key.keysym.mod & KMOD_CTRL) next_track();
        break;
    /* media keys */
    case SDLK_AUDIOPLAY:
        toggle_pause();
        break;
    case SDLK_AUDIONEXT:
        next_track();
        break;
    case SDLK_AUDIOPREV:
        prev_track();
        break;
    case SDLK_AUDIOSTOP:
        stop_playback();
        break;
    case SDLK_AUDIOMUTE:
        A.muted = !A.muted;
        player_set_mute(A.pl, A.muted);
        set_status(A.muted ? "muted" : "unmuted");
        break;
    case SDLK_VOLUMEUP:
        set_volume(A.vol + 5);
        break;
    case SDLK_VOLUMEDOWN:
        set_volume(A.vol - 5);
        break;
    default:
        break;
    }
}

static void handle_text(SDL_Event *ev)
{
    if (!A.modal) return;
    size_t l = strlen(A.m_fields[A.m_focus]);
    size_t cl = strlen(ev->text.text);
    if (l + cl < 511) strcat(A.m_fields[A.m_focus], ev->text.text);
}

static void handle_mouse(SDL_Event *ev)
{
    int x = ev->button.x, y = ev->button.y;
    if (ev->type == SDL_MOUSEBUTTONDOWN) {
        if (A.modal) {
            /* fields */
            int pw = A.w - 24 < 640 ? A.w - 24 : 640;
            SDL_Rect p = { (A.w - pw) / 2, 60, pw, 300 };
            for (int i = 0; i < 4; i++) {
                SDL_Rect f = { p.x + 10, p.y + 32 + i * 42 + 16, pw - 20, 22 };
                if (inr(f, x, y)) { A.m_focus = i; return; }
            }
            SDL_Rect ba = { p.x + 10, p.y + 32 + 4 * 42 + 6, 150, 22 };
            SDL_Rect bs = { p.x + 170, p.y + 32 + 4 * 42 + 6, 120, 22 };
            if (inr(ba, x, y)) { scrobble_auth_start(); return; }
            if (inr(bs, x, y)) { close_modal(1); return; }
            return;
        }
        if (ev->button.button == SDL_BUTTON_LEFT || ev->button.button == SDL_BUTTON_MIDDLE ||
            ev->button.button == SDL_BUTTON_RIGHT) {
            /* header buttons */
            if (inr(A.L.b_minimize, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                SDL_MinimizeWindow(A.win);
                return;
            }
            if (inr(A.L.b_mini, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                A.mini = !A.mini;
                SDL_SetWindowSize(A.win, 480, A.mini ? MINI_H : 640);
                return;
            }
            if (inr(A.L.b_gear, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                open_modal();
                return;
            }
            if (inr(A.L.b_close, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                A.running = 0;
                return;
            }
            /* transport */
            if (inr(A.L.b_prev, x, y) && ev->button.button == SDL_BUTTON_LEFT) { prev_track(); return; }
            if (inr(A.L.b_play, x, y) && ev->button.button == SDL_BUTTON_LEFT) { toggle_pause(); return; }
            if (inr(A.L.b_stop, x, y) && ev->button.button == SDL_BUTTON_LEFT) { stop_playback(); return; }
            if (inr(A.L.b_next, x, y) && ev->button.button == SDL_BUTTON_LEFT) { next_track(); return; }
            /* seek */
            if (inr(A.L.seek, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                double len = player_length(A.pl);
                if (len > 0) {
                    A.drag = 1;
                    double v = (double)(x - A.L.seek.x) / A.L.seek.w;
                    if (v < 0) v = 0;
                    if (v > 1) v = 1;
                    player_seek(A.pl, v * len);
                }
                return;
            }
            if (inr(A.L.time, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                A.show_rem = !A.show_rem;
                return;
            }
            /* volume */
            if (inr(A.L.vol, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                A.drag = 2;
                double v = (double)(x - A.L.vol.x) / A.L.vol.w;
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                set_volume((int)(v * 100));
                return;
            }
            /* shuffle / repeat */
            if (inr(A.L.b_shuf, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                A.q.shuffle = !A.q.shuffle;
                cfg.shuffle = A.q.shuffle;
                return;
            }
            if (inr(A.L.b_rep, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                A.q.repeat = (A.q.repeat + 1) % 3;
                cfg.repeat = A.q.repeat;
                player_set_repeat_one(A.pl, A.q.repeat == 2);
                return;
            }
            if (!A.mini) {
                /* tabs */
                if (inr(A.L.tabs_lib, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                    A.tab = 0;
                    layout();
                    return;
                }
                if (inr(A.L.tabs_q, x, y) && ev->button.button == SDL_BUTTON_LEFT) {
                    A.tab = 1;
                    layout();
                    return;
                }
                /* alphabet jump strip */
                if (A.tab == 0 && ev->button.button == SDL_BUTTON_LEFT && inr(A.L.alpha, x, y)) {
                    A.alpha_drag = 1;
                    alpha_pick(y);
                    return;
                }
                /* list rows */
                if (inr(A.L.list, x, y)) {
                    int row = (y - A.L.list.y) / ROW_H;
                    int scroll = A.tab == 0 ? A.scroll : A.q_scroll;
                    row += scroll;
                    int n = A.tab == 0 ? A.n_entries : A.q.n;
                    if (row < n) row_click(A.tab, row, ev->button.button);
                    return;
                }
                /* breadcrumb: click a segment to jump — handled via row click on
                   the ".." entry or by re-enter; simplest: click = enter parent */
                if (A.tab == 0 && inr(A.L.breadcrumb, x, y) &&
                    ev->button.button == SDL_BUTTON_LEFT && A.n_entries > 0 &&
                    A.entries[0].kind == L_UP) {
                    lib_activate(0, 0);
                    return;
                }
            }
        }
    } else if (ev->type == SDL_MOUSEBUTTONUP) {
        A.drag = 0;
        A.alpha_drag = 0;
    } else if (ev->type == SDL_MOUSEMOTION) {
        if (A.alpha_drag) {
            alpha_pick(ev->motion.y);
        } else if (A.drag == 1) {
            double len = player_length(A.pl);
            if (len > 0) {
                double v = (double)(ev->motion.x - A.L.seek.x) / A.L.seek.w;
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                player_seek(A.pl, v * len);
            }
        } else if (A.drag == 2) {
            double v = (double)(ev->motion.x - A.L.vol.x) / A.L.vol.w;
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            set_volume((int)(v * 100));
        }
    } else if (ev->type == SDL_MOUSEWHEEL) {
        if (A.modal || A.mini) return;
        SDL_GetMouseState(&x, &y);
        if (inr(A.L.list, x, y)) {
            if (A.tab == 0) {
                int vis = A.L.list.h / ROW_H;
                A.scroll -= ev->wheel.y * 3;
                if (A.scroll < 0) A.scroll = 0;
                if (A.scroll > A.n_entries - vis) A.scroll = A.n_entries - vis;
                if (A.scroll < 0) A.scroll = 0;
            } else {
                int vis = A.L.list.h / ROW_H;
                A.q_scroll -= ev->wheel.y * 3;
                if (A.q_scroll < 0) A.q_scroll = 0;
                if (A.q_scroll > A.q.n - vis) A.q_scroll = A.q.n - vis;
                if (A.q_scroll < 0) A.q_scroll = 0;
            }
        }
    }
}

static void handle_drop(const char *path)
{
    struct stat st;
    if (fs_stat(path, &st)) { set_status("drop failed"); return; }
    if (S_ISDIR(st.st_mode)) {
        LibEntry *e = NULL;
        int n = lib_scan(path, &e);
        int added = 0, first = -1, was_playing = A.q.cur >= 0;
        q_lock(&A.q);
        for (int i = 0; i < n; i++) {
            if (e[i].kind == L_FILE) {
                int idx = q_add(&A.q, e[i].path, e[i].name, e[i].size);
                if (first < 0) first = idx;
                added++;
            }
        }
        q_unlock(&A.q);
        lib_free_entries(e);
        if (added == 0) { set_status("no audio in dropped folder"); return; }
        set_status("added %d tracks", added);
        if (!was_playing) play_index(first);
        spawn_meta();
    } else {
        char name[512];
        base_name(path, name, sizeof name);
        q_lock(&A.q);
        int idx = q_add(&A.q, path, name, 0);
        q_unlock(&A.q);
        if (A.q.cur < 0) play_index(idx);
        else set_status("added %s", name);
        spawn_meta();
    }
}

static void handle_event(SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_QUIT:
        A.running = 0;
        break;
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_RESIZED ||
            ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            A.w = ev->window.data1;
            A.h = ev->window.data2;
            if (A.mini && (A.h != MINI_H || A.w != BAR_W)) {
                A.h = MINI_H;
                A.w = BAR_W;
                SDL_SetWindowSize(A.win, BAR_W, MINI_H);
            }
            layout();
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
        handle_mouse(ev);
        break;
    case SDL_KEYDOWN:
        handle_key(ev);
        break;
    case SDL_TEXTINPUT:
        handle_text(ev);
        break;
    case SDL_DROPFILE:
        handle_drop(ev->drop.file);
        SDL_free(ev->drop.file);
        break;
    default:
        break;
    }
}

/* ---------------- queue persistence ---------------- */

static char queue_path[1100];

static void save_queue(void)
{
    FILE *f = fopen(queue_path, "w");
    if (!f) return;
    q_lock(&A.q);
    fprintf(f, "cur=%d\n", A.q.cur);
    for (int i = 0; i < A.q.n; i++)
        fprintf(f, "%s\n", A.q.items[i].path);
    q_unlock(&A.q);
    fclose(f);
}

static void load_queue(void)
{
    FILE *f = fopen(queue_path, "r");
    char line[Q_PATH_MAX];
    int cur = -1, n = 0;
    if (!f) return;
    if (fgets(line, sizeof line, f) && !strncmp(line, "cur=", 4))
        cur = atoi(line + 4);
    q_lock(&A.q);
    q_clear(&A.q);
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l) continue;
        if (fs_access(line, R_OK)) continue;         /* file gone (NAS down?) */
        {
            const char *slash = strrchr(line, '/');
            q_add(&A.q, line, slash ? slash + 1 : line, 0);
            n++;
        }
    }
    if (cur >= 0 && cur < A.q.n) A.q.cur = cur;
    else A.q.cur = -1;
    q_unlock(&A.q);
    fclose(f);
    if (n > 0) {
        set_status("queue restored (%d tracks)", n);
        spawn_meta();
    }
}

/* ---------------- test modes ---------------- */

static int selftest(const char *cfgfile_global)
{
    int fails = 0;
    const char *fp;
    SDL_Window *win;
    SDL_Renderer *ren;

    printf("== marimo selftest ==\n");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { printf("FAIL: SDL_Init\n"); return 1; }
    win = SDL_CreateWindow("selftest", 0, 0, 64, 64, 0);
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { printf("FAIL: renderer\n"); SDL_Quit(); return 1; }

    fp = find_font();
    if (!fp) { printf("FAIL: unifont hex not found\n"); return 1; }
    if (font_load(&A.font, ren, fp) < 10000) {
        printf("FAIL: font load (%s)\n", fp);
        return 1;
    }
    {
        const char *glyphs[] = { "M", "あ", "😀", "♪", "⚙", "✕", "▶", "中" };
        int cps[] = { 'M', 0x3042, 0x1F600, 0x266A, 0x2699, 0x2715, 0x25B6, 0x4E2D };
        for (int i = 0; i < 8; i++) {
            if (!font_has(&A.font, cps[i])) {
                printf("FAIL: missing glyph %s (U+%04X)\n", glyphs[i], cps[i]);
                fails++;
            }
        }
        /* pixel check: 'A' and 'あ' must have ink in the right half of row 1 —
           catches the old bits_to_tex bug where the right half mirrored the left */
        for (int i = 0; i < 2; i++) {
            int cp = cps[i];
            for (int gi = 0; gi < A.font.ng; gi++) {
                if (A.font.glyphs[gi].cp == cp) {
                    if (A.font.glyphs[gi].w == 16 &&
                        !(A.font.glyphs[gi].bits[1] | A.font.glyphs[gi].bits[3] |
                          A.font.glyphs[gi].bits[5] | A.font.glyphs[gi].bits[7])) {
                        printf("FAIL: glyph %s has no right-half pixels (mirror bug?)\n", glyphs[i]);
                        fails++;
                    }
                    break;
                }
            }
        }
        printf("font: %d glyphs loaded, all test glyphs %s\n", A.font.ng,
               fails ? "MISSING" : "present");
    }

    /* md5 vectors */
    {
        char h[33];
        md5_hex("", 0, h);
        if (strcmp(h, "d41d8cd98f00b204e9800998ecf8427e")) { printf("FAIL: md5 empty\n"); fails++; }
        md5_hex("abc", 3, h);
        if (strcmp(h, "900150983cd24fb0d6963f7d28e17f72")) { printf("FAIL: md5 abc\n"); fails++; }
        md5_hex("The quick brown fox jumps over the lazy dog", 43, h);
        if (strcmp(h, "9e107d9d372bb6826bd81d3542a419d6")) { printf("FAIL: md5 fox\n"); fails++; }
        printf("md5: %s\n", fails ? "FAIL" : "ok");
    }

    /* config roundtrip */
    {
        const char *p = "/tmp/marimo_selftest.ini";
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "volume=42\nlf_key=abc123\nmusic_dir=/tmp/music\n");
            fclose(f);
        }
        config_load(p);
        if (cfg.volume != 42 || strcmp(cfg.lf_key, "abc123") || strcmp(cfg.music_dir, "/tmp/music")) {
            printf("FAIL: config roundtrip\n");
            fails++;
        } else {
            printf("config: ok\n");
        }
        unlink(p);
        config_load(cfgfile_global);   /* restore real settings */
    }

    /* listening diary: escaping, disk round-trip, tolerant parse */
    {
        const char *d = "/tmp/marimo_selftest_diary";
        char jp[256];
        HistoryEntry *ev;
        size_t n = 0;
        int ok = 1;

        snprintf(jp, sizeof jp, "%s/history.jsonl", d);
        remove(jp);
        history_init(d);
        /* quotes, a backslash, a real newline and a tab all have to survive */
        history_log("Say \"hi\"\\there", "Album\nTwo", "Tab\tTitle", 42000, 180000);
        history_log("artist2", "album2", "title2", 1000, 60000);

        ev = history_window(0, 4102444800000LL, &n);   /* 1970 -> 2100 */
        if (n != 2) {
            printf("FAIL: diary entries %zu (want 2)\n", n);
            fails++;
        } else {
            if (strcmp(ev[0].artist, "Say \"hi\"\\there") || strcmp(ev[0].album, "Album\nTwo") ||
                strcmp(ev[0].title, "Tab\tTitle")) {
                printf("FAIL: diary escaping [%s] [%s]\n", ev[0].artist, ev[0].title);
                ok = 0;
            }
            if (ev[0].sec != 42 || ev[0].dur != 180000 || ev[1].sec != 1 || ev[1].dur != 60000) {
                printf("FAIL: diary sec/dur %lld/%lld %lld/%lld\n",
                       ev[0].sec, ev[0].dur, ev[1].sec, ev[1].dur);
                ok = 0;
            }
            if (ev[0].ts < 1600000000000LL) { printf("FAIL: diary ts %lld\n", ev[0].ts); ok = 0; }
            if (strcmp(ev[1].artist, "artist2") || strcmp(ev[1].title, "title2")) {
                printf("FAIL: diary second entry\n");
                ok = 0;
            }
        }
        history_free(ev);

        /* a truncated line and outright junk must be skipped, not fatal */
        {
            FILE *f = fopen(jp, "ab");
            if (f) {
                fprintf(f, "{\"ts\":123,\"artist\":\"broken\n");
                fprintf(f, "not json at all\n");
                fclose(f);
            }
        }
        ev = history_window(0, 4102444800000LL, &n);
        if (n != 2) { printf("FAIL: diary tolerant parse got %zu\n", n); ok = 0; }
        history_free(ev);

        /* the window filter itself */
        ev = history_window(0, 1, &n);
        if (n != 0) { printf("FAIL: diary window filter got %zu\n", n); ok = 0; }
        history_free(ev);

        remove(jp);
        rmdir(d);
        history_shutdown();
        if (ok) printf("diary: ok\n");
        else fails++;
    }

    /* queue logic */
    {
        Queue q;
        q_init(&q, 0, 1);
        for (int i = 0; i < 5; i++) {
            char p[64], n[64];
            snprintf(p, sizeof p, "/tmp/t%d.flac", i);
            snprintf(n, sizeof n, "t%d", i);
            q_add(&q, p, n, 0);
        }
        q.cur = 0;
        if (q_next(&q) != 1) { printf("FAIL: next\n"); fails++; }
        q.cur = 4;
        if (q_next(&q) != 0) { printf("FAIL: wrap with repeat\n"); fails++; }
        q.repeat = 0;
        if (q_next(&q) != -1) { printf("FAIL: no wrap without repeat\n"); fails++; }
        q_remove(&q, 2);
        if (q.n != 4) { printf("FAIL: remove\n"); fails++; }
        if (q_find(&q, "/tmp/t3.flac") != 2) { printf("FAIL: find\n"); fails++; }
        q.shuffle = 1;
        if (q_next(&q) < 0 || q_next(&q) >= 4) { printf("FAIL: shuffle range\n"); fails++; }
        q_free(&q);
        printf("queue: %s\n", fails ? "FAIL" : "ok");
    }

    /* alphabet jump target */
    {
        static LibEntry te[7];
        memset(te, 0, sizeof te);
        te[0].kind = L_UP;   snprintf(te[0].name, sizeof te[0].name, "..");
        te[1].kind = L_FILE; snprintf(te[1].name, sizeof te[1].name, "1two");
        te[2].kind = L_DIR;  snprintf(te[2].name, sizeof te[2].name, "あいう");
        te[3].kind = L_DIR;  snprintf(te[3].name, sizeof te[3].name, "apple");
        te[4].kind = L_DIR;  snprintf(te[4].name, sizeof te[4].name, "Banana");
        te[5].kind = L_FILE; snprintf(te[5].name, sizeof te[5].name, "cherry");
        te[6].kind = L_FILE; snprintf(te[6].name, sizeof te[6].name, "中");
        if (alpha_target(te, 7, 0) != 1) { printf("FAIL: alpha #\n"); fails++; }
        if (alpha_target(te, 7, 1) != 3) { printf("FAIL: alpha A\n"); fails++; }
        if (alpha_target(te, 7, 2) != 4) { printf("FAIL: alpha B\n"); fails++; }
        if (alpha_target(te, 7, 3) != 5) { printf("FAIL: alpha C\n"); fails++; }
        if (alpha_target(te, 7, 26) != -1) { printf("FAIL: alpha Z\n"); fails++; }
        if (alpha_target(te, 7, 27) != 2) { printf("FAIL: alpha kana\n"); fails++; }
        printf("alpha: %s\n", fails ? "FAIL" : "ok");
    }

    /* library scan */
    {
        LibEntry *e = NULL;
        int n = lib_scan(cfg.music_dir, &e);
        if (n < 0) printf("scan: %s not accessible (NAS down?) — skipping\n", cfg.music_dir);
        else {
            int files = 0, dirs = 0;
            for (int i = 0; i < n; i++) {
                if (e[i].kind == L_FILE) files++;
                if (e[i].kind == L_DIR) dirs++;
            }
            printf("scan: %s → %d dirs, %d files, %d entries\n", cfg.music_dir, dirs, files, n);
            if (n > 1 && strcmp(e[0].name, "..")) { printf("FAIL: '..' not first\n"); fails++; }
            /* real-library jump targets: every slot should resolve */
            {
                printf("alpha targets:");
                for (int s = 0; s < ALPHA_N; s++) {
                    int t = alpha_target(e, n, s);
                    printf(" %s%s", s ? "|" : "", t >= 0 ? e[t].name : "-");
                }
                printf("\n");
            }
            lib_free_entries(e);
        }
    }

    /* tags + cover: exercise with a real album from the library if available */
    {
        LibEntry *top = NULL;
        int tn = lib_scan(cfg.music_dir, &top);
        int found = 0;
        if (tn > 0) {
            for (int i = 0; i < tn && !found; i++) {
                if (top[i].kind == L_DIR) {
                    LibEntry *alb = NULL;
                    int an = lib_scan(top[i].path, &alb);
                    int f1 = -1, f2 = -1, files = 0;
                    for (int j = 0; j < an; j++) if (alb[j].kind == L_FILE) {
                        if (f1 < 0) f1 = j; else if (f2 < 0) f2 = j;
                        files++;
                    }
                    if (files >= 2) {
                        int tr1 = -1, dc1 = -1, tr2 = -1, dc2 = -1;
                        Meta m;
                        tag_trackinfo(alb[f1].path, &tr1, &dc1);
                        tag_trackinfo(alb[f2].path, &tr2, &dc2);
                        if (tr1 > 0 && tr2 > 0 && tr1 > tr2) {
                            printf("FAIL: tag order (%s t=%d before %s t=%d)\n",
                                   alb[f1].name, tr1, alb[f2].name, tr2);
                            fails++;
                        }
                        if (tag_read_meta(alb[f1].path, &m) == 0) {
                            printf("tags: '%s' by '%s' [%s] %d:%02d\n",
                                   m.title[0] ? m.title : "(no title)", m.artist,
                                   m.album, m.duration_ms / 60000, (m.duration_ms / 1000) % 60);
                            if (!m.title[0] && !m.artist[0]) { printf("FAIL: no tags read\n"); fails++; }
                        } else {
                            printf("tags: no parseable tags on %s\n", alb[f1].name);
                        }
                        {
                            char cov[L_PATH_MAX];
                            if (lib_find_cover(top[i].path, cov, sizeof cov) == 0)
                                printf("cover: %s\n", cov);
                            else
                                printf("cover: none in %s\n", top[i].name);
                        }
                        found = 1;
                    }
                    lib_free_entries(alb);
                }
            }
        }
        lib_free_entries(top);
        if (!found) printf("tags/cover: no album folder available — skipping\n");
    }

    font_free(&A.font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf(fails ? "== SELFTEST FAILED (%d) ==\n" : "== SELFTEST PASSED ==\n", fails);
    return fails ? 1 : 0;
}


static int headless(const char *dir)
{
    LibEntry *e = NULL;
    int n, files = 0;
    int t0, t1;
    printf("== marimo headless play test ==\n");
    n = lib_scan(dir, &e);
    if (n < 0) { printf("FAIL: cannot scan %s\n", dir); return 1; }
    for (int i = 0; i < n; i++) if (e[i].kind == L_FILE) files++;
    if (files < 2) { printf("FAIL: need >= 2 audio files in %s\n", dir); return 1; }

    const char *aout = getenv("MIKAPLAY_AOUT");
    A.pl = player_create(aout && aout[0] ? aout : "null");
    if (!A.pl) { printf("FAIL: player create\n"); return 1; }
    A.vol = 20;   /* keep the real-audio test gentle */
    scrobble_init();
    q_init(&A.q, 0, 0);
    {
        int added = 0;
        for (int i = 0; i < n && added < 3; i++) {
            if (e[i].kind == L_FILE) {
                q_lock(&A.q);
                q_add(&A.q, e[i].path, e[i].name, e[i].size);
                q_unlock(&A.q);
                added++;
            }
        }
        if (added < 3) { printf("FAIL: need >= 3 audio files\n"); return 1; }
    }
    play_index(0);
    player_dbg_playlist(A.pl);
    t0 = (int)time(NULL);
    while (player_state(A.pl) != 1 && time(NULL) - t0 < 6) {
        player_poll(A.pl);
        SDL_Delay(20);
    }
    if (player_state(A.pl) != 1 || player_time(A.pl) <= 0) {
        printf("FAIL: not playing after 6s (state=%d)\n", player_state(A.pl));
        return 1;
    }
    printf("track 1 playing: %s @ %.1fs — OK\n", A.q.items[0].name, player_time(A.pl));

    /* skip near the end — expect auto-advance to track 2 (gapless path) */
    {
        double len = player_length(A.pl);
        if (len <= 2) { printf("FAIL: track too short\n"); return 1; }
        player_seek(A.pl, len - 1.5);
    }
    t1 = (int)time(NULL);
    while (A.q.cur != 1 && time(NULL) - t1 < 10) {
        int pe = player_poll(A.pl);
        { double pt = player_time(A.pl); if (pt >= 0) A.last_time = pt; }  /* render() does this in the real loop */
        if (pe) on_end(pe);
        SDL_Delay(20);
    }
    if (A.q.cur != 1) {
        printf("FAIL: no auto-advance (cur=%d state=%d)\n", A.q.cur, player_state(A.pl));
        return 1;
    }
    /* let track 2 actually start */
    t1 = (int)time(NULL);
    while ((player_state(A.pl) != 1 || player_time(A.pl) <= 0) && time(NULL) - t1 < 5)
        SDL_Delay(20);
    if (player_state(A.pl) != 1) {
        printf("FAIL: track 2 did not start (state=%d)\n", player_state(A.pl));
        return 1;
    }
    printf("auto-advanced to track 2: %s — OK\n", A.q.items[1].name);
    player_dbg_playlist(A.pl);
    if (player_playlist_count(A.pl) != 2) {
        printf("FAIL: playlist should be [2,3] but has %d entries\n", player_playlist_count(A.pl));
        return 1;
    }
    printf("gapless preload: playlist holds 2 entries — OK\n");

    /* finish track 2 → advance to 3 (no next to preload) */
    {
        double len = player_length(A.pl);
        if (len > 2) player_seek(A.pl, len - 1.5);
    }
    t1 = (int)time(NULL);
    while (A.q.cur != 2 && time(NULL) - t1 < 10) {
        int pe = player_poll(A.pl);
        { double pt = player_time(A.pl); if (pt >= 0) A.last_time = pt; }
        if (pe) on_end(pe);
        SDL_Delay(20);
    }
    if (A.q.cur != 2) {
        printf("FAIL: track 3 advance (cur=%d)\n", A.q.cur);
        return 1;
    }
    /* the remove command is async in mpv's queue — give it a moment */
    t1 = (int)time(NULL);
    while (player_playlist_count(A.pl) != 1 && time(NULL) - t1 < 3)
        SDL_Delay(50);
    if (player_playlist_count(A.pl) != 1) {
        printf("FAIL: playlist should have 1 entry, has %d\n", player_playlist_count(A.pl));
        player_dbg_playlist(A.pl);
        return 1;
    }
    printf("auto-advanced to track 3, playlist collapsed to 1 — OK\n");

    /* finish track 3 → queue end */
    {
        double len = player_length(A.pl);
        if (len > 2) player_seek(A.pl, len - 1.5);
    }
    t1 = (int)time(NULL);
    while (A.q.cur >= 0 && time(NULL) - t1 < 10) {
        int pe = player_poll(A.pl);
        { double pt = player_time(A.pl); if (pt >= 0) A.last_time = pt; }
        if (pe) on_end(pe);
        SDL_Delay(20);
    }
    if (A.q.cur != -1) {
        printf("FAIL: queue did not end (cur=%d)\n", A.q.cur);
        return 1;
    }
    printf("queue finished cleanly — OK\n");

    player_destroy(A.pl);
    A.pl = NULL;
    scrobble_shutdown();
    lib_free_entries(e);
    printf("== HEADLESS PASSED ==\n");
    return 0;
}

static int smoke(void)
{
    printf("== smoke test ==\n");
    {
        Uint64 t0 = SDL_GetTicks64();
        int frames = 0;
        while (frames < 120 && SDL_GetTicks64() - t0 < 4000) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) handle_event(&ev);
            render();
            SDL_Delay(16);
            frames++;
        }
        printf("rendered %d frames — OK\n", frames);
    }
    printf("== SMOKE PASSED ==\n");
    return 0;
}

static int makeicon(const char *out)
{
    /* just the little guy: a big moss ball, dead-centered, no props.
     * R=60 on the 128px canvas — chunky. exact RGBA via a direct
     * pixel buffer; no renderer or font needed. */
    const int S = 128;
    SDL_Surface *surf;
    Uint32 *px;
    int x, y, rc;

    surf = SDL_CreateRGBSurfaceWithFormat(0, S, S, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return 1;
    px = (Uint32 *)surf->pixels;
    for (y = 0; y < S; y++)
        for (x = 0; x < S; x++)
            px[y * S + x] = 0;          /* fully transparent */

    /* the ball — centered at (64,64), R=60 */
    {
        const double cx = 64, cy = 64, R = 60;
        for (y = 0; y < S; y++) {
            for (x = 0; x < S; x++) {
                double dx = x - cx, dy = y - cy;
                double d = sqrt(dx * dx + dy * dy);
                double t;
                int r, g, b;
                if (d > R) continue;
                t = (dy + R) / (2 * R);
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                r = (int)(126 + (46 - 126) * t);
                g = (int)(199 + (102 - 199) * t);
                b = (int)(110 + (52 - 110) * t);
                /* mottled texture: a few darker patches */
                {
                    double ax, ay;
                    ax = (x - (cx - 26)) / 17.0; ay = (y - (cy + 24)) / 12.0;
                    if (ax * ax + ay * ay < 1) { r = (int)(r * 0.78); g = (int)(g * 0.78); b = (int)(b * 0.78); }
                    ax = (x - (cx + 30)) / 19.0; ay = (y - (cy + 34)) / 14.0;
                    if (ax * ax + ay * ay < 1) { r = (int)(r * 0.70); g = (int)(g * 0.70); b = (int)(b * 0.70); }
                    ax = (x - (cx - 11)) / 21.0; ay = (y - (cy - 16)) / 11.0;
                    if (ax * ax + ay * ay < 1) { r = (int)(r * 0.88); g = (int)(g * 0.88); b = (int)(b * 0.88); }
                }
                /* top-left sheen */
                {
                    double hx = (x - (cx - 22)) / 13.0, hy = (y - (cy - 28)) / 9.0;
                    if (hx * hx + hy * hy < 1) {
                        r += (int)((255 - r) * 0.45); g += (int)((255 - g) * 0.45); b += (int)((255 - b) * 0.45);
                    }
                }
                px[y * S + x] = 0xFF000000 | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
            }
        }
        /* face: eyes */
        for (int e = 0; e < 2; e++) {
            int ecx = e ? 80 : 48, ecy = 57;
            for (y = ecy - 2; y <= ecy + 2; y++)
                for (x = ecx - 2; x <= ecx + 2; x++)
                    if ((x - ecx) * (x - ecx) + (y - ecy) * (y - ecy) <= 4) {
                        px[y * S + x] = 0xFF000000 | (8u << 16) | (11u << 8) | 8u;
                    }
        }
        /* blush */
        for (int e = 0; e < 2; e++) {
            int ecx = e ? 98 : 30, ecy = 69, ER = 6;
            for (y = ecy - ER; y <= ecy + ER; y++)
                for (x = ecx - ER; x <= ecx + ER; x++) {
                    double dx = x - ecx, dy = y - ecy;
                    if (dx * dx + dy * dy <= ER * ER) {
                        px[y * S + x] = 0xFF000000 | (224u << 16) | (142u << 8) | 141u;
                    }
                }
        }
        /* smile removed — nova prefers the blank face */
    }

    rc = IMG_SavePNG(surf, out);
    SDL_FreeSurface(surf);
    return rc == 0 ? 0 : 1;
}





static int screenshot(const char *out)
{
    int rc;
    SDL_Surface *surf;
    /* let the window map on wayland before grabbing pixels */
    for (int i = 0; i < 24; i++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) { /* drain */ }
        render();
        SDL_Delay(16);
    }
    surf = SDL_CreateRGBSurfaceWithFormat(0, A.w, A.h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(A.ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    rc = IMG_SavePNG(surf, out);
    SDL_FreeSurface(surf);
    printf("screenshot saved to %s\n", out);
    return rc == 0 ? 0 : 1;
}

/* ---------------- main ---------------- */

/* one-time migration helper: if dst is missing but src exists, copy it.
 * used for the old-name (mikaplay) -> marimo config/queue move so the
 * last.fm session, listenbrainz token and queue survive the rename. */
static void copy_file_if_missing(const char *dst, const char *src)
{
    FILE *a, *b;
    char buf[4096];
    size_t n;
    if (!fs_access(dst, R_OK) || fs_access(src, R_OK)) return;
    a = fs_fopen(src, "rb");
    b = fs_fopen(dst, "wb");
    if (a && b)
        while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SDL_SetMainReady();
#endif
    const char *music = NULL;
    const char *aout = getenv("MIKAPLAY_AOUT");
    const char *shot_path = NULL;
    int do_selftest = 0, do_headless = 0, do_smoke = 0;
    const char *headless_dir = NULL;
    const char *fontpath;
    char cfgfile[1024];
    const char *home = getenv("HOME") ? getenv("HOME")
                       : getenv("USERPROFILE") ? getenv("USERPROFILE")
                       : "/tmp";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) do_selftest = 1;
        else if (!strcmp(argv[i], "--headless")) {
            do_headless = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') headless_dir = argv[++i];
        }
        else if (!strcmp(argv[i], "--smoke")) do_smoke = 1;
        else if (!strcmp(argv[i], "--music") && i + 1 < argc) music = argv[++i];
        else if (!strcmp(argv[i], "--aout") && i + 1 < argc) aout = argv[++i];
        else if (!strcmp(argv[i], "--makeicon") && i + 1 < argc) {
            if (SDL_Init(SDL_INIT_VIDEO) < 0) return 1;
            if (IMG_Init(IMG_INIT_PNG) == 0) return 1;
            int rc = makeicon(argv[++i]);
            SDL_Quit();
            return rc;
        }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
        else { fprintf(stderr, "usage: %s [--selftest|--headless=DIR|--smoke] [--music DIR] [--aout NAME]\n", argv[0]); return 1; }
    }

    snprintf(cfgfile, sizeof cfgfile, "%s/.config/marimo/config.ini", home);
    {
        char d[1024], oldcfg[1100];
        snprintf(d, sizeof d, "%s/.config/marimo", home);
#ifdef _WIN32
        mkdir(d);
#else
        mkdir(d, 0755);
#endif
        /* old-name fallback: first launch after the rename copies the
         * mikaplay config (auth tokens included) so nothing re-auths */
        snprintf(oldcfg, sizeof oldcfg, "%s/.config/mikaplay/config.ini", home);
        copy_file_if_missing(cfgfile, oldcfg);
    }
    config_load(cfgfile);
    if (music) snprintf(cfg.music_dir, sizeof cfg.music_dir, "%s", music);

    /* the listening diary lives with the config, and has to be set up before the
     * --selftest/--headless dispatches below so those modes log too.
     * MARIMO_DIARY_DIR overrides it, so test runs don't write into the real
     * diary and pollute the recap. */
    {
        char diarydir[1100];
        const char *dd = getenv("MARIMO_DIARY_DIR");
        if (dd && dd[0]) snprintf(diarydir, sizeof diarydir, "%s", dd);
        else snprintf(diarydir, sizeof diarydir, "%s/.config/marimo", home);
        history_init(diarydir);
    }
    srand((unsigned)(time(NULL) ^ ((unsigned)getpid() << 16)));

    if (do_selftest) return selftest(cfgfile);

    if (do_headless) {
        if (SDL_Init(SDL_INIT_TIMER) < 0) { fprintf(stderr, "SDL_Init failed\n"); return 1; }
        const char *dir = headless_dir ? headless_dir : cfg.music_dir;
        int rc = headless(dir);
        SDL_Quit();
        return rc;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    int imgflags = IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP;
    if ((IMG_Init(imgflags) & imgflags) != imgflags)
        fprintf(stderr, "marimo: some image formats unavailable: %s\n", IMG_GetError());

    A.win = SDL_CreateWindow("marimo " APP_VER, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             480, 640, SDL_WINDOW_BORDERLESS);
    if (!A.win) { fprintf(stderr, "window failed: %s\n", SDL_GetError()); return 1; }
    /* drag by the marimo bar (borderless, fixed size) */
    SDL_SetWindowHitTest(A.win, hit_test, NULL);
    A.ren = SDL_CreateRenderer(A.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!A.ren) {
        A.ren = SDL_CreateRenderer(A.win, -1, 0);
        if (!A.ren) { fprintf(stderr, "renderer failed\n"); return 1; }
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    /* window icon: the badge-style png, or the pixel note as fallback */
    {
        SDL_Surface *ic = IMG_Load("assets/marimo.png");
        if (!ic) {
            ic = SDL_CreateRGBSurfaceWithFormat(0, 32, 32, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_LockSurface(ic);
            Uint32 *px = (Uint32 *)ic->pixels;
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 32; x++)
                    if (icon_art[ICON_NOTE][y / 2][x / 2] == '#')
                        px[y * 32 + x] = 0xFF7DFF7D;
            SDL_UnlockSurface(ic);
        }
        SDL_SetWindowIcon(A.win, ic);
        SDL_FreeSurface(ic);
    }

    fontpath = find_font();
    if (!fontpath) {
        fprintf(stderr, "marimo: unifont_all.hex not found (put it in assets/ or set MIKAPLAY_FONT)\n");
        return 1;
    }
    if (font_load(&A.font, A.ren, fontpath) < 10000) {
        fprintf(stderr, "marimo: font load failed\n");
        return 1;
    }

    A.pl = player_create(aout);
    if (!A.pl) {
        fprintf(stderr, "marimo: mpv init failed — is mpv installed?\n");
        return 1;
    }
    scrobble_init();
    mpris_init();
    q_init(&A.q, cfg.shuffle, cfg.repeat);
    A.vol = cfg.volume;
    player_set_volume(A.pl, A.vol);
    player_set_repeat_one(A.pl, cfg.repeat == 2);

    snprintf(A.cur_dir, sizeof A.cur_dir, "%s",
             cfg.last_dir[0] && path_under(cfg.last_dir, cfg.music_dir) ? cfg.last_dir : cfg.music_dir);
    A.n_entries = lib_scan(A.cur_dir, &A.entries);
    trim_up();
    snprintf(queue_path, sizeof queue_path, "%s/.config/marimo/queue.dat", home);
    {
        char oldq[1100];
        snprintf(oldq, sizeof oldq, "%s/.config/mikaplay/queue.dat", home);
        copy_file_if_missing(queue_path, oldq);
    }
    load_queue();
    if (A.n_entries < 0) {
        A.n_entries = 0;
        set_status("cannot read %s — is the NAS mounted?", A.cur_dir);
    } else {
        set_status(status_idle);
    }

    SDL_GetWindowSize(A.win, &A.w, &A.h);
    layout();
    A.running = 1;

    if (do_smoke) return smoke();
    if (shot_path) return screenshot(shot_path);

    while (A.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) handle_event(&ev);

        /* playback housekeeping */
        {
            int pe = player_poll(A.pl);
            if (pe) on_end(pe);
        }
        threshold_check();
        /* mpris: publish state, then drain commands from the dbus thread */
        {
            MprisState ms;
            int pst = player_state(A.pl);
            ms.status = pst;
            ms.position = A.last_time > 0 ? A.last_time : 0;
            ms.duration_us = (int64_t)(player_length(A.pl) * 1e6);
            ms.volume = A.vol;
            ms.can_next = A.q.n > 0;
            ms.can_prev = A.q.n > 0;
            ms.title[0] = ms.artist[0] = ms.album[0] = 0;
            snprintf(ms.trackid, sizeof ms.trackid, "/marimo/track/none");
            q_lock(&A.q);
            if (A.q.cur >= 0 && A.q.cur < A.q.n) {
                QItem *it = &A.q.items[A.q.cur];
                snprintf(ms.title, sizeof ms.title, "%s",
                         it->meta.title[0] ? it->meta.title : it->name);
                snprintf(ms.artist, sizeof ms.artist, "%s", it->meta.artist);
                snprintf(ms.album, sizeof ms.album, "%s", it->meta.album);
                snprintf(ms.trackid, sizeof ms.trackid, "/marimo/track/%d", A.q.cur);
                if (it->meta.duration_ms > 0)
                    ms.duration_us = (int64_t)it->meta.duration_ms * 1000;
            }
            q_unlock(&A.q);
            mpris_publish(&ms);
        }
        for (;;) {
            MprisCmd c = mpris_take_command();
            if (!c.cmd) break;
            switch (c.cmd) {
            case MPRIS_PLAYPAUSE: toggle_pause(); break;
            case MPRIS_PLAY:
                if (player_state(A.pl) == 2) player_set_pause(A.pl, 0);
                else if (player_state(A.pl) == 0 && A.q.n > 0)
                    play_index(A.q.cur >= 0 ? A.q.cur : 0);
                break;
            case MPRIS_PAUSE:
                if (player_state(A.pl) == 1) player_set_pause(A.pl, 1);
                break;
            case MPRIS_STOP: stop_playback(); break;
            case MPRIS_NEXT: next_track(); break;
            case MPRIS_PREV: prev_track(); break;
            case MPRIS_SEEK: {
                double t = player_time(A.pl);
                if (t >= 0) player_seek(A.pl, t + c.arg / 1e6);
                break;
            }
            case MPRIS_SETPOS:
                player_seek(A.pl, c.arg / 1e6);
                break;
            case MPRIS_VOLUME:
                set_volume((int)(c.arg / 100));
                break;
            case MPRIS_VOLUMEDELTA:
                set_volume(A.vol + (int)(c.arg / 100));
                break;
            case MPRIS_MUTE:
                A.muted = !A.muted;
                player_set_mute(A.pl, A.muted);
                set_status(A.muted ? "muted" : "unmuted");
                break;
            default:
                break;
            }
        }
        /* embedded art retry (metadata can lag) */
        if (A.art_retry > 0 && !A.art) {
            A.art_retry--;
            if ((A.art_retry % 5) == 0) {
                char *b64 = player_embedded_art(A.pl);
                if (b64) {
                    A.art = load_tex_b64(b64);
                    free(b64);
                    if (A.art) A.art_retry = 0;
                }
            }
        }
        /* metadata refresh: mpv said tags changed, or the current item is
         * stuck on the filename fallback — try the fast parser + mpv */
        {
            static int frame = 0;
            char path[Q_PATH_MAX] = "";
            int need = 0;
            if (A.q.cur >= 0 && A.q.cur < A.q.n) {
                q_lock(&A.q);
                need = !(A.q.items[A.q.cur].meta.artist[0] || A.q.items[A.q.cur].meta.title[0]);
                if (need) snprintf(path, sizeof path, "%s", A.q.items[A.q.cur].path);
                q_unlock(&A.q);
            }
            if (path[0] && (player_meta_dirty(A.pl) || (++frame % 15 == 0 && need))) {
                Meta m;
                if (tag_read_meta(path, &m) == 0 || player_meta(A.pl, &m)) {
                    q_lock(&A.q);
                    if (A.q.cur < A.q.n) A.q.items[A.q.cur].meta = m;
                    q_unlock(&A.q);
                }
            }
        }

        /* library auto-refresh: one stat per interval, rescan on change */
        {
            static Uint64 last_refresh_at;
            if (cfg.refresh > 0 && !A.mini &&
                SDL_GetTicks64() - last_refresh_at >= (Uint64)cfg.refresh * 1000) {
                last_refresh_at = SDL_GetTicks64();
                poll_refresh();
            }
        }

        render();
        SDL_Delay(8);
    }

    /* quit */
    diary_flush();
    threshold_check();
    cfg.volume = A.vol;
    cfg.shuffle = A.q.shuffle;
    cfg.repeat = A.q.repeat;
    snprintf(cfg.last_dir, sizeof cfg.last_dir, "%.*s", (int)sizeof cfg.last_dir - 1, A.cur_dir);
    config_save();
    save_queue();
    if (A.meta_running) {
        A.meta_stop = 1;
        pthread_join(A.meta_thr, NULL);
        A.meta_running = 0;
    }
    if (A.tr) tagreader_destroy(A.tr);
    player_destroy(A.pl);
    scrobble_shutdown();
    mpris_shutdown();
    q_free(&A.q);
    lib_free_entries(A.entries);
    font_free(&A.font);
    SDL_DestroyRenderer(A.ren);
    SDL_DestroyWindow(A.win);
    SDL_Quit();
    IMG_Quit();
    return 0;
}
