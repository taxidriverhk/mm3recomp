# Run with Start pulses until $until ms, then send "a" (Resume Race) once via PAD_LIVE; dump frames.
param([string]$tag = "lv", [int]$until = 215000, [int]$resumeAt = 260, [int]$secs = 420)
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent; Set-Location $root
Get-Process mm3_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
$live = Join-Path $root "pad_live.txt"
"" | Out-File $live -Encoding ascii
$env:RECOMP_WATCHDOG_SECS="900"; $env:RECOMP_STUB_LOG="1"; $env:RECOMP_AC97_READY="1"; $env:RECOMP_PB_EXEC="1"
$env:RECOMP_HEAP_RECLAIM="1"; $env:RECOMP_DISPATCH_GATE="1"; $env:RECOMP_PAD_PRESS="0x10"; $env:RECOMP_D3D11="1"; $env:RECOMP_D3D11_VSYNC="0"
$env:RECOMP_PAD_PRESS_UNTIL_MS="$until"; $env:RECOMP_PAD_LIVE=$live
$p = Start-Process build\Release\mm3_recomp.exe -RedirectStandardError "$tag.err" -RedirectStandardOutput "$tag.out" -PassThru -WindowStyle Hidden
$null = $p.Handle
Start-Sleep $resumeAt
"a:300" | Out-File $live -Encoding ascii -Append
Start-Sleep 20
"a:300" | Out-File $live -Encoding ascii -Append
if(-not $p.WaitForExit(($secs-$resumeAt-20)*1000)){ $p.Kill(); $p.WaitForExit() }
"LIVE_DONE"
