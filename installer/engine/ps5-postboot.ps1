# ps5-postboot.ps1 v1.0 (16.11.2026) - installed by PS5 Windows Setup as task "PS5-BootConfirm" (SYSTEM,
# at logon +90 s and at startup +4 min). Replaces the two old tasks PS5ConfirmStart (T1) and PS5NagiStartConfirm (T2).
#  1. Boot-loop guard of bc250kmd: UnconfirmedStarts=0 in the active StateV2 generation, once the desktop runs
#     (explorer present) or the GPU reports Status OK. At >= 2 unconfirmed starts the KMD refuses to start (Code 43).
#  2. A1 once per generation (only with exactly one generation): EnableGpuPresentBlit=1, EnableCddDwmInterop=0
#     in the active generation's Data, only when exactly one generation exists. Takes effect with the next driver start.
#  3. EnableDisplayModes=0 in the active generation's Data (the release KMD runs with mode 0).
# Log: C:\ps5-work\ps5-postboot.log
$ErrorActionPreference = 'Stop'
$log = 'C:\ps5-work\ps5-postboot.log'; New-Item -ItemType Directory -Force C:\ps5-work | Out-Null
function L($m) { Add-Content $log ('{0:o} {1}' -f (Get-Date), $m) }
try {
  $b = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\SharedState\StateV2'
  if (-not (Test-Path $b)) { L 'no StateV2 (bc250kmd never started yet)'; exit 0 }
  $a = (Get-ItemProperty $b).Active; $d = "$b\Generations\$a\Data"
  $gens = @(Get-ChildItem "$b\Generations" -ErrorAction SilentlyContinue)
  $gpu = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13F[CE]' })
  $desk = @(Get-Process explorer -ErrorAction SilentlyContinue).Count -gt 0
  $old = (Get-ItemProperty $d -ErrorAction SilentlyContinue).UnconfirmedStarts
  if ($desk -or ($gpu.Count -and $gpu[0].Status -eq 'OK')) { New-ItemProperty $d -Name UnconfirmedStarts -PropertyType DWord -Value 0 -Force | Out-Null }
  $changed = @()
  if ($gens.Count -eq 1 -and $gens[0].PSChildName -eq $a) {
    foreach ($kv in @(@('EnableGpuPresentBlit', 1), @('EnableCddDwmInterop', 0), @('EnableDisplayModes', 0))) {
      $cur = (Get-ItemProperty $d -Name $kv[0] -ErrorAction SilentlyContinue).($kv[0])
      if ($cur -ne $kv[1]) { New-ItemProperty $d -Name $kv[0] -PropertyType DWord -Value $kv[1] -Force | Out-Null; $changed += "$($kv[0])=$($kv[1]) (was $cur)" }
    }
  } else { $changed += "A1 skipped: generations=$($gens.Count) active=$a" }
  L ('gen={0} unconfirmed {1}->{2} desktop={3} gpu={4} {5}' -f $a, $old, (Get-ItemProperty $d).UnconfirmedStarts, $desk, ($gpu | Select-Object -First 1).Status, ($changed -join '; '))
} catch { L "ERROR $($_.Exception.Message)"; exit 1 }
