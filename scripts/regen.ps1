$ErrorActionPreference = "Continue"
# Paths are relative to this script (in <repo>\scripts), so the repo can live anywhere.
$proj = Split-Path $PSScriptRoot -Parent
$tk = Join-Path $proj "xboxrecomp"
$xbe = "..\game_files\default.xbe"   # relative to $tk, i.e. <repo>\game_files

# Tools: python and cmake from PATH ($env:PYTHON / $env:CMAKE override), then CMake's default install folder.
$python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$cmake = if ($env:CMAKE) { $env:CMAKE }
         elseif (Get-Command cmake -ErrorAction SilentlyContinue) { "cmake" }
         else { Join-Path $env:ProgramFiles "CMake\bin\cmake.exe" }

if (-not (Test-Path (Join-Path $proj "game_files\default.xbe"))) {
    throw "game_files\default.xbe not found. Extract your Midtown Madness 3 disc into game_files\ (see README)."
}
& (Join-Path $PSScriptRoot "apply_patches.ps1")

Set-Location $tk
# XBE header/section analysis; tools.disasm picks up <xbe stem>_analysis.json next to the XBE.
& $python -m tools.xbe_parser $xbe --json "..\game_files\default_analysis.json"
& $python -m tools.disasm $xbe --text-only --extra-sections DSOUND,XACTENG,WMADEC,XGRPH --seed-functions "$proj\config\seed_functions.json"
& $python -m tools.func_id $xbe
& $python -m tools.abi_analysis $xbe
Remove-Item "$proj\src\recomp\gen\*" -Force -ErrorAction SilentlyContinue
& $python -m tools.recomp $xbe --game-name "Midtown Madness 3" --all --split 250 --gen-dir "$proj\src\recomp\gen" --exclude-manual "$proj\src\recomp_manual.c"
& $python "$PSScriptRoot\patch_gen.py"
Set-Location $proj

# A build tree configured from another path (e.g. an old subst drive) cannot be reused: start it again.
$cache = Join-Path $proj "build\CMakeCache.txt"
if (Test-Path $cache) {
    $home_line = Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
    if ($home_line -and ((Resolve-Path $home_line.Matches[0].Groups[1].Value -ErrorAction SilentlyContinue).Path -ne $proj)) {
        "build\ was configured from $($home_line.Matches[0].Groups[1].Value); reconfiguring from scratch"
        Remove-Item (Join-Path $proj "build") -Recurse -Force
    }
}
# XBOXRECOMP_DIR is a cache variable, so pass it: an old cache would keep a previous location.
& $cmake -S . -B build "-DXBOXRECOMP_DIR=$tk"
& $cmake --build build --config Release -- /m /v:m
"BUILD_EXIT=$LASTEXITCODE"
