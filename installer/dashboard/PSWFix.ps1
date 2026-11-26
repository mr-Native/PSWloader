# PSWFix.ps1 v1.1 (PSWloader v1.2; v1.1: driver reinstall forces the PSWloader package onto the devices - a driver Windows Update put on them ranks higher and pnputil kept it; -Quiet for the welcome window) - Dashboard troubleshooting (EXPERIMENTAL). Runs as SYSTEM from the task "PSWloader Fix",
# which every user may start from the Dashboard (Health > Troubleshoot). Never acts on its own findings blindly:
#   1. a fresh check (PSWSelfCheck -ChecksOnly) - only findings with a fix key are worked on; nothing found = nothing done
#   2. per fix key, the smallest step that can solve it:
#        testsigning      bcdedit testsigning on (restart)
#        driver:<svc>     the installed PSWloader driver package again from the Windows driver store (pnputil /add-driver /install)
#        device:<svc>     restart the device (pnputil /restart-device); still failing -> the driver package again (as above)
#        m2filter         PSWloader Watch now (it puts the PS5 NVMe filter on every NVMe in the slot; active after a restart)
#        watchtask        enable PSWloader Watch again
#        task:<name>      a PSWloader background task that is off or ended with an error: enable and start it (PSW* tasks only)
#        service:<name>   a PSWloader service that should run: start it (psw*/ps5*/bc250* services only)
#        hardware         other console or SSD: the console's VBIOS from the boot ESP to System32\drivers, PSWloader Watch for the
#                         new SSD, then the new hardware is taken as the reference (restart)
#        restart          not done here - the Dashboard offers it
#   3. a second check, so the Dashboard shows the state after the fixes
# Progress: ProgramData\PSWloader\dashboard\fix-progress.txt ("<pct><TAB><text>", "DONE" at the end), result fix-result.json,
# log ProgramData\PSWloader\logs\fix.log.
$ErrorActionPreference = 'Continue'
$Data = "$env:ProgramData\PSWloader"; $Dash = Join-Path $Data 'dashboard'
$Prog = Join-Path $Dash 'fix-progress.txt'; $ResFile = Join-Path $Dash 'fix-result.json'; $Log = Join-Path $Data 'logs\fix.log'
New-Item -ItemType Directory -Force $Dash, (Split-Path $Log) | Out-Null
Set-Content -LiteralPath $Prog -Value '' -Encoding UTF8
function L([string]$m) { Add-Content -LiteralPath $Log -Value ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $m) -Encoding UTF8 }
function P([int]$Pct, [string]$Text) { Add-Content -LiteralPath $Prog -Value "$Pct`t$Text" -Encoding UTF8; L "$Pct% $Text" }
$SelfCheck = @("$PSScriptRoot\PSWSelfCheck.ps1") | Where-Object { Test-Path $_ } | Select-Object -First 1   # only next to this script (C:\Program Files\PSWloader, administrators write)
function Invoke-Check { if ($SelfCheck) { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $SelfCheck -ChecksOnly | Out-Null }
                        try { Get-Content (Join-Path $Dash 'checks.json') -Raw | ConvertFrom-Json } catch { $null } }
$Inf = @{ bc250kmd = 'ps5gpu.inf'; ps5pwr = 'ps5pwr.inf'; ps5gbe = 'ps5gbe.inf'; ps5wifi = 'ps5wifi.inf'; ps5btx = 'ps5btx.inf' }
$actions = New-Object System.Collections.Generic.List[object]; $restart = $false
try { Add-Type -Namespace PswFixNative -Name PswNewDev -MemberDefinition '[DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool UpdateDriverForPlugAndPlayDevicesW(System.IntPtr hwnd, string hardwareId, string infPath, uint flags, out bool reboot);' -ErrorAction Stop } catch { }
function Act([string]$Name, [bool]$Ok, [string]$Detail) { $actions.Add([ordered]@{ name = $Name; ok = $Ok; detail = $Detail }); L "$Name ok=$Ok $Detail" }

