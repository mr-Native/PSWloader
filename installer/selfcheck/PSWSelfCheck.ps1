<#
PSWSelfCheck.ps1 v1.1 - collects everything support needs into one zip.
Writes C:\PSWloader\selfcheck\results\PSW-selfcheck-<yyyy-MM-dd_HH-mm-ss>.zip and opens the folder.
Read-only on the system; the only change is a temporary drive letter for the EFI system partition (removed again).
Run as administrator (PSWSelfCheck.cmd does that).
#>
param([switch]$NoExplorer)
$ErrorActionPreference = 'Continue'
$Ver = '1.1'
$Stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$Base = 'C:\PSWloader\selfcheck\results'
$Work = Join-Path $Base "PSW-selfcheck-$Stamp"
$Zip = "$Work.zip"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Log = Join-Path $Work 'selfcheck.log'
function L([string]$m) { $l = '{0:HH:mm:ss} {1}' -f (Get-Date), $m; Add-Content -Path $Log -Value $l; Write-Host $l }
function Out-Txt([string]$Name, $Data) { $Data | Out-String -Width 400 | Set-Content -Path (Join-Path $Work $Name) -Encoding UTF8 }
function Step([string]$Name, [scriptblock]$Do) {
  try { & $Do; L "ok    $Name" } catch { L "error $Name : $($_.Exception.Message)" }
}
function Copy-IfThere([string]$Src, [string]$DstDir) {
  if (Test-Path -LiteralPath $Src) {
    New-Item -ItemType Directory -Force -Path $DstDir | Out-Null
    Copy-Item -LiteralPath $Src -Destination $DstDir -Recurse -Force -ErrorAction SilentlyContinue
  }
}
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
L "PSWSelfCheck $Ver  admin=$admin  output=$Zip"

# 1. summary
Step 'summary' {
  $os = Get-CimInstance Win32_OperatingSystem
  $cs = Get-CimInstance Win32_ComputerSystem
  $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
  $vj = $null; if (Test-Path 'C:\ProgramData\PSWloader\version.json') { $vj = Get-Content 'C:\ProgramData\PSWloader\version.json' -Raw | ConvertFrom-Json }
  $gpu = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13(FC|DB|DC|DA|F9|FA|FB|FE)' } | Select-Object -First 1
  $pwr = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_104D&DEV_9107' } | Select-Object -First 1
  $drv = { param($d) if ($d) { ($d | Get-PnpDeviceProperty DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data } }
  $bad = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.Status -ne 'OK' })
  $s = [ordered]@{
    'selfcheck version'     = $Ver
    'created'               = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz')
    'computer'              = $env:COMPUTERNAME
    'pswloader version'     = $(if ($vj) { $vj.version } else { 'no version.json' })
    'pswloader updated at'  = $(if ($vj) { $vj.updatedAt } else { '' })
    'pswloader previous'    = $(if ($vj) { $vj.previousVersion } else { '' })
    'windows installed at'  = $os.InstallDate.ToString('yyyy-MM-dd HH:mm:ss')
    'windows'               = "$($os.Caption) $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR) $($os.OSArchitecture)"
    'language'              = (Get-Culture).Name + ' / UI ' + (Get-UICulture).Name
    'last boot'             = $os.LastBootUpTime.ToString('yyyy-MM-dd HH:mm:ss')
    'model'                 = "$($cs.Manufacturer) $($cs.Model)"
    'memory GB'             = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
    'gpu'                   = $(if ($gpu) { "$($gpu.FriendlyName) | $($gpu.Status) $($gpu.Problem) | driver $(& $drv $gpu) | $($gpu.InstanceId)" } else { 'not found' })
    'ps5pwr'                = $(if ($pwr) { "$($pwr.Status) | driver $(& $drv $pwr)" } else { 'not found' })
    'devices not ok'        = $bad.Count
    'testsigning'           = ((bcdedit /enum '{current}' 2>$null | Select-String -Pattern 'testsigning') -join ' ').Trim()
  }
  (@($s.Keys) | ForEach-Object { '{0,-22} {1}' -f $_, $s[$_] }) | Set-Content (Join-Path $Work 'summary.txt') -Encoding UTF8
}

