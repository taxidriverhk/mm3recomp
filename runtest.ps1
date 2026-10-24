# Headless test run: runtest.ps1 <tag> [timeoutSeconds]; stderr -> <tag>.err
param([string]$tag="t", [int]$secs=420)
Set-Location $PSScriptRoot
$env:RECOMP_WATCHDOG_SECS="900"; $env:RECOMP_STUB_LOG="1"; $env:RECOMP_AC97_READY="1"
$env:RECOMP_PB_EXEC="1"; $env:RECOMP_HEAP_RECLAIM="1"; $env:RECOMP_DISPATCH_GATE="1"; $env:RECOMP_PAD_PRESS="0x10"
$p=Start-Process build\Release\mm3_recomp.exe -RedirectStandardError "$tag.err" -RedirectStandardOutput "$tag.out" -PassThru -WindowStyle Hidden
$null=$p.Handle
if($p.WaitForExit($secs*1000)){ "exit $($p.ExitCode)" } else { "still running after $secs s"; $p.Kill() }
Select-String -Path "$tag.err" -Pattern '^\[ICALL\] Failed|STUB\] unres|CRASH|not code' | group { ($_.Line -replace '\(total.*','') -replace 'at call.*','' } | sort Count -desc | select -first 15 | % { "$($_.Count) $($_.Name)" }
