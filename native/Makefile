# tgm3-native: runs the original 32-bit game.exe directly on x86 Linux (no Wine).
CC      ?= gcc
PYTHON  ?= python3
SDL32   := PKG_CONFIG_PATH=/usr/lib32/pkgconfig pkg-config sdl2
GL_HEADERS ?= /usr/include/GL/gl.h /usr/include/GL/glext.h

# game.exe has no relocations and must sit at 0x400000, so this binary is 32-bit and non-PIE.
CFLAGS  ?= -O2 -g
CFLAGS  += -m32 -std=gnu11 -Wall -Wextra -fno-pie -fno-omit-frame-pointer -Wno-array-bounds -Wno-stringop-overflow \
           -I../linux/src -Ibuild $(shell $(SDL32) --cflags)
LDFLAGS += -m32 -no-pie
LDLIBS  += $(shell $(SDL32) --libs) -lm -lpthread

B    := build
SRCS := src/loader.c src/common.c src/gmem.c src/thunk.c src/kernel32.c src/user32.c src/opengl.c src/dsound.c \
        src/comm.c src/sdlinput.c ../linux/src/jvs.c
OBJS := $(patsubst %.c,$(B)/%.o,$(notdir $(SRCS))) $(B)/guest.o

all: $(B)/tgm3-native

$(B):
	mkdir -p $@

$(B)/glwrap.h: tools/gen_glwrap.py | $(B)
	$(PYTHON) -I $< $(GL_HEADERS) > $@

$(B)/opengl.o: $(B)/glwrap.h

$(B)/%.o: src/%.c src/w32.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/jvs.o: ../linux/src/jvs.c ../linux/src/jvs.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/guest.o: src/guest.S | $(B)
	$(CC) -m32 -c -o $@ $<

$(B)/tgm3-native: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(B)

.PHONY: all clean
