<#
.SYNOPSIS
  PSWloader Version-Independent System Updater Engine.
.DESCRIPTION
  Declaratively updates any existing PSWloader Windows 11 installation (regardless
  of installed version) to the release bundled in <Root>\payload\.
  Writes a full timestamped log to C:\PSWSetup\Update\PSWUpdate-YYYYMMDD-HHMMSS.log
  and a machine-readable summary to C:\PSWSetup\Update\last-result.txt.
  Exit codes: 0 = complete success, 1 = completed with warnings, 2 = failed.
#>
[CmdletBinding()]
param(
    [string]$Root = '',
    [switch]$Quiet
)

$ErrorActionPreference = 'Continue'

if (-not $Root) {
    $Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
}

$PayloadDir = Join-Path $Root 'payload'
$UpdateLogDir = 'C:\PSWSetup\Update'
$DataDir = 'C:\ProgramData\PSWloader'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$LogFile = Join-Path $UpdateLogDir "PSWUpdate-$Stamp.log"
$ResultFile = Join-Path $UpdateLogDir 'last-result.txt'
# v1.2 (user 26.11.2026): steps for the progress window of PSWUpdater.exe (PSWProgress.ps1) - "<percent><TAB><text>" per line
$ProgressFile = Join-Path $UpdateLogDir 'progress.txt'
function Progress([int]$Pct, [string]$Text) {
    try { New-Item -ItemType Directory -Force -Path $UpdateLogDir -ErrorAction SilentlyContinue | Out-Null
          Add-Content -LiteralPath $ProgressFile -Value "$Pct`t$Text" -Encoding UTF8 -ErrorAction SilentlyContinue } catch { }
}
$DriverNames = @{ 'ps5gpu.inf' = 'graphics driver'; 'ps5pwr.inf' = 'power and fan driver'; 'ps5wifi.inf' = 'Wi-Fi driver (IW620)'; 'ps5gbe.inf' = 'Ethernet driver'
                  'mtkwl6ex.inf' = 'MediaTek Wi-Fi 6/6E driver'; 'mtkwecx.inf' = 'MediaTek Wi-Fi 7 driver'
                  'ps5btx.inf' = 'Bluetooth driver'; 'ps5nvmsi.inf' = 'M.2 storage driver'; 'ps5m2flt.inf' = 'M.2 storage filter'; 'ps5cmos.inf' = 'clock driver' }

New-Item -ItemType Directory -Path $UpdateLogDir -Force | Out-Null
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null

# Keep newest 20 update logs
Get-ChildItem -Path $UpdateLogDir -Filter 'PSWUpdate-*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -Skip 20 |
    Remove-Item -Force -ErrorAction SilentlyContinue

$script:Warnings = 0
$script:Errors = 0
$script:DriversInstalled = 0
$script:DriversUpToDate = 0
$script:DriversFailed = 0
$script:RestartTwice = $false

