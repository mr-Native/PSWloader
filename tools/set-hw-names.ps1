# set-hw-names.ps1 v1.0 (16.11.2026, naming rule: "AMD Oberon (PS5 Custom GPU)" / "AMD Zen 2 (PS5 Custom CPU)")
# Display names for the INSTALLED GPU instance without any PnP action (no pnputil, no device restart):
#   Enum\<PCI\VEN_1002&DEV_13FC...>\DeviceDesc + FriendlyName, class key DriverDesc + HardwareInformation.AdapterString
#   = "AMD Oberon (PS5 Custom GPU)", ProviderName = "AMD (PS5 port)".
# Originals are saved once under HKLM\SOFTWARE\PS5Names\Orig\* so -Mode revert restores them exactly.
# The CPU name comes from the firmware (r4-nvme-name, CPUID brand MSRs); this script does not touch it.
# Run as SYSTEM (Enum keys). Takes effect for Device Manager at once; DXGI/Task Manager after the next boot.
#   -Mode check | apply | revert       Log: C:\ps5-work\set-hw-names.txt
param([ValidateSet('check','apply','revert')][string]$Mode = 'check')
$ErrorActionPreference = 'Stop'
$Gpu = 'AMD Oberon (PS5 Custom GPU)'; $Prov = 'AMD (PS5 port)'
$out = 'C:\ps5-work\set-hw-names.txt'; New-Item -ItemType Directory -Force C:\ps5-work | Out-Null
function W($m) { Add-Content -Path $out -Value $m; $m }
"=== set-hw-names v1.0 $Mode $(Get-Date -Format o)" | Set-Content $out
$dev = @(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_1002&DEV_13FC*')
if ($dev.Count -ne 1) { W "ABORT: expected one present 1002:13FC, found $($dev.Count)"; exit 2 }
$id = $dev[0].InstanceId
$enum = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
$drv = (Get-ItemProperty $enum -Name Driver).Driver
$cls = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drv"
$orig = 'HKLM:\SOFTWARE\PS5Names\Orig'
$targets = @(
  @{Key=$enum; Name='DeviceDesc'; Kind='String'},
  @{Key=$enum; Name='FriendlyName'; Kind='String'},
  @{Key=$cls;  Name='DriverDesc'; Kind='String'},
  @{Key=$cls;  Name='ProviderName'; Kind='String'},
  @{Key=$cls;  Name='HardwareInformation.AdapterString'; Kind='Binary'})
function Show { foreach ($t in $targets) { $v = (Get-ItemProperty $t.Key -Name $t.Name -ErrorAction SilentlyContinue).($t.Name)
  if ($v -is [byte[]]) { $v = [Text.Encoding]::Unicode.GetString($v).TrimEnd([char]0) }
  W ("  {0} = {1}" -f $t.Name, $(if ($null -eq $v) { '(absent)' } else { $v })) | Out-Null } }
W "device $id  class $drv"; Show
if ($Mode -eq 'check') { exit 0 }
if ($Mode -eq 'apply') {
  if (-not (Test-Path $orig)) {                                   # save originals exactly once
    New-Item $orig -Force | Out-Null
    foreach ($t in $targets) { $p = Get-ItemProperty $t.Key -Name $t.Name -ErrorAction SilentlyContinue
      $n = ($t.Name -replace '[\\.]','_')
      if ($null -eq $p) { New-ItemProperty $orig -Name "$n.absent" -PropertyType DWord -Value 1 -Force | Out-Null }
      else { $v = $p.($t.Name); New-ItemProperty $orig -Name $n -PropertyType $(if ($v -is [byte[]]) {'Binary'} else {'String'}) -Value $v -Force | Out-Null } }
  }
  foreach ($t in $targets) {
    $val = if ($t.Name -eq 'ProviderName') { $Prov } else { $Gpu }
    if ($t.Kind -eq 'Binary') { New-ItemProperty $t.Key -Name $t.Name -PropertyType Binary -Value ([Text.Encoding]::Unicode.GetBytes($val + [char]0)) -Force | Out-Null }
    else { New-ItemProperty $t.Key -Name $t.Name -PropertyType String -Value $val -Force | Out-Null } }
  W 'applied (readback):'; Show; exit 0
}
if (-not (Test-Path $orig)) { W 'ABORT: no saved originals'; exit 3 }
$o = Get-ItemProperty $orig
foreach ($t in $targets) { $n = ($t.Name -replace '[\\.]','_')
  if ($o."$n.absent") { Remove-ItemProperty $t.Key -Name $t.Name -ErrorAction SilentlyContinue }
  else { $v = $o.$n; New-ItemProperty $t.Key -Name $t.Name -PropertyType $(if ($v -is [byte[]]) {'Binary'} else {'String'}) -Value $v -Force | Out-Null } }
Remove-Item $orig -Recurse -Force
W 'reverted (readback):'; Show; exit 0
