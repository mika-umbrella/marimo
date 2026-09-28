CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl2 SDL2_image mpv libcurl gio-2.0 2>/dev/null)
LIBS    := $(shell pkg-config --libs sdl2 SDL2_image mpv libcurl gio-2.0 2>/dev/null) -lm -lpthread -lz
SRC     := src/main.c src/player.c src/font.c src/icons.c src/library.c src/queue.c src/scrobble.c src/config.c src/md5.c src/tags.c src/mpris.c src/cJSON.c src/fs.c src/history.c src/recap.c src/waveform.c src/theme.c
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

.PHONY: all clean run