# v1.0.5: ps5pwr drives the console's power controller (PCI 104D:9107). A runtime restart of that device (what
# 'pnputil /install' does when the driver package changes) switches the console off at once. When ps5pwr is already
# bound, the new package is only added and the device is marked for reinstall at the next start (ConfigFlags 0x20),
# the same way PSWSetup does it. Enum keys are SYSTEM-only, so the flag is written through a one-shot SYSTEM task.
# v1.2 (HW 26.11.2026): the reinstall flag below is NOT safe - Windows processed it in the running session as soon as another
# 'pnputil /install' rescanned the devices: 104D:9107 was stopped, the Realtek boot disk reset in the same second and the next
# run ended in BUGCODE_USB3_DRIVER, again at every start until the reinstall finished. ps5pwr is updated without PnP now: the
# new package is only added, and the service points to its ps5pwr.sys - the next start loads the new driver (one restart).
function Set-Ps5PwrImageAtBoot([string]$StagedSys) {
    $want = (Get-FileHash -LiteralPath $StagedSys -Algorithm SHA256).Hash
    $hit = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository\ps5pwr.inf_amd64_*\ps5pwr.sys" -ErrorAction SilentlyContinue |
        Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq $want } | Select-Object -First 1
    if (-not $hit) { return $null }
    $img = '\SystemRoot\System32\DriverStore\FileRepository\' + $hit.Directory.Name + '\ps5pwr.sys'
    Set-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr' -Name ImagePath -Value $img -Type ExpandString -ErrorAction Stop
    return $img
}
function Set-ReinstallAtBoot([string]$InstanceId) {
    $key = "HKLM\SYSTEM\CurrentControlSet\Enum\$InstanceId"
    $cur = 0
    try { $cur = [uint32](Get-ItemProperty -LiteralPath "Registry::$key" -Name ConfigFlags -ErrorAction Stop).ConfigFlags } catch { $cur = 0 }
    $new = [int]($cur -bor 0x20)
    try {
        Set-ItemProperty -LiteralPath "Registry::$key" -Name ConfigFlags -Value $new -Type DWord -ErrorAction Stop
    } catch {
        $task = 'PSWloader-ReinstallFlag'
        $cmd = "reg.exe add `"$key`" /v ConfigFlags /t REG_DWORD /d $new /f"
        schtasks.exe /Create /TN $task /TR $cmd /SC ONCE /ST 00:00 /RU SYSTEM /F | Out-Null
        schtasks.exe /Run /TN $task | Out-Null
        Start-Sleep -Seconds 3
        schtasks.exe /Delete /TN $task /F | Out-Null
    }
    $chk = 0
    try { $chk = [uint32](Get-ItemProperty -LiteralPath "Registry::$key" -Name ConfigFlags -ErrorAction Stop).ConfigFlags } catch { }
    return (($chk -band 0x20) -ne 0)
}

function Write-Log {
    param([string]$Level, [string]$Message)
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $line = "[$ts] [$Level] $Message"
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
    if (-not $Quiet) {
        switch ($Level) {
            'ERROR' { Write-Host $line -ForegroundColor Red }
            'WARN'  { Write-Host $line -ForegroundColor Yellow }
            'OK'    { Write-Host $line -ForegroundColor Green }
            default { Write-Host $line }
        }
    }
}

function LogInfo([string]$m) { Write-Log 'INFO' $m }
function LogOk([string]$m)   { Write-Log 'OK' $m }
function LogWarn([string]$m) { $script:Warnings++; Write-Log 'WARN' $m }
# v1.2: the EFI system partition. A system whose ESP already has a drive letter made "mountvol X: /S" fail ("The parameter is
# incorrect" in the updater window, lab 25.11.) - the firmware was then never placed on the ESP. Use the existing letter (and
# never remove it), else mount it on a free letter as before. Returns @{ Root = 'S:'; Temp = $true } or $null.
function Mount-Esp {
    $p = Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.IsSystem } | Select-Object -First 1
    if ($p -and $p.DriveLetter -and $p.DriveLetter -ne [char]0 -and (Test-Path "$($p.DriveLetter):\")) { return @{ Root = "$($p.DriveLetter):"; Temp = $false } }
    foreach ($c in [char[]]'SRQPONMLKJIHGT') {
        if (Test-Path "${c}:\") { continue }
        $mp = Start-Process -FilePath "$env:SystemRoot\System32\mountvol.exe" -ArgumentList "${c}: /S" -NoNewWindow -Wait -PassThru
        if ($mp.ExitCode -eq 0 -and (Test-Path "${c}:\")) { return @{ Root = "${c}:"; Temp = $true } }
        return $null
    }
    return $null
}
function Dismount-Esp($Esp) {
    if ($Esp -and $Esp.Temp) { Start-Process -FilePath "$env:SystemRoot\System32\mountvol.exe" -ArgumentList "$($Esp.Root) /D" -NoNewWindow -Wait | Out-Null }
}
function LogErr([string]$m)  { $script:Errors++; Write-Log 'ERROR' $m }

function Save-Result([string]$Status, [string]$TargetVer) {
    Set-Content -Path $ResultFile -Value "$Status|$TargetVer|$LogFile" -Encoding UTF8
}

# Read target version from payload\version.json
$TargetVersion = 'unknown'
$MinFirmware = ''   # v1.1: firmware revision this release needs (PSWUpdateCheck warns at sign-in when an older one runs)
$VersionJsonPath = Join-Path $PayloadDir 'version.json'
if (Test-Path $VersionJsonPath) {
    try {
        $vj = Get-Content -Path $VersionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($vj.version) { $TargetVersion = [string]$vj.version }
        if ($vj.minFirmware) { $MinFirmware = [string]$vj.minFirmware }
    } catch {
        LogWarn "Could not parse payload\version.json: $_"
    }
}

# Read previously installed version if present
$PrevVersion = 'pre-1.0.3 (untracked)'
$InstalledVersionPath = Join-Path $DataDir 'version.json'
if (Test-Path $InstalledVersionPath) {
    try {
        $iv = Get-Content -Path $InstalledVersionPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($iv.version) { $PrevVersion = [string]$iv.version }
    } catch {}
}

LogInfo "============================================================"
LogInfo "PSWloader System Updater"
LogInfo "Previous version : $PrevVersion"
LogInfo "Target version   : $TargetVersion"
LogInfo "Updater root     : $Root"
LogInfo "Log file         : $LogFile"
LogInfo "OS               : $([System.Environment]::OSVersion.VersionString)"
LogInfo "============================================================"

# 1a. v1.1: no downgrade - an older updater package must not replace a newer installation (drivers, scripts, firmware
#     files). Versions like v1.0.4, v1.0.5-beta, v1.1: numbers first, a release ranks above its own betas; an unreadable
#     installed version (pre-1.0.3 installs) is not blocked. Updaters up to v1.0.5 do not have this check.
function Compare-PswVersion([string]$A, [string]$B) {   # -1 / 0 / 1, $null when one of them is not a version
    $re = '^\s*[vV]?(\d+(?:\.\d+)*)(?:-(\S+))?\s*$'
    if ($A -notmatch $re -or $B -notmatch $re) { return $null }
    $p = foreach ($v in $A, $B) {
        [void]($v -match $re)
        $n = @($Matches[1].Split('.') | ForEach-Object { [int]$_ }); while ($n.Count -lt 3) { $n += 0 }
        , @($n, $Matches[2]) }
    for ($i = 0; $i -lt [Math]::Max($p[0][0].Count, $p[1][0].Count); $i++) {
        $x = if ($i -lt $p[0][0].Count) { $p[0][0][$i] } else { 0 }; $y = if ($i -lt $p[1][0].Count) { $p[1][0][$i] } else { 0 }
        if ($x -ne $y) { return [Math]::Sign($x - $y) } }
    if ($p[0][1] -eq $p[1][1]) { return 0 }
    if (-not $p[0][1]) { return 1 }; if (-not $p[1][1]) { return -1 }
    return [Math]::Sign([string]::CompareOrdinal($p[0][1], $p[1][1]))
}
$VerCmp = Compare-PswVersion $TargetVersion $PrevVersion
if ($null -ne $VerCmp -and $VerCmp -lt 0) {
    LogErr "Downgrade blocked: PSWloader $PrevVersion is installed, this updater is for the older $TargetVersion. Nothing was changed."
    LogErr "Get the current updater from https://github.com/mr-Native/PSWloader/releases"
    Save-Result 'BLOCKED' $TargetVersion
    exit 3
}

Progress 3 'Checking the installation'
# 1. Verify elevation & payload
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    LogErr "Administrator privileges are required. Please run PSWUpdater.exe as Administrator."
    Save-Result 'FAIL' $TargetVersion
    Progress 100 'Update failed'
    exit 2
}

if (-not (Test-Path $PayloadDir)) {
    LogErr "Payload directory not found: $PayloadDir"
    Save-Result 'FAIL' $TargetVersion
    Progress 100 'Update failed'
    exit 2
}

Progress 8 'Installing the PSWloader certificate'
# 2. Install / refresh signing certificates
$CertsDir = Join-Path $PayloadDir 'certs'
if (Test-Path $CertsDir) {
    LogInfo "Step 1/5: Updating PSWloader signing certificates..."
    $certFiles = Get-ChildItem -Path $CertsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in '.cer', '.crt' }
    foreach ($cf in $certFiles) {
        try {
            $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cf.FullName)
            foreach ($storeName in @('Root', 'TrustedPublisher')) {
                $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
                $store.Open('ReadWrite')
                $existing = $store.Certificates.Find('FindByThumbprint', $cert.Thumbprint, $false)
                if ($existing.Count -eq 0) {
                    $store.Add($cert)
                    LogOk "Installed certificate $($cf.Name) ($($cert.Thumbprint)) into LocalMachine\$storeName"
                } else {
                    LogInfo "Certificate $($cf.Name) already present in LocalMachine\$storeName"
                }
                $store.Close()
            }
        } catch {
            LogErr "Failed to install certificate $($cf.Name): $_"
        }
    }
} else {
    LogInfo "Step 1/5: No certs directory in payload, skipping."
}

Progress 12 'Preparing the graphics driver settings'
# 2b. v1.0.5 (GitHub issue 2: GPU driver install failed on a Windows To Go drive that never ran PSWSetup): the GPU driver
#     needs the console VBIOS in System32\drivers\ps5-vbios.bin and a few registry values that only PSWSetup wrote. The
#     firmware puts \PS5VBIOS.BIN on the boot EFI partition at every start; copy it (checked like the driver checks it) and
#     set the same values as PSWSetup, before the driver package is installed.
try {
    $vbDst = Join-Path $env:SystemRoot 'System32\drivers\ps5-vbios.bin'
    $okRom = { param([byte[]]$r) $len = if ($r.Length -ge 3) { [int]$r[2] * 512 } else { 0 }; $sum = 0
               if ($len -ge 512 -and $len -le $r.Length) { for ($k = 0; $k -lt $len; $k++) { $sum = ($sum + $r[$k]) % 256 } }
               ($r.Length -ge 3 -and $r[0] -eq 0x55 -and $r[1] -eq 0xAA -and $len -ge 512 -and $len -le $r.Length -and $sum -eq 0) }
    $have = (Test-Path $vbDst) -and (& $okRom ([IO.File]::ReadAllBytes($vbDst)))
    if (-not $have) {
        $found = $false
        $esp = Mount-Esp
        if ($esp) {
            try {
                if (Test-Path "$($esp.Root)\PS5VBIOS.BIN") {
                    $rom = [IO.File]::ReadAllBytes("$($esp.Root)\PS5VBIOS.BIN")
                    if (& $okRom $rom) { [IO.File]::WriteAllBytes($vbDst, $rom); $found = $true; LogOk "GPU: console VBIOS copied from the EFI partition to $vbDst" }
                    else { LogWarn "GPU: \PS5VBIOS.BIN on the EFI partition is not a valid ROM image" }
                }
            } finally { Dismount-Esp $esp }
        }
        if (-not $found) { $script:Warnings++; LogWarn "GPU: no console VBIOS found - start Windows once with PSWloader, then run the updater again" }
    } else { LogInfo "GPU: console VBIOS present ($vbDst)" }
    $cs = 'HKLM:\SYSTEM\CurrentControlSet'
    $regs = @(
        @("$cs\Services\bc250kmd\Parameters\DefaultsV2", 'EnableDisplayModes', 2),
        @('HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter', 'DwmForceCpu', 0),
        @('HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter', 'RequireKmdSwitches', 0),
        @('HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter', 'DirectFlipFront', 0),
        @("$cs\Control\GraphicsDrivers", 'TdrDelay', 10),
        @("$cs\Control\GraphicsDrivers", 'TdrDdiDelay', 10))
    foreach ($r in $regs) {
        if (-not (Test-Path $r[0])) { New-Item -Path $r[0] -Force | Out-Null }
        New-ItemProperty -Path $r[0] -Name $r[1] -Value $r[2] -PropertyType DWord -Force | Out-Null
    }
    # v1.2.1: Direct3D 10/11 programs on the GPU driver (AppRouter Mode, REG_SZ). v1.2 and older never set it: every
    # Direct3D 11 program ran on the CPU driver at feature level 10_0 (Unity games failed)
    if (-not (Test-Path 'HKLM:\SOFTWARE\amdgpu-wddm\AppRouter')) { New-Item -Path 'HKLM:\SOFTWARE\amdgpu-wddm\AppRouter' -Force | Out-Null }
    New-ItemProperty -Path 'HKLM:\SOFTWARE\amdgpu-wddm\AppRouter' -Name Mode -Value 'gpu-default' -PropertyType String -Force | Out-Null
    # v1.2.1: DXVK cachedDynamicResources (machine env) - speeds the per-draw dynamic constant-buffer Map, the dominant CPU-submit cost
    [Environment]::SetEnvironmentVariable('DXVK_CONFIG', 'd3d11.cachedDynamicResources=a', 'Machine')
    LogOk "GPU: driver settings set (DefaultsV2, DesktopRouter, AppRouter, TDR) as by PSWSetup"
} catch {
    $script:Warnings++; LogWarn "GPU: could not prepare the driver prerequisites: $_"
}

Progress 16 'Installing the Visual C++ runtime'
# 2c. v1.1: x64 VC runtime DLLs for the GPU user-mode driver - BEFORE the drivers (a v1.0.4 install had none: during the GPU
# driver install DWM crash-looped, the screen flashed and the updater looked stuck, DM report 24.11.) (msvcp140, vcruntime140, vcruntime140_1). Without them DWM
# cannot create its D3D device (0x889800B2) and the screen stays black after the GPU driver - installs where VC_redist hung.
$VcrtDir = Join-Path $PayloadDir 'vcruntime\x64'
if (Test-Path $VcrtDir) {
    foreach ($src in Get-ChildItem -Path $VcrtDir -Filter '*.dll' -File) {
        try {
            $dst = Join-Path "$env:SystemRoot\System32" $src.Name
            $new = $src.VersionInfo.FileVersionRaw
            $old = if (Test-Path $dst) { (Get-Item $dst).VersionInfo.FileVersionRaw } else { $null }
            if (-not $old -or $old -lt $new) {
                Copy-Item -Path $src.FullName -Destination $dst -Force
                LogOk "$($src.Name) $new -> System32 (was $(if ($old) { $old } else { 'missing' }))"
            }
        } catch {
            LogWarn "Could not install $($src.Name): $_"
        }
    }
}

# 3. Install / update all drivers in payload\drivers\*
$DriversDir = Join-Path $PayloadDir 'drivers'
if (Test-Path $DriversDir) {
    LogInfo "Step 2/5: Updating PSWloader drivers..."
    $infs = Get-ChildItem -Path $DriversDir -Filter '*.inf' -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object FullName
    $di = 0
    foreach ($inf in $infs) {
        $dn = if ($DriverNames.ContainsKey($inf.Name.ToLower())) { $DriverNames[$inf.Name.ToLower()] } else { "driver $($inf.BaseName)" }
        Progress (20 + [int](50 * $di / [Math]::Max(1, $infs.Count))) "Installing the $dn"; $di++
        $rel = $inf.FullName.Substring($DriversDir.Length).TrimStart('\')
        LogInfo "  Installing driver: $rel"
        $pwrDevs = @()
        if ($inf.Name -ieq 'ps5pwr.inf') {
            $pwrDevs = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                Where-Object { @($_.HardwareID) -like 'PCI\VEN_104D&DEV_9107*' -and $_.Service -eq 'ps5pwr' })
            # v1.2 (HW 26.11.2026): a bound ps5pwr never gets 'pnputil /install' - not even with the same version number (two
            # builds can share one). The loaded file decides: same as the package -> nothing to do, else stage + image switch.
            $img = [string](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr' -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
            $imgFile = $img -replace '^\\SystemRoot', $env:SystemRoot
            $same = (Test-Path -LiteralPath $imgFile) -and ((Get-FileHash -LiteralPath $imgFile).Hash -eq (Get-FileHash -LiteralPath (Join-Path $inf.DirectoryName 'ps5pwr.sys')).Hash)
            if ($pwrDevs.Count -and $same) { LogInfo "  ps5pwr: the loaded driver file is this package's - nothing to do"; $script:DriversUpToDate++; continue }
        }
        # v1.1: the GPU driver too is only staged and put in at the next start (ConfigFlags 0x20) when bc250kmd already runs -
        # a live display driver swap can keep pnputil busy or leave the screen dark (HW 24.11.: 0.7.216.1227 staged + one
        # restart, loaded and HDMI audio on). Unlike ps5pwr one restart is enough.
        $gpuDevs = @()
        if ($inf.Name -ieq 'ps5gpu.inf') {
            $newVer = ((Select-String -Path $inf.FullName -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9.]+)' | Select-Object -First 1).Matches.Groups[1].Value)
            $gpuDevs = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                Where-Object { $_.Service -eq 'bc250kmd' -and @($_.HardwareID) -like 'PCI\VEN_1002&DEV_13*' } |
                Where-Object { ($_ | Get-PnpDeviceProperty DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data -ne $newVer })
        }
        $pnpArgs = if ($pwrDevs.Count -or $gpuDevs.Count) { "/add-driver `"$($inf.FullName)`"" } else { "/add-driver `"$($inf.FullName)`" /install" }
        $p = Start-Process -FilePath "$env:SystemRoot\System32\pnputil.exe" `
            -ArgumentList $pnpArgs `
            -NoNewWindow -PassThru `
            -RedirectStandardOutput "$env:TEMP\pswupd-pnp-out.txt" `
            -RedirectStandardError "$env:TEMP\pswupd-pnp-err.txt"
        $null = $p.Handle   # Windows PowerShell 5.1: keeps ExitCode readable after WaitForExit without -Wait
        # v1.1: at most 5 minutes per package - a display driver swap can keep pnputil busy; the package is in the driver
        # store by then and Windows finishes it at the next start
        if (-not $p.WaitForExit(300000)) {
            $script:Warnings++
            LogWarn "  Driver $rel`: pnputil did not finish in 5 minutes - the update goes on, Windows completes the driver at the next start"
            continue
        }
        $out = ''
        if (Test-Path "$env:TEMP\pswupd-pnp-out.txt") { $out = (Get-Content "$env:TEMP\pswupd-pnp-out.txt" -Raw -ErrorAction SilentlyContinue).Trim() }
        if ($out) {
            foreach ($ol in ($out -split "`r?`n")) {
                if ($ol.Trim()) { LogInfo "    pnputil: $($ol.Trim())" }
            }
        }
        if (($p.ExitCode -eq 0 -or $p.ExitCode -eq 3010 -or $p.ExitCode -eq 259) -and $pwrDevs.Count) {
            $img = $null; try { $img = Set-Ps5PwrImageAtBoot (Join-Path $inf.DirectoryName 'ps5pwr.sys') } catch { $img = $null }
            if ($img) {
                $script:DriversInstalled++
                LogOk "  Driver $rel staged; the service loads it at the next start ($img) - no device restart now"
            } else { $script:Warnings++; LogWarn "  Driver $rel staged, but its file was not found in the driver store - ps5pwr stays on the old version" }
        } elseif (($p.ExitCode -eq 0 -or $p.ExitCode -eq 3010 -or $p.ExitCode -eq 259) -and $gpuDevs.Count) {
            foreach ($d in $gpuDevs) {
                if (Set-ReinstallAtBoot $d.InstanceId) { LogOk "  $($d.InstanceId): GPU driver put in at the next start (no display restart now)" }
                else { $script:Warnings++; LogWarn "  $($d.InstanceId): could not set the reinstall flag - the GPU stays on the old driver" }
            }
            $script:DriversInstalled++
            LogOk "  Driver $rel staged; it is installed at the next start"
        } elseif ($p.ExitCode -eq 0 -or $p.ExitCode -eq 3010) {
            $script:DriversInstalled++
            LogOk "  Driver $rel installed/updated (exit $($p.ExitCode))"
        } elseif ($p.ExitCode -eq 259) {
            $script:DriversUpToDate++
            LogInfo "  Driver $rel staged / already up to date (exit 259)"
        } else {
            $script:DriversFailed++
            LogWarn "  Driver $rel returned exit code $($p.ExitCode)"
        }
    }
    Remove-Item "$env:TEMP\pswupd-pnp-out.txt", "$env:TEMP\pswupd-pnp-err.txt" -Force -ErrorAction SilentlyContinue
} else {
    LogInfo "Step 2/5: No drivers directory in payload, skipping."
}

