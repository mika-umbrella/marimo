/* mpris.c — MPRIS2 service over GDBus.
 * Exposes org.mpris.MediaPlayer2.mikaplay on the session bus:
 *   org.freedesktop.DBus.Properties  (Get/GetAll/Set)
 *   org.mpris.MediaPlayer2           (Identity etc.)
 *   org.mpris.MediaPlayer2.Player    (PlayPause/Play/Pause/Stop/Next/Previous/
 *                                     Seek/SetPosition + properties)
 * g_bus_own_name runs its own thread; that thread never touches mpv. */
#include "mpris.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* Shared command queue: fed by the dbus thread (unix) or the global
 * hotkey thread (windows), drained by the main loop every frame. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { int cmd; int64_t arg; } Cmd;
static Cmd cmds[16];
static int n_cmds;

static void enqueue(int cmd, int64_t arg)
{
    pthread_mutex_lock(&lock);
    if (n_cmds < 16) {
        cmds[n_cmds].cmd = cmd;
        cmds[n_cmds].arg = arg;
        n_cmds++;
    }
    pthread_mutex_unlock(&lock);
}

MprisCmd mpris_take_command(void)
{
    pthread_mutex_lock(&lock);
    MprisCmd c = { 0, 0 };
    if (n_cmds > 0) {
        c.cmd = cmds[0].cmd;
        c.arg = cmds[0].arg;
        memmove(cmds, cmds + 1, (n_cmds - 1) * sizeof(Cmd));
        n_cmds--;
    }
    pthread_mutex_unlock(&lock);
    return c;
}

#ifdef _WIN32
/* windows: no session dbus, no MPRIS. instead, global media hotkeys
 * (RegisterHotKey on a hidden message-only window) feed the same queue. */
#include <windows.h>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#ifndef VK_MEDIA_PLAY_PAUSE
#define VK_MEDIA_PLAY_PAUSE 0xB3
#define VK_MEDIA_PAUSE 0xB4
#define VK_MEDIA_STOP 0xB2
#define VK_MEDIA_NEXT_TRACK 0xB0
#define VK_MEDIA_PREV_TRACK 0xB1
#define VK_VOLUME_MUTE 0xAD
#define VK_VOLUME_DOWN 0xAE
#define VK_VOLUME_UP 0xAF
#endif

#define HK_PLAYPAUSE 1
#define HK_NEXT      2
#define HK_PREV      3
#define HK_STOP      4
#define HK_VOLUP     5
#define HK_VOLDOWN   6
#define HK_MUTE      7

static const struct { int id; UINT vk; int cmd; int arg; } hotkeys[] = {
    { HK_PLAYPAUSE, VK_MEDIA_PLAY_PAUSE, MPRIS_PLAYPAUSE, 0 },
    { HK_NEXT,      VK_MEDIA_NEXT_TRACK, MPRIS_NEXT,      0 },
    { HK_PREV,      VK_MEDIA_PREV_TRACK, MPRIS_PREV,      0 },
    { HK_STOP,      VK_MEDIA_STOP,       MPRIS_STOP,      0 },
    { HK_VOLUP,     VK_VOLUME_UP,        MPRIS_VOLUMEDELTA, +200 },
    { HK_VOLDOWN,   VK_VOLUME_DOWN,      MPRIS_VOLUMEDELTA, -200 },
    { HK_MUTE,      VK_VOLUME_MUTE,      MPRIS_MUTE,       0 },
};
#define N_HOTKEYS (int)(sizeof hotkeys / sizeof hotkeys[0])

static const wchar_t HK_CLASS[] = L"mikaplay_hotkeys";
static volatile LONG hk_running;
static DWORD hk_tid;

static LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_HOTKEY) {
        int id = (int)wp;
        for (int i = 0; i < N_HOTKEYS; i++) {
            if (hotkeys[i].id == id) {
                enqueue(hotkeys[i].cmd, hotkeys[i].arg);
                break;
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI hk_thread(LPVOID ud)
{
    WNDCLASSW wc;
    HWND wnd;
    MSG msg;
    (void)ud;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = hk_wndproc;
    wc.lpszClassName = HK_CLASS;
    wc.hInstance = GetModuleHandleW(NULL);
    if (!RegisterClassW(&wc)) { InterlockedExchange(&hk_running, 0); return 1; }
    wnd = CreateWindowExW(0, HK_CLASS, L"mikaplay_hotkeys", 0,
                          0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!wnd) { InterlockedExchange(&hk_running, 0); return 1; }
    /* registration failures (key taken by another app) are skipped: the
     * remaining hotkeys still work */
    for (int i = 0; i < N_HOTKEYS; i++)
        RegisterHotKey(wnd, hotkeys[i].id, MOD_NOREPEAT, hotkeys[i].vk);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (int i = 0; i < N_HOTKEYS; i++)
        UnregisterHotKey(wnd, hotkeys[i].id);
    DestroyWindow(wnd);
    UnregisterClassW(HK_CLASS, wc.hInstance);
    InterlockedExchange(&hk_running, 0);
    return 0;
}

static void hk_start(void)
{
    HANDLE h;
    if (InterlockedCompareExchange(&hk_running, 1, 0) != 0) return;
    h = CreateThread(NULL, 0, hk_thread, NULL, 0, &hk_tid);
    if (!h) InterlockedExchange(&hk_running, 0);
    else CloseHandle(h);
}

static void hk_stop(void)
{
    if (!hk_running) return;
    PostThreadMessageW(hk_tid, WM_QUIT, 0, 0);
    for (int i = 0; i < 200 && hk_running; i++) Sleep(10);
}

void mpris_init(void)     { hk_start(); }
void mpris_shutdown(void) { hk_stop(); }
void mpris_publish(const MprisState *s) { (void)s; }
#else
#include <gio/gio.h>

#define BUS_NAME "org.mpris.MediaPlayer2.mikaplay"
#define OBJ_PATH "/org/mpris/MediaPlayer2"

static MprisState st;
static guint owner_id = 0;

static GDBusConnection *gconn;
static GVariant *prop_value(const char *iface, const char *prop);   /* defined below */

static void emit_changed(void)
{
    GVariantBuilder b;
    GVariant *props[] = {
        prop_value("org.mpris.MediaPlayer2.Player", "PlaybackStatus"),
        prop_value("org.mpris.MediaPlayer2.Player", "Metadata"),
        prop_value("org.mpris.MediaPlayer2.Player", "Volume"),
        prop_value("org.mpris.MediaPlayer2.Player", "CanGoNext"),
        prop_value("org.mpris.MediaPlayer2.Player", "CanGoPrevious"),
        NULL
    };
    const char *names[] = { "PlaybackStatus", "Metadata", "Volume",
                            "CanGoNext", "CanGoPrevious" };
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    for (int i = 0; props[i]; i++)
        g_variant_builder_add(&b, "{sv}", names[i], props[i]);
    /* NOTE: this glib's g_variant_new chokes on raw builder/strv args in
     * format strings — build the containers first and use @ references. */
    if (!g_dbus_connection_emit_signal(gconn, NULL, OBJ_PATH,
                                       "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged",
                                       g_variant_new("(s@a{sv}@as)",
                                                     "org.mpris.MediaPlayer2.Player",
                                                     g_variant_builder_end(&b),
                                                     g_variant_new_strv(NULL, 0)),
                                       NULL))
        fprintf(stderr, "mpris: emit_signal failed\n");
}

void mpris_publish(const MprisState *s)
{
    int changed;
    pthread_mutex_lock(&lock);
    changed = s->status != st.status ||
              strcmp(s->title, st.title) ||
              strcmp(s->artist, st.artist) ||
              strcmp(s->album, st.album) ||
              s->duration_us != st.duration_us ||
              s->volume != st.volume ||
              s->can_next != st.can_next ||
              s->can_prev != st.can_prev;
    st = *s;
    pthread_mutex_unlock(&lock);
    if (changed && gconn)
        emit_changed();
}

static void get_state(MprisState *out)
{
    pthread_mutex_lock(&lock);
    *out = st;
    pthread_mutex_unlock(&lock);
}

/* ---------------- property values ---------------- */

static GVariant *prop_value(const char *iface, const char *prop)
{
    MprisState s;
    get_state(&s);
    if (!strcmp(iface, "org.mpris.MediaPlayer2")) {
        if (!strcmp(prop, "CanQuit")) return g_variant_new_boolean(TRUE);
        if (!strcmp(prop, "CanRaise")) return g_variant_new_boolean(FALSE);
        if (!strcmp(prop, "HasTrackList")) return g_variant_new_boolean(FALSE);
        if (!strcmp(prop, "Identity")) return g_variant_new_string("mikaplay");
        if (!strcmp(prop, "DesktopEntry")) return g_variant_new_string("mikaplay");
        if (!strcmp(prop, "SupportedUriSchemes")) {
            const char *a[] = { "file", NULL };
            return g_variant_new_strv(a, -1);
        }
        if (!strcmp(prop, "SupportedMimeTypes"))
            return g_variant_new_strv(NULL, 0);
    } else if (!strcmp(iface, "org.mpris.MediaPlayer2.Player")) {
        if (!strcmp(prop, "PlaybackStatus"))
            return g_variant_new_string(s.status == 1 ? "Playing"
                                         : s.status == 2 ? "Paused" : "Stopped");
        if (!strcmp(prop, "LoopStatus")) return g_variant_new_string("None");
        if (!strcmp(prop, "Rate")) return g_variant_new_double(1.0);
        if (!strcmp(prop, "Shuffle")) return g_variant_new_boolean(FALSE);
        if (!strcmp(prop, "Metadata")) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
            if (s.title[0]) {
                const char *artists[] = { s.artist[0] ? s.artist : "Unknown Artist", NULL };
                g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(s.title));
                g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(artists, -1));
                if (s.album[0])
                    g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(s.album));
            }
            if (s.duration_us > 0)
                g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64(s.duration_us));
            g_variant_builder_add(&b, "{sv}", "mpris:trackid",
                                  g_variant_new_object_path(s.trackid[0] ? s.trackid : "/mikaplay/track/none"));
            return g_variant_builder_end(&b);
        }
        if (!strcmp(prop, "Volume")) return g_variant_new_double(s.volume / 100.0);
        if (!strcmp(prop, "Position")) return g_variant_new_int64((gint64)(s.position * 1e6));
        if (!strcmp(prop, "MinimumRate")) return g_variant_new_double(1.0);
        if (!strcmp(prop, "MaximumRate")) return g_variant_new_double(1.0);
        if (!strcmp(prop, "CanGoNext")) return g_variant_new_boolean(s.can_next);
        if (!strcmp(prop, "CanGoPrevious")) return g_variant_new_boolean(s.can_prev);
        if (!strcmp(prop, "CanPlay")) return g_variant_new_boolean(TRUE);
        if (!strcmp(prop, "CanPause")) return g_variant_new_boolean(TRUE);
        if (!strcmp(prop, "CanSeek")) return g_variant_new_boolean(TRUE);
        if (!strcmp(prop, "CanControl")) return g_variant_new_boolean(TRUE);
    }
    return NULL;
}

