@echo off
rem Launches the recompiled Midtown Madness 3 from this folder so that
rem game_files\default.xbe is found. Log output goes to run.log as well.
rem Extra arguments are passed to the exe, e.g. run_mm3.bat --headless
rem
rem STATE: playable. A window opens (title shows FPS and draw count).
rem   - The intro movies (about 2 minutes) play through the NV2A video overlay,
rem     composited by the D3D11 back end (NOTES 29); the loading screen shows too.
rem   - Menus work with the keyboard or an XInput pad; races render on the GPU with
rem     correct colours (NOTES.md 28) at ~40-50 fps (vertex programs on the GPU, NOTES 30).
rem   - Sound plays through XAudio2. No crash seen in 10+ min runs.
rem See NOTES.md for details.

cd /d "%~dp0"

if not exist "build\Release\mm3_recomp.exe" (
    echo build\Release\mm3_recomp.exe not found. Build the project first.
    pause
    exit /b 1
)
if not exist "game_files\default.xbe" (
    echo game_files\default.xbe not found. Extract your disc into game_files\ first, see README.
    pause
    exit /b 1
)

rem Print a state dump if the title stops making progress this long (seconds).
set RECOMP_WATCHDOG_SECS=900
rem Log each unresolved-function stub the first time it is reached.
set RECOMP_STUB_LOG=1
rem Report the audio codec ready so the XDK's DirectSoundCreate succeeds.
set RECOMP_AC97_READY=1
rem Render on the GPU (D3D11 back end, src\d3d11_backend.c). Remove this line to
rem fall back to the toolkit's CPU rasteriser (about 1 fps).
set RECOMP_D3D11=1
set RECOMP_PB_EXEC=1
rem Reuse freed memory (the toolkit ignores frees by default, which exhausts the 64 MB arena).
set RECOMP_HEAP_RECLAIM=1
rem One thread at DISPATCH_LEVEL at a time, DPCs included, as on the console. Without it
rem DirectSound's voice-release DPC walks lists the game thread is freeing: crash on
rem leaving/finishing a race (NOTES 31).
set RECOMP_DISPATCH_GATE=1
rem The D3D11 back end opens its own window; this is only for the CPU rasteriser.
rem set RECOMP_FB_WINDOW=1
set RECOMP_KEYBOARD=1
rem Keyboard (arrows, Enter=Start, Z/X/C/V=A/B/X/Y, WASD=left stick, 1/3=triggers) and any XInput pad
rem drive the game. A synthetic Start pulse (RECOMP_PAD_PRESS=0x10) would pause every race; leave it off.
rem set RECOMP_PAD_PRESS=0x10

rem Optional: also save frames to frames\ as BMP files (about 1 MB every 4 s).
rem   if not exist frames mkdir frames
rem   set RECOMP_FB_DUMP=frames\f
rem   set RECOMP_PB_REPORT_MS=4000

echo Starting Midtown Madness 3 (recompiled)...
"build\Release\mm3_recomp.exe" %* 2> run.log
echo.
echo Exited with code %ERRORLEVEL%. Log written to run.log
pause
