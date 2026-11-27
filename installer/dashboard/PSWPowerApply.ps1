# PSWPowerApply.ps1 v1.0 (PSWloader v1.2.1): SYSTEM helper invoked by the Dashboard's "Power & Fan" tab.
# Reads a strictly whitelisted request JSON (powerPreset, fanPreset, customCurve) and applies it live:
#   - powerPreset (efficiency|standard|oc): persists via ps5-thermal-manager.ps1 -SetPreset and updates GPU clock immediately
#   - fanPreset (0=Silent, 1=Balanced, 2=Performance, 3=Custom): writes ps5pwr\Parameters\FanPreset (picked up within 1 s)
#   - customCurve (16 ints: 8 temps C + 8 duties %): writes ps5pwr\Parameters\FanCustomCurve (REG_BINARY, safety-clamped)
# SPDX-License-Identifier: MIT
param([string]$RequestFile = 'C:\ProgramData\PSWloader\power-req\request.json')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $RequestFile)) { exit 0 }
$req = Get-Content -LiteralPath $RequestFile -Raw -Encoding UTF8 | ConvertFrom-Json
$pwrKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters'
$stateOut = 'C:\ProgramData\PSWloader\dashboard\power-state.json'

# 1. Custom fan curve (16 bytes: 8x temp_C [40..89], 8x duty_pct [10..100], last point always 100%)
if ($null -ne $req.customCurve -and @($req.customCurve).Count -eq 16) {
  $bytes = New-Object byte[] 16
  $prevT = 40; $prevD = 10
  for ($i = 0; $i -lt 8; $i++) {
    $tc = [Math]::Max($prevT, [Math]::Min(89, [int]$req.customCurve[$i])); $prevT = $tc
    $dc = [Math]::Max($prevD, [Math]::Min(100, [int]$req.customCurve[$i + 8])); $prevD = $dc
    $bytes[$i] = [byte]$tc; $bytes[$i + 8] = [byte]$dc
  }
  $bytes[15] = 100
  if (Test-Path $pwrKey) { New-ItemProperty -Path $pwrKey -Name 'FanCustomCurve' -PropertyType Binary -Value $bytes -Force | Out-Null }
}

# 2. Fan preset (0=Silent, 1=Balanced, 2=Performance, 3=Custom)
$fanApplied = $null
if ($null -ne $req.fanPreset) {
  $fp = [int]$req.fanPreset
  if ($fp -ge 0 -and $fp -le 3 -and (Test-Path $pwrKey)) {
    New-ItemProperty -Path $pwrKey -Name 'FanPreset' -PropertyType DWord -Value $fp -Force | Out-Null
    $fanApplied = $fp
  }
}

# 3. Power preset (efficiency | standard | oc)
$pwrApplied = $null
if ($req.powerPreset -in 'efficiency', 'standard', 'oc') {
  $p = [string]$req.powerPreset
  $tm = 'C:\ps5-work\ps5-thermal-manager.ps1'
  if (Test-Path -LiteralPath $tm) {
    # Save preset to PS5UEFI.CFG + guard file (preserve user's explicit fanPreset if they chose Custom or a different fan preset)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $tm -SetPreset $p | Out-Null
    if ($null -ne $fanApplied -and (Test-Path $pwrKey)) {
      New-ItemProperty -Path $pwrKey -Name 'FanPreset' -PropertyType DWord -Value $fanApplied -Force | Out-Null
    }
    # Apply GPU clock ceiling immediately so the change is felt live without waiting for reboot
    $drv = 'C:\BC250\drivers\ps5pwr\v1.10\ps5pwr.ps1'
    if (Test-Path -LiteralPath $drv) {
      $clk = if ($p -eq 'efficiency') { 1800 } else { 2230 }
      try { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $drv -SetGfxClk $clk | Out-Null } catch { }
    }
    # Restart ThermalManager task so its TargetC / MaxMhz / GuardC adopt the new preset live
    try {
      Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -EA SilentlyContinue |
        Where-Object { $_.CommandLine -match 'ps5-thermal-manager\.ps1' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -EA SilentlyContinue }
      Start-ScheduledTask -TaskName 'PSW-ThermalManager' -EA SilentlyContinue
    } catch { }
  }
  $pwrApplied = $p
}

# Write confirmed state for Dashboard
try {
  $curFan = if (Test-Path $pwrKey) { (Get-ItemProperty $pwrKey -Name 'FanPreset' -EA SilentlyContinue).FanPreset } else { 1 }
  $curBin = if (Test-Path $pwrKey) { (Get-ItemProperty $pwrKey -Name 'FanCustomCurve' -EA SilentlyContinue).FanCustomCurve } else { $null }
  $st = [ordered]@{
    powerPreset  = $(if ($pwrApplied) { $pwrApplied } else { [string]$req.powerPreset })
    fanPreset    = $(if ($null -ne $curFan) { [int]$curFan } else { 1 })
    customCurve  = $(if ($curBin -and $curBin.Length -eq 16) { @($curBin | ForEach-Object { [int]$_ }) } else { $null })
    customTemps  = $(if ($req.customTemps -and @($req.customTemps).Count -ge 2) { @($req.customTemps | ForEach-Object { [int]$_ }) } else { $null })
    customDuties = $(if ($req.customDuties -and @($req.customDuties).Count -ge 2) { @($req.customDuties | ForEach-Object { [int]$_ }) } else { $null })
    appliedAt    = (Get-Date).ToString('o')
  }
  $dir = Split-Path -Parent $stateOut; if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
  Set-Content -LiteralPath $stateOut -Value ($st | ConvertTo-Json -Compress) -Encoding UTF8
} catch { }