/* ---------------- method calls ---------------- */

static GDBusPropertyInfo *player_props[];
static GDBusPropertyInfo *root_props[];

static void reply_ok(GDBusMethodInvocation *inv)
{
    g_dbus_method_invocation_return_value(inv, g_variant_new("()"));
}

static void on_method_call(GDBusConnection *c, const gchar *sender,
                           const gchar *path, const gchar *iface,
                           const gchar *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)ud;
    if (!strcmp(iface, "org.freedesktop.DBus.Properties")) {
        if (!strcmp(method, "Get")) {
            const gchar *pi, *pn;
            GVariant *v;
            g_variant_get(params, "(&s&s)", &pi, &pn);
            v = prop_value(pi, pn);
            if (v)
                g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
            else
                g_dbus_method_invocation_return_dbus_error(
                    inv, "org.freedesktop.DBus.Error.InvalidArgs", "no such property");
            return;
        }
        if (!strcmp(method, "GetAll")) {
            const gchar *pi;
            GVariantBuilder b;
            GDBusPropertyInfo *const *props = NULL;
            g_variant_get(params, "(&s)", &pi);
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
            if (!strcmp(pi, "org.mpris.MediaPlayer2.Player")) props = player_props;
            else if (!strcmp(pi, "org.mpris.MediaPlayer2")) props = root_props;
            if (props) {
                for (int i = 0; props[i]; i++) {
                    GVariant *v = prop_value(pi, props[i]->name);
                    if (v) g_variant_builder_add(&b, "{sv}", props[i]->name, v);
                }
            }
            g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sv})", &b));
            return;
        }
        if (!strcmp(method, "Set")) {
            const gchar *pi, *pn;
            GVariant *val;
            g_variant_get(params, "(&s&sv)", &pi, &pn, &val);
            if (!strcmp(pi, "org.mpris.MediaPlayer2.Player") && !strcmp(pn, "Volume")) {
                enqueue(MPRIS_VOLUME, (int64_t)(g_variant_get_double(val) * 10000));
                reply_ok(inv);
                return;
            }
            g_dbus_method_invocation_return_dbus_error(
                inv, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
            return;
        }
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
        return;
    }
    if (!strcmp(iface, "org.mpris.MediaPlayer2.Player")) {
        if (!strcmp(method, "PlayPause")) { enqueue(MPRIS_PLAYPAUSE, 0); reply_ok(inv); return; }
        if (!strcmp(method, "Play")) { enqueue(MPRIS_PLAY, 0); reply_ok(inv); return; }
        if (!strcmp(method, "Pause")) { enqueue(MPRIS_PAUSE, 0); reply_ok(inv); return; }
        if (!strcmp(method, "Stop")) { enqueue(MPRIS_STOP, 0); reply_ok(inv); return; }
        if (!strcmp(method, "Next")) { enqueue(MPRIS_NEXT, 0); reply_ok(inv); return; }
        if (!strcmp(method, "Previous")) { enqueue(MPRIS_PREV, 0); reply_ok(inv); return; }
        if (!strcmp(method, "Seek")) {
            gint64 off;
            g_variant_get(params, "(x)", &off);
            enqueue(MPRIS_SEEK, off);
            reply_ok(inv);
            return;
        }
        if (!strcmp(method, "SetPosition")) {
            gint64 pos;
            g_variant_get(params, "(ox)", NULL, &pos);
            enqueue(MPRIS_SETPOS, pos);
            reply_ok(inv);
            return;
        }
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
        return;
    }
    g_dbus_method_invocation_return_dbus_error(
        inv, "org.freedesktop.DBus.Error.UnknownMethod", "unknown interface");
}

