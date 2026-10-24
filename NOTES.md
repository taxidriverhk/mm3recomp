# Midtown Madness 3 – xboxrecomp bring-up notes

Static recompilation of the original-Xbox *Midtown Madness 3* (title 0x4D53002A,
retail XBE built 2003-05-08, XDK 5233) with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).
Nothing from the game is distributed; `game_files\` is an unpacked copy of the
user's own disc image.

## Layout

| Path | What it is |
|---|---|
| `xboxrecomp\` | the toolkit (cloned; since modified additively, see 20, 21(e), 3(a)) |
| `mm3recomp\` | the game project (CMake, generated C, overrides, scripts) |
| `game_files\` | unpacked ISO (`default.xbe`, `Data\...`). `mm3recomp\game_files` is a junction to it |
| `mm3recomp\config\seed_functions.json` | function-start seeds fed to `tools.disasm` (~9,000) |
| `mm3recomp\src\recomp_manual.c` | hand-written overrides (replace, not wrap; see below) |
| `mm3recomp\src\recomp\gen\` | generated C (about 2.2M lines, ~130 files). Regenerated, do not edit |
| `mm3recomp\run_mm3.bat` | runs the exe with the debug env vars set |

Build tooling used: VS 2022 Community (MSVC 19.40), CMake 3.30, Python 3.10 + capstone.
The project path is long, so builds were done through a `subst M:` drive that maps to
this folder (`subst M: "D:\Documents\Xbox Recompilation"`; not persistent across reboot).
CMake's cache has `M:/...` paths in it. From `D:\...` directly, reconfigure first.

## Pipeline (what the scripts do)

`mm3recomp\regen.ps1`: disasm (with seeds) -> func_id -> abi_analysis -> recompile
(`--all --split 250 --exclude-manual src\recomp_manual.c`) -> cmake build.
`mm3recomp\loop3.ps1`: N rounds of regen, 60 s boot (AC97 ready), `tools.seed_from_log`
on the log, reseed from unresolved stubs (`reseed.py`). Per-round logs: `auto_r1..3.err`.

## Findings (in the order they were hit)

1. **Unresolved stubs** (call targets the disassembler never made functions) were the
   dominant failure: 779 -> ~6 after iterative seeding. Seed *every* unresolved call target
   (`reseed.py`), including mid-function entries.
2. **D3D fence wait** (`sub_00344640`) spins forever (nothing advances the GPU semaphore).
   Overridden: point `dev+0x30` at `dev+0x2C`, `ret 8`. Device pointer is `MEM32(0x351F48)`.
3. **DirectSoundCreate** fails unless the AC'97 codec reads ready: run with `RECOMP_AC97_READY=1`.
4. **Two-level switch** in `sub_00337B07` (byte table 0x3387AF -> jump table 0x3386FF) was not
   followed by the disassembler; skipped cases left `esi` clobbered. Seeded the targets of all
   19 `RECOMP_ITAIL` tables (note: BINK* section targets need the BINK sections allowed).
5. **XPP seeds** `0x359480,0x359ABB,0x359CC8,0x35A2A7` caused a hang before fix 4; fine after.
6. **Native stack**: guest threads use the exe default; linked with `/STACK:67108864`.
7. **XNet** receive callback `sub_002B2314` divides by an uninitialised NIC field. Overridden
   as a no-op (`ret 16`).
8. **Intro movies must exist.** With `Data\Shared\WMV` missing the game hits
   `R6025 pure virtual function call` in a loop. Keep the movies; Bink has to run.

## Debug recipes that worked

* `RECOMP_WATCHDOG_SECS=N`, `RECOMP_STUB_LOG=1`, `RECOMP_WATCH=<va>` (names the writer),
  `RECOMP_NV2A_TRACE=1`, `RECOMP_AC97_READY=1`.
* The VEH handler only reports access violations. Other exits: `-1073741819` AV,
  `-1073741676` divide by zero, `-1073741571` stack overflow. Read the exit code with
  `$p=Start-Process ... -PassThru; $null=$p.Handle; $p.WaitForExit(); $p.ExitCode`.
* cdb (Windows SDK) with a command file: `sxi av`, `sxi sse`, `sxe -c "k 12;q" dz`, `g`
  gives the generated function + source line of a divide fault. Attach with `-p <pid> -pv`
  to sample a running thread's stack. The PDB maps host RIPs to `sub_XXXXXXXX`.
* A debug `fprintf` can be dropped into a generated `.c` for a quick rebuild; the next regen
  overwrites it.

9. **Video overlay stop** (`sub_003430A0`, PVIDEO_BUFFER/STOP at NV2A `+0x8700/+0x8704`) waits for
   hardware to clear a busy bit; Bink plays movies through the YUY2 overlay. Overridden to do the
   same writes and clear the bit itself. This is what lets `dice.bik` -> `msgs.bik` -> `intro.bik` play.
10. **XNet status wrapper** (`sub_002AF497`) overridden to fail, so the game's network check takes its
    "no network" exit instead of polling an async op forever (loading screen hang).
11. **DSOUND divide** (`sub_0027CC4F`, byte `[ecx+0x64]` is 0 for some buffers because a flag test
    in `sub_0027C9DF` takes an error branch, `DSERR_INVALIDCALL`). Guarded in `patch_gen.py`
    (`esi==0 -> 1`). The real cause (DirectSound buffer state vs the emulated APU) is NOT fixed.
12. Seeding `0x279724` (DSOUND) makes buffer creation run and fail, and Bink then releases a garbage
    pointer when a movie ends: removed. The pinned seed file is `config\seed_functions.pinned.json`.
13. **`xbox_OhciInit()` is never called by the toolkit**; `main.c` calls it now (needs `RECOMP_USB=1`).

14. **Gamepad via XAPI overrides, not USB.** The real XPP USB stack never finished enumerating on the
    emulated OHCI (and its behaviour changed with every seed). `recomp_manual.c` now replaces the XAPI
    calls the game makes (addresses are specific to this XBE): `XGetDevices 0x35A81D` (gamepad type
    `0x3592B8` -> port 0 connected), `XInputOpen 0x361BCF`, `XInputClose 0x361C25`,
    `XInputGetCapabilities 0x361C31`, `XInputGetState 0x361E0F` (fills the Xbox XINPUT_STATE from
    `usb_gamepad_report`, so a host pad, `RECOMP_KEYBOARD`, `RECOMP_PAD_PRESS`, `RECOMP_PAD_SCRIPT`
    all work), `XInputSetState 0x361E7B`. (I first had GetState/GetCapabilities swapped; the per-frame
    code `sub_001A6278` passes `esi+0x10` and reads the gamepad at `esi+0x14`.)
    `RECOMP_PAD_TRACE=1` prints running call counts of these. USB (`RECOMP_USB`) is not needed.
    Other XPP entry points seen: `XInitDevices 0x35A818` (tail-jumps to `0x3599C5`), `XGetDeviceChanges
    0x35A83F` (memory-unit type `0x359238`), a voice open `0x361666`.

## Pipeline additions since the first notes

* `mm3recomp\patch_gen.py`: text patches applied to generated C after the recompile (run by regen.ps1).
* `mm3recomp\regen_once.ps1`: one regenerate+build, no seeding. Prefer this now.
* `loop3.ps1` reseeds automatically and changes behaviour round to round. Avoid it unless a
  new stub shows up. `reseed.py` / `tools.seed_from_log` are the manual tools.
* `snapshots\mm3_recomp_attract.exe(.pdb)`: a known-good build that reaches attract mode.
* Frame dumps: `RECOMP_PB_EXEC=1 RECOMP_FB_DUMP=<prefix> RECOMP_PB_REPORT_MS=4000` write 640x480
  BMPs. Convert with a few lines of Python (BMP -> PNG) to view.
* Scripted/synthetic input: `RECOMP_PAD_PRESS=0x10` (Start pulse), `RECOMP_PAD_SCRIPT=ms:button,...`,
  `RECOMP_PAD_LIVE=<file>`; `RECOMP_INPUT_DIAG=1` prints the input chain once a second.

## Current state

* Boots, loads city data, plays the three intro movies (about two minutes), loads the frontend
  (`Startup.tga`, loading meter, `LocalCache08.bin`) and cycles attract-mode movies
  (`AttractMode2/0.bik`). `SetDisplayMode` is called 9-11 times. Unresolved stubs: 2-7.
* The CPU rasteriser draws real frames (the loading meter; a frontend background with one tile missing).
* NOT done: no window (nothing presents the framebuffer, `RECOMP_FB_WINDOW` untested), the game never
  reads a pad (USB enumeration stops after 4 control setups, before SET_CONFIGURATION / the XID
  request; OHCI trace: `RECOMP_USB_TRACE=1`), audio is broken in the DirectSound layer, the intro
  movies are not shown (overlay not emulated).

## State after the pad overrides

* The game polls the pad every frame (`XInputGetState`) and no longer goes into attract mode within 5 minutes.
* It shows a loading screen ("LOADING" + meter) for ~35 s and then renders black frames, but the GPU
  executor is busy: ~14 fps (3,900 flips in 5 min), ~66,000 draws, ~88,000 triangles, 3M unhandled
  NV2A methods (167 distinct), "brightest pixel written 0x08FFFDCA" (alpha 8: a fade?), vertex
  x range -865..1059114 (suspicious transforms). Snapshot: `snapshots\mm3_recomp_pad.exe`,
  seeds `config\seed_functions.pad.json`.

## World loading (latest)

15. **`RECOMP_HEAP_RECLAIM=1` is required.** The toolkit ignores `MmFreeContiguousMemory` unless it is set,
    so the 64 MB contiguous arena fills (467 allocs, 0 frees) and the game raises STATUS_NO_MEMORY
    (`0xC0000017`) while loading a city. With it: ~30 MB in use, frees happen. (In `run_mm3.bat`.)
16. More seeds that mattered (all runtime-observed indirect targets in gaps/after a ret):
    `0x1021B5`, `0x22C0E5` (game thunks), `0xC9859`, `0xC989D`, `0x686A9`, `0x137809`, `0x1377FC`,
    `0x12E750` (game), `0x9BB15` (CRT x87 log/pow fragment, uses the caller's frame), WMA decoder
    `0x309448 0x30950B 0x30958A 0x309788 0x3094C9 0x309506`. DSOUND targets (`0x27F1A6`, `0x2772A9`,
    `0x279724`, `0x27C51E`) are deliberately NOT seeded (seeding makes buffer creation run and fail).
17. The game now goes frontend -> profile (`u:\...\score`) -> audio banks (`Ambient.xwb`, `Jingles.xwb`) ->
    `Paris.xbc` / `Paris.pak` / `Paris.toc` -> `Envmap.dds` -> car models (`Microcar`, `Microcar_lod1`).
    The CPU rasteriser then draws a real 3D frame (white building facades with windows against a
    blue-grey sky; lighting/textures wrong, ~1 fps).
18. Crash sites seen after that (each deterministic for a build): `sub_001A4120` with a NULL scene object
    (`[ebx+0x8C]==0`), fixed/moved by seeding `0x12E750`; now `sub_001EC520+0xC9C` reading
    `0x89131DD2` with `edi=0x61756C2E` (".lua": script/string data used as a pointer).
    Executor notes: scan mode (default) is better than `RECOMP_PB_WALK=dma`, which loses sync here.

Snapshots: `snapshots\mm3_recomp_world.exe(.pdb)` with `config\seed_functions.world.json`.

## DirectSound crash fixed, D3D11 back end (2026-10-21)

19. **The `.lua` / null-vtable crashes were DirectSound, not the game.** `tools.disasm --text-only` never sweeps
    the XDK library sections, so DSOUND/XACTENG/WMADEC code was only reachable through seeds, and every
    vtable/callback target in them was an unresolved indirect call. The ICALL macro skips those with `eax=0`,
    which the XDK reads as "S_OK, out pointer written", so the game then used uninitialised pointers (the `.lua`
    string as a pointer, null vtables, `esp=0`). Earlier "seeding makes buffer creation fail" was partial seeding.
    Fix: `regen.ps1` now passes `--extra-sections DSOUND,XACTENG,WMADEC`, and `config\seed_functions.json` has
    94 more seeds (every `.rdata`/`.data` pointer into DSOUND/XACTENG: vtables) plus `0x279724 0x27C51E 0x27F1A6
    0x2772A9 0x357DF1`, `0x12D56D 0xC96F4 0x9B4E0 0xC96D1 0x3093D0 0x30943C`. No ICALL "Failed to resolve" remains and
    the game ran 10 min in the race scene without a fault. (The old file is `seed_functions.pre_dsound.json`; the
    current one is also copied to `seed_functions.dsound.json`. Snapshot of the good exe: `snapshots\mm3_recomp_dsound.exe`.)
    The generic lesson: a *run of* "Failed to resolve" ICALLs in a library section means that section needs
    `--extra-sections`, not one-by-one seeds.

20. **GPU back end: `src\d3d11_backend.c`**, enabled with `RECOMP_D3D11=1` (also turns `RECOMP_PB_EXEC` on).
    It registers through the toolkit's `nv2a_backend.h`. Because that interface only passed stage 0 + diffuse, the
    toolkit was extended (additively; `nv2a_backend.h`, `nv2a_pb_exec.c`): `Nv2aBackend.draw_ext` receives
    `Nv2aBatchExt` (combiner programme, four texture stages, fog colour, clip-plane mode) and `Nv2aVertexExt`
    (d0, d1, fog, four clip-space texcoord sets) -- everything the CPU rasteriser's `nv2a_rc_eval` path uses.
    The pixel shader is a data-driven HLSL port of `nv2a_rc_eval` (all 8 general stages + final combiner), so no
    shader permutations. Colour surfaces are D3D11 textures keyed by guest address (a texture whose offset equals a
    rendered surface samples it directly: render-to-texture/post), other textures are decoded once with
    `nv2a_backend_decode_texture` and re-decoded when a hash of their guest bytes changes. Blend, depth, stencil, cull,
    alpha test, colour mask, sampler wrap/filter are mapped. Result: ~8-9 fps (was ~1 fps); limit is now the CPU
    (vertex programs run in `nv2a_vsh_interp.c`, ~55k triangles per frame) and the 116-byte vertex copy.
    Knobs: `RECOMP_D3D11_SCALE`, `_VSYNC=0`, `_NOCULL`, `_CULLFLIP`, `_NODEPTH`, `_TRACE=1`, `_DUMP=<prefix>`
    (+`_DUMP_MS`), `_LOGFRAME=<a>-<b>` (every batch of those flips: state, combiner words, textures, first vertex),
    `_ONLY=<lo>-<hi>` (draw only those batches of each frame), `_PRESENT_FB=1`.
    Not done: cube maps (face 0 only, env-mapping looks wrong), bump/dot-product stage modes (read as zero, as in the
    CPU path), point/line primitives, mip chain from the game (mips are generated), anisotropy, the Bink/overlay video.
21. **The darkness was mostly the renderer, not the game** (found with the tools below). What a race frame looks like
    (`RECOMP_D3D11_LOGFRAME`, flip ~6300, ~620 batches): scene into `0x80084000` (depth `0x802DC000`), a few passes
    into `0x802DC000`/`0x80451000` (40x30 luminance)/`0x804F8000`, then more into `0x80084000`: a colour-grade quad that
    samples `0x80084000` *itself*, passes that sample the depth buffer as a 1280x480 `LIN_L16` texture, glare sprites,
    tree sprites, HUD. Four renderer bugs hid it, all fixed in `d3d11_backend.c`:
    (a) a depth buffer shared with a smaller surface (the 40x30 pass) was *recreated* on the size change, wiping the scene
    depth for everything after (now: depth buffers only grow); (b) the presented surface was "last big surface drawn"
    (wrong); now it is the executor's own flip surface (`nv2a_backend_flip_surface()`, `RECOMP_D3D11_PRESENT=0|1|2`);
    (c) a draw that samples the surface it draws to read a white dummy; it now reads a snapshot copy (`rt_snapshot`);
    (d) a texture whose address is a depth buffer got the unrelated colour surface; it is now a depth readback
    (`depth_view`: Z24 -> two L16 texels per pixel, the way the game reads it).
    Also: `nv2a_vsh_interp.c` defaulted unwritten `oD0.rgb` to 0; MM3's terrain programmes write only `oD0.a`, so
    terrain was black. Default is now white (`RECOMP_VSH_DIFFUSE0=1` restores 0). Not proven against hardware.
    After these the frame is a warm, hazy scene instead of black. Two more fixes then made the race render:
    (e) **inline-array vertex size** (toolkit, `draw_inline_array` in `nv2a_pb_exec.c`): 16-bit attributes were sized
    `4 x components`, so the HUD/particle program `5DEB9A41` (shorts + colour, 16 bytes) was read at stride 28 and
    every other vertex was garbage -- those were the blue/black "streak" triangles. Shorts are 2 bytes per component,
    packed 11:11:10 is 4. The HUD, the speedometer, the minimap and the menus (title, car select, pause, results) all
    render now. (f) an *opaque* full-screen pass that samples the depth-buffer address (`0x802DC000`, read as a
    1280x480 `LIN_L16` texture; it is also a colour target elsewhere) painted a depth image over the whole frame
    (that was the flat beige "panorama" ground). It is skipped by default (`RECOMP_D3D11_DEPTHREAD=1` draws it); the
    later grade/sky/sprite passes then give the intended tan road, cream facades with red shutters, and sky.
    The "pink facades" (`97FC3D33`) were just the pre-grade state (the grade quad multiplies geometry by (0.56,0.39,0.28));
    no fix needed.
    **The black / scrambled player car** (programme `F42AA52B`, plus the other car programmes) was three things:
    (g) unwritten `oFog` defaulted to 0; the title enables linear fog with bias 1 / scale 1 (factor = oFog.x) and the
    final combiner blends by the fog factor (`fin 130E0300`), so factor 0 = full black fog. Default is now `oFog.x = 1`
    (`RECOMP_VSH_FOG0=1` restores 0). (h) `ARL` floored `x + 0.001`; MM3 stores matrix/colour palette indices as bytes
    scaled by n/255 (`v3.y * 21`), so a byte of 12 gives 0.988 and floored to the neighbouring matrix: scrambled
    triangles. Now floor(x + 0.25), see 33 (`RECOMP_VSH_ARL_FLOOR=1` restores). (i) triangle strips handed to the back end alternated
    winding (odd triangles were not swapped), which only matters once the back end culls: front faces of the car's strips
    were culled. Odd strip triangles are now swapped in both executor paths. The car renders as a coherent, silver
    hatchback with tail lights, plate and exhaust. Env-map reflections (cube map) are still only face 0 of the 2D sample.
    Remaining: two red HUD quads near the car, thin dark slivers in some frames, and `RECOMP_PAD_PRESS` pauses the race
    (use `runlive.ps1`: Start pulses until a time, then `a` via `RECOMP_PAD_LIVE`).
    Tools added (all env vars): `RECOMP_VSH_TRACE=<n>` with `RECOMP_VSH_TRACE_PROG=<hash>` / `_ZERO=1`, `RECOMP_VSH_DUMP=1`
    (programme disassembly with a hash; `_DUMP_CONSTS=1` for constant loads), `RECOMP_VSH_WATCH=<c index>`,
    `RECOMP_PAD_PRESS_UNTIL_MS`; in the back end `RECOMP_D3D11_DBG=1..7` (1 d0, 2 tex0, 3 d1, 4-6 tex1-3, 7 d0.a),
    `_ONLYPROG`/`_SKIPPROG=<hash>`, `_SNAP=<flip>,<every>` + `_SNAPRANGE=<lo>-<hi>` (filmstrip of the presented surface;
    `every` 0 = dump each surface the title leaves), `_LOGFRAME` now also logs clears, depth range and the programme hash.
    Scripts: `runtest.ps1`, `dbgmodes.ps1`, `progmodes.ps1`, `ftrace.ps1` (one-frame method trace). A good build is
    `snapshots\mm3_recomp_d3d11.exe`.
23. **Keyboard / gamepad in the D3D11 window.** The back end's window forwards `WM_KEYDOWN/UP` into the toolkit's key
    table (`input/keyboard.c`) and `main.c` turns `RECOMP_KEYBOARD` on with the back end: arrows = D-pad, Enter = Start,
    Backspace = Back, Z/X/C/V = A/B/X/Y, Q/E = White/Black, 1/3 = triggers, W/A/S/D = left stick, I/J/K/L = right
    stick. A real XInput pad on port 0 is merged (xinput_device.c). Verified by posting Enter to the window
    (`keytest.ps1`: title -> profile select -> main menu). `RECOMP_PAD_PRESS` is no longer in `run_mm3.bat` (it
    pauses races).
24. **Audio, what is known.** Output works: the intro movie audio reaches XAudio2 (`RECOMP_AUDIO_WAV=<file>` dumps what
    the device gets: loud, even clipping, for the first ~150 s, then silent). After the movies nothing sounds: music
    (`frontend.wma`, `paris_brief.wma`) and effects never reach an APU voice. Evidence: `RECOMP_APU_VOICES=1`
    (`[APU-VSTAT]` per voice per second, `[APU-IRQ]`) shows only voices 64-67, four mono 31-sample silent placeholder
    voices (buffer `0x47C22E`, rms 0), and the movie voices. `RECOMP_DSTRACE=1` (`recomp_manual.c`, wrappers for
    ~2000 indirect-call targets in DSOUND/XACT/WMADEC, `[DST]` first calls + `[DST-COUNTS]` every 4000) shows: the
    WMA decoder runs (`0x309448` x5000, inner loops 500k), every DSOUND call returns S_OK, the service interrupt is
    delivered (`0x27B8B4`, claimed), the game calls the stream's GetStatus (`0x2772A9`) 5000 times (about 26/s,
    forever) and Process (`0x2772FA`) ~210 times, and `Pause` (`0x27C769`) with the same arguments each time. The
    stream voice is never given the decoded data. The likely cause is in DirectSound's stream state machine /
    packet completion (GetStatus reports "ready" only when its free-packet list is non-empty; `0x27BBEC`), not in the
    codec. Next: trace `0x27BF39`, `0x27C439`, `0x27C148` (Pause case 2) and the packet list at stream+0xB8.
    Diagnostics note: `RECOMP_APU_VOICES` itself overflowed a stack after ~150 s; `RECOMP_APU_TRACE` floods.
22. Verified: a 10 min run with `RECOMP_D3D11=1` (scene at ~8-9 fps, camera moving) with no fault and no ICALL
    failures; the only unresolved stub left is `0x00338B41` (D3D library). Still open: audio output (DirectSound/XACT now
    run against the emulated APU but nothing has been checked by ear), the scene's darkness and the blue streak
    triangles (see 21), 8-9 fps CPU-bound, no keyboard path into the D3D11 window (`RECOMP_KEYBOARD` was wired to
    the toolkit's own window), the intro movies are not shown.

25. **Audio re-checked (supersedes the "nothing sounds" part of 24).** A 330 s run with `RECOMP_AUDIO_WAV` has
    non-silent output throughout (RMS ~3000-5800 per 20 s window, incl. 260-280 s, after the movies), and
    `RECOMP_APU_VOICES=1` shows voices 76-86 (a WMA stream voice plus effect voices) rendering real signal.
    The GetStatus "not ready" theory was wrong: the stream reports ready (`[DST-STREAM]`). Probes added:
    `RECOMP_APU_FE=1` (first 800 front-end methods), `[DST-STREAM]`/`[DST-PROC]` in `recomp_manual.c` under
    `RECOMP_DSTRACE`. Gaps at ~100 s and 220-240 s in the dump are unexplained (movie boundaries / loading?).

26. **Cube maps on the GPU.** The executor's CUBEMAP stage logic (face pick, `tex_face_stride`) is now shared:
    `nv2a_backend_cube_face_stride` / `nv2a_backend_decode_cube_face` (toolkit), and the back end uploads six faces as a
    `Texture2DArray` (`get_cube_texture`, SRV slots t5-t8, `stage_valid == 2`, `cube_fetch` in the pixel shader).
    Used by the car's env-map stage 2 (e.g. 128x128 DXT5 at `8070C000`, 64x64 at `809BE000`); logged as `[D3D11] cube map`.
    No mips, so a minified cube shimmers. The car's windows still render near black: unchecked whether that is the
    reflection data or lighting. The two red HUD quads did not show in ~90 dumped frames of a 5 min race run (a small red
    rectangle above the car appeared in one frame earlier, unidentified); the scene is no longer dark in those dumps.

27. **Car colours (open).** Reference: silver car, red tail lights, dark window trim. Ours: khaki body, dark-blue trim and
    lights, black glass. Findings so far:
    - Textures are right (`RECOMP_D3D11_TEXDUMP=<dir>` writes every decoded texture; the car's tex0 shows white body + red
      lights). A8 (0x19) must stay white-RGB + alpha (xemu's black RGB blacks out the ground); A8L8 (0x1A/0x20) is now decoded.
    - Every draw has the same one-stage combiner (tex0 * diffuse); the car/glass final combiner (`fin 130E0300`/`0000000E`)
      adds the secondary colour: out = (V1 + tex0*d0) blended with fog. `RECOMP_D3D11_NOSPEC=1` (V1 = 0) removes the
      blue and gives a dark glass and a grey-khaki body, so the wrong colour is the specular (d1), (0, 0.64, 1) on the glass.
    - The glass vertex programme (dump hash `7201C434`, same family `0F1DC2DE`/`F42AA52B`/`9FBF6EE0`/`559A6E2B`) writes
      oD1.y, .z and .w only; x is never written (instruction trace: `RECOMP_VSH_ITRACE=<hash>`). The arithmetic matches the
      hardware spec. Keeping the previous vertex's output (`RECOMP_VSH_KEEPOUT=1`) does not change the blue.
    - SET_SPECULAR_ENABLE (0x03B8) is now tracked (`specular_enable` in Nv2aBatchExt) but honouring it blacks out sky and
      buildings, so it is opt-in (`RECOMP_D3D11_SPECGATE=1`).
    - `RECOMP_D3D11_PICK=x,y,seconds` lists the draws covering a pixel. At the rear window: body (`E87B8C71`, d0 0.78/.84/.88
      = the silver paint colour), then two full-screen passes with the generic pass-through programme `969115CF` (tex0 = the
      framebuffer itself, d0 = vertex colour 0.56/0.39/0.28, V1 = attribute 4; then one sampling the depth buffer), then the
      alpha-blended glass (`E8101962`, tex0 = 64x32 sky gradient). The orange cast on the whole scene comes from those
      passes (framebuffer sampled while rendered to). Not yet checked whether the real tint is that strong.
    - Tint passes checked (`RECOMP_D3D11_SKIPSELF=1|2|3` skips draws sampling the render target / depth / just the
      depth-GREATER quad): pass 1 is a full-screen quad at the far plane (z = 2^24), depth GREATER, no depth write, stencil
      ALWAYS/KEEP, no blend, out = framebuffer * (0.56, 0.39, 0.28) + (0.05, 0.07, 0.09). GREATER against the far plane
      passes everywhere except sky pixels, so it tints everything on hardware too (our depth test agrees). Skipping it
      leaves a white road and a pale cyan car: the scene before the grade is over-bright and bluish, so the grade is
      intended, and the open question is why our pre-grade colours differ from what the grade expects. The road is drawn
      (`DDAEB9E5`) with a one-stage combiner of tex0 (A8, reads white) * diffuse (1,1,1); the 1024x1024 DXT1 detail textures
      on stages 1-3 are bound but never read by the combiner, and no other draw covers the road pixel. Pass 2 samples the
      depth buffer (no blend, always) and is only visible on the hood. Stencil shadow volumes (INCR_WRAP/DECR_WRAP,
      colour mask 0) precede the passes. Not yet verified: that those A8 + single-stage draws are really what the title
      programmed (every draw logs the same `rc0`, which is suspicious).
    - Combiner latching checked (`ftlive.ps1`: a race run with a one-frame method trace): the title sends only ~7
      combiner setups per frame (D3D re-emits on pixel-shader change), alternating `1E60 = 1` and `= 2`, and the executor's
      per-draw copy (`x.rc = s_gpu.rc`) matches: one frame had 35 one-stage and 23 two-stage draws. The earlier "all
      draws identical" log was just the draws I sampled. World/building draws (`6B438B1F`, `B291E0D2`) are two-stage with stage 1 =
      tex1 * 1 (replaces stage 0, lighting is baked into the 1024x1024 atlas). Car draws (`0F1DC2DE`, 281 tris) are
      one-stage: out = (1x1 paint texture * d0 + V1) blended with fog (`fin 130E0300`); stages 1-2 are cube maps
      (mode 3) that the combiner never reads, so the reflection comes only from the vertex-programme specular V1. Both
      car programmes (`0F1DC2DE`, `7201C434`/`E8101962` family) write oD1.y/z/w and never x, giving V1 = (0, ~0.64, 1)
      = cyan. That is the open question: what oD1.x really holds on hardware.
    - xemu comparison (fetched `glsl/vsh.c` and `vsh-prog.c`): output registers start at (0,0,0,1) and unwritten components
      stay 0; output address is `& 0xF`; mask bit 3 = x; D0/B0 (and D1/B1 when specular is on) are clamped; with
      SET_SPECULAR_ENABLE off, V1 = (0,0,0,1) (implemented under `RECOMP_D3D11_SPECGATE=1`; the car draws have it on, so no
      change). A write to oFog applies the most significant masked component to x (ours reads x only; the car programmes
      write oFog.w, so they keep our default of 1 -- worth revisiting). So oD1.x = 0 is what xemu would compute too: the
      cyan is real and the difference must be on the pixel side or in the grade.
    - Car draws at a car pixel (`PICK`): dark base passes (`F42AA52B`, d0 0.06-0.16), then the body pass with texture
      `81D1A000` 256x256 DXT3, d0 = paint (0.78, 0.84, 0.88), V1 = (0, 0.16, 0.5): a sky-blue reflection. Then
      the grade quad (same combiner as every draw: tex0 * d0 + V1, stencil ALWAYS, so it covers the car), then the glass.
    - The grade's colours are per-frame vertex colours the game supplies: neutral (d0 0.33, d1 0.29) in the early/menu
      frames, (0.56, 0.39, 0.28) / (0.05, 0.07, 0.09) in the race. A silver car cannot come out of framebuffer * that, so
      either the real values differ (they are CPU-computed; where from is unknown) or the grade does not reach the car.
    - Grade source (`RECOMP_D3D11_FINDGRADE`, `RECOMP_VSH_WATCH=162`): the quad's colours are the game's vertex-constant
      c[162].yzw = (0.5608, 0.3922, 0.2824) = bytes (143, 100, 72) and c[163].yzw = (14, 18, 23)/255, also read by the
      car/glass programmes (`c[162].yzww`). They are neutral (1/3) in the menus and these authored warm values from the race
      start, i.e. level lighting data, not a computation we get wrong. `RECOMP_D3D11_GRADEDUMP=<prefix>,<seconds>` writes the
      framebuffer before/after the quad: the shader's output equals pre * d0 + d1 exactly. The PRE-grade picture is
      the odd one: hot-pink buildings, white road, cyan-white car, blue other cars.
    - Pink buildings (programme `97FC3D33`, one stage, out = tex0 * d0 + V1, tex0 = white A8 dummy or the facade): V1 =
      (0.83-0.93, 0, 0.29). oD1.y = v2.w * R4.w * R8.x and v2.w is the alpha byte of attribute 2, which is 0 in the data
      (raw vertex bytes checked, our D3DCOLOR fetch is right). So the arithmetic is faithful; oD1 is evidently not a plain RGB
      added to the colour on hardware, or the pixel stage uses it differently from our model.
    - The A8 (0x19) textures bound at stage 0 of road and car draws are entirely 0xFF (dummies), so those draws are
      white * vertex colour. With specular zeroed (`RECOMP_D3D11_NOSPEC=1`) the car reads silver-grey with dark glass, but
      the road is a flat orange and the buildings nearly black, nothing like the reference's textured asphalt and cream
      facades: the road/building materials do not come through at all.
    - Other probes: `RECOMP_D3D11_DBG=8` (colour by programme hash), `_LOGPROG=<hash>`, `_LOGAT=<s>`, `RECOMP_VSH_DUMP`
      now keeps up to 512 programmes. The dump hash and the draw's hash sometimes differ (E8101962 is never dumped).

28. **Colours fixed: the pixel shaders never assembled (supersedes the open questions in 27).** MM3 ships its pixel
    shaders as `xps.1.1` *source* (`Data_hd.zip`: `Object0.psh`, `Houses/*.psh`, `Roads/*.psh`, 89 files; vertex shaders
    are precompiled `.cvsx`) and assembles them at load with the XDK assembler linked into the XBE (section `XGRPH`,
    0x321FC0-0x33F954). `XGRPH` was not in `--extra-sections`, so its code existed only as branch-target fragments, and
    the assembler's opcode keyword chain (`strcmp` against `tex`, `texbem`, ... at 0x338Axx) ended in the stub
    `0x00338B41` (the "last unresolved D3D stub"): every shader using `mul`/`lrp`/`mad`/`xdm` failed, the engine set no
    pixel shader, and D3D drew with its fixed-function combiner (`t0*v0`, final = R0 + V1 when SPECULARENABLE). The
    vertex programmes pack shader *parameters* into oD1 (`;v1.g = envmap intensity, v1.b = specular, v1.a = diffuse`),
    so adding V1 as specular gave the pink buildings, cyan car and white road; the grade then turned it orange.
    How it was found: xemu's on-disk shader cache (`%APPDATA%\xemu\xemu\shaders\*\*`, xemu 0.8.136-58) stores the whole
    `ShaderState` per programme; its vertex programme FNV hash equals ours (`nv2a_vsh_program_hash`), so per-draw pixel
    state can be compared directly (scratch parser: PshState = last 360 bytes of ShaderState; ShaderState size from the
    u64 program size that follows it). xemu had 5-stage combiners for buildings (`cc 00011105`, fin `14080400`), 3 for
    the road (fin `00000D0F 0D0B1C80`), 5 for the car (fin `1D090D00`); we had 1.
    Fix: `regen.ps1` now passes `--extra-sections DSOUND,XACTENG,WMADEC,XGRPH`, and seeds `0x338B41`, `0x338D97`.
    Result (`verify_ps.ps1`, frames `frames\xg`): building/road programmes now draw with exactly xemu's combiners; textured
    asphalt and kerbs, cream/grey facades, trees, silver car with red lights and dark glass. Snapshot
    `snapshots\mm3_recomp_xgrph.exe`. Beware: a broken `tools.disasm` command line still exits the regen with
    `BUILD_EXIT=0` (it reuses the old disassembly) -- check `tools\disasm\output\functions.json`'s date.
    Also matched to xemu: a masked write to oFog (the car writes `oFog.w`) now lands in oFog.x (`nv2a_vsh_interp.c`).
    Still unswept: `D3D`, `D3DX`, `XPP` (stubs at 0x35Bxxx-0x35Exxx are XPP); nothing points at them yet.
    Other xemu differences noted, not changed: NaN colours read as 1 (ours 0), `0*inf = 0` in MUL, an unwritten oFog
    carries the last value over (ours defaults x = 1), a disabled texture stage samples (0,0,0,1) (ours white), ARL is
    floor(x + 0.001) (ours floor(x + 0.25), see 33).

29. **Movies and the loading screen now show (D3D11 back end).** Neither goes through the 3D engine, and the back end
    only ever presented GPU render targets at a flip:
    - Bink movies play through the PVIDEO overlay (YUY2 frames in memory, registers at NV2A +0x8900..; buffer-0 use bit
      at +0x8700). `overlay_update`/`present_frame` in `d3d11_backend.c` composite it at scanout exactly as xemu's
      `render_display_pvideo_overlay` does (output rect, DS_DX/DT_DY scale, colour key, BT.601 YUY2 conversion).
    - The loading screen is drawn by the CPU into the display framebuffer from a loader thread (`AvSetDisplayMode`
      fb 0x001B0000; the GPU stops drawing). `memory_frame` scans out display memory at `PCRTC_START` (fallback: the
      AvSetDisplayMode fb) once no flip has happened for 250 ms.
    - Both run from a new optional `Nv2aBackend.idle` hook (toolkit `nv2a_backend.h`), called by the ack thread on every
      pass (`nv2a_backend_idle`), which also pumps the window: before, the window was only pumped inside `be_flip`, so it
      went "not responding" whenever the title stopped flipping.
    Probe: `RECOMP_D3D11_OVLDUMP=<prefix>` writes the window image every 3 s while the overlay or display memory is shown.
    Verified: intro movies (`dice.bik`, `msgs.bik`, `intro.bik`), attract movies, the "LOADING" screen.

30. **Vertex programs run on the GPU: ~6 -> ~47 fps in the race.** Measured first (`RECOMP_PERF=1`, `[PERF]` every
    2 s from the executor): the interpreter took 55-81% of the GPU thread's wall time (~0.9M vertices/s). Now, when the
    back end offers `Nv2aBackend.vsh_gpu`, `raster_batch_vsh_gpu` (nv2a_pb_exec.c) fetches each distinct vertex's
    array attributes once (hash map index -> slot), assembles the primitives into a triangle list with the same rules
    (odd strip triangles swapped), and sends an `Nv2aVshBatch` (attributes, indices, program, `nv2a_vsh_constants` +
    generation counter, SET_VERTEX_DATA values, fog state, AA scale). The D3D11 back end generates HLSL from the
    microcode (`vsh_generate`: a line-for-line port of `nv2a_vsh_run`, including our tuned defaults -- white oD0.rgb,
    oFog.x = 1, ARL floor(x + 0.25), the oFog mask rule), caches it per program hash, reads attributes from a structured buffer
    by SV_VertexID, and does the tail the executor did: colour clamp, fog factor, w = +0 -> 1, AA scale, screen ->
    clip space. Near-plane clipping is now the rasteriser's (depth clip on for these draws, z clamped at the far end).
    Programs that write c[] stay on the CPU. Knobs: `RECOMP_VSH_CPU=1` / `RECOMP_D3D11_VSH_CPU=1` (old path),
    `RECOMP_D3D11_VSH_DUMP=1` (print each generated shader). Result: race at ~95 flips per 2 s (was ~12), the
    back end's submit is now the largest cost (~40% of the GPU thread); frames match the CPU path.

31. **Crash on leaving/finishing a race: fixed with `RECOMP_DISPATCH_GATE=1`** (now in run_mm3.bat and the scripts).
    `sub_0027656A` (DSOUND refcount Release) faulted with ecx=0xFE0001E4, called from DSOUND's voice-release DPC
    (`0x27B638` -> `0x27B0F5`, lists at object+0x6C0..0x6D8): a node's object had been freed and its memory reused
    (its vtable was a COM table, so slot +0x14 was a stdcall Release). The game thread frees voices under its
    raise-to-DISPATCH guard; without the gate KfRaiseIrql excludes nothing and the timer thread's DPC walked the
    list mid-free. Reproduced with `interactive.ps1` (pause -> Quit Race -> left -> A) in both directions:
    crash with the gate off, clean quit and a second race with it on.
    Also: the display-memory fallback (29) now also requires the GPU idle for 500 ms (`s_last_draw_ms`).
    Gate cost: ~42 fps in the race (was ~47 without).
32. **Gameplay flicker: render targets recreated on every size change.** Found with 300 ms frame dumps
    (`verify_ps.ps1 <tag> ... <dumpms>` + scratch `flick.py`, which scores frames unlike both neighbours): single
    frames with a black world, huge black wedges, or only sky + HUD -- present on the old CPU vertex path too.
    `get_rt` recreated a surface whenever a pass used its address at another size: the glow buffer `0x804F8000`
    flipped 320x240 <-> 640x480 twice per frame, and the scene surface `0x80084000` was now and then drawn as
    320x240 / 256x256 / 16x16, which wiped the frame so far. Surfaces now only grow (like depth buffers): a
    smaller pass draws into the top-left with its own viewport, clears of it are rect-limited
    (`ID3D11DeviceContext1_ClearView`), and sampling a grown surface scales coordinates by its real size. Black
    frames gone; left: ~1 in 300 dumps without the colour grade, one with the player car body missing (unexplained).
    Also matched to xemu: GPU-transformed draws use depth clamp, not near/far clipping (xemu: GL_DEPTH_CLAMP always
    on); `RECOMP_D3D11_DEPTHCLIP=1` restores clipping. Logs: `[D3D11] surface ... grows`, `depth ... grows`,
    `... table full ... evicted`.
33. **Pedestrians lying down (fixed: ARL rounding, see the end of this item).** They lie from the start of a race (an empty sidewalk at the race-intro
    cutscene, before any traffic), in a ragdoll-like pose, so this is not a dive that fails to finish. The state
    table `AI/Personalities/Pedestrians/default.tbl` (walk / flee / jump=dive with physics locomotion / rise =
    man_getup1.ani) does not have a "lying" state, so the pose most likely comes from the physics/ragdoll side.
    Checked and fine: x87 compare idiom (`fnstsw/test ah/jp`, RECOMP_PARITY8, RECOMP_FCMP_CC) and `fpatan`
    (atan2(st1, st0); tools/recomp/fpdiff.py covers the transcendentals). Pointers: .tbl parser near 0x2528CA
    (`"state"` at 0x3A625C), personality loading near 0x136959, `ped::m_*` tweakables 0x3A59D0.. read by the
    steering code at 0x12F990..0x12FEB5 (`sub_0012FD91`), `ai::Pedestrian` type record at 0x431B20.
    **Narrowed further (render side):** pedestrians are drawn two ways, both also used by xemu (its shader cache has
    them): skinned (`514A6CA7`, `EEAF5AFC`, `059ABB70`: 3 bones from c[113+a0], world c[109..111]) and rigid
    (`D747A9E1`: v0 * c[112].x, world c[109..111], no bones), on the *same* vertex buffers (e.g. `@81AF6000`, a
    Y-up standing body, box y 0..0.65). Skinned ones (20-45 m in the logged frames) have an upright world matrix
    (middle row (0,1,0)) and near-identity bone matrices: they stand. Rigid ones (250+ m) have world =
    heading x X with X rows (0,-1,0),(0,0,-1),(1,0,0) and y ~4-6 lower: the 3ds Max biped root ("Bip") rest
    rotation and hip offset, so they lie and sink. The pedestrian object's own matrix is upright (found in RAM next
    to its position, e.g. 0x02565DB0). So the rigid path multiplies by the root bone's rest transform where the
    console must get ~identity (root animated x inverse bind). Not yet found: the routine that builds that matrix
    (it is not stored; X is computed, likely from a quaternion). Nearby lying bodies seen in earlier runs were not
    captured with logging; whether they are rigid draws too is open. Checked and fine: CMP 11:11:10 decode (matches
    xemu bit for bit), the D3DX matrix routines the game calls (fully generated, direct x87). Probes:
    `RECOMP_VSH_WORLDLOG=<s>` + `RECOMP_VSH_WORLDHASH=<h,...>` (per-batch world rows + v0 bounding box),
    `RECOMP_VSH_WORLDSCAN=<h>` (+ `_EXACT=1`): RAM scans for a batch's position / matrix rows / the X pattern.
    **Far pedestrians: found and worked around.** The chain: per-pedestrian render object (0x54C bytes, ctor
    `sub_000DB297`, vtables 0x3785B8/C0/C8). `sub_000DB5BB` picks its LOD by camera distance against
    `renderPedestrianLodDistance` (Scripts/ai_init_common.lua sets 50,100,120,130,160; verified at runtime): levels
    0-3 skinned (interface +4, provider `sub_000DA14A`), level 4+ (> 130 m) rigid (interface +0x58, provider
    `sub_000DA071`), which copies the render object's matrix (+0x10, set via `sub_000D9F35` from the AI's upright body
    matrix: callers 0x137850 / 0x1485C6), rotates it by 1.5707 rad about X (`0x5118D`) and lowers it 0.9 m, then
    uploads it as c[109..111] through a constant-provider (vtable 0x378C30, block 0x42AB90, engine constant cache
    `sub_00070407` -> D3D `sub_00349B60`). With an upright input that lays every pedestrian beyond 130 m flat (all
    rigid draws logged were >= 150 m). Whether the console feeds this path a different matrix was not settled; the
    skinned path draws them correctly, so `patch_gen.py` now sends LOD 4+ through it (`mm3_ped_rigid_lod` in
    recomp_manual.c; `RECOMP_PED_RIGID=1` restores the original). Skeleton base matrices (0x3C6164 = RotX(-90),
    0x3C6194 = that + 180) checked at runtime: correct.
    **Root cause (fixed): every skinned pedestrian lay flat, not just some.** "Upright world + near-identity bones =
    standing" above was wrong. Running the skinned programme on the CPU with c[96..99], c[58], c[59] set to identity
    (oPos = world position) gave every body a world box ~1.9 m long and ~0.4 m tall. The bone index is v3 (UB_OGL)
    * c[112].y = 127.5, and the meshes store bone k's first row (3k) as byte 2k+1, so the index comes out at exactly
    3k + 0.5 (e.g. 31/255 * 127.5 = 15.5). Our ARL rounded to nearest (chosen for the car, 21 (h): 12 *
    21/255 = 0.988 must give 1), so a0 = 3k+1 and each vertex read rows 1, 2 of its bone plus row 0 of the next:
    a row rotation that tips the body 90 degrees while the animation still plays (legs walking while lying).
    xemu uses floor(x + 0.001). Now `floor(x + 0.25)`, which gives the intended integer for both encodings, in
    `nv2a_vsh_interp.c` (arl) and the HLSL translation (`d3d11_backend.c`); `RECOMP_VSH_ARL_FLOOR=1` gives xemu's.
    Verified: all pedestrian bodies ~1.9 m tall, standing and walking in race frames; the car is unchanged.
    The rigid LOD 4+ path (`D747A9E1`, no ARL) still multiplies in the X rotation, so the LOD patch stays.
    Probes left in the toolkit, both under `RECOMP_PEDPOSE=1`: `[PEDBOX]` (CPU-run world box per ped draw, TALL/FLAT,
    first vertex's attributes, bones 0-3) and the older bone-0 classifier with RAM dump. `RECOMP_D3D11_PICK` takes an
    optional 4th value (repeat every N s); it only sees CPU-transformed draws (`RECOMP_VSH_CPU=1`, too slow to reach
    a race). Debug hooks used in generated code (provider/SetTransform/animation logs) were removed.

34. **W-buffer, the haze pass, the loading screen, texture hashing (2026-10-24, runs `ld1`-`ld3`, `boot1`).**
    - **MM3 w-buffers its scene.** SET_CONTROL0 (0x0290) = `0x00110001` on 36 scene programmes: bit 16
      Z_PERSPECTIVE_ENABLE, so the NV2A stores and tests *w* (linear in distance), not z/w. The clip range
      4415..2^24 is in w units (w ~ 43780 x metres: 0.1 m .. 383 m). We stored z/w (crowded near 1), so the
      passes that read depth back as distance saturated a few metres out. Now `Nv2aRenderState.control0`
      (toolkit) and a second pixel shader `PSW` writes `SV_Depth = SV_Position.w * zscale` for those draws
      (`wbuffer_on`, `RECOMP_D3D11_WBUFFER=0` = z/w). Cost ~6 fps in the race (~47 vs ~53): SV_Depth turns
      off early-Z.
    - **The skipped "opaque depth read" of 21(f) is the distance haze** (`969115CF`, sp `000A621`: 2D on the
      depth as LIN_L16, DOT_PRODUCT, DOT_ST into the 64x32 lookup `80762000`; final = lerp(t3 = scene, v0, t2)).
      Drawn with z/w depth it gave exactly the user's screenshot: everything beyond ~30 m replaced by the
      skyline backdrop with a hard horizontal edge across the road (`frames\ld2`). With the w-buffer it gives
      the intended warm haze/dawn sky (`frames\ld3`), so it is drawn by default now (`RECOMP_D3D11_DEPTHREAD=0`
      skips it). The alpha-mask pass (`969115CF` sp `006C621`) is a 24-triangle 3D mesh, LEQUAL, colour mask
      alpha only, blend dst*src_alpha.
    - **Near/far like xemu:** with SET_ZMIN_MAX_CONTROL near/far cull on and ZCLAMP_EN = CULL (MM3: 1), xemu
      clamps depth but discards fragments outside the clip range (psh.c `depth_clipping`). The PS now does the
      same (`cb.zclip`; compares w in w-buffer mode; far side +2^-16 slack) for draws that write colour;
      colour-less draws (stencil shadow volumes) keep plain clamp. `RECOMP_D3D11_ZDISCARD=0` = clamp only.
    - **Loading screen:** `D3320CDA` before each load is not a copy: combiner r0 = v0 x 1, final = r0, so it
      fills the display buffer `801B0000` with white (it ignores the bound texture). The loader thread then
      CPU-draws the "LOADING"/"PRESS" panel into that memory. Our white fill lived only in the host surface, so
      the panel showed on black. `memory_frame` now writes the host surface back into guest memory once per
      hand-over, only into pixels still black (`RECOMP_D3D11_MEMMERGE=0` = off; log `display memory ... takes
      over`). No image file is opened during loading, so white + panel is what this code path produces; if the
      user's reference shows artwork, it comes from somewhere else.
    - **Texture hashing:** `guest_hash` now covers level 0 in the texture's own format
      (`nv2a_backend_cube_face_stride` with levels = 1: DXT1 w*h/2) instead of w*h*4; no `re-decoded` lines in
      `ld1` (`80624000` used to re-decode every frame). `RECOMP_D3D11_HASH_ARGB=1` = old size.
    - Probes: `RECOMP_D3D11_LOWFLIP=<prefix>` (flips with <= 40 draws: draw list + stage-0 texture resolved or
      not, guest memory at the flip surface as BMP; at a hand-over the previous flip's draws and every 640-wide
      surface); `[D3D11] opaque depth read drawn/skipped` (log_batch once per programme).

## Open issues (2026-10-23, D3D11 back end)

First pass done 2026-10-23 (runs `inv2`, `drv1`, `drv2`; driving script in that session's scratchpad: verify_ps.ps1
plus `rt` / `rt+lleft` lines appended to RECOMP_PAD_LIVE). Runs need `subst M: "D:\Documents\Xbox Recompilation"`:
`mm3recomp\game_files` is a junction to `M:\game_files` ("Cannot open XBE" otherwise).

1. **Sky through geometry (user: "the cube map always shows").** Fixed (item 34: w-buffer + haze pass); seen
   right in Paris (our runs) and Washington (user-confirmed 2026-10-24). The sky is a cube-map draw at the far
   plane (`9EC67266`, stage 0 CUBEMAP, depth LEQUAL, no depth write, 3 blended passes). Every draw of a logged frame
   has the same clip range (4415..2^24), so a per-draw depth-scale mismatch is ruled out. Only capture so far:
   `frames\drv2\f00452.bmp`, the skyline backdrop over a building wall with the camera pushed into it after a crash
   (`f00600`, same spot, is right) -- may be legitimate camera-in-wall.
   User screenshot (taxi, dusk, scaled window): everything beyond ~30 m reads as a washed-out "backdrop" image,
   with a sharp horizontal edge across the road just above the taxi roof. Ruled out: `9EC67266` is NOT a backdrop --
   it is a small instanced cube-mapped glass panel (12 verts, ~3x4.3 units at world (739,39,1315), per-instance data
   in SET_VERTEX_DATA v5..v10; alpha = 0.5 + 8e-5 * dist^2 from c[100]), drawn at its true depth. Scene depth is
   ordinary perspective z crowded near 1 (car ~0.992, far buildings ~0.9985, w ~ 43780 x metres). A horizontal
   edge on a flat road is what a hard *depth threshold* looks like, so the depth-reading passes (`969115CF`
   haze/alpha mask, the skipped opaque depth-read pass of item 21(f)) are the next suspects. Not seen in our own
   race frames (start area, daylight). `RECOMP_D3D11_LOGPROG=<hash>` now also prints the batch's 16 vertex inputs
   and non-zero c[] once.
   Related: the full-screen pass `969115CF` (sp `006C621` = 2D, DOT_PRODUCT, DOT_PRODUCT, DOT_STR_3D; stage 0 = the
   depth buffer as 1280x480 L16, lookup 32x32 at `804E1000`) writes scene *alpha* = alpha * lookup(depth).a; before
   the dot modes existed it zeroed alpha. The 40x30 pass after it reads the scene: probably the glow/sky mask.
2. **Water.** xemu's shader cache has MM3 pixel shaders with dependent stages: `0064621` (2D, DOT_PRODUCT x2,
   DOT_RFLCT_SPEC = texm3x3vspec, x6: the water/reflection shader), `006C621` (texm3x3tex), `000A621` (DOT_ST). The
   back end returned 0 for every mode >= 6. Now implemented (`fetch_all`/`dotmap` in d3d11_backend.c, toolkit passes
   SET_DOT_RGBMAPPING 0x1E74 and SET_SHADER_OTHER_STAGE_INPUT 0x1E78 in Nv2aBatchExt; DOT_STR_3D samples 2D at
   xy, bump-env modes sample unperturbed). `[D3D11] dot stages:` logs the first 12 such draws. Not yet verified on
   water: no run reached the river (only `969115CF` used these modes in the start area).
3. **Flicker -- resolved.** Mostly by the honest fence wait and the ring-wrap fix (a) below (user-confirmed
   2026-10-23); the remaining class (b) no longer occurs (user-confirmed 2026-10-24). Captured in `frames\drv1\f00775.bmp`: a huge black wedge from the top-left and the car's rear panel
   missing for one frame; `RECOMP_D3D11_FRAMESTATS=1` (`[FSTAT]` per flip: batches, tris, dropped, presented rt)
   showed the same batch/triangle counts as its neighbours, so the draws ran with wrong data. Cause found:
   `sub_00344640` (D3D_BlockOnTime, recomp_manual.c) said "GPU always caught up", written before the executor
   existed; with the async executor D3D rewrote dynamic vertex buffers (deformable car body, shadow volumes) still
   queued -- exactly what xboxrecomp `docs/technical/d3d8ltcg-device-context.md` describes for X-Men Legends. Now
   honest: semaphore (`[D3D] GPU semaphore at 0x80003000`) handed to the executor, newest fence inserted+kicked via
   `sub_00344410(0)`, wait bounded at 250 ms (`RECOMP_D3D_FENCE_FAKE=1` = old). `drv2`: 2 timeouts at boot only,
   no wedge/missing-panel frame in 1200 dumps, but one frame without the colour grade (`f00173`), the "~1 in 300"
   of item 32 -- still open.
   **Second pass (same day): two classes.** Method: dump *every* frame for 45-70 s of driving
   (`RECOMP_D3D11_DUMP_MS=0` + new `_DUMP_FROM`/`_DUMP_FOR` seconds; files now `f<n>_f<flip>.bmp`), score frames
   unlike both neighbours per screen tile (scratch `flick2.py`), then compare per-draw fingerprints
   (`RECOMP_D3D11_BHASH=1|<vsh>`: geometry/constants/state/sources per draw, `[BH]`), pre/post-grade dumps
   (`RECOMP_D3D11_PREDUMP=<prefix>`), per-pass dumps (`_STEPDUMP=<prefix>,<b>,...`) and per-draw 16x12 luminance
   signatures after the grade (`_STEPSIG=1`, `[SIG]`). Beware: `[BH]`/`[SIG]` flip N is dump/`[FSTAT]` flip N+1.
   Scripted driving: `drive_ps.ps1` (session scratchpad) = verify_ps plus `a`/`rt`/stick lines; Start pulses can
   leave the race paused, so it presses A each loop.
   (a) **Fixed: the pushbuffer segment scan lost commands at every ring wrap.** On `PUT < last PUT` the ack loop
   scanned [last PUT, highest PUT ever seen] + [lowest PUT seen, PUT]; D3D's JUMP sits after the commands queued
   since the last kick, so those were dropped each wrap (a few seconds apart). Seen as a frame with one draw fewer
   (the 16x16 pass of the 128->16 chain) and the following render-target switch gone, so the whole HUD drew into
   the 16x16 surface (frame without HUD), or black 8-16 px blocks. Now `nv2a_pb_scan_wrap` (nv2a_pb_scan.c) walks
   from the old PUT through the JUMP to the new PUT, and a method whose parameters run past PUT is finished by the
   next kick instead of having its rest read as headers (`RECOMP_PB_WRAP_OLD=1` = old bounds; `[PB] ring wrap`
   warns if no JUMP is found -- none seen). `RECOMP_PB_WALK=dma` would also do it but can hang D3D (header there).
   (b) **No longer seen (user, 2026-10-24): whole-screen or wedge-shaped dark / orange (backdrop-coloured) frames**, mostly near walls, sometimes
   several frames in a row. Draw lists, inputs, render state and texture sources are identical to the neighbours;
   the scene is intact before and right after the grade (colour and alpha), and the damage starts later in the
   post chain. The orange is the backdrop pass `22CCA55E` (blend DST_ALPHA/1-DST_ALPHA, texture `80746000`), so
   the scene alpha it reads is wrong; that alpha is modulated by the depth-lookup pass `969115CF` (dot-product
   modes, reads the depth buffer through `depth_view`, rebuilt only when `z->dirty`). Next: check that depth view's
   staleness and the alpha right before `22CCA55E`. Ruled out: shadow volumes (`SKIPPROG=426F6C5D` still glitches),
   clears (`[FSTAT]` now has clear count/signature), small depth clears (`[D3D11] small depth clear` never fires).
   Third pass: the alpha *right before* `22CCA55E` (`RECOMP_D3D11_VSHDUMP=<prefix>,<vsh>`: target RGB + alpha
   before that programme's first draw each frame) is identical in glitch frames too, so the wedge is made at or
   after that pass. `TEXWHY` shows the depth-lookup pass does get the depth view (a colour surface at
   `802DC000` exists but has no draws). Ruled out by A/B (2380+ frames each): stale vertex constants
   (`RECOMP_D3D11_VC_ALWAYS=1` uploads c[] every draw) and the NO_OVERWRITE attribute/index rings
   (`RECOMP_D3D11_VSH_DISCARD=1`), alone or together; a second thread in the back end (`thread_check`, never
   fires); the HUD inline arrays (vertex data sane). Every remaining glitch captured so far had the car pushed
   into a wall/shopfront (camera against geometry); none seen in open driving, but the scripted driving rarely
   stays in the open. Side finding: `guest_hash` covers w*h*4 bytes, 4-8x a DXT texture's size, so textures
   next to changing memory re-decode every frame (`80409000`, `80624000`; `[D3D11] texture ... re-decoded`) --
   wasted time, not wrong pixels.
   Near/far: MM3 sends SET_ZMIN_MAX_CONTROL = 0x00000001 (CULL) on every draw (now in `Nv2aRenderState
   .zmin_max_control`); always-clip vs always-clamp A/B runs were inconclusive, so the default stays clamp
   (`RECOMP_D3D11_DEPTHCLIP=1` clip, `=2` follow the register).
4. **Gamepad rumble.** The pad should vibrate on impacts etc. The game drives rumble through `XInputSetState`; route
   that to the host controller (XInput on Windows) instead of dropping it.
5. **Window aspect ratio.** The window/back buffer should keep the game's own aspect ratio (letterbox/pillarbox
   on resize) instead of stretching to the window size.
6. **Anti-aliasing.** Add host-side AA to the D3D11 back end (e.g. MSAA render targets resolved before present,
   with an env/config switch for the sample count).

## Next steps

1. The two red HUD quads, the dark slivers, cube-map reflections; then input into the D3D11 window and audio.
2. Why the camera is where it is: does the car move normally? (`RECOMP_PAD_PRESS` pulses Start forever, which pauses a
   race; `RECOMP_PAD_PRESS_UNTIL_MS=230000` stops it after the menus.)
3. Audio works (see 25); check by ear and explain the silent gaps. Then the intro movies.
4. Speed: vertex programs on the CPU (`nv2a_vsh_interp.c`) and the per-vertex copy are the limit now.
