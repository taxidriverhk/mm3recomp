# Midtown Madness 3 — Static Recompilation

![Gameplay](https://github.com/user-attachments/assets/6800a589-887e-4c1e-9b14-660017ff122c)

A native Windows (x64) port of the original-Xbox *Midtown Madness 3* (Digital Illusions / Microsoft, 2003),
produced by **static recompilation**: every x86 function in the retail `default.xbe` is translated to C,
compiled with MSVC and linked against a replacement Xbox kernel, audio, input and GPU runtime. No emulator,
no interpreter, no JIT — the game's own code runs directly on your CPU, and its NV2A GPU command stream is
rendered with Direct3D 11.

Built on the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit (MIT), pulled in as a git
submodule at v0.15.0 plus a patch of MM3-driven changes — see [Toolkit changes](#toolkit-changes).

> **No game content is in this repository.** You need your own legally obtained copy of the game. The
> generated C, the build output and every runtime dump are derived from the game and are git-ignored.

**Status: roughly 80 % done — boots, plays the intro movies, loading screens, menus, and full races with audio,
a gamepad (with rumble), up to 6× internal resolution and a resizable 4:3 window.**
See [Status](#status) for what works and what doesn't.

---

## Contents

- [Requirements](#requirements)
- [First-time setup with the launcher](#first-time-setup-with-the-launcher)
- [Getting the game files](#getting-the-game-files)
- [Building](#building)
- [Running](#running)
- [Controls](#controls)
- [How the recompilation works](#how-the-recompilation-works)
- [Status](#status)
- [Repository layout](#repository-layout)
- [Debugging](#debugging)
- [Legal](#legal)

---

## Requirements

| | |
|---|---|
| OS | Windows 10/11, x64 |
| Compiler | Visual Studio 2022 (MSVC 19.40+) with the *Desktop development with C++* workload and a Windows SDK |
| Build tools | CMake 3.20+ (tested with 3.30) |
| Python | 3.10+ with [capstone](https://www.capstone-engine.org/): `pip install capstone` |
| GPU | Any Direct3D 11 (feature level 11_0) GPU |
| Controller | An XInput gamepad (Xbox 360 / One / Series controller) is strongly recommended; the keyboard works too |
| Game | A **retail Midtown Madness 3 Xbox disc dump** that you own (see below) |
| Disk | ~5 GB free (game data + ~100 MB generated C + build tree) |

## Getting the source

Clone with the toolkit submodule:

```bash
git clone --recursive <this repo's URL>
```

(Already cloned without it? Run `git submodule update --init`.)

## First-time setup with the launcher

The launcher can do everything in [Getting the game files](#getting-the-game-files) and
[Building](#building) for you. Install the [requirements](#requirements) and clone the repository, then:

1. Get the launcher: download `recomp_launcher.exe` from the
   [recomp-launcher releases](https://github.com/taxidriverhk/recomp-launcher/releases) into the repository
   root, or build it from the submodule in a few seconds:

   ```powershell
   cmake -S launcher -B launcher/build
   cmake --build launcher/build --config Release
   ```

   (The exe is `launcheruild\Releaseecomp_launcher.exe`.)
2. Run it. It opens on the **Setup** page.
3. Choose your disc image (*Disc image…*, an XISO or a full dump) or the folder of an already extracted disc
   (*Folder…*, linked as `game_files\` with a directory junction), and press **Run setup**.

It checks for Git, Python with capstone, CMake and Visual Studio. It then extracts the disc into
`game_files\` and checks `default.xbe` against the SHA-1 below. Finally it fetches the toolkit submodule and
runs `regen_once.ps1`. Its output shows in the window and goes to `setup.log`. When it finishes, press
**Play**. The build has also put `mm3_launcher.exe` in the repository root; use that one from now on.

## Getting the game files

The recompiler and the overrides are tied to one specific executable. Check yours:

| | |
|---|---|
| Title ID | `0x4D53002A` |
| XBE build date | 2003-05-08, XDK 5233 |
| `default.xbe` SHA-1 | `e63b03dd956544d3838bead8be5622da6cd189e1` |

Hand-written overrides in `src/recomp_manual.c` name guest addresses (e.g. `XInputGetState` at
`0x361E0F`). A different revision of the XBE will not work without redoing them.

1. Dump the disc from **your own** console/disc into an ISO (XISO).
2. Extract it with [extract-xiso](https://github.com/XboxDev/extract-xiso) or
   [xdvdfs](https://github.com/antangelo/xdvdfs) into `game_files/` at the repository root:

   ```bash
   extract-xiso -x "Midtown Madness 3.iso" -d game_files
   ```

   You should end up with `game_files/default.xbe` and `game_files/Data/...`.
   Keep **everything**, including `Data/Shared/WMV` — the game loops on a pure-virtual-call error if the
   intro movies are missing.

## Building

The commands below run from the repository root, wherever you cloned it. `python` (with capstone) must be
on `PATH`. CMake comes from `PATH`, or from its default install folder if it isn't there. Set `$env:PYTHON`
or `$env:CMAKE` to use a specific executable.

### Full regenerate + build (first time, or after changing seeds/overrides)

In PowerShell:

```powershell
& .\regen_once.ps1
```

This first applies `patches\xboxrecomp.patch` to the submodule (`apply_patches.ps1`; it skips
the step if the patch is already in). Then it runs the whole pipeline (below) and
CMake. It takes **about 15–20 minutes**: disassembly and recompilation of ~4 MB of x86, then MSVC
compiling ~2.6 million lines of generated C. It prints `BUILD_EXIT=0` at the end. A bad disassembly still
ends in `BUILD_EXIT=0`, so check that `xboxrecomp\tools\disasm\output\functions.json` got a fresh
timestamp. If you move the repository, the next run notices that `build\` was configured from the old
path and reconfigures from scratch.

### Code-only rebuild

After editing `main.c`, `recomp_manual.c`, `d3d11_backend.c` or toolkit sources (no regeneration needed,
but it needs one full regenerate first):

```powershell
cmake --build build --config Release
```

About a minute. The output is `build\Release\mm3_recomp.exe`.

## Running

Plug in the gamepad first, then double-click `mm3_launcher.exe` in the repository root (the build puts it
there and next to the game). It is a settings window — resolution, FXAA, vsync, keyboard, rumble, logging —
that starts the game with the environment variables below and saves your choices to `launcher.user.ini`.
The options and fixed variables are in `launcher.ini`; the launcher itself is the `launcher/` submodule
([recomp-launcher](https://github.com/taxidriverhk/recomp-launcher)). `mm3_launcher.exe --play` starts the
game straight away with the saved settings.

The launcher starts the exe from the repository root and writes its output to `run.log`.
A window opens; its title shows FPS and draw count.
Expect about two minutes of intro movies (press Start/Enter to skip ahead), then the title screen.
The first race compiles its vertex shaders in the background; they are cached in `shader_cache\`
(git-ignored), so later runs start faster.

The required settings, set by `launcher.ini` (the last two are options, on by default):

| Variable | Why |
|---|---|
| `RECOMP_D3D11=1` | Render on the GPU through `src/d3d11_backend.c` (without it: the toolkit's CPU rasteriser, ~1 fps) |
| `RECOMP_PB_EXEC=1` | Execute the NV2A pushbuffer (implied by `RECOMP_D3D11`) |
| `RECOMP_AC97_READY=1` | Report the AC'97 codec ready and start the emulated APU, so `DirectSoundCreate` succeeds |
| `RECOMP_HEAP_RECLAIM=1` | Honour `MmFreeContiguousMemory`; otherwise the 64 MB arena fills while a city loads |
| `RECOMP_DISPATCH_GATE=1` | One thread at DISPATCH_LEVEL at a time, as on the console (DirectSound DPC race otherwise) |
| `RECOMP_KEYBOARD=1` | Keyboard → virtual pad (a real pad is merged in either way) |
| `RECOMP_WATCHDOG_SECS=900` | Print a state dump if the game stops making progress |

Useful optional knobs (the launcher has options for resolution, FXAA, vsync, hitch log and rumble; put the
others in its *Extra environment* box):

| Variable | Effect |
|---|---|
| `RECOMP_D3D11_RES=<n>` | Internal resolution: n × 640×480 (default 2 = 1280×960, max 6). Above the window size it is averaged down, which is supersampling anti-aliasing; 1 = the console's own resolution |
| `RECOMP_D3D11_FXAA=1` | FXAA pass over the final picture (cheap; softens HUD text slightly) |
| `RECOMP_D3D11_SCALE=<f>` | Initial window size, f × 640×480 (default 1.5). The window can be resized or maximised; the picture keeps 4:3 |
| `RECOMP_D3D11_STRETCH=1` | Fill the window instead of keeping 4:3 |
| `RECOMP_D3D11_VSYNC=0` | No vsync |
| `RECOMP_D3D11_HITCH=<ms>` | Log a `[HITCH]` line for every frame slower than this (the launcher's default is 60) |
| `RECOMP_RUMBLE=0` | Don't forward rumble to the controller |

## Controls

Player 1 is the first XInput controller; it maps 1:1 onto the Xbox controller (Duke/S) the game expects.
The keyboard works at the same time:

| Xbox | Keyboard |
|---|---|
| Left stick (steer) | W / A / S / D |
| Right stick | I / J / K / L |
| D-pad | Arrow keys |
| A / B / X / Y | Z / X / C / V |
| White / Black | Q / E |
| Left / Right trigger (brake / gas) | 1 / 3 |
| Start | Enter |
| Back | Backspace |

Rumble: the game's `XInputSetState` calls are forwarded to the first controller (left = low-frequency motor,
right = high-frequency). `RECOMP_RUMBLE=0` turns it off.

---

## How the recompilation works

```
 default.xbe ──► xbe_parser ──► disasm ──► func_id ──► abi_analysis ──► recomp ──► patch_gen.py ──► MSVC
                  (sections,     (recursive  (CRT/XDK   (calling         (x86 → C,    (text fixes      │
                   imports)       descent +   library    conventions,     250 fns       to the          ▼
                                  seeds)      matching)  stack frames)    per file)     generated C)  mm3_recomp.exe
                                                                                                        │
                       xboxrecomp runtime: kernel · memory · threads · file I/O · DirectSound/APU ·     │ links
                       XInput · NV2A pushbuffer executor  +  mm3recomp: overrides · D3D11 back end ◄────┘
```

### 1. Lifting x86 to C (offline)

The Python pipeline in `xboxrecomp/tools/` reads the XBE and emits C:

- **`disasm`** does recursive-descent disassembly with capstone. Statically it only finds code reachable through
  direct calls, so it is fed **seeds** — `config/seed_functions.json`, ~9,000 function starts
  collected from runtime logs of indirect calls (vtables, callbacks, jump tables) that hit unknown targets.
  `--extra-sections DSOUND,XACTENG,WMADEC,XGRPH` also sweeps those XDK library sections, which matters:
  an unswept library section shows up as a burst of *"ICALL Failed to resolve"* and broken behaviour
  (this was the cause of both the DirectSound crash and every pixel shader failing to assemble).
- **`func_id`** recognises CRT/XDK library functions; **`abi_analysis`** works out calling conventions and frames.
- **`recomp`** turns each function into a C function. The guest's registers are C variables, guest memory is
  one flat 32-bit address space accessed through `MEM8/16/32` macros, flags are computed lazily, and indirect
  calls go through a binary-search dispatch table of all known function addresses. Output lands in
  `src/recomp/gen/` (~144 files, ~2.6 M lines). Never edit it — it's regenerated.
- **`patch_gen.py`** applies a few anchored text patches to the generated C (e.g. a divide-by-zero guard in
  DirectSound, the pedestrian LOD fix). An anchor that stops matching is reported.

`regen.ps1` is the whole sequence. `reseed.py` / `tools.seed_from_log` turn unresolved-call logs into new seeds.

### 2. The runtime (xboxrecomp)

At run time the recompiled code thinks it is on an Xbox. The toolkit provides:

- a reimplementation of the **Xbox kernel** exports the game imports (memory, threads, events, DPCs, IRQL,
  file I/O with path translation to `game_files\`, EEPROM, …);
- an emulated **MCPX APU** (voice processor) under the game's own statically linked DirectSound/XACT/WMA code,
  mixed to XAudio2;
- the **input** layer (XInput + keyboard) that feeds the Xbox `XINPUT_STATE` the game polls;
- the **NV2A pushbuffer executor**: the game's own Direct3D 8 (statically linked XDK D3D) writes GPU
  commands into a ring buffer exactly as on hardware; the executor parses those methods and tracks GPU state
  (vertex programs, register combiners, textures, surfaces) and hands draws to a back end.

### 3. The game layer (this repository)

- **`src/main.c`** — boot: maps memory, loads the XBE image, starts the APU, kernel and back end, then jumps to the
  recompiled entry point.
- **`src/recomp_manual.c`** — hand-written replacements for individual guest functions, where the original depends
  on real hardware behaviour. Examples: the D3D GPU fence wait (made honest against the executor's semaphore),
  the video-overlay stop used by Bink movies, XNet (forced "no network"), and the XAPI gamepad calls
  (`XGetDevices`, `XInputOpen`, `XInputGetState`, `XInputSetState` for rumble, …) which are answered directly
  instead of emulating USB.
- **`src/d3d11_backend.c`** — the GPU renderer:
  - NV2A **vertex programs are translated to HLSL** and run on the GPU (this took the frame rate from ~8 to ~40–50 fps);
  - the **register combiners** (8 general stages + final combiner) are a single data-driven HLSL pixel shader, so
    there are no shader permutations; texture-stage modes including cube maps and the dot-product/`texm3x3` modes;
  - render targets are D3D11 textures keyed by guest address, so render-to-texture and the game's post chain
    (colour grade, glare, depth-based haze) work; depth buffers can be read back as textures the way the game does;
  - textures (swizzled, DXT, palettised…) are decoded on first use and re-decoded when their guest bytes change;
  - MM3 **w-buffers** its scene (linear depth); the back end writes w as depth for those draws, which the
    distance-haze pass needs;
  - CPU writes into a render target (the loading-screen TGA is decoded straight into the back buffer) are
    picked up from guest memory;
  - everything renders at an **internal resolution** of n × 640×480 and is averaged down at present
    (supersampling), with optional FXAA, letterboxed 4:3 in a resizable window;
  - vertex shaders compile on a worker thread (the CPU interpreter covers the gap) and are cached on disk;
  - Bink movies drawn through the NV2A video overlay are composited at present time.

`NOTES.md` is the detailed engineering log: every bug found, why, and how it was fixed — read it before
changing anything.

### Toolkit changes

`xboxrecomp/` is a git submodule pinned to upstream **v0.15.0** (`6376eb8`). The changes made for this port
live in `patches/xboxrecomp.patch` (9 files) and are applied on top by `apply_patches.ps1`; the
submodule is set to `ignore = dirty`, so the patched files don't show up as changes in this repo.
After changing toolkit files, regenerate the patch with
`git -C xboxrecomp diff > patches/xboxrecomp.patch`. The patch mainly covers:
`nv2a_backend.h` / `nv2a_pb_exec.c` (`draw_ext`: full combiner, four texture stages, fog, clip state,
`SET_CONTROL0` for the w-buffer and extra vertex outputs handed to back ends), the pushbuffer ring-wrap fix in
`nv2a_pb_scan.c`, the inline-array vertex sizing fix, vertex-program interpreter defaults/`ARL` rounding in
`nv2a_vsh_interp.c`, the executor's stats report made opt-in (`xbox_memory_layout.c`; it stalled the GPU
thread), and APU / USB-gamepad fixes.

---

## Status

**Works**

- Boot, the three intro movies (with sound), title screen, profile, menus, car select
- Loading screens with their artwork (`Startup.tga` at boot, per-city images before races) and the meter
- Full races in **Paris** and **Washington** with correct colours, HUD, minimap, pedestrians, traffic, water
  and the distance haze, ~40–50 fps on the GPU at 2× internal resolution
- Audio: music, effects and movie sound via the emulated APU → XAudio2
- Gamepad and keyboard input; rumble forwarded to the controller
- Supersampling (`RECOMP_D3D11_RES`), optional FXAA, resizable window that keeps 4:3
- 10+ minute sessions without a crash

**Open issues**

1. **Short hitches when a race loads** (~3.8 s, textures decoded at once) and on the results screen; the
   first stream-in of new shaders costs a few ~50 ms frames on the first run (cached afterwards).
2. The colour-grade pass is missing in about 1 frame in 300.
3. 44 recompiled functions read CPU flags set by their caller (split chunks); one of them broke the loading
   screen and was patched, the rest are unchecked (`NOTES.md` 37). A lifter fix would cover them all.
4. Rumble is forwarded but not yet confirmed by feel in a race.
5. No MSAA (deliberately; supersampling instead, see `NOTES.md` 36).
6. Xbox Live / System Link: XNet is stubbed out, no networking.

## Repository layout

| Path | What it is |
|---|---|
| `CMakeLists.txt` | Builds `mm3_recomp.exe` from the generated C + game sources + toolkit |
| `src/main.c` | Boot / entry |
| `src/recomp_manual.c` | Hand-written guest-function overrides |
| `src/d3d11_backend.c` | Direct3D 11 renderer for the NV2A executor |
| `src/dstrace.inc` | Generated DirectSound/XACT call-trace wrappers (`RECOMP_DSTRACE=1`) |
| `src/sampler.c` | Game-thread sampling profiler (`RECOMP_SAMPLE=1`) |
| `config/seed_functions.json` | Disassembler seeds |
| `regen.ps1`, `regen_once.ps1` | Full pipeline + build |
| `patches/xboxrecomp.patch`, `apply_patches.ps1` | This port's changes to the toolkit, and the script that applies them |
| `patch_gen.py` | Post-generation patches |
| `reseed.py`, `loop3.ps1` | Seed discovery from runtime logs |
| `launcher.ini`, `launcher/` | Launcher settings, and the launcher itself (submodule); builds `mm3_launcher.exe` |
| `verify_ps.ps1`, `runtest.ps1`, `runlive.ps1`, `drive_ps.ps1` | Scripted test runs (frame dumps, scripted input) |
| `src/recomp/gen/` | *(ignored)* generated C |
| `shader_cache/` | *(ignored)* compiled vertex shaders, built at run time |
| `xboxrecomp/` | The recompilation toolkit (MIT), git submodule |
| `NOTES.md` | Engineering log |
| `game_files/` | *(ignored)* your extracted disc |

## Debugging

A few starting points (many more in `NOTES.md` → "Debug recipes"):

- `RECOMP_STUB_LOG=1` logs each call to an address the disassembler never found. A run of these in one library
  section means that section needs `--extra-sections`; isolated ones need seeds.
- `RECOMP_WATCH=<va>` names the code that writes a guest address.
- `RECOMP_D3D11_DUMP=<prefix>` + `RECOMP_D3D11_DUMP_MS=<ms>` save presented frames as BMP;
  `RECOMP_D3D11_LOGFRAME=<a>-<b>` logs every draw of those frames (state, combiners, textures, vertex program hash).
- The exe has a PDB, so the crash handler and cdb/WinDbg map a host RIP back to `sub_XXXXXXXX`, i.e. the guest
  function. Exit codes: `-1073741819` access violation, `-1073741676` divide by zero, `-1073741571` stack overflow.
- `verify_ps.ps1 <tag>` runs a scripted race and dumps frames to `frames\<tag>` (the race starts ~4 min in).
  Scripted pad input works in menus but does not drive the car in a race.
- `RECOMP_D3D11_HITCH=<ms>` and `RECOMP_SAMPLE=1` find frame stalls; `RECOMP_PAD_TRACE=1` logs the XAPI pad
  calls (including rumble speeds).
- stderr goes through a pipe drained by a logger thread so logging never blocks the game; `RECOMP_LOG_SYNC=1`
  writes directly.

## Legal

This is an independent, non-commercial preservation and research project. It is not affiliated with or
endorsed by Microsoft, Xbox Game Studios, Digital Illusions CE (DICE) or Volkswagen.
*Midtown Madness* and all related assets are the property of their respective owners.

This repository contains **no game code, data or assets**: no XBE, no disc content, no generated C, no builds.
To use it you must own the game and extract the files from your own disc. Do not open issues or pull requests
containing game files, generated code or links to them.

The toolkit in `xboxrecomp/` is MIT-licensed (see `xboxrecomp/LICENSE`, `xboxrecomp/NOTICE` and
`xboxrecomp/LICENSES/` for third-party components).
