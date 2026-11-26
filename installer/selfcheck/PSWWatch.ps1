<#
PSWWatch.ps1 v1.3 (v1.3: USB disks run BOT - UASPStor loads pswbot.sys; v1.2: ps5nvmsi off the SCSIAdapter class filters; v1.1: boot guard against INACCESSIBLE_BOOT_DEVICE, see below) - background log for sporadic problems. Runs as SYSTEM from the task "PSWloader Watch"
(at startup and every minute) and appends to C:\PSWloader\logs\watch\watch.log:
  - every new warning, error and critical event of the System and Application logs (minus known noise),
    plus boot, shutdown and restart events (who asked for a restart, unexpected shutdowns, bug checks)
  - every change of a device problem (for example the GPU going to Code 43, a USB device dropping off)
  - at each start: when this script last ran before (= roughly when the previous session ended) and a copy of the
    firmware logs on the EFI system partition (they are overwritten at the next start) to ...\watch\uefi
Old files are rotated (watch.log at 5 MB, 6 files kept; the newest 30 firmware log copies kept). It also rotates the other
automatic PSWloader logs in C:\ps5-work, C:\PSWSetup\Update and C:\ProgramData\PSWloader\logs (5 MB -> <name>.old).
PSWSelfCheck.ps1 puts the whole folder into its zip.
#>
$ErrorActionPreference = 'SilentlyContinue'
$Dir = 'C:\PSWloader\logs\watch'
$Log = Join-Path $Dir 'watch.log'
$StateFile = Join-Path $Dir 'state.json'
New-Item -ItemType Directory -Force -Path $Dir | Out-Null

function W([string]$m) { Add-Content -Path $Log -Value $m -Encoding UTF8 }
function T($d) { '{0:yyyy-MM-dd HH:mm:ss}' -f $d }