# 2. PSWloader files and logs
Step 'pswloader logs' {
  $d = Join-Path $Work 'pswloader'
  foreach ($p in 'C:\ProgramData\PSWloader\version.json', 'C:\ProgramData\PSWloader\journal.json', 'C:\ProgramData\PSWloader\updater.json',
                 'C:\ProgramData\PSWloader\logs', 'C:\PSWloader\logs\watch', 'C:\ps5-work\ps5-postboot.log', 'C:\ps5-work\swap-kmd.log') { Copy-IfThere $p $d }
  if (Test-Path 'C:\PSWSetup') { Get-ChildItem 'C:\PSWSetup' -File -Include *.log, *.txt, *.json -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.Length -lt 20MB } | ForEach-Object { Copy-IfThere $_.FullName (Join-Path $d 'PSWSetup') } }
  Get-ChildItem 'C:\ps5-work' -Filter *.log -File -ErrorAction SilentlyContinue | Where-Object { $_.Length -lt 20MB } |
    ForEach-Object { Copy-IfThere $_.FullName (Join-Path $d 'ps5-work') }
}

# 3. devices and drivers
Step 'devices' {
  $all = Get-PnpDevice -ErrorAction SilentlyContinue
  Out-Txt 'devices-all.txt' ($all | Sort-Object Class, FriendlyName | Format-Table Status, Problem, Class, FriendlyName, InstanceId -AutoSize)
  $rows = foreach ($dv in ($all | Where-Object { $_.Present -and ($_.Status -ne 'OK' -or $_.InstanceId -match 'VEN_1002|VEN_104D|VEN_1B4B|VEN_15B7|VEN_1022|USB\\') })) {
    $p = $dv | Get-PnpDeviceProperty DEVPKEY_Device_DriverVersion, DEVPKEY_Device_DriverInfPath, DEVPKEY_Device_Service, DEVPKEY_Device_ProblemStatus, DEVPKEY_Device_LocationInfo -ErrorAction SilentlyContinue
    $g = { param($k) ($p | Where-Object KeyName -eq $k).Data }
    [pscustomobject]@{ Status = $dv.Status; Problem = $dv.Problem; ProblemStatus = ('0x{0:X8}' -f [uint32](& $g 'DEVPKEY_Device_ProblemStatus'));
      Name = $dv.FriendlyName; Driver = (& $g 'DEVPKEY_Device_DriverVersion'); Inf = (& $g 'DEVPKEY_Device_DriverInfPath');
      Service = (& $g 'DEVPKEY_Device_Service'); Location = (& $g 'DEVPKEY_Device_LocationInfo'); Id = $dv.InstanceId }
  }
  Out-Txt 'devices-detail.txt' ($rows | Format-List)
  pnputil /enum-drivers > (Join-Path $Work 'drivers-pnputil.txt') 2>&1
  Out-Txt 'drivers-system.txt' (Get-CimInstance Win32_SystemDriver | Sort-Object Name | Format-Table Name, State, StartMode, PathName -AutoSize)
}

