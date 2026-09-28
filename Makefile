CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl2 SDL2_image mpv libcurl gio-2.0 2>/dev/null)
LIBS    := $(shell pkg-config --libs sdl2 SDL2_image mpv libcurl gio-2.0 2>/dev/null) -lm -lpthread -lz
SRC     := src/main.c src/player.c src/font.c src/icons.c src/icons20.c src/library.c src/queue.c src/scrobble.c src/config.c src/md5.c src/tags.c src/mpris.c src/cJSON.c src/fs.c src/history.c src/recap.c src/waveform.c src/theme.c src/bg.c src/album.c src/libcache.c
OBJ     := $(SRC:src/%.c=build/%.o)

all: build/marimo

build/marimo: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LIBS)

build/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build

run: build/marimo
	./build/marimo

# install: the desktop entry points at $(PREFIX)/bin/marimo, so this is what "marimo"
# runs. The font goes to share/marimo because the binary looks it up there (and in
# ./assets) rather than beside itself, and the icon to hicolor so the entry can name it
# as just "marimo". Installed by hand once on 2026-09-28; this is that, made repeatable.
PREFIX ?= $(HOME)/.local
install: build/marimo
	install -d $(PREFIX)/bin $(PREFIX)/share/marimo $(PREFIX)/share/applications \
	           $(PREFIX)/share/icons/hicolor/256x256/apps
	install -m 755 build/marimo $(PREFIX)/bin/marimo
	install -m 644 assets/unifont_all.hex assets/marimo.png $(PREFIX)/share/marimo/
	install -m 644 assets/marimo.png $(PREFIX)/share/icons/hicolor/256x256/apps/marimo.png
	@echo "installed $(PREFIX)/bin/marimo (entry: $(PREFIX)/share/applications/marimo.desktop)"

.PHONY: all clean run install
