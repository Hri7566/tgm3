# TGM3 on Linux

Tools for running *Tetris The Grand Master 3: Terror-Instinct* on Linux. The PC release is the
Taito Type X arcade `game.exe` (32-bit Windows, OpenGL + DirectSound) plus a loader that fakes
the cabinet's JVS I/O board. This repo has three ways to run it, each one less dependent on
Windows or x86 than the last.

| Directory | Approach | Runs on |
|---|---|---|
| [`linux/`](linux/README.md) | Wine, plus a rebuilt JVS loader (`jvsd`, `jvsbridge.dll`, `tgm3inject.exe`) | x86 Linux with Wine |
| [`native/`](native/README.md) | A PE loader and a Win32 layer written in C on SDL2/OpenGL. The game's own x86 code runs directly on the CPU, without Wine | x86 Linux (32-bit build) |
| [`recomp/`](recomp/README.md) | Static recompilation. Every function in `game.exe` becomes C, built on the same Win32 layer as `native/` | 32-bit and 64-bit x86 Linux, tested. The sources have no x86-specific code, but no other CPU has been tested |

`native/` is the reference the recompiled build is checked against. With the same input
script, both produce byte-identical frames and memory snapshots through 9000 frames of attract
mode and gameplay.

## You need your own copy of the game

No game files are in this repo, and `.gitignore` keeps them out. Every build takes the game
folder as an argument (or `TGM3_DIR`). It must contain `game.exe` and `data/`. The recompiler
reads `game.exe` at build time too (`GAME_EXE` in `recomp/Makefile`). The default paths assume
the folder sits next to these directories as `Tetris The Grand Master 3  (PC)`.

Saves go to `TGM3/sramdata` inside the game folder. When testing, run against a copy of the
folder so test runs don't overwrite real saves.

## Quick start

```sh
# Wine
cd linux && make && ./tgm3.sh

# Native loader (needs gcc -m32, lib32-sdl2, 32-bit libGL)
cd native && make && ./build/tgm3-native "../Tetris The Grand Master 3  (PC)"

# Recompiled (needs Python 3 + capstone, SDL2, OpenGL)
cd recomp && make -j && ./build/native/tgm3-recomp "../Tetris The Grand Master 3  (PC)"
```

Default keys: arrows, Z/X/C/V = A/B/C/D, 1 = start, 5 = coin, 9 = test, Esc = quit. Edit
`bindings.conf` in each directory to change them. Each directory's README covers its options,
input recording and replay, and how it works.
