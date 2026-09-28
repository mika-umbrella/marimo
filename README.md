# marimo

a tiny retro pixel music player for linux. dark grey chrome, winamp-green accents, GNU Unifont for every character, hand-drawn icons, and a little sunset badge for an icon.

built with C, SDL2, libmpv and a lot of stubbornness. no GTK, no webview, no feelings of guilt.

this is the desktop marimo — the sibling of the android app, and the mainline of this repo. it grew out of mikaplay, so your queue, config and scrobbles carry over untouched the first time you run it.

## features

- **browse your library by folder** — the browser is rooted at your music folder, so it can't wander off into your filesystem
- **album-first workflow** — double-click adds to the end of the queue, middle-click replaces the queue and plays now, right-click a folder to peek inside it
- **true gapless playback** — mpv's `gapless-audio`, bit-perfect FLAC, encoder-delay-aware MP3
- **queue memory** — your queue survives quitting; restore on launch, skip files that vanished
- **disc/track ordering** — folders queue by tag track numbers, not by whatever the filenames decided to do. multi-disc sets are one album, ordered by their disc and track tags rather than by which subfolder a file happens to sit in
- **album art** — folder covers (`cover.jpg`, `folder.png`, `%album - %artist.jpg`…), then embedded tag art, then a lonely `♪`
- **track numbers in the list** — folder rows carry their `disc.track`, the way the queue always has
- **waveform seekbar** — 96 RMS peaks per track: the phone's `waves.marimo` sidecars when they exist, decoded locally when they don't
- **listening diary** — one jsonl line per finished listen, the same format as the android app's HistoryDiary; rolled aside per year and pruned
- **a recap** — week/month/year totals, top artists/albums/tracks, streaks, skip rate, replay king, discovery vs comfort. the same arithmetic as the android Recap, pinned against its unit tests
- **light and dark** — the phone's three panel alphas, with the light accents darkened on purpose: a bright green at 1.4:1 is not text
- **an art-derived backdrop** — a seeded tileable drift that takes its colours from the current cover, at about 2% of a core
- **a library cache** — per-album counts keyed on folder mtime, so the list is not an `opendir` per row per frame
- **Last.fm + ListenBrainz scrobbling** — device-flow auth for Last.fm, token for ListenBrainz, both on background threads
- **MPRIS2 over D-Bus** — KDE media keys, `playerctl`, the tray widget; all work globally
- **media keys** — play/pause/next/prev/stop/mute/volume right on the keyboard
- **GNU Unifont** — all 128,071 glyphs from `unifont_all.hex`, so あ, 😀 and every weird filename character render exactly as the font intended
- **mini mode** — the whole player folds to a 480×170 strip; the window itself is fixed-size, winamp-style
- **borderless + draggable** — drag by the title bar, minimize/fold/settings/close in the corner

## building

```
# fedora / rhel
sudo dnf install gcc make pkg-config SDL2-devel SDL2_image-devel mpv-devel libcurl-devel glib2-devel

# debian / ubuntu
sudo apt install build-essential pkg-config libsdl2-dev libsdl2-image-dev libmpv-dev libcurl4-openssl-dev libglib2.0-dev

# arch / manjaro
sudo pacman -S gcc make pkg-config sdl2 sdl2_image mpv curl glib2

# opensuse
sudo zypper install gcc make pkg-config SDL2-devel SDL2_image-devel mpv-devel libcurl-devel glib2-devel

# alpine
sudo apk add build-base pkgconf sdl2-dev sdl2_image-dev mpv-dev curl-dev glib-dev

make
```

### installing

```
make install          # PREFIX defaults to ~/.local
```

that puts the binary at `~/.local/bin/marimo`, `unifont_all.hex` and the icon in
`~/.local/share/marimo/`, and the icon again in
`~/.local/share/icons/hicolor/256x256/apps/`, which is where a desktop entry expects to
find it by name.

### windows (cross-compile from linux)

