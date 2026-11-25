# PSWUpdateCheck.ps1 - Silent boot-time update checker for PSWloader on Windows 11.
# Reads C:\ProgramData\PSWloader\updater.json and C:\ProgramData\PSWloader\version.json.
# Exits silently if disabled, offline, or already up to date.
# If a newer GitHub release is available, prompts the user to download & run the Updater,
# skip for now, or permanently disable update checks.
param(
    [switch]$Force,
    [string]$OverrideLatest = ''
)

$ErrorActionPreference = 'SilentlyContinue'
$DataDir    = 'C:\ProgramData\PSWloader'
$ConfigPath = Join-Path $DataDir 'updater.json'
$VerPath    = Join-Path $DataDir 'version.json'
$LogDir     = 'C:\PSWSetup\Update'
$LogPath    = Join-Path $LogDir 'PSWUpdateCheck.log'

New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
function Write-Log([string]$Msg) {
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Add-Content -Path $LogPath -Value "[$ts] $Msg" -Encoding UTF8
}

# v1.1: outdated PSWloader.elf. A release older than PSWloader v1.0.5 cannot start the firmware the updater placed on the
# EFI partition, so Windows then runs on the old firmware built into that ELF (restarts, drivers and fan control can
# fail). Warned at every sign-in, also with update checks turned off: the running firmware revision (BIOS version
# "PSWloader v1.1 (r5.26.1)"; no revision = before r5.25.13) is compared with minFirmware from version.json.
function Get-FwRev([string]$s) {   # "r5.26.1" -> 5026001, $null without a revision
    if ($s -match 'r(\d+)\.(\d+)\.(\d+)') { return [int64]$Matches[1] * 1000000 + [int64]$Matches[2] * 1000 + [int64]$Matches[3] }
    return $null
}
try {
    $vjFw = Get-Content -LiteralPath $VerPath -Raw | ConvertFrom-Json
    $bios = [string](Get-CimInstance Win32_BIOS).SMBIOSBIOSVersion
    $need = Get-FwRev ([string]$vjFw.minFirmware)
    if ($need -and $bios -like 'PSWloader*') {
        $have = Get-FwRev $bios
        if ($null -eq $have -or $have -lt $need) {
            Write-Log "Outdated PSWloader.elf/firmware: running '$bios', this installation needs $($vjFw.minFirmware) or newer"
            Add-Type -AssemblyName System.Windows.Forms
            [void][System.Windows.Forms.MessageBox]::Show(
                "PSWloader.elf is outdated.`r`n`r`nThis Windows is set up for PSWloader $($vjFw.version) (firmware $($vjFw.minFirmware) or newer), " +
                "but the console was started with an older PSWloader.elf (running: $bios).`r`n`r`n" +
                "Restarts, drivers and fan control can fail, and serious problems are possible.`r`n`r`n" +
                "Replace PSWloader.elf in your Payload Manager with the files of the $($vjFw.version) release, then start PSWloader again.",
                'PSWloader - outdated PSWloader.elf', 'OK', 'Warning')
        }
    }
} catch { Write-Log "Firmware check skipped: $($_.Exception.Message)" }

function Parse-SemVer([string]$v) {
    if (-not $v) { return [version]'0.0.0' }
    $clean = ($v.Trim() -replace '^[vV]', '') -replace '[^0-9.].*$', ''
    $parts = $clean.Split('.') | Where-Object { $_ -ne '' }
    while ($parts.Count -lt 3) { $parts += '0' }
    try { return [version](($parts[0..2]) -join '.') } catch { return [version]'0.0.0' }
}

# 1. Load config (default: autoCheck = true)
$cfg = [pscustomobject]@{
    autoCheck = $true
    repo      = 'mr-Native/PSWloader'
}
if (Test-Path -LiteralPath $ConfigPath) {
    try {
        $loaded = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
        if ($null -ne $loaded.autoCheck) { $cfg.autoCheck = [bool]$loaded.autoCheck }
        if ($loaded.repo) { $cfg.repo = [string]$loaded.repo }
    } catch {
        Write-Log "WARN: could not parse $ConfigPath, using defaults"
    }
}

if (-not $Force -and -not $cfg.autoCheck) {
    Write-Log "Auto update check is disabled in $ConfigPath - exiting."
    exit 0
}

# 2. Load installed version
$installedStr = 'v1.0.0'
if (Test-Path -LiteralPath $VerPath) {
    try {
        $vj = Get-Content -LiteralPath $VerPath -Raw | ConvertFrom-Json
        if ($vj.version) { $installedStr = [string]$vj.version }
    } catch {}
}
$installedVer = Parse-SemVer $installedStr

# 3. Query GitHub latest release (wait up to 45s for network after boot)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$release = $null
$latestStr = ''
$zipUrl = ''
$zipName = ''