Progress 74 'Applying the PS5 system settings'
# 4. Reconcile critical PS5 system settings (version-independent, idempotent)
LogInfo "Step 3/5: Reconciling PS5 system and boot configuration..."

# 4a. Clean legacy SCSIAdapter class LowerFilters for ps5nvmsi (prevents 0x7B double-attach)
try {
    $scsiClassKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}'
    if (Test-Path $scsiClassKey) {
        $lf = (Get-ItemProperty -Path $scsiClassKey -Name 'LowerFilters' -ErrorAction SilentlyContinue).LowerFilters
        if ($lf -and ($lf -contains 'ps5nvmsi')) {
            $newLf = @($lf | Where-Object { $_ -ne 'ps5nvmsi' })
            if ($newLf.Count -gt 0) {
                Set-ItemProperty -Path $scsiClassKey -Name 'LowerFilters' -Value $newLf -Type MultiString
            } else {
                Remove-ItemProperty -Path $scsiClassKey -Name 'LowerFilters' -ErrorAction SilentlyContinue
            }
            LogOk "Removed legacy ps5nvmsi entry from SCSIAdapter class LowerFilters"
        }
    }
} catch {
    LogWarn "Could not inspect SCSIAdapter class LowerFilters: $_"
}

# 4b. Ensure BCD flags (testsigning, disabledynamictick, bootstatuspolicy)
foreach ($target in @('{current}', '{default}')) {
    foreach ($pair in @(
        @('testsigning', 'on'),
        @('disabledynamictick', 'yes'),
        @('bootstatuspolicy', 'IgnoreAllFailures')
    )) {
        $bp = Start-Process -FilePath "$env:SystemRoot\System32\bcdedit.exe" `
            -ArgumentList "/set $target $($pair[0]) $($pair[1])" `
            -NoNewWindow -Wait -PassThru
        if ($bp.ExitCode -ne 0) {
            LogWarn "bcdedit /set $target $($pair[0]) $($pair[1]) returned $($bp.ExitCode)"
        }
    }
}
LogOk "Verified BCD boot configuration"

