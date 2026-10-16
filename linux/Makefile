CC      ?= cc
WINEGCC ?= winegcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra
EVDEV_CFLAGS := $(shell pkg-config --cflags libevdev)
EVDEV_LIBS   := $(shell pkg-config --libs libevdev)
# game.exe is 32-bit, so the DLL loaded into it must be too.
WINEFLAGS := -b i686-windows -O2 -Wall

B := build

all: $(B)/jvsd $(B)/jvsbridge.dll $(B)/tgm3inject.exe

$(B):
	mkdir -p $@

$(B)/jvsd: src/jvsd.c src/jvs.c src/input.c src/jvs.h src/input.h | $(B)
	$(CC) $(CFLAGS) $(EVDEV_CFLAGS) -o $@ src/jvsd.c src/jvs.c src/input.c $(EVDEV_LIBS)

$(B)/test_jvs: src/test_jvs.c src/jvs.c src/jvs.h | $(B)
	$(CC) $(CFLAGS) -o $@ src/test_jvs.c src/jvs.c

$(B)/jvsbridge.dll: wine/jvsbridge.c | $(B)
	$(WINEGCC) $(WINEFLAGS) -shared -o $@ $< -lws2_32

$(B)/tgm3inject.exe: wine/tgm3inject.c | $(B)
	$(WINEGCC) $(WINEFLAGS) -o $@ $<

test: $(B)/test_jvs
	./$(B)/test_jvs

clean:
	rm -rf $(B)

.PHONY: all test clean