/* ---------------- interface info (introspection + dispatch) ---------------- */

#define PINFO(nm, sig) { -1, (nm), (sig), G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL }

static GDBusPropertyInfo p_canquit         = PINFO("CanQuit", "b");
static GDBusPropertyInfo p_canraise        = PINFO("CanRaise", "b");
static GDBusPropertyInfo p_hastracklist    = PINFO("HasTrackList", "b");
static GDBusPropertyInfo p_identity        = PINFO("Identity", "s");
static GDBusPropertyInfo p_desktopentry    = PINFO("DesktopEntry", "s");
static GDBusPropertyInfo p_urischemes      = PINFO("SupportedUriSchemes", "as");
static GDBusPropertyInfo p_mimetypes       = PINFO("SupportedMimeTypes", "as");
static GDBusPropertyInfo *root_props[] = {
    &p_canquit, &p_canraise, &p_hastracklist, &p_identity,
    &p_desktopentry, &p_urischemes, &p_mimetypes, NULL
};

static GDBusPropertyInfo p_playbackstatus  = PINFO("PlaybackStatus", "s");
static GDBusPropertyInfo p_loopstatus      = PINFO("LoopStatus", "s");
static GDBusPropertyInfo p_rate            = PINFO("Rate", "d");
static GDBusPropertyInfo p_shuffle         = PINFO("Shuffle", "b");
static GDBusPropertyInfo p_metadata        = PINFO("Metadata", "a{sv}");
static GDBusPropertyInfo p_volume          = { -1, "Volume", "d", G_DBUS_PROPERTY_INFO_FLAGS_READABLE | G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE, NULL };
static GDBusPropertyInfo p_position        = PINFO("Position", "x");
static GDBusPropertyInfo p_minrate         = PINFO("MinimumRate", "d");
static GDBusPropertyInfo p_maxrate         = PINFO("MaximumRate", "d");
static GDBusPropertyInfo p_cangonext       = PINFO("CanGoNext", "b");
static GDBusPropertyInfo p_cangoprev       = PINFO("CanGoPrevious", "b");
static GDBusPropertyInfo p_canplay         = PINFO("CanPlay", "b");
static GDBusPropertyInfo p_canpause        = PINFO("CanPause", "b");
static GDBusPropertyInfo p_canseek         = PINFO("CanSeek", "b");
static GDBusPropertyInfo p_cancontrol      = PINFO("CanControl", "b");
static GDBusPropertyInfo *player_props[] = {
    &p_playbackstatus, &p_loopstatus, &p_rate, &p_shuffle, &p_metadata,
    &p_volume, &p_position, &p_minrate, &p_maxrate, &p_cangonext,
    &p_cangoprev, &p_canplay, &p_canpause, &p_canseek, &p_cancontrol, NULL
};

