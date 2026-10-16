# tgm3-recomp: game.exe statically recompiled to C, on the Win32 layer from ../native.
CC      ?= gcc
PYTHON  ?= python3
GAME_EXE ?= ../Tetris The Grand Master 3  (PC)/game.exe
SDL32   := PKG_CONFIG_PATH=/usr/lib32/pkgconfig pkg-config sdl2
GL_HEADERS ?= /usr/include/GL/gl.h /usr/include/GL/glext.h

GEN := build/gen
N   := ../native

# HOST=native (default): this machine's word size, guest memory in a 4 GiB window at M.
# HOST=i386: 32-bit x86 build where guest addresses are host addresses (M = 0).
HOST ?= native
ifeq ($(HOST),i386)
B       := build/i386
ARCH    := -m32 -msse2 -mfpmath=sse
SDL     := $(SDL32)
LDFLAGS += -no-pie
else
B       := build/$(HOST)
ARCH    :=
SDL     := pkg-config sdl2
CFLAGS  += -DGUEST_WINDOW
endif
OPT     ?= -O2 -g
CFLAGS  += $(OPT) $(ARCH) -std=gnu11 -fno-strict-aliasing -I$(N)/src -I../linux/src -Irt -I$(GEN) -I$(B) $(shell $(SDL) --cflags)
GENFLAGS := -Wno-unused-variable -Wno-unused-label -Wno-unused-but-set-variable
LDFLAGS += $(ARCH)
LDLIBS  += $(shell $(SDL) --libs) -lm -lpthread

HLE  := kernel32.c user32.c opengl.c dsound.c comm.c sdlinput.c common.c gmem.c
OBJS := $(addprefix $(B)/hle/,$(HLE:.c=.o)) $(B)/hle/jvs.o $(B)/rt/rt.o $(B)/rt/host.o

all: $(B)/tgm3-recomp

# Translation: regenerated when the recompiler or the exe changes.
$(GEN)/files.mk: tools/recomp.py tools/analyze.py tools/pe.py
	$(PYTHON) tools/recomp.py "$(GAME_EXE)" $(GEN)

-include $(GEN)/files.mk
GEN_OBJS := $(addprefix $(B)/genobj/,$(GEN_SRCS:.c=.o))

$(B)/hle $(B)/rt $(B)/genobj:
	mkdir -p $@

$(GEN_OBJS) $(OBJS): Makefile

$(B)/glwrap.h: $(N)/tools/gen_glwrap.py
	mkdir -p $(B)
	$(PYTHON) -I $< $(GL_HEADERS) > $@

$(B)/hle/opengl.o: $(B)/glwrap.h
$(B)/hle/%.o: $(N)/src/%.c $(N)/src/w32.h | $(B)/hle
	$(CC) $(CFLAGS) -Wall -Wextra -Wno-array-bounds -Wno-stringop-overflow -c -o $@ $<
$(B)/hle/jvs.o: ../linux/src/jvs.c | $(B)/hle
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/rt/%.o: rt/%.c rt/rt.h rt/rt_host.h | $(B)/rt
	$(CC) $(CFLAGS) -Wall -Wextra -c -o $@ $<
$(B)/genobj/%.o: $(GEN)/%.c rt/rt.h | $(B)/genobj
	$(CC) $(CFLAGS) $(GENFLAGS) -c -o $@ $<

$(B)/tgm3-recomp: $(GEN_OBJS) $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf build

.PHONY: all clean
