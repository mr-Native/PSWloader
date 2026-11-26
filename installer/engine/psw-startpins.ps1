# psw-startpins.ps1 v1.1 (PSWloader v1.2) - pins "PSWloader Dashboard" to the Start menu once (v1.0: the two separate entries).
# Windows 11 has no API for an app to pin itself. This uses the Start policy ConfigureStartPins (applyOnce) - which REPLACES the
# pinned list - so the user's current pins are exported first and ours are appended; nothing is written when the export is
# empty or unreadable. Run by the logon task "PSWloader Start pins" (registered by PSWSetup and the updater), in two sign-ins:
#   1st: export, append, write the policy (the pins appear at the next sign-in);
#   2nd: our pins are in the layout -> the policy is removed again (the user can unpin/rearrange freely), task removed.
# Log: %ProgramData%\PSWloader\startpins.log
$ErrorActionPreference = 'Stop'
$log = "$env:ProgramData\PSWloader\startpins.log"
function L([string]$m) { try { Add-Content -LiteralPath $log -Value ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $m) } catch { } }
$pol = 'HKLM:\SOFTWARE\Microsoft\PolicyManager\current\device\Start'
$want = @('%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs\PSWloader Dashboard.lnk')
$gone = 'PSWloader UEFI Boot Manager.lnk', 'PSWloader Support.lnk', 'PSWloader Display.lnk'   # v1.1: entries of older versions, now in the Dashboard
function Done { L 'done - task removed'; Unregister-ScheduledTask -TaskName 'PSWloader Start pins' -Confirm:$false -ErrorAction SilentlyContinue; exit 0 }
try {
  $want = @($want | Where-Object { Test-Path -LiteralPath ([Environment]::ExpandEnvironmentVariables($_)) })
  if (-not $want.Count) { L 'no PSWloader shortcuts in the Start menu'; Done }
  $tmp = Join-Path $env:TEMP "psw-start-$PID.json"
  Export-StartLayout -Path $tmp
  $j = Get-Content -LiteralPath $tmp -Raw | ConvertFrom-Json; Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
  $list = @($j.pinnedList | Where-Object { $l = [string]$_.desktopAppLink; -not ($gone | Where-Object { $l -like "*\$_" }) })
  if ($list.Count -lt 1) { L 'export has no pinned list - nothing changed'; exit 0 }
  $have = @($list | ForEach-Object { $_.desktopAppLink } | Where-Object { $_ })
  $missing = @($want | Where-Object { $have -notcontains $_ })
  $set = (Get-ItemProperty -Path $pol -Name ConfigureStartPins -ErrorAction SilentlyContinue).ConfigureStartPins
  if (-not $missing.Count) {   # 2nd sign-in (or already pinned): remove our policy again
    if ($set -and $set -match 'PSWloader') {
      Remove-ItemProperty -Path $pol -Name ConfigureStartPins, ConfigureStartPins_ProviderSet -ErrorAction SilentlyContinue
      L 'pins present - policy removed'
    } else { L 'pins already present' }
    Done
  }
  if ($set -and $set -match 'PSWloader') { L 'policy already written, waiting for the next sign-in'; exit 0 }
  foreach ($m in $missing) { $list += [pscustomobject]@{ desktopAppLink = $m } }
  $json = [ordered]@{ pinnedList = $list; applyOnce = $true } | ConvertTo-Json -Depth 6 -Compress
  New-Item -Path $pol -Force | Out-Null
  Set-ItemProperty -Path $pol -Name ConfigureStartPins -Value $json -Type String
  Set-ItemProperty -Path $pol -Name ConfigureStartPins_ProviderSet -Value 1 -Type DWord
  L "policy written: $($list.Count) pins ($($missing.Count) added) - applied at the next sign-in"
} catch { L "error: $($_.Exception.Message)" }