# 4c. Prevent Windows Update from overwriting PS5 drivers
try {
    $wuKey = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'
    New-Item -Path $wuKey -Force | Out-Null
    Set-ItemProperty -Path $wuKey -Name 'ExcludeWUDriversInQualityUpdate' -Value 1 -Type DWord
    LogOk "Verified ExcludeWUDriversInQualityUpdate = 1"
} catch {
    LogWarn "Could not set ExcludeWUDriversInQualityUpdate: $_"
}

# 4d. Cache console-extracted \PS5WIFIFW.BIN from ESP to System32\drivers\ps5wifi_fw.bin (warm-reboot fallback)
try {
    $wifiFwDst = "$env:SystemRoot\System32\drivers\ps5wifi_fw.bin"
    if (-not (Test-Path $wifiFwDst)) {
        $foundFw = $null
        foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
            $cand = Join-Path $d.Root 'PS5WIFIFW.BIN'
            if (Test-Path $cand) { $foundFw = $cand; break }
        }
        $espW = $null
        if (-not $foundFw) {
            $espW = Mount-Esp
            if ($espW -and (Test-Path "$($espW.Root)\PS5WIFIFW.BIN")) { $foundFw = "$($espW.Root)\PS5WIFIFW.BIN" }
        }
        if ($foundFw) {
            Copy-Item -Path $foundFw -Destination $wifiFwDst -Force
            LogOk "Cached console Wi-Fi firmware from $foundFw to $wifiFwDst"
        }
        Dismount-Esp $espW
    }
} catch {
    LogWarn "Could not cache PS5WIFIFW.BIN fallback: $_"
}

