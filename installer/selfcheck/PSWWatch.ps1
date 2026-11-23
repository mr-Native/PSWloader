<#
PSWWatch.ps1 v1.0 - background log for sporadic problems. Runs as SYSTEM from the task "PSWloader Watch"
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
    $letter = $null
    foreach ($c in [char[]]'SRQPONMLKJIHGT') { if (-not (Test-Path "${c}:\")) { $letter = $c; break } }
    if ($letter) {
      mountvol "${letter}:" /s 2>$null | Out-Null
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
        mountvol "${letter}:" /d 2>$null | Out-Null
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
