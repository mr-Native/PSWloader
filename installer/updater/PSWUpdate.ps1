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
function LogErr([string]$m)  { $script:Errors++; Write-Log 'ERROR' $m }

function Save-Result([string]$Status, [string]$TargetVer) {
    Set-Content -Path $ResultFile -Value "$Status|$TargetVer|$LogFile" -Encoding UTF8
}

# Read target version from payload\version.json
$TargetVersion = 'unknown'
$VersionJsonPath = Join-Path $PayloadDir 'version.json'
if (Test-Path $VersionJsonPath) {
    try {
        $vj = Get-Content -Path $VersionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($vj.version) { $TargetVersion = [string]$vj.version }
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

# 1. Verify elevation & payload
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    LogErr "Administrator privileges are required. Please run PSWUpdater.exe as Administrator."
    Save-Result 'FAIL' $TargetVersion
    exit 2
}

if (-not (Test-Path $PayloadDir)) {
    LogErr "Payload directory not found: $PayloadDir"
    Save-Result 'FAIL' $TargetVersion
    exit 2
}

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

# 3. Install / update all drivers in payload\drivers\*
$DriversDir = Join-Path $PayloadDir 'drivers'
if (Test-Path $DriversDir) {
    LogInfo "Step 2/5: Updating PSWloader drivers..."
    $infs = Get-ChildItem -Path $DriversDir -Filter '*.inf' -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object FullName
    foreach ($inf in $infs) {
        $rel = $inf.FullName.Substring($DriversDir.Length).TrimStart('\')
        LogInfo "  Installing driver: $rel"
        $p = Start-Process -FilePath "$env:SystemRoot\System32\pnputil.exe" `
            -ArgumentList "/add-driver `"$($inf.FullName)`" /install" `
            -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput "$env:TEMP\pswupd-pnp-out.txt" `
            -RedirectStandardError "$env:TEMP\pswupd-pnp-err.txt"
        $out = ''
        if (Test-Path "$env:TEMP\pswupd-pnp-out.txt") { $out = (Get-Content "$env:TEMP\pswupd-pnp-out.txt" -Raw -ErrorAction SilentlyContinue).Trim() }
        if ($out) {
            foreach ($ol in ($out -split "`r?`n")) {
                if ($ol.Trim()) { LogInfo "    pnputil: $($ol.Trim())" }
            }
        }
        if ($p.ExitCode -eq 0 -or $p.ExitCode -eq 3010) {
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
        $mountedTemp = $false
        if (-not $foundFw -and -not (Test-Path 'S:\')) {
            $mp = Start-Process -FilePath "$env:SystemRoot\System32\mountvol.exe" -ArgumentList 'S: /S' -NoNewWindow -Wait -PassThru
            if ($mp.ExitCode -eq 0) {
                $mountedTemp = $true
                if (Test-Path 'S:\PS5WIFIFW.BIN') { $foundFw = 'S:\PS5WIFIFW.BIN' }
            }
        }
        if ($foundFw) {
            Copy-Item -Path $foundFw -Destination $wifiFwDst -Force
            LogOk "Cached console Wi-Fi firmware from $foundFw to $wifiFwDst"
        }
        if ($mountedTemp) {
            Start-Process -FilePath "$env:SystemRoot\System32\mountvol.exe" -ArgumentList 'S: /D' -NoNewWindow -Wait | Out-Null
        }
    }
} catch {
    LogWarn "Could not cache PS5WIFIFW.BIN fallback: $_"
}

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

# Record installed version
$meta = [ordered]@{
    version         = $TargetVersion
    previousVersion = $PrevVersion
    updatedAt       = (Get-Date -Format 'o')
    logFile         = $LogFile
    driversUpdated  = $script:DriversInstalled
    driversUpToDate = $script:DriversUpToDate
    warnings        = $script:Warnings
    errors          = $script:Errors
}
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
    exit 2
} elseif ($script:Warnings -gt 0) {
    LogWarn "Update completed with $($script:Warnings) warning(s). See $LogFile"
    Save-Result 'WARN' $TargetVersion
    exit 1
} else {
    LogOk "Update to $TargetVersion completed successfully!"
    Save-Result 'OK' $TargetVersion
    exit 0
}