static GDBusArgInfo a_seek_offset = { -1, "Offset", "x", NULL };
static GDBusArgInfo *seek_in[] = { &a_seek_offset, NULL };
static GDBusArgInfo a_sp_trackid  = { -1, "TrackId", "o", NULL };
static GDBusArgInfo a_sp_position = { -1, "Position", "x", NULL };
static GDBusArgInfo *setpos_in[] = { &a_sp_trackid, &a_sp_position, NULL };

static GDBusMethodInfo m_playpause = { -1, "PlayPause", NULL, NULL, NULL };
static GDBusMethodInfo m_play      = { -1, "Play", NULL, NULL, NULL };
static GDBusMethodInfo m_pause     = { -1, "Pause", NULL, NULL, NULL };
static GDBusMethodInfo m_stop      = { -1, "Stop", NULL, NULL, NULL };
static GDBusMethodInfo m_next      = { -1, "Next", NULL, NULL, NULL };
static GDBusMethodInfo m_previous  = { -1, "Previous", NULL, NULL, NULL };
static GDBusMethodInfo m_seek      = { -1, "Seek", seek_in, NULL, NULL };
static GDBusMethodInfo m_setpos    = { -1, "SetPosition", setpos_in, NULL, NULL };
static GDBusMethodInfo *player_methods[] = {
    &m_playpause, &m_play, &m_pause, &m_stop, &m_next,
    &m_previous, &m_seek, &m_setpos, NULL
};

static GDBusInterfaceInfo player_iface = {
    -1, "org.mpris.MediaPlayer2.Player",
    player_methods, NULL, player_props, NULL
};
static GDBusInterfaceInfo root_iface = {
    -1, "org.mpris.MediaPlayer2",
    NULL, NULL, root_props, NULL
};

/* ---------------- init ---------------- */

static GMainLoop *loop;
static pthread_t thr;

static void bus_acquired(GDBusConnection *c, const gchar *name, gpointer ud)
{
    static const GDBusInterfaceVTable vtable = { .method_call = on_method_call };
    GError *err = NULL;
    (void)name; (void)ud;
    gconn = g_object_ref(c);
    g_dbus_connection_register_object(c, OBJ_PATH, &player_iface, &vtable, NULL, NULL, &err);
    if (!err)
        g_dbus_connection_register_object(c, OBJ_PATH, &root_iface, &vtable, NULL, NULL, &err);
    if (err) {
        fprintf(stderr, "mpris: register object failed: %s\n", err->message);
        g_error_free(err);
    }
}

static void name_lost(GDBusConnection *c, const gchar *name, gpointer ud)
{
    (void)c; (void)ud;
    fprintf(stderr, "mpris: bus name %s unavailable (another instance?)\n", name);
}

/* g_bus_own_name + message dispatch need an iterated GMainContext, so the
 * bus lives on its own thread with its own loop. All callbacks run here and
 * only touch mutex-protected state + the command queue. */
static void *bus_thread(void *ud)
{
    GMainContext *ctx;
    (void)ud;
    ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    loop = g_main_loop_new(ctx, FALSE);
    owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME,
                              G_BUS_NAME_OWNER_FLAGS_NONE,
                              bus_acquired, NULL, name_lost, NULL, NULL);
    g_main_loop_run(loop);
    g_bus_unown_name(owner_id);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    loop = NULL;
    return NULL;
}

void mpris_init(void)
{
    memset(&st, 0, sizeof st);
    snprintf(st.trackid, sizeof st.trackid, "/mikaplay/track/none");
    pthread_create(&thr, NULL, bus_thread, NULL);
}

void mpris_shutdown(void)
{
    if (loop) g_main_loop_quit(loop);
    pthread_join(thr, NULL);
    loop = NULL;
}
#endif