# one run at a time (the task also says IgnoreNew)
$mutex = New-Object Threading.Mutex($false, 'Global\PSWloaderWatch')
if (-not $mutex.WaitOne(0)) { exit 0 }
try {
  # rotation
  if ((Test-Path $Log) -and (Get-Item $Log).Length -gt 5MB) {
    Move-Item $Log (Join-Path $Dir ('watch-{0:yyyy-MM-dd_HH-mm-ss}.log' -f (Get-Date))) -Force
    Get-ChildItem $Dir -Filter 'watch-*.log' | Sort-Object Name -Descending | Select-Object -Skip 5 | Remove-Item -Force
  }

  # every other automatic PSWloader log (thermal manager/supervisor, power button, boot task, update check, ...): at 5 MB the
  # file becomes <name>.old<ext> (replacing the previous one), so each log needs at most 10 MB. The writers open the file
  # for every line, so the rename happens between two writes; a file in use is simply tried again next minute.
  foreach ($d in 'C:\ps5-work', 'C:\PSWSetup\Update', 'C:\ProgramData\PSWloader\logs') {
    Get-ChildItem -Path "$d\*" -Include *.log, *.jsonl, *.txt -File -ErrorAction SilentlyContinue |
      Where-Object { $_.Length -gt 5MB -and $_.BaseName -notlike '*.old' } | ForEach-Object {
        $old = Join-Path $_.DirectoryName ($_.BaseName + '.old' + $_.Extension)
        Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $_.FullName $old -ErrorAction SilentlyContinue
      }
  }

  # v1.1 boot guard (INACCESSIBLE_BOOT_DEVICE 0x7B): Windows switches boot-start drivers a start did not need to "on demand"
  # (Services\<name>\StartOverride, F10). A disk that later boots another way - internal M.2 <-> USB, a clone, a restart through
  # a different path - then finds its storage chain unloaded and stops with 0x7B (P2 C0000034). Every minute and at startup:
  #  1. the whole boot storage chain starts at boot: NVMe + PS5 filters, disk/partition/volume, USB xHCI/hub/storage
  #  2. BootDriverFlags carries USB and USB3 disk boot (0x14), so a Windows installed internally also boots from USB
  #  3. every NVMe instance (PCI class 01/08/02, any vendor - only the M.2 slot carries one) Windows has seen gets ps5nvmsi as lower filter and the MSI limit 1 (also on a USB Windows,
  #     so it boots once the disk sits in the internal slot); only complete instance keys (with Service) are touched, never created
  #  4. ps5m2flt stays the first upper filter of the disk class (nested GPT of the internal M.2)
  $cs = 'HKLM:\SYSTEM\CurrentControlSet'
  foreach ($svc in 'stornvme', 'ps5nvmsi', 'ps5m2flt', 'disk', 'partmgr', 'volmgr', 'pci', 'USBXHCI', 'UCX01000', 'usbhub3', 'USBSTOR', 'UASPStor') {
    $k = "$cs\Services\$svc"
    if (-not (Test-Path $k)) { continue }
    if (Test-Path "$k\StartOverride") { Remove-Item "$k\StartOverride" -Recurse -Force; W "$(T (Get-Date)) boot guard: removed StartOverride of $svc" }
    $sv = (Get-ItemProperty $k -Name Start -ErrorAction SilentlyContinue).Start
    if ($null -ne $sv -and $sv -ne 0) { Set-ItemProperty $k -Name Start -Value 0 -Type DWord; W "$(T (Get-Date)) boot guard: $svc Start $sv -> 0" }
  }
  # 5. v1.3 (PSWloader v1.2, HW 26.11.2026): USB disks run Bulk-Only (BOT), not UAS. A USB SSD at 10 Gbit/s with UAS on the PS5
  #    xHCI got UASPStor resets (129), BUGCODE_USB3_DRIVER (0x144) and 0x7B; the same disk as BOT was stable. Generic for every
  #    UAS disk: the UASPStor service loads pswbot.sys, a copy of Windows' own USBSTOR.SYS (one image cannot serve two services).
  #    Only registry and a file - it takes effect at the next start, a running disk is never switched.
  $drv = "$env:SystemRoot\System32\drivers"
  if ((Test-Path "$cs\Services\UASPStor") -and (Test-Path "$drv\USBSTOR.SYS")) {
    if (-not (Test-Path "$drv\pswbot.sys")) { Copy-Item "$drv\USBSTOR.SYS" "$drv\pswbot.sys" -Force; W "$(T (Get-Date)) boot guard: pswbot.sys = copy of USBSTOR.SYS" }
    $uip = [string](Get-ItemProperty "$cs\Services\UASPStor" -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
    if ((Test-Path "$drv\pswbot.sys") -and $uip -notmatch '(?i)\\pswbot\.sys$') {
      Set-ItemProperty "$cs\Services\UASPStor" -Name ImagePath -Value '\SystemRoot\System32\drivers\pswbot.sys' -Type ExpandString
      W "$(T (Get-Date)) boot guard: UAS disks run BOT from the next start (UASPStor image $uip -> pswbot.sys)" }
  }
  $bdf = [int](Get-ItemProperty "$cs\Control" -Name BootDriverFlags -ErrorAction SilentlyContinue).BootDriverFlags
  if (($bdf -band 0x14) -ne 0x14) { Set-ItemProperty "$cs\Control" -Name BootDriverFlags -Value ($bdf -bor 0x14) -Type DWord; W "$(T (Get-Date)) boot guard: BootDriverFlags $bdf -> $($bdf -bor 0x14)" }
  if (Test-Path "$cs\Services\ps5nvmsi") {
    $nvmeN = 0
    foreach ($inst in Get-ChildItem "$cs\Enum\PCI\*\*" -ErrorAction SilentlyContinue) {   # any vendor (issue #12: Samsung 144D:A80C)
      $ip = Get-ItemProperty $inst.PSPath -ErrorAction SilentlyContinue
      if (-not $ip.Service -or ((@($ip.CompatibleIDs) + @($ip.HardwareID)) -join ' ') -notmatch '(?i)CC_010802') { continue }
      $nvmeN++
      if (@($ip.LowerFilters) -notcontains 'ps5nvmsi') {   # v1.2: put in front, never drop another filter of the instance
        Set-ItemProperty $inst.PSPath -Name LowerFilters -Value ([string[]](@('ps5nvmsi') + @($ip.LowerFilters | Where-Object { $_ }))) -Type MultiString
        W "$(T (Get-Date)) boot guard: ps5nvmsi lower filter on $($inst.PSChildName)" }
      $mk = "$($inst.PSPath)\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties"
      if ((Get-ItemProperty $mk -Name MessageNumberLimit -ErrorAction SilentlyContinue).MessageNumberLimit -ne 1) {
        New-Item $mk -Force | Out-Null; Set-ItemProperty $mk -Name MessageNumberLimit -Value 1 -Type DWord; W "$(T (Get-Date)) boot guard: MSI limit 1 on $($inst.PSChildName)" }
    }
    # v1.2 (USB-NVMe Windows 0x7B, HW 25.11.): ps5nvmsi must not stay a SCSIAdapter CLASS filter (set by the WinPE hive patch for the first
    # boot of a fresh install). It also sits under UASPStor of a USB boot disk - if it does not load, that disk fails (0x7B, P2 C0000034).
    # Off once every NVMe instance carries the device filter (step 3 above), or when Windows does not run from an NVMe-bus disk.
    $sc = "$cs\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}"
    $clf = @((Get-ItemProperty $sc -Name LowerFilters -ErrorAction SilentlyContinue).LowerFilters | Where-Object { $_ })
    if ($clf -contains 'ps5nvmsi') {
      $bootBus = try { [string](Get-Partition -DriveLetter $env:SystemDrive[0] -ErrorAction Stop | Get-Disk -ErrorAction Stop).BusType } catch { '' }
      if ($nvmeN -gt 0 -or ($bootBus -and $bootBus -ne 'NVMe')) {
        $rest = @($clf | Where-Object { $_ -ine 'ps5nvmsi' })
        if ($rest.Count) { Set-ItemProperty $sc -Name LowerFilters -Value ([string[]]$rest) -Type MultiString } else { Remove-ItemProperty $sc -Name LowerFilters -ErrorAction SilentlyContinue }
        W "$(T (Get-Date)) boot guard: ps5nvmsi removed from the SCSIAdapter class filters (NVMe instances $nvmeN, system disk bus $bootBus)" }
    }
  }
  if (Test-Path "$cs\Services\ps5m2flt") {
    $dc = "$cs\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
    $uf = @((Get-ItemProperty $dc -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters | Where-Object { $_ })
    if ($uf.Count -eq 0 -or $uf[0] -ine 'ps5m2flt') {
      Set-ItemProperty $dc -Name UpperFilters -Value ([string[]](@('ps5m2flt') + @($uf | Where-Object { $_ -ine 'ps5m2flt' }))) -Type MultiString
      W "$(T (Get-Date)) boot guard: ps5m2flt first disk upper filter (was: $($uf -join ','))" }
  }

  $st = $null
  if (Test-Path $StateFile) { $st = Get-Content $StateFile -Raw | ConvertFrom-Json }
  $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
  $bootKey = T $boot
  $now = Get-Date
  $rec = @{ System = 0; Application = 0 }
  $devs = @()
  if ($st) {
    if ($st.rec.System) { $rec.System = [long]$st.rec.System }
    if ($st.rec.Application) { $rec.Application = [long]$st.rec.Application }
    if ($st.devs) { $devs = @($st.devs) }
  }
  $newBoot = -not $st -or $st.boot -ne $bootKey
  $fwlogs = @{}; if ($st -and $st.fwlogs) { foreach ($pr in $st.fwlogs.PSObject.Properties) { $fwlogs[$pr.Name] = $pr.Value } }

  if ($newBoot) {
    $os = Get-CimInstance Win32_OperatingSystem
    $ver = ''; if (Test-Path 'C:\ProgramData\PSWloader\version.json') { $ver = (Get-Content 'C:\ProgramData\PSWloader\version.json' -Raw | ConvertFrom-Json).version }
    W ''
    W "==== $(T $now) start: Windows booted $bootKey, build $($os.BuildNumber), PSWloader $ver"
    if ($st) { W "     previous session: booted $($st.boot), last seen running $($st.seen)" }
    else { W '     first run of PSWWatch on this installation' }
    $devs = @()   # list every device problem again after a start

    # firmware logs of this start (EFI system partition), kept before the next start overwrites them
    $u = Join-Path $Dir 'uefi'; New-Item -ItemType Directory -Force -Path $u | Out-Null
    # PSWloader v1.2: an ESP that already has a drive letter is used as it is (mountvol /s fails then, no logs were collected)
    $letter = $null; $espTemp = $false
    $sp = Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.IsSystem } | Select-Object -First 1
    if ($sp -and $sp.DriveLetter -and $sp.DriveLetter -ne [char]0) { $letter = [string]$sp.DriveLetter }
    else { foreach ($c in [char[]]'SRQPONMLKJIHGT') { if (-not (Test-Path "${c}:\")) { $letter = $c; $espTemp = $true; break } } }
    if ($letter) {
      if ($espTemp) { mountvol "${letter}:" /s 2>$null | Out-Null }
      if (Test-Path "${letter}:\") {
        # the firmware writes its logs only into prepared slot files (16-byte marker + 64 KiB ring, it never creates files
        # before Windows): create missing ones, so every later start and restart leaves its log here
        foreach ($n in 'PS5UEFI.LOG', 'PS5WARM.LOG', 'PS5RESCU.LOG') {
          $p = "${letter}:\$n"
          if (-not (Test-Path $p)) {
            $b = New-Object byte[] 65568; [Text.Encoding]::ASCII.GetBytes('PS5LOGFILE1').CopyTo($b, 0)
            try { [IO.File]::WriteAllBytes($p, $b); W "     created the firmware log slot $n on the EFI partition" } catch { W "     could not create $n : $($_.Exception.Message)" }
          }
        }
        # the firmware has no reliable clock, so the file dates are useless: a log is copied when its content changed
        $tag = '{0:yyyy-MM-dd_HH-mm-ss}' -f $boot; $got = @(); $hashes = @{}
        if ($st -and $st.fwlogs) { foreach ($pr in $st.fwlogs.PSObject.Properties) { $hashes[$pr.Name] = $pr.Value } }
        foreach ($n in 'PS5UEFI.LOG', 'PS5WARM.LOG', 'PS5RESCU.LOG', 'PS5DIAG.LOG') {
          $p = "${letter}:\$n"
          if (-not (Test-Path $p)) { continue }
          $h = (Get-FileHash $p -Algorithm SHA256).Hash
          if ($hashes[$n] -ne $h) { Copy-Item $p (Join-Path $u "$tag-$n") -Force; $got += $n; $hashes[$n] = $h }
        }
        $fwlogs = $hashes
        if ($espTemp) { mountvol "${letter}:" /d 2>$null | Out-Null }
        W "     firmware logs of this start: $(if ($got) { $got -join ', ' } else { 'unchanged since the last start' })"
      }
    }
    Get-ChildItem $u -File | Sort-Object Name -Descending | Select-Object -Skip 30 | Remove-Item -Force
  }

  # events: warnings/errors/critical, plus start/stop/restart events of any level
  $noise = 'Microsoft-Windows-DistributedCOM', 'Microsoft-Windows-Security-SPP', 'Microsoft-Windows-Perflib', 'Microsoft-Windows-WMI',
           'Microsoft-Windows-User Device Registration', 'Microsoft-Windows-Search', 'edgeupdate', 'gupdate', 'VSS'
  $sel = @{
    System      = '(Level=1 or Level=2 or Level=3 or (EventID=12 and Provider[@Name=''Microsoft-Windows-Kernel-General'']) or (EventID=13 and Provider[@Name=''Microsoft-Windows-Kernel-General'']) or EventID=1074 or EventID=6008 or EventID=1001)'
    Application = '(Level=1 or Level=2 or Level=3 or (EventID=1001 and Provider[@Name=''Windows Error Reporting'']))'
  }
  foreach ($ln in 'System', 'Application') {
    $newest = (Get-WinEvent -LogName $ln -MaxEvents 1).RecordId
    if ($null -eq $newest) { continue }
    if ($newest -lt $rec[$ln]) { W "$(T $now) note  $ln log was cleared"; $rec[$ln] = 0 }
    $first = $rec[$ln] -eq 0
    $xp = "*[System[EventRecordID>$($rec[$ln]) and $($sel[$ln])" + $(if ($first) { ' and TimeCreated[timediff(@SystemTime) <= 86400000]' } else { '' }) + ']]'
    $ev = @(Get-WinEvent -LogName $ln -FilterXPath $xp -MaxEvents 2000 | Sort-Object RecordId)
    $last = ''; $rep = 0
    foreach ($e in $ev) {
      if ($noise -contains $e.ProviderName) { continue }
      $lv = switch ($e.Level) { 1 { 'CRIT ' } 2 { 'ERROR' } 3 { 'WARN ' } default { 'INFO ' } }
      $msg = $e.Message
      if (-not $msg) { $msg = ($e.Properties | ForEach-Object { $_.Value }) -join ' | ' }
      $msg = ($msg -replace '\s+', ' ').Trim()
      # error reports: only kernel-side ones (LiveKernelEvent = GPU/driver reset, BlueScreen, WHEA); app crashes come as 'Application Error'
      if ($e.Id -eq 1001 -and $e.ProviderName -eq 'Windows Error Reporting' -and $msg -notmatch 'LiveKernelEvent|BlueScreen|WHEA') { continue }
      if ($msg.Length -gt 400) { $msg = $msg.Substring(0, 400) + ' ...' }
      $key = "$lv $ln $($e.ProviderName) $($e.Id): $msg"
      if ($key -eq $last) { $rep++; continue }
      if ($rep) { W "                    (same line $rep more times)"; $rep = 0 }
      W "$(T $e.TimeCreated) $key"; $last = $key
    }
    if ($rep) { W "                    (same line $rep more times)" }
    $rec[$ln] = $newest
  }

  # device problems: log what appeared and what went away
  $cur = @(Get-CimInstance Win32_PnPEntity -Filter 'ConfigManagerErrorCode<>0' | ForEach-Object { "Code $($_.ConfigManagerErrorCode) | $($_.Name) | $($_.PNPDeviceID)" } | Sort-Object)
  foreach ($d in $cur) { if ($devs -notcontains $d) { W "$(T $now) DEV   problem: $d" } }
  foreach ($d in $devs) { if ($cur -notcontains $d) { W "$(T $now) DEV   cleared: $d" } }

  $o = [ordered]@{ boot = $bootKey; seen = (T $now); rec = $rec; devs = $cur; fwlogs = $fwlogs }
  $tmp = "$StateFile.tmp"
  $o | ConvertTo-Json -Depth 4 | Set-Content -Path $tmp -Encoding UTF8
  Move-Item $tmp $StateFile -Force
} finally { $mutex.ReleaseMutex() }