# 4d2. v1.0.5 (issue #8): memory integrity (HVCI) and VBS off - with them on, Windows refuses the PS5 kernel drivers (Code 39)
try {
    $dg = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
    $hv = "$dg\Scenarios\HypervisorEnforcedCodeIntegrity"
    if (-not (Test-Path $hv)) { New-Item -Path $hv -Force | Out-Null }
    if ((Get-ItemProperty -Path $hv -Name Enabled -ErrorAction SilentlyContinue).Enabled -ne 0) {
        Set-ItemProperty -Path $hv -Name Enabled -Value 0 -Type DWord; LogOk "Memory integrity (HVCI) turned off - takes effect after the restart" }
    if ((Get-ItemProperty -Path $dg -Name EnableVirtualizationBasedSecurity -ErrorAction SilentlyContinue).EnableVirtualizationBasedSecurity -ne 0) {
        Set-ItemProperty -Path $dg -Name EnableVirtualizationBasedSecurity -Value 0 -Type DWord; LogOk "VBS turned off - takes effect after the restart" }
} catch {
    LogWarn "Could not turn off memory integrity: $_"
}

Progress 80 'Updating tools and Start menu entries'
# 5. Update tools and scripts in C:\ProgramData\PSWloader\tools
LogInfo "Step 4/5: Updating PSWloader tools and scripts..."
$ToolsSrc = Join-Path $PayloadDir 'tools'
$ToolsDst = Join-Path $DataDir 'tools'
if (Test-Path $ToolsSrc) {
    try {
        New-Item -ItemType Directory -Path $ToolsDst -Force | Out-Null
        Copy-Item -Path (Join-Path $ToolsSrc '*') -Destination $ToolsDst -Recurse -Force
        LogOk "Synced tools to $ToolsDst"
    } catch {
        LogErr "Failed to sync tools: $_"
    }
} else {
    LogInfo "No tools directory in payload, skipping."
}

# 5c. v1.0.5: ThermalManager (C:\ps5-work) - with ps5pwr 1.17+ the driver runs the fan curve and the manager stops writing fan targets
$TmSrc = Join-Path $ToolsSrc 'thermal\ps5-thermal-manager.ps1'
if ((Test-Path $TmSrc) -and (Test-Path 'C:\ps5-work\ps5-thermal-manager.ps1')) {
    try {
        $tmTask = Get-ScheduledTask -TaskName 'PSW-ThermalManager' -ErrorAction SilentlyContinue
        if ($tmTask) { Stop-ScheduledTask -TaskName 'PSW-ThermalManager' -ErrorAction SilentlyContinue }
        Copy-Item -Path $TmSrc -Destination 'C:\ps5-work\ps5-thermal-manager.ps1' -Force
        if ($tmTask) { Start-ScheduledTask -TaskName 'PSW-ThermalManager' -ErrorAction SilentlyContinue }
        LogOk "Updated the ThermalManager in C:\ps5-work"
        # v1.1: the fixed 60 C fan target task fought the driver's PS5 Linux fan curve (ps5pwr 1.17+) - removed
        if (Get-ScheduledTask -TaskName 'PSW-FanTarget' -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName 'PSW-FanTarget' -Confirm:$false; LogOk "Removed the task PSW-FanTarget (the driver controls the fan)" }
    } catch {
        LogWarn "Could not update the ThermalManager: $_"
    }
}