function Install-DriverAgain([string]$Svc) {   # newest copy of our package in the Windows driver store (only administrators write there)
  $name = $Inf[$Svc]; if (-not $name) { return $false, 'no PSWloader driver package for this device' }
  $store = @(Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository\$($name -replace '\.inf$', '').inf_*\$name" -ErrorAction SilentlyContinue |
             Sort-Object { $_.Directory.LastWriteTime } -Descending)
  $src = if ($store.Count) { $store[0].FullName } else { $null }
  if (-not $src) { return $false, 'the driver package was not found - run the PSWloader updater' }
  # v1.2 (HW 26.11.2026): ps5pwr is never reinstalled while Windows runs - a stop of 104D:9107 reset the USB boot disk and ended
  # in BUGCODE_USB3_DRIVER. A bound ps5pwr only gets its service pointed at the newest package; it loads at the next start.
  if ($Svc -eq 'ps5pwr' -and @(Get-CimInstance Win32_PnPEntity -Filter "Service='ps5pwr'" -ErrorAction SilentlyContinue).Count) {
    $img = '\SystemRoot\System32\DriverStore\FileRepository\' + (Split-Path -Leaf (Split-Path -Parent $src)) + '\ps5pwr.sys'
    Set-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr' -Name ImagePath -Value $img -Type ExpandString
    $script:restart = $true
    return $true, "ps5pwr loads $img at the next start (never reinstalled while Windows runs)" }
  $out = (& pnputil.exe /add-driver $src /install 2>&1) -join ' '
  $ok = ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 3010 -or $out -match '(?i)up to date|already')
  if ($LASTEXITCODE -eq 3010) { $script:restart = $true }
  $msg = "pnputil exit $LASTEXITCODE"
  # v1.1: devices of this package that still have another driver (e.g. one from Windows Update, which ranks higher) or a
  # problem get the PSWloader package forced (UpdateDriverForPlugAndPlayDevices, INSTALLFLAG_FORCE)
  $ids = @([regex]::Matches((Get-Content -LiteralPath $src -Raw), '(?i)PCI\\VEN_[0-9A-F]{4}&DEV_[0-9A-F]{4}(&SUBSYS_[0-9A-F]{8})?(&REV_[0-9A-F]{2})?') | ForEach-Object { $_.Value.ToUpper() } | Sort-Object -Unique)
  foreach ($dev in @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'PCI\*' })) {
    $hw = @($dev.HardwareID | ForEach-Object { "$_".ToUpper() }); $hit = @($hw | Where-Object { $ids -contains $_ }) | Select-Object -First 1
    if (-not $hit -or ($dev.Service -eq $Svc -and $dev.ConfigManagerErrorCode -eq 0)) { continue }
    $rb = $false; $r = [PswFixNative.PswNewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $hit, $src, 1, [ref]$rb)
    $msg += "; forced onto $hit (was $(if ($dev.Service) { $dev.Service } else { 'no driver' }), code $($dev.ConfigManagerErrorCode)): $(if ($r) { 'ok' } else { 'error ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error() })"
    if ($r) { $script:restart = $true } else { $ok = $false }
  }
  return $ok, "$msg ($src)"
}
# v1.1 (HW 26.11.): the GPU driver creates its state at its first start only while Parameters holds no values besides
# DefaultsV2 - with the 1230 INF's hotplug values it failed on every fresh install (code 37, 0xC0000182). Values out,
# device restarted, state created, values back (they are read at the next driver start).
function Repair-GpuState {
  $p = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'; $st = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\SharedState\StateV2'
  if ((Test-Path $st) -or -not (Test-Path $p)) { return $null }
  $vals = @((Get-Item $p).Property); if (-not $vals.Count) { return $null }
  $keep = @{}; foreach ($v in $vals) { $keep[$v] = (Get-ItemProperty $p -Name $v).$v; Remove-ItemProperty $p -Name $v }
  foreach ($dev in Get-SvcDevices 'bc250kmd') { & pnputil.exe /restart-device "$($dev.PNPDeviceID)" 2>&1 | Out-Null }
  Start-Sleep -Seconds 12
  $ok = Test-Path $st
  foreach ($k in $keep.Keys) { New-ItemProperty $p -Name $k -Value $keep[$k] -PropertyType DWord -Force | Out-Null }
  $bad = @(Get-SvcDevices 'bc250kmd' | Where-Object { $_.ConfigManagerErrorCode -ne 0 })
  return @(($ok -and -not $bad.Count), "GPU driver state $(if ($ok) { 'created' } else { 'still missing' }), settings ($($vals -join ', ')) put back")
}
function Get-SvcDevices([string]$Svc) { @(Get-CimInstance Win32_PnPEntity -Filter "Service='$Svc'" -ErrorAction SilentlyContinue) }

