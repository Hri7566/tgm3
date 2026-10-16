# TGM3 on Linux

The "PC port" in `../Tetris The Grand Master 3  (PC)` isn't a port. It's the Taito Type X
arcade `game.exe` (32-bit, OpenGL + DirectSound) plus a loader that fakes the cabinet's
JVS I/O board on COM2. This directory rebuilds that loader for Linux.

| Windows piece | What it does | Linux replacement |
|---|---|---|
| `typex_loader.exe` + `detoured.dll` | `CreateProcessA(game.exe, CREATE_SUSPENDED)`, Detours adds `typex_io.dll` to the import table, `ResumeThread` | `build/tgm3inject.exe` (remote `LoadLibraryA` before resume) |
| `typex_io.dll` hooks | IAT-patches `CreateFileA("COM2")`, `ReadFile`/`WriteFile`, `*Comm*` → fake handle | `build/jvsbridge.dll` (same hooks, bytes go to 127.0.0.1) |
| `typex_io.dll` JVS board | Answers reset/address/identify/switches/coins as "NAMCO I/O PCB-1000" | `build/jvsd` (native) |
| DirectInput8 + `typex_bindings.bin` | Keyboard/pad → switch bits | evdev + `bindings.conf` |
| `game.exe` 2-byte patch at file offset 0x6A0AC | Save root `D:\` → `.\` (so `TGM3\sramdata` lives in the game folder) | unchanged; `tgm3.sh` runs from the game dir |

The JVS "sense" line is the one part that can't go through a socket or a pty. The game reads
it as CTS (`GetCommModemStatus`, bit 0x10) and expects it low after reset and high once
addressed. The bridge derives it from the reset (F0) and set-address (F1) packets the game sends.

## Build & run

Needs `gcc`, `libevdev`, Wine with `winegcc` (32-bit PE support), and membership of the `input` group.

```sh
make            # jvsd, jvsbridge.dll, tgm3inject.exe
make test       # JVS protocol tests
./tgm3.sh       # TGM3_DESKTOP=1280x960 ./tgm3.sh  for a Wine virtual desktop
```

Default keys: arrows, Z/X/C/V = A/B/C/D, 1 = start, 5 = coin, 9 = test, Esc = quit. A gamepad maps to P1.
Edit `bindings.conf` to change them. `JVSD_VERBOSE=1` logs JVS commands, and `jvsd -v -v` dumps raw bytes.
The bridge writes `jvsbridge.log` in the game directory.

jvsd reads every bound keyboard and pad whether or not the game has focus, the same as the
original's DirectInput setup. Quitting jvsd (Esc) closes the socket, and the bridge then exits the game.
