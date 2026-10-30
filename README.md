# Midtown Madness 3 — Static Recompilation

![Gameplay](https://github.com/user-attachments/assets/22b2e26e-8ae0-4eef-b0b2-8764bb9408a9)

A native Windows (x64) port of the original-Xbox *Midtown Madness 3* (Digital Illusions / Microsoft, 2003).
Every x86 function in the retail `default.xbe` is translated to C, compiled with MSVC and linked against a
replacement Xbox kernel, audio, input and GPU runtime; the NV2A command stream is rendered with Direct3D 11.
No emulator, interpreter or JIT.

Built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) v0.15.0 (MIT, git submodule) plus
`patches/xboxrecomp.patch`.

> **No game content is in this repository.** You need your own copy of the game. Everything derived from it
> (generated C, builds, dumps) is git-ignored.

**Status: about 80 % done.** Intro movies, menus and full races (Paris, Washington) with audio, gamepad and
rumble, ~40–50 fps at 2× internal resolution, resizable 4:3 window.

## Quick start

1. Download `mm3recomp-<version>-windows.zip` from [Releases](https://github.com/xbox-games-recomp/mm3recomp/releases)
   and unzip it to a short path such as `C:\Games`.
2. Run `mm3_launcher.exe`, choose your disc image or extracted disc folder, press **Run setup**, then **Play**.

Setup installs the build tools with winget if needed and builds the game (about 17 minutes, ~5 GB). The
launcher's [player guide](https://github.com/xbox-games-recomp/xbox-recomp-launcher#readme) covers each step and
troubleshooting.

## Game version

The overrides name guest addresses, so only this executable works:

| | |
|---|---|
| Title ID | `0x4D53002A` (XBE 2003-05-08, XDK 5233) |
| `default.xbe` SHA-1 | `e63b03dd956544d3838bead8be5622da6cd189e1` |

Extract the whole disc (including `Data/Shared/WMV`; the game hangs without the intro movies) into
`game_files\`, e.g. `extract-xiso -x "Midtown Madness 3.iso" -d game_files`.

## Building from source

Needs Windows 10/11, Visual Studio 2022 (C++ workload), CMake 3.20+, Python 3.10+ with `pip install capstone`,
and a Direct3D 11 GPU.

```bash
git clone --recursive https://github.com/xbox-games-recomp/mm3recomp.git
```

```powershell
& .\scripts\regen_once.ps1             # patch toolkit, lift x86 to C, build (15–20 min, prints BUILD_EXIT=0)
cmake --build build --config Release   # code-only rebuild (~1 min)
```

The output is `build\Release\mm3_recomp.exe`; `mm3_launcher.exe` is copied to the repository root.

## Running

`mm3_launcher.exe` sets resolution, FXAA, vsync, keyboard, rumble and logging, saves them to
`launcher.user.ini` and logs to `run.log`; `--play` skips the window. The required environment variables are
in `launcher.ini`. Useful extras for the *Extra environment* box:

| Variable | Effect |
|---|---|
| `RECOMP_D3D11_RES=<n>` | Internal resolution n × 640×480 (default 2, max 6), averaged down to the window |
| `RECOMP_D3D11_SCALE=<f>` | Initial window size f × 640×480 (default 1.5) |
| `RECOMP_D3D11_STRETCH=1` | Fill the window instead of keeping 4:3 |
| `RECOMP_D3D11_HITCH=<ms>` | Log every frame slower than this |

**Controls:** the first XInput pad maps 1:1 to the Xbox pad. Keyboard: WASD steer, IJKL right stick, arrows
D-pad, Z/X/C/V = A/B/X/Y, Q/E = White/Black, 1/3 = brake/gas, Enter = Start, Backspace = Back.

## How it works

```
default.xbe ─► disasm ─► func_id ─► abi_analysis ─► recomp ─► patch_gen.py ─► MSVC ─► mm3_recomp.exe
               (capstone + seeds in config/seed_functions.json)      (x86 → C)         + xboxrecomp runtime
```

- **xboxrecomp** supplies the Xbox kernel, an emulated MCPX APU (→ XAudio2), XInput and the NV2A pushbuffer
  executor.
- **`src/recomp_manual.c`** replaces guest functions that depend on hardware (GPU fence, video overlay, XNet,
  XAPI gamepad calls).
- **`src/d3d11_backend.c`** translates NV2A vertex programs to HLSL, runs the register combiners in one
  data-driven pixel shader, and handles render-to-texture, w-buffering, supersampling and Bink overlays.

`NOTES.md` is the engineering log (every bug, cause and fix, plus debug recipes). Read it before changing
anything.

## Open issues

1. Short hitches when a race loads and on the results screen.
2. The colour-grade pass is missing in about 1 frame in 300.
3. 44 recompiled functions read flags set by their caller; one was patched, the rest are unchecked.
4. No MSAA (supersampling instead); no Xbox Live / System Link.

## Legal

An independent, non-commercial preservation project, not affiliated with Microsoft, Xbox Game Studios,
Digital Illusions CE or Volkswagen. *Midtown Madness* is the property of its owners. Do not open issues or pull
requests containing game files or generated code. The toolkit in `xboxrecomp/` is MIT-licensed.
