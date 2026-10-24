# Race run that dumps presented frames and logs one frame's batches (combiner state per draw).
# usage: verify_ps.ps1 <tag> [logframe range] [secs]
param([string]$tag = "xg", [string]$logframe = "6300-6300", [int]$until = 215000, [int]$resumeAt = 260, [int]$secs = 400, [int]$dumpms = 10000)
Set-Location $PSScriptRoot
Get-Process mm3_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
$live = Join-Path $PSScriptRoot "pad_live.txt"
"" | Out-File $live -Encoding ascii
New-Item -ItemType Directory -Force "frames\$tag" | Out-Null
Remove-Item "frames\$tag\*.bmp" -ErrorAction SilentlyContinue
$env:RECOMP_WATCHDOG_SECS="900"; $env:RECOMP_STUB_LOG="1"; $env:RECOMP_AC97_READY="1"; $env:RECOMP_PB_EXEC="1"
$env:RECOMP_HEAP_RECLAIM="1"; $env:RECOMP_DISPATCH_GATE="1"; $env:RECOMP_PAD_PRESS="0x10"; $env:RECOMP_D3D11="1"; $env:RECOMP_D3D11_VSYNC="0"
$env:RECOMP_PAD_PRESS_UNTIL_MS="$until"; $env:RECOMP_PAD_LIVE=$live
$env:RECOMP_D3D11_DUMP="frames\$tag\f"; $env:RECOMP_D3D11_DUMP_MS="$dumpms"
$env:RECOMP_D3D11_LOGFRAME=$logframe
$p = Start-Process build\Release\mm3_recomp.exe -RedirectStandardError "$tag.err" -RedirectStandardOutput "$tag.out" -PassThru -WindowStyle Hidden
$null = $p.Handle
Start-Sleep $resumeAt
"a:300" | Out-File $live -Encoding ascii -Append
Start-Sleep 20
"a:300" | Out-File $live -Encoding ascii -Append
if(-not $p.WaitForExit(($secs-$resumeAt-20)*1000)){ $p.Kill(); $p.WaitForExit() }
"VERIFY_DONE"