# 4. registry
Step 'registry' {
  $d = Join-Path $Work 'registry'; New-Item -ItemType Directory -Force $d | Out-Null
  foreach ($s in 'bc250kmd', 'ps5pwr', 'ps5gbe', 'ps5wifi', 'ps5nvmsi', 'ps5m2flt', 'ps5cmos', 'pswdisk', 'pswsvc', 'stornvme', 'ps5usb') {
    if (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\$s") { reg export "HKLM\SYSTEM\CurrentControlSet\Services\$s" (Join-Path $d "service-$s.reg") /y | Out-Null }
  }
  foreach ($k in 'HKLM\SOFTWARE\amdgpu-wddm', 'HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers', 'HKLM\SYSTEM\Setup', 'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion') {
    $n = ($k -replace '[\\ ]', '_') + '.reg'; reg export $k (Join-Path $d $n) /y 2>$null | Out-Null
  }
  foreach ($id in 'PCI\VEN_1002', 'PCI\VEN_104D', 'PCI\VEN_15B7', 'PCI\VEN_1B4B') {
    Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\PCI" -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like ($id.Split('\')[1] + '*') } |
      ForEach-Object { reg export ($_.Name) (Join-Path $d ("enum-" + $_.PSChildName.Replace('&', '_') + '.reg')) /y 2>$null | Out-Null }
  }
}

# 5. boot configuration, disks, network
Step 'boot and disks' {
  bcdedit /enum all /v > (Join-Path $Work 'bcd.txt') 2>&1
  Out-Txt 'disks.txt' @((Get-Disk | Format-List Number, FriendlyName, SerialNumber, BusType, PartitionStyle, Size, HealthStatus, OperationalStatus),
                        (Get-Partition | Format-Table DiskNumber, PartitionNumber, DriveLetter, Type, GptType, Size -AutoSize),
                        (Get-Volume | Format-Table DriveLetter, FileSystemLabel, FileSystem, DriveType, HealthStatus, Size, SizeRemaining -AutoSize))
  Out-Txt 'network.txt' @((Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Format-Table Name, InterfaceDescription, Status, LinkSpeed, MacAddress, DriverVersion -AutoSize),
                          (ipconfig /all))
}

# 6. UEFI self check and firmware logs (EFI system partition and USB drives)
Step 'uefi logs' {
  $d = Join-Path $Work 'uefi'; New-Item -ItemType Directory -Force $d | Out-Null
  $names = 'PS5DIAG.LOG', 'PS5UEFI.LOG', 'PS5WARM.LOG', 'PS5RESCU.LOG', 'PS5UEFI.CFG', 'PS5POWER.CFG', 'PSWloader\logs\PS5DIAG.LOG'
  $letter = $null
  foreach ($c in [char[]]'SRQPONMLKJIHGT') { if (-not (Test-Path "${c}:\")) { $letter = $c; break } }
  if ($letter) {
    mountvol "${letter}:" /s 2>$null | Out-Null
    if (Test-Path "${letter}:\") {
      foreach ($n in $names) { if (Test-Path "${letter}:\$n") { Copy-Item "${letter}:\$n" (Join-Path $d ('esp-' + ($n -replace '\\', '_'))) -Force } }
      Get-ChildItem "${letter}:\" -Recurse -File -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime |
        Format-Table -AutoSize | Out-String -Width 300 | Set-Content (Join-Path $d 'esp-files.txt')
      mountvol "${letter}:" /d 2>$null | Out-Null
      # firmware log slots are a ring (16-byte file marker, 'PS5LOG1', u64 bytes written, 64 KiB ring): readable copy in order
      foreach ($f in Get-ChildItem $d -Filter 'esp-PS5*.LOG' -File) {
        $b = [IO.File]::ReadAllBytes($f.FullName)
        if ($b.Length -lt 32 + 65536 -or [Text.Encoding]::ASCII.GetString($b, 16, 7) -ne 'PS5LOG1') { continue }
        $tot = [BitConverter]::ToUInt64($b, 24); $r = 65536
        if ($tot -le $r) { $txt = [Text.Encoding]::ASCII.GetString($b, 32, [int]$tot) }
        else { $s = [int]($tot % $r); $txt = [Text.Encoding]::ASCII.GetString($b, 32 + $s, $r - $s) + [Text.Encoding]::ASCII.GetString($b, 32, $s) }
        Set-Content -Path ($f.FullName -replace '\.LOG$', '.txt') -Value $txt -Encoding ASCII
      }
    }
  }
  foreach ($v in (Get-Volume | Where-Object { $_.DriveLetter -and $_.DriveType -eq 'Removable' -or ($_.DriveLetter -and (Get-Partition -DriveLetter $_.DriveLetter -ErrorAction SilentlyContinue | Get-Disk -ErrorAction SilentlyContinue).BusType -eq 'USB') })) {
    foreach ($n in $names) { $p = "$($v.DriveLetter):\$n"; if (Test-Path $p) { Copy-Item $p (Join-Path $d ("usb$($v.DriveLetter)-" + ($n -replace '\\', '_'))) -Force } }
  }
}

# 7. event logs (full evtx of the last 14 days plus a readable list of errors and warnings)
Step 'event logs' {
  $d = Join-Path $Work 'events'; New-Item -ItemType Directory -Force $d | Out-Null
  $ms = 14 * 24 * 3600 * 1000
  foreach ($ln in 'System', 'Application', 'Setup') {
    wevtutil epl $ln (Join-Path $d "$ln.evtx") "/q:*[System[TimeCreated[timediff(@SystemTime) <= $ms]]]" 2>$null
    Get-WinEvent -FilterHashtable @{ LogName = $ln; Level = 1, 2, 3; StartTime = (Get-Date).AddDays(-14) } -MaxEvents 3000 -ErrorAction SilentlyContinue |
      Select-Object TimeCreated, Id, LevelDisplayName, ProviderName, @{ n = 'Message'; e = { ($_.Message -replace '\s+', ' ') } } |
      Format-Table -AutoSize -Wrap | Out-String -Width 400 | Set-Content (Join-Path $d "$ln-errors.txt") -Encoding UTF8
  }
  Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1074, 6005, 6006, 6008, 41, 12, 13, 20, 27 } -MaxEvents 200 -ErrorAction SilentlyContinue |
    Select-Object TimeCreated, Id, ProviderName, @{ n = 'Message'; e = { ($_.Message -replace '\s+', ' ') } } | Format-Table -AutoSize -Wrap |
    Out-String -Width 400 | Set-Content (Join-Path $d 'boots-and-shutdowns.txt') -Encoding UTF8
}

# 8. Windows setup and driver install logs, crash dumps
Step 'setup logs and dumps' {
  $d = Join-Path $Work 'windows'
  foreach ($p in 'C:\Windows\INF\setupapi.dev.log', 'C:\Windows\INF\setupapi.setup.log', 'C:\Windows\Panther\setupact.log', 'C:\Windows\Panther\setuperr.log',
                 'C:\Windows\Panther\UnattendGC\setupact.log', 'C:\Windows\debug\netsetup.log') { Copy-IfThere $p $d }
  Get-ChildItem 'C:\Windows\Minidump' -Filter *.dmp -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 5 |
    ForEach-Object { Copy-IfThere $_.FullName (Join-Path $d 'minidump') }
  Get-ChildItem 'C:\Windows\LiveKernelReports' -Recurse -File -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime |
    Format-Table -AutoSize | Out-String -Width 300 | Set-Content (Join-Path $d 'livekernelreports-list.txt')
  if (Test-Path 'C:\Windows\MEMORY.DMP') { Get-Item 'C:\Windows\MEMORY.DMP' | Select-Object Length, LastWriteTime | Out-String | Set-Content (Join-Path $d 'memory-dmp-info.txt') }
}

# 9. scheduled tasks and processes of PSWloader
Step 'tasks' {
  Out-Txt 'tasks.txt' (Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -match 'PSW|PS5' } |
    ForEach-Object { $i = $_ | Get-ScheduledTaskInfo -ErrorAction SilentlyContinue; [pscustomobject]@{ Name = $_.TaskName; State = $_.State; LastRun = $i.LastRunTime; LastResult = ('0x{0:X}' -f $i.LastTaskResult); Action = ($_.Actions | ForEach-Object { "$($_.Execute) $($_.Arguments)" }) -join '; ' } } |
    Format-List)
}

# 10. graphics report (dxdiag, takes up to a minute)
Step 'dxdiag' {
  $p = Start-Process -FilePath "$env:SystemRoot\System32\dxdiag.exe" -ArgumentList "/t `"$(Join-Path $Work 'dxdiag.txt')`"" -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { try { $p.Kill() } catch { }; L 'dxdiag stopped after 90 s' }
}

# 11. zip
L 'creating zip'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $Zip) { Remove-Item $Zip -Force }
# entry by entry with '/' separators (CreateFromDirectory in Windows PowerShell 5.1 writes '\', which other tools misread)
$za = [IO.Compression.ZipFile]::Open($Zip, [IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($f in Get-ChildItem -LiteralPath $Work -Recurse -File) {
    $rel = $f.FullName.Substring($Base.Length + 1).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
  }
} finally { $za.Dispose() }
Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
# keep the newest 10 support zips
Get-ChildItem $Base -Filter 'PSW-selfcheck-*.zip' -File -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -Skip 10 | Remove-Item -Force -ErrorAction SilentlyContinue
$mb = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host ''
Write-Host "Done: $Zip ($mb MB)"
Write-Host 'Send this file with your support request.'
if (-not $NoExplorer) { Start-Process explorer.exe "/select,`"$Zip`"" }