# 5d. v1.1: desktop corner text - psw-hide-testmode.ps1 v2.1 also hides the edition line ("Windows 11 Pro"); only where the
#     Watermark component is installed (the script and its startup task exist)
$HtSrc = Join-Path $ToolsSrc 'psw-hide-testmode.ps1'; $HtDst = Join-Path $DataDir 'psw-hide-testmode.ps1'
if ((Test-Path $HtSrc) -and (Test-Path $HtDst) -and (Get-ScheduledTask -TaskName 'PSW-HideTestMode' -ErrorAction SilentlyContinue)) {
    try {
        Copy-Item -Path $HtSrc -Destination $HtDst -Force
        $ea = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\conhost.exe" `
            -Argument "--headless $env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$HtDst`" -Edition"
        $es = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances Parallel -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        Register-ScheduledTask -TaskName 'PSW-HideEdition' -Action $ea -Trigger (New-ScheduledTaskTrigger -AtLogOn) `
            -Principal (New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited) -Settings $es -Force -ErrorAction Stop | Out-Null
        Start-ScheduledTask -TaskName 'PSW-HideEdition' -ErrorAction SilentlyContinue
        # v1.1: the same as SYSTEM from system start with the native PSWEdition.exe - the line is gone ~30 ms after Explorer starts
        $exeSrc = Join-Path $ToolsSrc 'PSWEdition.exe'; $exeDst = Join-Path $DataDir 'PSWEdition.exe'
        Stop-ScheduledTask -TaskName 'PSW-HideEditionSys' -ErrorAction SilentlyContinue
        Get-Process PSWEdition -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        if (Test-Path $exeSrc) { Copy-Item -Path $exeSrc -Destination $exeDst -Force }
        $ya = New-ScheduledTaskAction -Execute $exeDst
        $ys = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        Register-ScheduledTask -TaskName 'PSW-HideEditionSys' -Action $ya -Trigger (New-ScheduledTaskTrigger -AtStartup) `
            -Principal (New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest) -Settings $ys -Force -ErrorAction Stop | Out-Null
        Start-ScheduledTask -TaskName 'PSW-HideEditionSys' -ErrorAction SilentlyContinue
        LogOk "Desktop corner text: edition line hidden too (tasks PSW-HideEditionSys and PSW-HideEdition)"
    } catch {
        LogWarn "Could not update the desktop corner text script: $_"
    }
}

# 5e. v1.2 (user 26.11.2026): "PSWloader Display" is gone - with GPU driver 0.7.216.1230 Windows Settings offers the monitor's
#     resolutions and switches them directly; the Start-menu entry and its script are removed
foreach ($old in "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\PSWloader Display.lnk", (Join-Path $DataDir 'psw-display.ps1'), (Join-Path $DataDir 'tools\psw-display.ps1')) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue; LogInfo "Removed $old (resolution now in Windows Settings)" }
}

# 5f. v1.2 (user 26.11.2026): at the first sign-in after this update's restart psw-welcome.ps1 shows
#     - the welcome window (Buy Me a Coffee + QR code) ONCE per machine: its mark (HKLM\SOFTWARE\PSWloader WelcomeShown /
#       welcome-shown.txt) counts, not the version - an update from before v1.2 shows it once, later updates do not;
#     - "What's new in <version>" once per version (mark WhatsNewShown), after the welcome or alone. Keep this step in later
#       updaters (the script carries the version's text).
$WcSrc = Join-Path $ToolsSrc 'psw-welcome.ps1'
if (Test-Path $WcSrc) {
    try {
        $wcDst = Join-Path $DataDir 'psw-welcome.ps1'
        Copy-Item -Path $WcSrc -Destination $wcDst -Force
        $wcAct = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
            -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$wcDst`" -Welcome -WhatsNew -After $((Get-Date).ToFileTimeUtc())"
        $wcTrg = New-ScheduledTaskTrigger -AtLogOn; $wcTrg.Delay = 'PT20S'
        $wcPrn = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Highest
        $wcSet = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
        Register-ScheduledTask -TaskName 'PSWloader Welcome' -Action $wcAct -Trigger $wcTrg -Principal $wcPrn -Settings $wcSet -Force -ErrorAction Stop | Out-Null
        LogOk "What's new (and the welcome window if it never came) at the first sign-in after the restart"
    } catch { LogWarn "Could not set up the What's new window: $_" }
}

# 5b. v1.0.5: support tool C:\PSWloader\selfcheck (PSWSelfCheck.cmd -> one zip with every log in ...\results)
$SelfSrc = Join-Path $ToolsSrc 'selfcheck'
if (Test-Path $SelfSrc) {
    try {
        New-Item -ItemType Directory -Path 'C:\PSWloader\selfcheck\results' -Force | Out-Null
        Copy-Item -Path (Join-Path $SelfSrc '*') -Destination 'C:\PSWloader\selfcheck' -Force
        LogOk "Installed the support tool to C:\PSWloader\selfcheck"
        if (Test-Path 'C:\PSWloader\selfcheck\PSWWatch.ps1') {   # background log (C:\PSWloader\logs\watch), at startup and every minute
            $wa = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
                -Argument '-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\PSWloader\selfcheck\PSWWatch.ps1"'
            $wt = @((New-ScheduledTaskTrigger -AtStartup), (New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 1)))
            $wp = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
            $ws = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
            Register-ScheduledTask -TaskName 'PSWloader Watch' -Action $wa -Trigger $wt -Principal $wp -Settings $ws -Force -ErrorAction Stop | Out-Null
            LogOk "Background log task 'PSWloader Watch' registered (C:\PSWloader\logs\watch)"
        }
    } catch {
        LogWarn "Could not install the support tool: $_"
    }
}

# 5g. v1.2 (user 26.11.2026): the PSWloader Dashboard replaces the separate Start-menu entries of the builds before it ("PSWloader UEFI Boot
#     Manager", "PSWloader Support"). The icons and the Boot Manager script are copied here; the Dashboard itself (files in
#     C:\Program Files\PSWloader, its tasks, Start-menu and desktop entry) is PSWSetup's component Dashboard in step 5h.
try {
    $IconDir = Join-Path $DataDir 'icons'; New-Item -ItemType Directory -Force -Path $IconDir | Out-Null
    foreach ($i in 'psw-bootmanager.ico', 'psw-support.ico', 'psw-dashboard.ico') { $is = Join-Path $ToolsSrc "icons\$i"; $id = Join-Path $IconDir $i
        # v1.2: an icon the open Dashboard (or Explorer) holds is in use - an identical one is simply kept
        if ((Test-Path $is) -and -not ((Test-Path $id) -and (Get-FileHash $is).Hash -eq (Get-FileHash $id).Hash)) { Copy-Item $is $id -Force } }
    $bmSrc = Join-Path $ToolsSrc 'psw-uefi-bootmanager.ps1'
    if (Test-Path $bmSrc) { Copy-Item -Path $bmSrc -Destination (Join-Path $DataDir 'psw-uefi-bootmanager.ps1') -Force }
} catch { LogWarn "Icons: $_" }

# 5h. v1.2 (user 26.11.2026: in-place updates from v0.4, v0.5, v1.0 or v1.1 must end with everything a new installation has):
#     the updater carries PSWSetup's engine with the non-driver parts of its payload (setup\, built by assemble) and runs the
#     components the steps above do not cover. Each component checks itself first and only installs what is missing or older
#     (the same Detect/Install as a new installation); drivers stay with step 3. Without -Quiet PSWSetup never restarts by itself.
$SetupRoot = Join-Path $Root 'setup'
$SetupEngine = Join-Path $SetupRoot 'engine\PSWSetup.ps1'
if (Test-Path $SetupEngine) {
    Progress 86 'Bringing all PSWloader components up to date'
    LogInfo "Running PSWSetup components from $SetupRoot ..."
    try {
        $comps = 'Boot,SelfCheck,PowerButton,Cmos,Names,System,PowerPlan,Thermal,Tasks,Watermark,BootMenu,Dashboard,Signing'
        $sp = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Wait -PassThru -WindowStyle Hidden `
            -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$SetupEngine`" -Mode Install -Root `"$SetupRoot`" -Components $comps -NoRestart"
        if ($sp.ExitCode -in 0, 3010) { LogOk "PSWSetup components up to date (exit $($sp.ExitCode))" }
        else { LogWarn "PSWSetup components finished with exit $($sp.ExitCode) - see C:\PSWSetup\logs and C:\ProgramData\PSWloader\setup-result.json" }
    } catch { LogWarn "PSWSetup components could not run: $_" }
} else { LogInfo 'No setup\ folder in this package - PSWSetup components skipped' }

