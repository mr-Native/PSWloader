# install-ps5-powerbutton.ps1 v1.0 (16.11.2026) - run ELEVATED. -Mode Install | Remove | Status
# Install: copies ps5-powerbutton.exe (next to this script) to C:\ps5-work\, registers task "PS5-PowerButton" (SYSTEM, at
# startup, no time limit, restart every 1 min up to 999x on failure) and starts it. Needs ps5pwr v1.10 for presses; with an
# older driver the listener only logs "event not available" every start and waits.
param([ValidateSet('Install', 'Remove', 'Status')][string]$Mode = 'Status')
$ErrorActionPreference = 'Stop'
$dst = 'C:\ps5-work\ps5-powerbutton.exe'; $name = 'PS5-PowerButton'
if ($Mode -eq 'Install') {
  New-Item -ItemType Directory -Force C:\ps5-work | Out-Null
  Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
  Get-Process ps5-powerbutton -ErrorAction SilentlyContinue | Stop-Process -Force
  Copy-Item (Join-Path $PSScriptRoot 'ps5-powerbutton.exe') $dst -Force
  $s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartInterval (New-TimeSpan -Minutes 1) -RestartCount 999
  Register-ScheduledTask -TaskName $name -Action (New-ScheduledTaskAction -Execute $dst) -Trigger (New-ScheduledTaskTrigger -AtStartup) `
    -Principal (New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest) -Settings $s -Force | Out-Null
  Start-ScheduledTask -TaskName $name; Start-Sleep 2
}
if ($Mode -eq 'Remove') {
  Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
  Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
  Get-Process ps5-powerbutton -ErrorAction SilentlyContinue | Stop-Process -Force
  'removed (exe and log stay in C:\ps5-work)'
}
"task: " + ((Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue).State)
"process: " + (@(Get-Process ps5-powerbutton -ErrorAction SilentlyContinue).Count)
"PBUTTONACTION: " + ((powercfg /q SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION | Select-String '0x[0-9a-f]{8}' | Select-Object -Last 2) -join ' ')
"EnablePowerButton: " + (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters -Name EnablePowerButton -ErrorAction SilentlyContinue).EnablePowerButton
Get-Content C:\ps5-work\powerbutton.log -Tail 5 -ErrorAction SilentlyContinue
