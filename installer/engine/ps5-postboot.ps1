# ps5-postboot.ps1 v1.0.2 (16.11.2026, v1.0.2 26.11.: the hotplug settings of GPU driver 1228+ once the state exists; v1.0.1 25.11.: every PS5 GPU id, not only 13FC/13FE) - installed by PS5 Windows Setup as task "PS5-BootConfirm" (SYSTEM,
# at logon +90 s and at startup +4 min). Replaces the two old tasks PS5ConfirmStart (T1) and PS5NagiStartConfirm (T2).
#  1. Boot-loop guard of bc250kmd: UnconfirmedStarts=0 in the active StateV2 generation, once the desktop runs
#     (explorer present) or the GPU reports Status OK. At >= 2 unconfirmed starts the KMD refuses to start (Code 43).
#  2. A1 once per generation (only with exactly one generation): EnableGpuPresentBlit=1, EnableCddDwmInterop=0
#     in the active generation's Data, only when exactly one generation exists. Takes effect with the next driver start.
#  3. EnableDisplayModes=0 in the active generation's Data (the release KMD runs with mode 0).
#  4. v1.0.2: Parameters\EnableHotplugModeSet=7 and Ps5EdidDescriptor=1 (monitor change, real resolutions) - only once StateV2
#     exists: the KMD creates its state only while Parameters holds nothing but DefaultsV2 (fresh installs failed with
#     0xC0000182, HW 26.11.), so the INF no longer writes them. Takes effect with the next driver start.
# Log: C:\ps5-work\ps5-postboot.log
$ErrorActionPreference = 'Stop'
$log = 'C:\ps5-work\ps5-postboot.log'; New-Item -ItemType Directory -Force C:\ps5-work | Out-Null
function L($m) { Add-Content $log ('{0:o} {1}' -f (Get-Date), $m) }
try {
  $b = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\SharedState\StateV2'
  if (-not (Test-Path $b)) { L 'no StateV2 (bc250kmd never started yet)'; exit 0 }
  $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'   # v1.0.2
  foreach ($kv in @(@('EnableHotplugModeSet', 7), @('Ps5EdidDescriptor', 1))) {
    if ((Get-ItemProperty $pk -Name $kv[0] -ErrorAction SilentlyContinue).($kv[0]) -ne $kv[1]) { New-ItemProperty $pk -Name $kv[0] -PropertyType DWord -Value $kv[1] -Force | Out-Null; L "hotplug setting $($kv[0])=$($kv[1]) set (active from the next start)" } }
  $a = (Get-ItemProperty $b).Active; $d = "$b\Generations\$a\Data"
  $gens = @(Get-ChildItem "$b\Generations" -ErrorAction SilentlyContinue)
  $gpu = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13(FC|DB|DC|DA|F9|FA|FB|FE)' })
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