# 5i. Start menu: "PSWloader Dashboard" pinned for the current accounts (psw-startpins.ps1: export + append, Start policy once,
#     removed again at the next sign-in); the entries of older versions leave the pinned list
try {
    $spSrc = Join-Path $ToolsSrc 'psw-startpins.ps1'
    $dashLnk = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\PSWloader Dashboard.lnk"
    if (Test-Path $dashLnk) { LogOk 'PSWloader Dashboard installed (Start menu and desktop)' } else { LogWarn 'PSWloader Dashboard entry not found - see the PSWSetup log' }
    if ((Test-Path $spSrc) -and (Test-Path $dashLnk)) {
        $spDst = Join-Path $DataDir 'psw-startpins.ps1'; Copy-Item -Path $spSrc -Destination $spDst -Force
        Register-ScheduledTask -TaskName 'PSWloader Start pins' -Force -ErrorAction Stop `
            -Action (New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$spDst`"") `
            -Trigger (New-ScheduledTaskTrigger -AtLogOn) -Principal (New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Highest) `
            -Settings (New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5)) | Out-Null
        # pin now in this sign-in (the updater runs as the signed-in user, elevated) and reload the Start menu; the task
        # finishes at the next sign-in (removes the policy again)
        try { & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $spDst
              Get-Process -Name StartMenuExperienceHost -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
              LogInfo "Start pins: $((Get-Content "$env:ProgramData\PSWloader\startpins.log" -ErrorAction SilentlyContinue | Select-Object -Last 1))" } catch { }
        LogOk 'Start menu: PSWloader Dashboard pinned (at the latest after the restart)'
    }
} catch { LogWarn "Start menu pin: $_" }

Progress 88 'Updating the PSWloader firmware'
# 6. Optional: update UEFI payload on any mounted PSWloader ESP / USB \PSW\ directory
LogInfo "Step 5/5: Checking for mounted PSWloader firmware directory (\PSW\)..."
$FwSrc = Join-Path $PayloadDir 'firmware'
if (Test-Path $FwSrc) {
    $updatedFw = 0
    $drives = Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue
    foreach ($d in $drives) {
        $pswDir = Join-Path $d.Root 'PSW'
        if (Test-Path (Join-Path $pswDir 'pswloader-upl.bin')) {
            try {
                foreach ($fn in @('pswloader-upl.bin', 'pswloader-kernel.bin')) {
                    $sf = Join-Path $FwSrc $fn
                    if (Test-Path $sf) {
                        Copy-Item -Path $sf -Destination (Join-Path $pswDir $fn) -Force
                    }
                }
                $updatedFw++
                LogOk "Updated PSWloader firmware files in $pswDir"
            } catch {
                LogWarn "Could not update firmware in $pswDir : $_"
            }
        }
    }
    if ($updatedFw -eq 0) {
        LogInfo "No mounted \PSW\ firmware directory found (All-in-One PSWloader.elf does not require external files)."
    }
} else {
    LogInfo "No firmware directory in payload, skipping."
}

# 6b. v1.0.5: firmware for the next start on the EFI partition. The running firmware starts a newer
#     \PSWloader\firmware\pswloader-upl.bin by itself, so PSWloader.elf does not have to be replaced for firmware updates.
# v1.1 (HW 24.11.): a firmware before r5.26.3 restarts into an updated image with the firmware volume records of its own image
#     and hangs when the layout differs (r5.26.2 is larger than r5.26.0/r5.26.1). Then the new firmware comes with the new
#     PSWloader.elf only: nothing is placed, and a file left there for such a firmware is removed (it would hang twice before
#     being marked failed). The sign-in warning (PSWUpdateCheck) asks for the new ELF.
$uplSrc = Join-Path $FwSrc 'pswloader-upl.bin'
$runRev = 0
if ([string](Get-CimInstance Win32_BIOS -ErrorAction SilentlyContinue).SMBIOSBIOSVersion -match 'r(\d+)\.(\d+)\.(\d+)') {
    $runRev = [int64]$Matches[1] * 1000000 + [int64]$Matches[2] * 1000 + [int64]$Matches[3] }
