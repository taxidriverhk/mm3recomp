# Race run with scripted driving: throttle held, steering now and then, A pressed each loop
# (Start pulses at boot can leave the race paused; A on the pause menu resumes).
# usage: drive_ps.ps1 <tag> [secs] [dumpms] [steer pattern, e.g. "lleft,,lright,"] [gas button]
param([string]$tag = "drv", [int]$secs = 600, [int]$dumpms = 5000, [string]$steer = ",lleft,,,lright,,", [string]$gas = "rt")
Set-Location $PSScriptRoot
Get-Process mm3_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
$live = Join-Path $PSScriptRoot "pad_live.txt"
"" | Out-File $live -Encoding ascii
New-Item -ItemType Directory -Force "frames\$tag" | Out-Null
Remove-Item "frames\$tag\*.bmp" -ErrorAction SilentlyContinue
$env:RECOMP_WATCHDOG_SECS="900"; $env:RECOMP_STUB_LOG="1"; $env:RECOMP_AC97_READY="1"; $env:RECOMP_PB_EXEC="1"
$env:RECOMP_HEAP_RECLAIM="1"; $env:RECOMP_DISPATCH_GATE="1"; $env:RECOMP_PAD_PRESS="0x10"; $env:RECOMP_D3D11="1"; $env:RECOMP_D3D11_VSYNC="0"
$env:RECOMP_PAD_PRESS_UNTIL_MS="215000"; $env:RECOMP_PAD_LIVE=$live
$env:RECOMP_D3D11_DUMP="frames\$tag\f"; $env:RECOMP_D3D11_DUMP_MS="$dumpms"
$p = Start-Process build\Release\mm3_recomp.exe -RedirectStandardError "$tag.err" -RedirectStandardOutput "$tag.out" -PassThru -WindowStyle Hidden
$null = $p.Handle
Start-Sleep 260
$moves = $steer.Split(",")
$i = 0
$t0 = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt ($secs - 260)) {
    "a:200" | Out-File $live -Encoding ascii -Append
    Start-Sleep -Milliseconds 400
    $m = $moves[$i % $moves.Count]; $i++
    if ($m) { "$gas+$($m):1500" | Out-File $live -Encoding ascii -Append; Start-Sleep -Milliseconds 1600 }
    "$($gas):6000" | Out-File $live -Encoding ascii -Append
    Start-Sleep -Milliseconds 6100
}
if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
"DRIVE_DONE"
