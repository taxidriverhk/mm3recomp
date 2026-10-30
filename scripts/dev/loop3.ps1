$proj = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent; $tk = Join-Path $proj "xboxrecomp"
$python = if ($env:PYTHON) { $env:PYTHON } else { "python" }
for($r=1;$r -le 1;$r++){
  "ROUND $r"
  & "$proj\scripts\regen.ps1"
  Set-Location $proj
  $env:RECOMP_AC97_READY="1"; $env:RECOMP_STUB_LOG="1"; $env:RECOMP_WATCHDOG_SECS="25"
  $p=Start-Process build\Release\mm3_recomp.exe -WorkingDirectory $proj -RedirectStandardOutput "$proj\auto.out" -RedirectStandardError "$proj\auto.err" -PassThru -WindowStyle Hidden
  $null=$p.Handle
  if(-not $p.WaitForExit(60000)){ $p.Kill() }
  "ROUND $r exit=$($p.ExitCode)"
  Remove-Item Env:RECOMP_AC97_READY,Env:RECOMP_STUB_LOG,Env:RECOMP_WATCHDOG_SECS
  Copy-Item "$proj\auto.err" "$proj\auto_r$r.err" -Force
  Set-Location $tk
  & $python -m tools.seed_from_log "$proj\auto.err" ..\game_files\default.xbe --functions tools\disasm\output\functions.json --seeds "$proj\config\seed_functions.json" | Select -Last 12
  & $python "$proj\scripts\reseed.py"
}
"LOOP3_DONE"