$espSwitchOk = $runRev -ge 5026003
if (-not $espSwitchOk) { LogInfo "Running firmware $(if ($runRev) { 'r{0}.{1}.{2}' -f [int]($runRev / 1000000), [int]($runRev / 1000 % 1000), [int]($runRev % 1000) } else { 'without revision' }) cannot switch to a firmware on the EFI partition safely - the new firmware comes with the new PSWloader.elf" }
if (Test-Path $uplSrc) {
    $esp = Mount-Esp
    if ($true) {
        try {
            if ($esp) {
                $fwDir = "$($esp.Root)\PSWloader\firmware"
                if (-not $espSwitchOk) {
                    if (Test-Path "$fwDir\pswloader-upl.bin") {
                        Remove-Item -Path "$fwDir\pswloader-upl.bin", "$fwDir\update.txt" -Force -ErrorAction SilentlyContinue
                        LogOk "Removed the firmware file from the EFI partition (the running firmware cannot start it safely)" }
                    throw [System.OperationCanceledException]::new('skip')
                }
                New-Item -ItemType Directory -Path $fwDir -Force | Out-Null
                Copy-Item -Path $uplSrc -Destination "$fwDir\pswloader-upl.bin.new" -Force   # copy first, then swap: never a half file
                if ((Get-FileHash "$fwDir\pswloader-upl.bin.new").Hash -ne (Get-FileHash $uplSrc).Hash) { throw 'copy check failed' }
                Move-Item -Path "$fwDir\pswloader-upl.bin.new" -Destination "$fwDir\pswloader-upl.bin" -Force
                LogOk "Firmware for the next start placed on the EFI partition (\PSWloader\firmware\pswloader-upl.bin)"
            } else { LogWarn "EFI partition not reachable - the firmware update needs the new PSWloader.elf" }
        } catch [System.OperationCanceledException] {
        } catch {
            LogWarn "Could not place the firmware on the EFI partition: $_"
        } finally {
            Dismount-Esp $esp
        }
    }
}

Progress 95 'Setting up the update check'
# 7. Install / refresh silent boot-time auto-update checker (preserves user config)
try {
    $chkSrc = Join-Path (Join-Path $Root 'engine') 'PSWUpdateCheck.ps1'
    if (-not (Test-Path -LiteralPath $chkSrc)) {
        $chkSrc = Join-Path (Join-Path $PayloadDir 'updater') 'PSWUpdateCheck.ps1'
    }
    if (Test-Path -LiteralPath $chkSrc) {
        $updDir = Join-Path $DataDir 'updater'
        New-Item -ItemType Directory -Path $updDir -Force | Out-Null
        $chkDst = Join-Path $updDir 'PSWUpdateCheck.ps1'
        Copy-Item -Path $chkSrc -Destination $chkDst -Force

        $cfgPath = Join-Path $DataDir 'updater.json'
        if (-not (Test-Path -LiteralPath $cfgPath)) {
            [ordered]@{
                autoCheck = $true
                repo      = 'mr-Native/PSWloader'
            } | ConvertTo-Json -Depth 3 | Set-Content -Path $cfgPath -Encoding UTF8
            LogOk "Created default auto-update config at $cfgPath"
        } else {
            LogInfo "Preserving existing auto-update config at $cfgPath"
        }

        $action  = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
            -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$chkDst`""
        $trigger = New-ScheduledTaskTrigger -AtLogOn
        $trigger.Delay = 'PT25S'
        $principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
        $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
        Register-ScheduledTask -TaskName 'PSWloader Update Check' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force -ErrorAction Stop | Out-Null
        LogOk "Registered boot-time auto-update checker task (PSWloader Update Check)"
    }
} catch {
    LogWarn "Could not configure auto-update checker: $_"
}

# v1.2.1: reboot cover on the following boot(s). Copy the cover script to a persistent place and register a user-session
# logon task that shows the full-screen PSWloader "Processing updates" screen while C:\PSWSetup\Update\reboot-pending.txt
# exists (PSWUpdater's restart writes the marker; ps5-postboot removes the marker AND this task once the update is fully
# applied). So the unavoidable second restart for the GPU real-mode settings is covered and automatic, not a blank screen.
try {
    $coverSrc = Join-Path (Split-Path -Parent $PSCommandPath) 'PSWRebootScreen.ps1'
    $coverDst = 'C:\PSWSetup\Update\PSWRebootScreen.ps1'
    if (Test-Path $coverSrc) {
        New-Item -ItemType Directory -Force 'C:\PSWSetup\Update' | Out-Null
        Copy-Item $coverSrc $coverDst -Force
        $ca = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\conhost.exe" `
            -Argument "--headless $env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$coverDst`""
        $cs = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        Register-ScheduledTask -TaskName 'PSWloader Reboot Cover' -Action $ca -Trigger (New-ScheduledTaskTrigger -AtLogOn) `
            -Principal (New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited) -Settings $cs -Force -ErrorAction Stop | Out-Null
        LogOk 'Reboot cover task registered (shows the PSWloader screen on the update restart[s])'
    } else { LogWarn "Reboot cover script not found at $coverSrc" }
} catch { LogWarn "Could not set up the reboot cover: $_" }

# Record installed version
$meta = [ordered]@{
    version         = $TargetVersion
    previousVersion = $PrevVersion
    updatedAt       = (Get-Date -Format 'o')
    logFile         = $LogFile
    driversUpdated  = $script:DriversInstalled
    driversUpToDate = $script:DriversUpToDate
    restartTwice    = $script:RestartTwice
    warnings        = $script:Warnings
    errors          = $script:Errors
}
if ($MinFirmware) { $meta.minFirmware = $MinFirmware }
try {
    $meta | ConvertTo-Json -Depth 3 | Set-Content -Path $InstalledVersionPath -Encoding UTF8
} catch {
    LogWarn "Could not write $InstalledVersionPath : $_"
}

LogInfo "============================================================"
LogInfo "SUMMARY: target=$TargetVersion installed=$($script:DriversInstalled) uptodate=$($script:DriversUpToDate) failed=$($script:DriversFailed) warnings=$($script:Warnings) errors=$($script:Errors)"
LogInfo "============================================================"

if ($script:Errors -gt 0) {
    LogErr "Update FAILED with $($script:Errors) error(s). See $LogFile"
    Save-Result 'FAIL' $TargetVersion
    Progress 100 'Update failed'
    exit 2
} elseif ($script:Warnings -gt 0) {
    LogWarn "Update completed with $($script:Warnings) warning(s). See $LogFile"
    Save-Result 'WARN' $TargetVersion
    Progress 100 'Update finished with warnings'
    exit 1
} else {
    LogOk "Update to $TargetVersion completed successfully!"
    Save-Result 'OK' $TargetVersion
    Progress 100 'Update finished'
    exit 0
}