```
# fedora: mingw toolchain
sudo dnf install mingw64-gcc mingw64-SDL2_image mingw64-sdl2-compat mingw64-curl

make -f Makefile.win dist    # -> build-win/marimo-windows.zip
```

the zip bundles the exe, the runtime dlls, and the assets — extract and run
`marimo.exe`. mpv's windows dev files (headers + import lib) are vendored
in `vendor/mpv-win/`; the 117MB `libmpv-2.dll` ships only in the dist zip.
tested under wine (set `SDL_RENDER_DRIVER=software` there; real windows uses
the fast D3D path).

the scrobbler needs `cacert.pem` beside the exe on windows — libcurl there has no
default CA bundle, so the lookup is resolved relative to the executable and the dist
target copies it in.

(the glib2 dependency is what powers MPRIS media-key integration — every desktop distro carries it.)

the binary needs `unifont_all.hex` — it looks in `assets/`, `~/.local/share/marimo/`, or
beside itself. grab it from the [GNU unifont releases](https://ftp.gnu.org/gnu/unifont/) (any recent `unifont_all-*.hex`).

run it:

```
./build/marimo
```

test modes: `--selftest`, `--headless /path/to/album`, `--smoke`, `--makeicon assets/marimo.png`.

## controls

| input | action |
|---|---|
| double-click track / folder | add to end of queue (plays if idle) |
| middle-click track / folder | replace queue, play now |
| right-click folder | browse inside |
| right-click queue row | remove from queue |
| `space` / media play-pause | play / pause |
| `←` `→` | seek ±5s |
| `↑` `↓` / media volume | volume (changes unmute) |
| `enter` | same as double-click |
| `delete` | remove selected queue row |
| `ctrl`+`delete` | clear the queue (asks first) |
| `d` | toggle light / dark |

## scrobbling setup

1. get a Last.fm API key + secret at <https://www.last.fm/api/account/create>
2. get a ListenBrainz token at <https://listenbrainz.org/profile>
3. `⚙` → paste them in → **authorize last.fm** → approve in your browser

config and queue live in `~/.config/marimo/`. `refresh=N` sets the library
auto-refresh interval in seconds (default 5, `0` disables): the app stats the
current folder each interval and rescans only when it changed, so new/renamed/
deleted albums show up without re-entering the folder.

## architecture

```
src/main.c      UI, layout, events, queue driving
src/player.c    libmpv wrapper (gapless, art, metadata)
src/mpris.c     MPRIS2 D-Bus service (own thread, GDBus)
src/scrobble.c  Last.fm + ListenBrainz (worker threads)
src/tags.c      fast FLAC/ID3v2 tag parser (no deps)
src/font.c      GNU Unifont .hex parser + texture cache
src/library.c   folder scan + album art discovery
src/album.c     folder-name metadata (artist/title/year) + cover thumbnails
src/libcache.c  per-album track counts, keyed on folder mtime
src/queue.c     the queue (shuffle/repeat, mutex)
src/history.c   the listening diary (jsonl, rotated yearly)
src/recap.c     the recap aggregator (the android Recap's arithmetic)
src/waveform.c  waves.marimo sidecars, or ffmpeg decode + disk cache
src/theme.c     palette from the cover, light/dark tables, panel alphas
src/bg.c        the seeded tileable drift backdrop
src/icons.c     hand-drawn 16×16 pixel icons
src/icons20.c   generated 20px masks, ported from the android drawables
```

## credits & license

- GNU Unifont is © Roman Czyborra et al., GPL-2.0+ with the font embedding exception (glyphs are data — they render here, not embedded in the binary)
- music playback via [mpv](https://mpv.io) / libmpv (LGPL)
- the icon shapes come from the android app's own Feather-style drawables, traced to 1-bit by hand
- this project: GPL-3.0 — see [LICENSE](LICENSE)

made with love on a fedora box, for listening to vocaloid at 2am.
