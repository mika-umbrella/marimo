# mikaplay

a tiny retro pixel music player for linux. dark grey chrome, winamp-green accents, GNU Unifont for every character, hand-drawn 16×16 icons, and a little sunset badge for an icon.

built with C, SDL2, libmpv and a lot of stubbornness. no GTK, no webview, no feelings of guilt.

## features

- **browse your library by folder** — the browser is rooted at your music folder, so it can't wander off into your filesystem
- **album-first workflow** — double-click adds to the end of the queue, middle-click replaces the queue and plays now, right-click a folder to peek inside it
- **true gapless playback** — mpv's `gapless-audio`, bit-perfect FLAC, encoder-delay-aware MP3
- **queue memory** — your queue survives quitting; restore on launch, skip files that vanished
- **disc/track ordering** — folders queue by tag track numbers, not by whatever the filenames decided to do (a library sweep showed 35 of 582 albums had wrong filename order — all fixed by tags)
- **album art** — folder covers (`cover.jpg`, `folder.png`, `%album - %artist.jpg`…), then embedded tag art, then a lonely `♪`
- **Last.fm + ListenBrainz scrobbling** — device-flow auth for Last.fm, token for ListenBrainz, both on background threads
- **MPRIS2 over D-Bus** — KDE media keys, `playerctl`, the tray widget; all work globally
- **media keys** — play/pause/next/prev/stop/mute/volume right on the keyboard
- **GNU Unifont** — all 128,071 glyphs from `unifont_all.hex`, so あ, 😀 and every weird filename character render exactly as the font intended
- **mini mode** — the whole player folds to a 480×170 strip; the window itself is fixed-size, winamp-style
- **borderless + draggable** — drag by the title bar, minimize/fold/settings/close in the corner

## building

```
# fedora / rpm-based
sudo dnf install gcc make pkg-config SDL2-devel SDL2_image-devel mpv-devel libcurl-devel glib2-devel

make
```

the binary needs `unifont_all.hex` — it looks in `assets/`, next to the binary, or in `~/.local/share/mikaplay/`. grab it from the [GNU unifont releases](https://ftp.gnu.org/gnu/unifont/) (any recent `unifont_all-*.hex`).

run it:

```
./build/mikaplay
```

test modes: `--selftest`, `--headless /path/to/album`, `--smoke`, `--makeicon assets/mikaplay.png`.

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

## scrobbling setup

1. get a Last.fm API key + secret at <https://www.last.fm/api/account/create>
2. get a ListenBrainz token at <https://listenbrainz.org/profile>
3. `⚙` → paste them in → **authorize last.fm** → approve in your browser

config and queue live in `~/.config/mikaplay/`.

## architecture

```
src/main.c      UI, layout, events, queue driving
src/player.c    libmpv wrapper (gapless, art, metadata)
src/mpris.c     MPRIS2 D-Bus service (own thread, GDBus)
src/scrobble.c  Last.fm + ListenBrainz (worker threads)
src/tags.c      fast FLAC/ID3v2 tag parser (no deps)
src/font.c      GNU Unifont .hex parser + texture cache
src/library.c   folder scan + album art discovery
src/queue.c     the queue (shuffle/repeat, mutex)
src/icons.c     hand-drawn 16×16 pixel icons
```

## credits & license

- GNU Unifont is © Roman Czyborra et al., GPL-2.0+ with the font embedding exception (glyphs are data — they render here, not embedded in the binary)
- music playback via [mpv](https://mpv.io) / libmpv (LGPL)
- this project: GPL-3.0 — see [LICENSE](LICENSE)

made with love on a fedora box, for listening to vocaloid at 2am.