try {
  L '--- troubleshooting started'
  P 5 'Checking PSWloader'
  $before = Invoke-Check
  $todo = @(@($before.checks) | Where-Object { $_.fix -and $_.state -ge 1 -and $_.fix -ne 'restart' })
  if (-not $todo.Count) { P 100 'Nothing to fix'; Act 'Check' $true 'no fixable problem found'; return }
  $n = 0
  foreach ($c in $todo) {
    $n++; $pct = 10 + [int](70 * ($n - 1) / $todo.Count)
    $key, $arg = ([string]$c.fix) -split ':', 2
    switch ($key) {
      'testsigning' {
        P $pct 'Turning on test signing'
        $out = (& bcdedit.exe /set '{current}' testsigning on 2>&1) -join ' '
        Act $c.name ($LASTEXITCODE -eq 0) "bcdedit: $out"; $restart = $true }
      'driver' {
        if ($arg -eq 'bc250kmd') { $g = Repair-GpuState; if ($g) { Act $c.name $g[0] $g[1]; if ($g[0]) { $restart = $true; break } } }   # v1.1
        P $pct "Installing the $($c.name.ToLower()) again"
        $ok, $d = Install-DriverAgain $arg; Act $c.name $ok $d; $restart = $true }
      'device' {
        if ($arg -eq 'bc250kmd') { $g = Repair-GpuState; if ($g) { Act $c.name $g[0] $g[1]; if ($g[0]) { $restart = $true; break } } }   # v1.1
        P $pct "Restarting the device ($($c.name))"
        foreach ($dev in Get-SvcDevices $arg) { & pnputil.exe /restart-device "$($dev.PNPDeviceID)" 2>&1 | Out-Null }
        Start-Sleep -Seconds 6
        $bad = @(Get-SvcDevices $arg | Where-Object { $_.ConfigManagerErrorCode -ne 0 })
        if (-not $bad.Count) { Act $c.name $true 'device restarted, it works again' }
        else { P $pct "Installing the $($c.name.ToLower()) again"; $ok, $d = Install-DriverAgain $arg
               Act $c.name $ok "device still code $($bad[0].ConfigManagerErrorCode) after the restart; $d"; $restart = $true } }
      'm2filter' {
        P $pct 'Setting up the internal M.2 SSD'
        $ok = $true; try { Start-ScheduledTask -TaskName 'PSWloader Watch' -ErrorAction Stop; Start-Sleep -Seconds 8 } catch { $ok = $false }
        Act $c.name $ok $(if ($ok) { 'PSWloader Watch added the PS5 NVMe filter' } else { 'PSWloader Watch is missing - run the PSWloader updater' }); $restart = $true }
      'usbbot' {   # v1.2: USB disks as Bulk-Only - PSWloader Watch sets it (UASPStor -> pswbot.sys), active after the restart
        P $pct 'Switching USB disks to Bulk-Only'
        $ok = $true; try { Start-ScheduledTask -TaskName 'PSWloader Watch' -ErrorAction Stop; Start-Sleep -Seconds 8 } catch { $ok = $false }
        $ip = [string](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\UASPStor' -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
        $ok = $ok -and $ip -match '(?i)\\pswbot\.sys$'
        Act $c.name $ok $(if ($ok) { 'set - USB disks run Bulk-Only after the restart' } else { 'PSWloader Watch is missing or did not set it - run the PSWloader updater' }); $restart = $true }
      'watchtask' {
        P $pct 'Turning PSWloader Watch on'
        $ok = $true; try { Enable-ScheduledTask -TaskName 'PSWloader Watch' -ErrorAction Stop | Out-Null; Start-ScheduledTask -TaskName 'PSWloader Watch' } catch { $ok = $false }
        Act $c.name $ok $(if ($ok) { 'enabled' } else { 'the task is missing - run the PSWloader updater' }) }
      'task' {
        P $pct "Starting $($c.name.ToLower())"
        if ($arg -notmatch '^PSW[\w -]+$') { Act $c.name $false "not a PSWloader task: $arg"; break }
        try { Enable-ScheduledTask -TaskName $arg -ErrorAction Stop | Out-Null; Start-ScheduledTask -TaskName $arg -ErrorAction Stop; Start-Sleep -Seconds 8
              $tk = Get-ScheduledTask -TaskName $arg; $ti = $tk | Get-ScheduledTaskInfo
              $ok = ($tk.State -eq 'Running' -or [int64]$ti.LastTaskResult -eq 0)
              Act $c.name $ok $(if ($ok) { "started (state $($tk.State))" } else { "started, but it ended again with error 0x{0:X} - create a support zip" -f [int64]$ti.LastTaskResult }) }
        catch { Act $c.name $false $_.Exception.Message } }
      'service' {
        P $pct "Starting the service $arg"
        if ($arg -notmatch '^(psw|ps5|bc250)[\w-]*$') { Act $c.name $false "not a PSWloader service: $arg"; break }
        try { Start-Service -Name $arg -ErrorAction Stop; Start-Sleep -Seconds 3; $st = (Get-Service -Name $arg).Status
              Act $c.name ($st -eq 'Running') "state $st" } catch { Act $c.name $false $_.Exception.Message } }
      'hardware' {
        P $pct 'Setting PSWloader up for this console'
        # the VBIOS of this console: the firmware writes \PS5VBIOS.BIN to the boot ESP at every start
        $free = [char[]](70..90) | Where-Object { -not (Test-Path "$($_):\") } | Select-Object -Last 1; $vb = $null
        if ($free) { & mountvol.exe "$($free):" /S 2>&1 | Out-Null }
        try { foreach ($cand in @("$($free):\PS5VBIOS.BIN") + @(Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemType -match 'FAT' } | ForEach-Object { "$($_.DriveLetter):\PS5VBIOS.BIN" })) {
                if (Test-Path -LiteralPath $cand) { $b = [IO.File]::ReadAllBytes($cand); if ($b.Length -ge 32768 -and $b[0] -eq 0x55 -and $b[1] -eq 0xAA) { $vb = $b; break } } } }
        finally { if ($free) { & mountvol.exe "$($free):" /D 2>&1 | Out-Null } }
        $dst = "$env:SystemRoot\System32\drivers\ps5-vbios.bin"
        if ($vb) {
          $same = (Test-Path $dst) -and ([Convert]::ToBase64String([Security.Cryptography.SHA256]::Create().ComputeHash($vb)) -eq
                                         [Convert]::ToBase64String([Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes($dst))))
          if (-not $same) { if (Test-Path $dst) { Copy-Item $dst "$dst.old" -Force }; [IO.File]::WriteAllBytes($dst, $vb) }
          Act 'Graphics BIOS' $true $(if ($same) { 'already the one of this console' } else { 'taken from this console' })
        } else { Act 'Graphics BIOS' $false 'not found on the boot disk - start Windows through PSWloader once' }
        try { Start-ScheduledTask -TaskName 'PSWloader Watch' -ErrorAction Stop; Start-Sleep -Seconds 8 } catch { }
        Remove-Item -LiteralPath "$Dash\hardware.json" -Force -ErrorAction SilentlyContinue   # the next check takes this hardware as the reference
        Act $c.name ([bool]$vb) 'this console is now the reference'; $restart = $true }
      default { Act $c.name $false "no automatic fix for '$($c.fix)'" }
    }
  }
  P 85 'Checking again'
  $after = Invoke-Check
  $left = @(@($after.checks) | Where-Object { $_.state -eq 2 }).Count
  P 100 $(if ($restart) { 'Done - restart Windows to finish' } elseif ($left) { "Done - $left problem$(if ($left -gt 1) { 's' }) left" } else { 'Done - no problems left' })
} catch { L "error: $($_.Exception.Message)"; Act 'Troubleshooting' $false $_.Exception.Message; P 100 'Stopped with an error' }
finally {
  # .ToArray(): @() around a generic list throws "Argument types do not match" on Windows PowerShell 5.1
  [ordered]@{ time = (Get-Date).ToString('o'); restart = [bool]$restart; actions = $actions.ToArray() } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ResFile -Encoding UTF8
  Add-Content -LiteralPath $Prog -Value 'DONE' -Encoding UTF8; L '--- troubleshooting ended'
}
