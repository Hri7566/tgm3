# tgm3-native

Runs the original TGM3 `game.exe` on x86 Linux **without Wine**. A small loader maps the
32-bit PE at its fixed base (`0x400000`, it has no relocations) and points its 197 imports
at C implementations built on SDL2 and OpenGL. The game's own x86 code then runs directly
on the CPU.

The Windows layer here is shared with the static recompilation in `../recomp`, and this build
is the reference the recompiled build gets compared against frame by frame (see "Recording and
replay").

## Build & run

Needs gcc with `-m32`, 32-bit SDL2 (`lib32-sdl2`), 32-bit libGL, and Python 3.

```sh
make
./build/tgm3-native "../Tetris The Grand Master 3  (PC)"    # or set TGM3_DIR
```

`bindings.conf` (next to `build/`) is loaded automatically. It uses SDL key names and `pad:*`
controller buttons. P1's layout matches `../linux/bindings.conf`. Esc quits, F11 or Alt+Enter
toggles fullscreen, and F12 saves the current frame. Saves go to `TGM3/sramdata` in the game
folder, the same place as on Windows.

| Variable | Effect |
|---|---|
| `TGM3_SCALE=n` | initial window scale (default 2 → 1280×960) |
| `TGM3_FULLSCREEN=1` | start fullscreen (desktop resolution, aspect kept) |
| `TGM3_FPS=60` | frame limiter target. The arcade runs at 60. 0 disables it |
| `TGM3_VSYNC=1` | also sync to the display |
| `TGM3_BINDINGS=file` | alternative bindings file |
| `-v` / `--trace` | log API activity / every API call |

## How the pieces map

| Windows | Here |
|---|---|
| KERNEL32 (files, heap, VirtualAlloc, threads, TLS, locale) | `kernel32.c`. `C:\` (any drive) maps to the game folder, with case-insensitive lookup |
| COM2 + `typex_io.dll` JVS board | `comm.c` + `../linux/src/jvs.c`, with the sense line from the board's address |
| DirectInput | `sdlinput.c` (keyboard + per-player game controllers) |
| USER32 / GDI32 | `user32.c`: one SDL window, a real message queue, WM_CREATE/ACTIVATEAPP sent to the game's WndProc |
| OPENGL32 + WGL (GLEW, `WGL_ARB_pbuffer`) | `opengl.c`. GL calls go through typed wrappers generated from the system GL headers (`tools/gen_glwrap.py`) that read the guest's stdcall arguments. The game draws into an FBO at its 640×448 child-window size, centered in its 640×480 frame and scaled to the window. The pbuffer is a shared context with its own FBO |
| DSOUND | `dsound.c`: software mixer (volume, pan, frequency, looping, duplicate buffers) on SDL audio |
| TEB / `fs:` | `set_thread_area` per thread, with Windows' 53-bit x87 precision (`fldcw 0x27F`) |

Every API is written as `uint32_t fn(uint32_t *args)` with guest pointers going through
`G2H()`. In this loader that's a cast; the 64-bit recompiled build defines `GUEST_WINDOW`, which
makes it guest-memory base + offset. Anything handed to the game (heap blocks, COM objects,
strings, sound buffers) comes from `gmem.c`, so it's addressable either way.

## Recording and replay

- `TGM3_RECORD=file` writes every press as `<frame> <action> <frames held>`, with frames
  counted by `SwapBuffers`.
- `TGM3_INPUT=file` plays such a file back (hand-written ones work too).
- `TGM3_SNAPSHOT=600,1200` saves those frames as BMP.
- `TGM3_AUDIO_DUMP=out.wav` writes the mixed audio (paced by the audio device, so it isn't
  frame-exact).
- `TGM3_MEMDUMP=600,1200` writes game.exe's `.data`/`.bss` at those frames, for diffing the
  game state of two builds.

Together these give a reproducible run whose frames and sound a recompiled build must match.

## Not implemented

- GDI `TextOutA`. The game only draws a blank string with it at startup.
- SEH dispatch. `RaiseException` stops with a message, because the game never raises during normal play.
- Suspending a thread other than the caller (the game only suspends itself).
- `TerminateThread` on a running thread (it's only marked finished).
- The ANSI code page is 1252.