if ($OverrideLatest) {
    $latestStr = $OverrideLatest
} else {
    $apiUrl = "https://api.github.com/repos/$($cfg.repo)/releases/latest"
    for ($try = 1; $try -le 3; $try++) {
        try {
            $release = Invoke-RestMethod -Uri $apiUrl -Headers @{ 'User-Agent' = 'PSWloader-Updater' } -TimeoutSec 12
            if ($release -and $release.tag_name) {
                $latestStr = [string]$release.tag_name
                break
            }
        } catch {
            if ($try -lt 3) { Start-Sleep -Seconds 15 }
        }
    }
    if (-not $latestStr) {
        Write-Log "Offline or GitHub API unreachable - exiting silently."
        exit 0
    }
    if ($release.assets) {
        $asset = $release.assets | Where-Object { $_.name -match 'Updater\.zip$' } | Select-Object -First 1
        if ($asset) {
            $zipUrl  = [string]$asset.browser_download_url
            $zipName = [string]$asset.name
        }
    }
}

$latestVer = Parse-SemVer $latestStr
Write-Log "Check complete: installed=$installedStr ($installedVer), latest=$latestStr ($latestVer)"

if ($latestVer -le $installedVer) {
    exit 0
}

# 4. Show clean 3-button Windows dialog
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object System.Windows.Forms.Form
$form.Text = 'PSWloader Update Available'
$form.Size = New-Object System.Drawing.Size(470, 205)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.TopMost = $true
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9.5)

$lbl = New-Object System.Windows.Forms.Label
$lbl.Location = New-Object System.Drawing.Point(20, 18)
$lbl.Size = New-Object System.Drawing.Size(420, 88)
$lbl.Text = "A new update for PSWloader is available ($latestStr).`r`nYour current version: $installedStr`r`n`r`nWould you like to download and update your system now?"
$form.Controls.Add($lbl)

$script:choice = 'No'

$btnYes = New-Object System.Windows.Forms.Button
$btnYes.Text = 'Yes'
$btnYes.Location = New-Object System.Drawing.Point(20, 118)
$btnYes.Size = New-Object System.Drawing.Size(105, 32)
$btnYes.Add_Click({ $script:choice = 'Yes'; $form.Close() })
$form.Controls.Add($btnYes)

$btnNo = New-Object System.Windows.Forms.Button
$btnNo.Text = 'No'
$btnNo.Location = New-Object System.Drawing.Point(135, 118)
$btnNo.Size = New-Object System.Drawing.Size(105, 32)
$btnNo.Add_Click({ $script:choice = 'No'; $form.Close() })
$form.Controls.Add($btnNo)

$btnDisable = New-Object System.Windows.Forms.Button
$btnDisable.Text = 'Always disable updates'
$btnDisable.Location = New-Object System.Drawing.Point(250, 118)
$btnDisable.Size = New-Object System.Drawing.Size(185, 32)
$btnDisable.Add_Click({ $script:choice = 'Disable'; $form.Close() })
$form.Controls.Add($btnDisable)

$form.AcceptButton = $btnYes
$form.CancelButton = $btnNo
[void]$form.ShowDialog()
$form.Dispose()

Write-Log "User selected: $($script:choice)"

if ($script:choice -eq 'Disable') {
    try {
        New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
        $cfg.autoCheck = $false
        $cfg | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ConfigPath -Encoding UTF8
        Write-Log "Disabled autoCheck in $ConfigPath"
        [System.Windows.Forms.MessageBox]::Show(
            "Automatic update checks have been disabled.`r`n`r`nYou can re-enable them anytime by setting `"autoCheck`": true in:`r`n$ConfigPath",
            'PSWloader Update Checker',
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Information
        ) | Out-Null
    } catch {
        Write-Log "ERROR: failed to save $ConfigPath : $_"
    }
    exit 0
}

if ($script:choice -ne 'Yes') {
    exit 0
}

# 5. Download & extract the Updater ZIP, then launch PSWUpdater*.exe
if (-not $zipUrl) {
    $relPage = "https://github.com/$($cfg.repo)/releases/tag/$latestStr"
    Write-Log "No Updater.zip asset found; opening release page $relPage"
    Start-Process $relPage
    exit 0
}

$stageDir = Join-Path $env:TEMP "PSWUpdate-$latestStr"
$fileName = if ($zipName) { $zipName } else { "pswloader-$latestStr-Updater.zip" }
$zipFile  = Join-Path $stageDir $fileName
try {
    if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    Write-Log "Downloading $zipUrl -> $zipFile"
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipFile -Headers @{ 'User-Agent' = 'PSWloader-Updater' } -UseBasicParsing
    $extractDir = Join-Path $stageDir 'extracted'
    Expand-Archive -Path $zipFile -DestinationPath $extractDir -Force
    $exe = Get-ChildItem -Path $extractDir -Filter 'PSWUpdater*.exe' -Recurse -File | Select-Object -First 1
    if (-not $exe) { throw 'PSWUpdater executable not found in archive.' }
    Write-Log "Launching $($exe.FullName)"
    Start-Process -FilePath $exe.FullName -WorkingDirectory $exe.DirectoryName -Verb RunAs
} catch {
    Write-Log "ERROR: download/launch failed: $_"
    [System.Windows.Forms.MessageBox]::Show(
        "Failed to download or start the PSWloader $latestStr updater:`r`n$_`r`n`r`nYou can download it manually from:`r`nhttps://github.com/$($cfg.repo)/releases/latest",
        'PSWloader Update Checker',
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
    ) | Out-Null
}
