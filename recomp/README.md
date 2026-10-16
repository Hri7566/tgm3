# tgm3-recomp

TGM3's `game.exe` statically recompiled to C. No x86 code from the game runs: every function
becomes C, built for whatever the host is, on top of the Win32 layer in `../native/src`.

## Build & run

Needs Python 3 with `capstone`, gcc or clang, SDL2, and OpenGL headers.

```sh
make -j                   # 64-bit (or any) host  -> build/native/tgm3-recomp
make -j HOST=i386         # 32-bit x86 host       -> build/i386/tgm3-recomp
./build/native/tgm3-recomp "../Tetris The Grand Master 3  (PC)"
```

The first `make` translates `game.exe` (path in `GAME_EXE`) into `build/gen/` (~400k lines,
~10 s to compile on 16 cores). It's regenerated when the translator changes. The binary
still reads `game.exe` at startup for its data sections, and refuses a different exe. Options,
environment variables and `bindings.conf` are the same as `../native` (see its README).

## How it works

`tools/analyze.py` finds the code: it starts from the entry point and direct call targets,
then adds dwords in data and immediate operands that point at plausible code (it rejects
candidates that decode to things compiled C never contains, or that land inside known
instructions). Jump tables are bounded by the `cmp`/`and` that guards them. CRT `memcpy`'s
tables, which index backwards and overlap code, are handled by switching over every table
target in the function.

`tools/recomp.py` emits one C function per guest function, `void F_<va>(CPU *c)`:

- Registers and flags are C locals, synced to the `CPU` struct around calls. The compiler
  drops flag computations nobody reads. Flags are assumed dead across calls and returns,
  which holds for compiled code.
- `call` pushes the real return address on the guest stack and calls the C function; `ret`
  records what it popped, and the caller checks it (`CHECK_RET` warns on mismatch). A `jmp`
  to another function is a tail call.
- Indirect calls and jumps go through `rt_call`, a table from guest address to C function.
  Win32 imports are entry addresses at `0xF0000000+` that run the C implementation against
  the guest stack.
- x87 is modeled with `double` registers. Windows runs the FPU at 53-bit precision, so
  IEEE double arithmetic gives the same results. `fist` honors the rounding mode.
- Guest memory goes through `RD32()`/`WR32()` at `M + address`. On i386 `M` is 0. Elsewhere
  it's a 4 GiB reserved window (`GUEST_WINDOW`), and `gmem.c` hands out everything the game
  sees (image, stacks, TEBs, heap, COM objects, sound buffers) from inside it.

`rt/` holds the runtime (dispatch, string ops, x87 helpers) and the host side (PE data
loading, threads, entry points, crash dumps that show the last guest block entered).

## Checking against the native loader

Both builds count frames by `SwapBuffers`, so an input script plus snapshots compares them:

```sh
TGM3_INPUT=play.txt TGM3_SNAPSHOT=1000,3000,9000 ../native/build/tgm3-native  gameA
TGM3_INPUT=play.txt TGM3_SNAPSHOT=1000,3000,9000 build/native/tgm3-recomp      gameB
cmp gameA-run/tgm3-frame009000.bmp gameB-run/tgm3-frame009000.bmp
```

Use separate copies of the game folder (they write `TGM3/sramdata`) with the same saves.
Compare at the normal 60 fps. With `TGM3_FPS=0` the BGM loader thread races the game loop,
so the game can start a frame or two apart and deal different pieces, even between two
correct builds. `TGM3_MEMDUMP` writes the game's globals at given frames for finding where
two runs diverge.

Status: the i386 and x86-64 builds match the native loader byte-for-byte on every snapshot
through 9000 frames of scripted attract mode and gameplay. The sources have no x86-specific
code, but nothing has been built or run on a non-x86 host yet.

## Limits

- Code reached only through a pointer the analysis didn't find stops with "not a known
  function" and the address. Add it as an entry in `analyze.py`.
- SEH dispatch isn't implemented (same as the native loader). The game doesn't raise
  exceptions in normal play.
- `sin`/`cos`/`tan`/`atan` come from the host libm, which can differ from x87 in the last bit
  on some hosts.
