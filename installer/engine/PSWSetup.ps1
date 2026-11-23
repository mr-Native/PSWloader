# PSWSetup.ps1 v0.5.7 (v0.5.7: SelfCheck also installs PSWWatch.ps1 + task 'PSWloader Watch' (background log C:\PSWloader\logs\watch); v0.5.6: component SelfCheck - the support tool in C:\PSWloader\selfcheck; v0.5.5: DefaultsV2\EnableDisplayModes stays at the catalog factory value 2 - the KMD rejects any
#   DefaultsV2 value that differs from its compiled factory value (0xC0000182, Code 37), so a fresh install never got a first
#   driver start; mode 0 comes from the generation override written by ps5-postboot.ps1; v0.5.4: PowerPlan also turns off USB selective suspend and USB 3 link power management (U1/U2) - HW 18.11.:
#   with selective suspend on, an idle USB SSD (RTL9210B, UAS) on the AMD xHCI does not come back (events 129/153, no more I/O);
#   isolated with reboots + 150 s idle: selective suspend is the cause, LPM off is kept as a harmless extra; off: 8 GiB at 310 MB/s, no reset; v0.5.3: v0.5.3: after a reinstall-at-boot update the log and the final line say 'restart twice' - HW 17.11.: the
#   first start reinstalls the device and switches ImagePath, the new driver is loaded only at the second start; v0.5.2: Power update without restarting the device 104D:9107: when ps5pwr is already bound, the new
#   package goes in with pnputil /add-driver (no /install) and the device gets ConfigFlags |= 0x20 (CONFIGFLAG_REINSTALL), so
#   PnP installs it at the next start - restarting 104D:9107 at runtime is not safe on the PS5 (HW 17.11.); v0.5.1: Monitor via Windows Update with a hard 60 s limit when no user is signed in or with
#   -FromSetupComplete (SetupComplete.cmd runs before the first sign-in; a WU search can take minutes); v0.5.0: PSWloader names - PSWSetup.exe/engine\PSWSetup.ps1, data C:\ProgramData\PSWloader (an old
#   C:\ProgramData\PS5Setup is moved there once), tasks PSW-* (old PS5-* tasks are removed when their component installs),
#   components VCRedist (Visual C++ 2015-2022 x64 + x86, offline) and Watermark (test-mode text hidden, -Restore undoes);
#   C:\ps5-work keeps its name for now (thermal scripts and logs use it); v0.4.2: drivers count as current when the Authenticode code hash matches (same build, other signature ->
#   no needless driver swap with device restart); per-process logs setup-<ts>-<pid>.log in ProgramData rotated to the newest 10;
#   v0.4.1: release set rc4 - ps5pwr v1.10.3, ThermalManager v1.6.7 with -OcMaxMhz 2230, guard v18.3; v0.4: run log logs\setup_psw<timestamp>.log next to the setup with system report, warnings and
#   summary, rotated to the newest 10; component Monitor; v0.3: ps5pwr v1.10 + power-button listener) - engine of "PSWloader Setup" (16.11.2026). Started by PSWSetup.exe (same folder layout:
#   <dir>\PSWSetup.exe, <dir>\engine\PSWSetup.ps1, <dir>\payload\...). Offline: everything comes from <dir>\payload.
# Turns a fresh Windows 11 (stock ISO, any build >= 22631) on the PS5 into today's working state. Idempotent: each component
# has Detect / Install, and every change goes to a journal (C:\ProgramData\PSWloader\journal.json) so -Mode Uninstall can undo it.
#   PSWSetup.ps1 -Mode Detect|Install|Uninstall [-Components A,B] [-Include Remote] [-Force] [-Quiet] [-LogPath file]
# Flow of Install:
#   - The admin process re-runs this script as SYSTEM through a one-shot task (Enum\ and bc250kmd SharedState keys are
#     SYSTEM-only) and shows its log. Run log: <setup folder>\logs\setup_psw<yyyyMMdd-HHmmss>.log (one file per run, also
#     across the phase-1 reboot; fallback C:\ProgramData\PSWloader\logs if the setup folder is read-only); newest 10 are kept.
#   - Phase 1 (test signing not active in the running Windows): Preflight + Boot (BCD testsigning etc.), copy of the setup
#     to C:\ProgramData\PSWloader\installer, task "PSWSetup-Resume" (at startup) -> exit 3010, reboot.
#   - Phase 2 (after the reboot, test mode on): all other components, task removed -> exit 3010 (one more reboot loads
#     the drivers) or 0 when nothing changed.
# Signing: no private key is shipped. A device-local code-signing certificate is created (key not exportable), trusted in
#   LocalMachine Root + TrustedPublisher, the driver catalogs are created on the device (New-FileCatalog) and signed
#   (Set-AuthenticodeSignature), then the private key is deleted. The .sys files keep their embedded test signature.
# Exit codes: 0 ok, 3010 ok + reboot required, 1 error, 2 precondition failed.
[CmdletBinding(SupportsShouldProcess)]
param([ValidateSet('Detect', 'Install', 'Uninstall')][string]$Mode = 'Detect', [string[]]$Components = @(), [string[]]$Include = @(),
      [switch]$Force, [switch]$Quiet, [switch]$Version, [switch]$Resume, [switch]$NoSystemHop, [switch]$NoRestart, [switch]$FromSetupComplete, [string]$LogPath = '',
      [string]$Root = '', [string]$Data = "$env:ProgramData\PSWloader")
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }   # v0.4: $PSScriptRoot is empty in param defaults on Windows PowerShell 5.1
$SetupVersion = '0.5.5'
if ($Version) { "PSWloader Setup $SetupVersion"; return }
# v0.5.0: data of an older setup (C:\ProgramData\PS5Setup: journal, logs) moves to the PSWloader folder once
$OldData = "$env:ProgramData\PS5Setup"
if ((Test-Path -LiteralPath "$OldData\journal.json") -and -not (Test-Path -LiteralPath "$Data\journal.json")) {
  try { New-Item -ItemType Directory -Force -Path $Data -WhatIf:$false | Out-Null
        foreach ($i in Get-ChildItem -LiteralPath $OldData -Force) { Move-Item -LiteralPath $i.FullName -Destination $Data -Force -WhatIf:$false }
        Remove-Item -LiteralPath $OldData -Force -Recurse -WhatIf:$false -ErrorAction SilentlyContinue } catch { }
}
function Remove-LegacyTask([string]$Old) {   # v0.5.0: PS5-* tasks of an older setup (the PSW-* task replaces it)
  if (Get-ScheduledTask -TaskName $Old -ErrorAction SilentlyContinue) {
    if ($PSCmdlet.ShouldProcess($Old, 'remove old task')) { Unregister-ScheduledTask -TaskName $Old -Confirm:$false; Log "  old task $Old removed" } }
}
$Payload = Join-Path $Root 'payload'

# ================================================================ infrastructure
New-Item -ItemType Directory -Force -Path $Data, "$Data\logs" -WhatIf:$false | Out-Null
# v0.4.2: one technical log per process (the admin process and its SYSTEM re-run each write one); newest 10 kept
try { Get-ChildItem -LiteralPath "$Data\logs" -Filter 'setup-*.log' -ErrorAction Stop | Sort-Object Name -Descending | Select-Object -Skip 9 |
      Remove-Item -Force -WhatIf:$false -ErrorAction SilentlyContinue } catch { }
$LogFile = Join-Path $Data "logs\setup-$(Get-Date -Format yyyyMMdd-HHmmss)-$PID.log"
$JournalFile = Join-Path $Data 'journal.json'
$script:RebootNeeded = $false
$script:RestartTwice = $false   # v0.5.3
$script:Warnings = New-Object System.Collections.Generic.List[string]
$script:Errors = New-Object System.Collections.Generic.List[string]
function Log([string]$m) {
  $l = "$(Get-Date -Format 'HH:mm:ss') $m"
  Add-Content -LiteralPath $LogFile -Value $l -WhatIf:$false
  if ($LogPath) { try { Add-Content -LiteralPath $LogPath -Value $l -Encoding UTF8 -WhatIf:$false } catch { } }
  Write-Host $l }
function LogWarn([string]$m) { $script:Warnings.Add($m); Log "WARNING: $m" }
function LogError([string]$m) { $script:Errors.Add($m); Log "ERROR: $m" }
function New-RunLog {   # <setup folder>\logs\setup_psw<timestamp>.log, newest 10 kept; fallback ProgramData if read-only
  $name = "setup_psw$(Get-Date -Format yyyyMMdd-HHmmss).log"
  foreach ($dir in @((Join-Path $Root 'logs'), (Join-Path $Data 'logs'))) {
    try { New-Item -ItemType Directory -Force -Path $dir -WhatIf:$false | Out-Null; $f = Join-Path $dir $name
          Set-Content -LiteralPath $f -Value "PSWloader Setup $SetupVersion - run log $name" -Encoding UTF8 -WhatIf:$false
          Get-ChildItem -LiteralPath $dir -Filter 'setup_psw*.log' | Sort-Object Name -Descending | Select-Object -Skip 10 | Remove-Item -Force -WhatIf:$false
          return $f } catch { continue } }
  return $LogFile }
function Write-SystemReport {   # read-only facts that help to reproduce problems on other consoles
  Log '---- system report ----'
  $q = { param($sb, $label) try { & $sb } catch { LogWarn "system report: $label not readable ($($_.Exception.Message))" } }
  & $q { $o = Get-CimInstance Win32_OperatingSystem; $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
         Log "  windows: $($o.Caption) $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR) $($o.OSArchitecture) lang=$((Get-Culture).Name) installed=$($o.InstallDate)" } 'windows'
  & $q { $c = Get-CimInstance Win32_Processor | Select-Object -First 1; Log "  cpu: $($c.Name.Trim()) cores=$($c.NumberOfCores) threads=$($c.NumberOfLogicalProcessors) max=$($c.MaxClockSpeed) MHz" } 'cpu'
  & $q { $m = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory; Log "  memory: $([math]::Round($m / 1GB, 1)) GB visible" } 'memory'
  & $q { $b = Get-CimInstance Win32_BIOS; Log "  firmware: $($b.Manufacturer) $($b.SMBIOSBIOSVersion) $($b.ReleaseDate)" } 'firmware'
  & $q { foreach ($v in Get-CimInstance Win32_VideoController) { Log "  gpu: $($v.Name) driver=$($v.DriverVersion) mode=$($v.CurrentHorizontalResolution)x$($v.CurrentVerticalResolution)@$($v.CurrentRefreshRate) status=$($v.Status)" } } 'gpu'
  & $q { foreach ($m in Get-PnpDevice -Class Monitor -PresentOnly) { Log "  monitor: $($m.FriendlyName) [$($m.Status)] $($m.InstanceId)" } } 'monitor'
  & $q { foreach ($d in Get-Disk) { Log "  disk $($d.Number): $($d.FriendlyName) bus=$($d.BusType) size=$([math]::Round($d.Size / 1GB)) GB style=$($d.PartitionStyle) boot=$($d.IsBoot) system=$($d.IsSystem)" } } 'disks'
  & $q { $bd = (Get-Partition | Where-Object IsBoot | Select-Object -First 1); Log "  windows disk: $(if ($bd) { "disk $($bd.DiskNumber)" } else { 'unknown' }) sandisk-internal=$(Get-BootDiskIsSandisk)" } 'boot disk'
  & $q { foreach ($n in Get-NetAdapter -Physical) { Log "  network: $($n.InterfaceDescription) status=$($n.Status) speed=$($n.LinkSpeed)" } } 'network'
  & $q { Log "  test signing active: $(Test-TestModeActive); secure boot: $(try { Confirm-SecureBootUEFI } catch { 'n/a' })" } 'boot state'
  & $q { foreach ($id in 'PCI\VEN_1002*', 'PCI\VEN_14C3*', 'PCI\VEN_1B4B*', 'PCI\VEN_11AB*', 'PCI\VEN_15B7*', 'PCI\VEN_104D*', 'PCI\VEN_1022*') { foreach ($d in @(Get-PciDevice $id)) { Log "  device: $($d.FriendlyName) [$($d.Status)] $($d.InstanceId)" } } } 'devices'
  foreach ($svc in 'bc250kmd', 'ps5pwr', 'ps5cmos', 'ps5gbe', 'ps5wifi', 'ps5nvmsi', 'ps5m2flt') {
    try { $k = "$CS\Services\$svc"
      if (-not (Test-Path $k)) { Log "  driver $svc not installed"; continue }
      $img = "$(Get-RegValue $k 'ImagePath')" -replace '^\\SystemRoot', $env:SystemRoot -replace '^System32', "$env:SystemRoot\System32" -replace '^\\\?\?\\', ''
      $h = if ($img -and (Test-Path -LiteralPath $img)) { (-join ([Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes($img)) | ForEach-Object { $_.ToString("x2") })).Substring(0, 8) } else { 'no image' }
      Log "  driver $svc start=$(Get-RegValue $k 'Start') sha256=$h" }
    catch { LogWarn "system report: driver $svc not readable ($($_.Exception.Message))" } }
  & $q { $esp = Get-Partition | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -and $_.IsSystem } | Select-Object -First 1
         $vol = if ($esp) { ($esp | Get-Volume -ErrorAction SilentlyContinue).Path } else { $null }
         $fw = if ($vol -and (Test-Path -LiteralPath "$($vol)PS5UEFI.LOG")) { (Select-String -LiteralPath "$($vol)PS5UEFI.LOG" -Pattern 'start screen: (\S+ v[0-9.]+)' | Select-Object -Last 1).Matches.Groups[1].Value } else { $null }
         $cfg = if ($vol -and (Test-Path -LiteralPath "$($vol)PS5UEFI.CFG")) { ((Get-Content -LiteralPath "$($vol)PS5UEFI.CFG") -match '^(preset|lang|fastboot|bootorder|pstates)=') -join ' ' } else { 'no settings file' }
         Log "  loader: $(if ($fw) { $fw } else { 'unknown (no boot log on the system partition)' }); settings: $cfg" } 'loader'
  Log "  setup folder: $Root; payload present: $(Test-Path $Payload)"
  Log '---- end of system report ----' }
function Get-Journal { if (Test-Path $JournalFile) { @(Get-Content $JournalFile -Raw | ConvertFrom-Json) } else { @() } }
function Add-Journal($entry) { $entry.v = $SetupVersion; $entry.at = (Get-Date).ToString('s')
  $j = @(Get-Journal) + [pscustomobject]$entry; ConvertTo-Json -InputObject $j -Depth 6 | Set-Content -LiteralPath $JournalFile -Encoding UTF8 -WhatIf:$false }
function Get-RegValue([string]$Key, [string]$Name) {
  try { $p = Get-ItemProperty -LiteralPath $Key -Name $Name -ErrorAction Stop; return , $p.$Name } catch { return $null } }
function Get-RegKind([string]$Key, [string]$Name) { try { (Get-Item -LiteralPath $Key).GetValueKind($Name).ToString() } catch { $null } }
function Set-RegValue([string]$Component, [string]$Key, [string]$Name, $Value, [string]$Kind) {
  $old = Get-RegValue $Key $Name; $oldKind = Get-RegKind $Key $Name
  if ($null -ne $old -and (Compare-Object @($old) @($Value) -SyncWindow 0).Count -eq 0) { return $false }   # already right
  if ($PSCmdlet.ShouldProcess("$Key\$Name", "set $Kind $Value")) {
    if (-not (Test-Path -LiteralPath $Key)) { New-Item -Path $Key -Force | Out-Null; Add-Journal ([ordered]@{ c = $Component; t = 'key'; rk = $Key; old = 'absent' }) }
    New-ItemProperty -LiteralPath $Key -Name $Name -Value $Value -PropertyType $Kind -Force | Out-Null
    Add-Journal ([ordered]@{ c = $Component; t = 'reg'; rk = $Key; name = $Name; old = $old; oldKind = $oldKind; new = $Value; kind = $Kind })
    Log "  set $Key\$Name = $($Value -join ',') (was $(if ($null -eq $old) { 'absent' } else { $old -join ',' }))"
  }
  return $true
}
function Remove-RegKeyJournaled([string]$Component, [string]$Key) {
  if (-not (Test-Path -LiteralPath $Key)) { return }
  $vals = @{}; (Get-Item -LiteralPath $Key).Property | ForEach-Object { $vals[$_] = @{ v = (Get-RegValue $Key $_); k = (Get-RegKind $Key $_) } }
  if ($PSCmdlet.ShouldProcess($Key, 'remove key')) {
    Remove-Item -LiteralPath $Key -Recurse -Force
    Add-Journal ([ordered]@{ c = $Component; t = 'delkey'; rk = $Key; oldValues = $vals }); Log "  removed $Key" }
}
function Invoke-Tool([string]$Exe, [string[]]$ToolArgs, [int]$TimeoutSec = 180) {   # hard timeout (pnputil can hang)
  $o = "$Data\tool-$PID.out"; $e = "$Data\tool-$PID.err"
  $p = Start-Process -FilePath $Exe -ArgumentList $ToolArgs -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
  $null = $p.Handle   # v0.4: without touching Handle, ExitCode stays empty on Windows PowerShell 5.1 (every tool counted as failed)
  if (-not $p.WaitForExit($TimeoutSec * 1000)) { try { $p.Kill() } catch { }; throw "$Exe timed out after $TimeoutSec s ($($ToolArgs -join ' '))" }
  $out = ((Get-Content $o -Raw -ErrorAction SilentlyContinue) + (Get-Content $e -Raw -ErrorAction SilentlyContinue))
  Log "  $Exe $($ToolArgs -join ' ') -> $($p.ExitCode)"; return @($p.ExitCode, $out)
}
$script:Manifest = $null
function Get-Manifest { if (-not $script:Manifest) { $script:Manifest = Get-Content (Join-Path $Payload 'manifest.json') -Raw | ConvertFrom-Json }; $script:Manifest }
function Get-PayloadFiles([string]$Component) { @((Get-Manifest).files | Where-Object { $_.component -eq $Component }) }
function Get-PayloadEntry([string]$Rel) { (Get-Manifest).files | Where-Object { $_.path -eq $Rel } | Select-Object -First 1 }
function Test-PayloadFile($f) {   # manifest entry {path, sha256} -> full path, after the hash check
  $p = Join-Path $Payload ($f.path -replace '/', '\')
  if (-not (Test-Path -LiteralPath $p)) { throw "payload file missing: $($f.path)" }
  $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
  if ($h -ine $f.sha256) { throw "payload hash mismatch: $($f.path) $h != $($f.sha256)" }
  return $p
}
function Test-FileHashIs([string]$Path, [string]$Sha) { (Test-Path -LiteralPath $Path) -and (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ieq $Sha }
function Install-PayloadFile([string]$Component, [string]$Rel, [string]$Dst) {   # copy with hash check; a loaded image is renamed first
  $f = Get-PayloadEntry $Rel; if (-not $f) { throw "manifest has no $Rel" }
  if (Test-FileHashIs $Dst $f.sha256) { return $false }
  $src = Test-PayloadFile $f
  if ($PSCmdlet.ShouldProcess($Dst, "install $Rel")) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Dst) | Out-Null
    $bak = $null; if (Test-Path -LiteralPath $Dst) { $bak = "$Dst.old-$(Get-Date -Format yyyyMMddHHmmss)"; Move-Item -LiteralPath $Dst $bak -Force }
    Copy-Item -LiteralPath $src $Dst -Force
    Add-Journal ([ordered]@{ c = $Component; t = 'file'; path = $Dst; backup = $bak }); Log "  file $Dst ($($f.sha256.Substring(0,8)))" }
  return $true
}
function Get-ServiceImageFile([string]$Service) {
  $ip = Get-RegValue "HKLM:\SYSTEM\CurrentControlSet\Services\$Service" 'ImagePath'; if (-not $ip) { return $null }
  $ip = [Environment]::ExpandEnvironmentVariables($ip) -replace '^\\\?\?\\', '' -replace '^\\SystemRoot\\', "$env:SystemRoot\"
  if ($ip -match '^(?i)system32\\') { $ip = Join-Path $env:SystemRoot $ip }
  return $ip
}
function Register-Task([string]$Component, [string]$Name, $Action, $Trigger, $Settings) {
  $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
  $existed = [bool](Get-ScheduledTask -TaskName $Name -ErrorAction SilentlyContinue)
  if ($PSCmdlet.ShouldProcess($Name, 'register task')) {
    Register-ScheduledTask -TaskName $Name -Action $Action -Trigger $Trigger -Principal $p -Settings $Settings -Force | Out-Null
    if (-not $existed) { Add-Journal ([ordered]@{ c = $Component; t = 'task'; name = $Name }) }; Log "  task $Name" }
}
function Get-PciDevice([string]$Pattern) { @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like $Pattern -and $_.Present }) }   # callers count with @(...).Count (v0.4: one CIM device unrolls to a scalar without .Count on PS 5.1)
function Get-BcdCurrentElement([string]$Element) {   # language independent: BCD hive, {current} object, element value bytes
  $id = ((Invoke-Tool bcdedit.exe @('/enum', '{current}', '/v'))[1] | Select-String '\{[0-9a-fA-F-]{36}\}').Matches | Select-Object -First 1
  if (-not $id) { return $null }
  $k = "HKLM:\BCD00000000\Objects\$($id.Value)\Elements\$Element"
  $v = Get-RegValue $k 'Element'; if ($null -eq $v) { return $null }; return , ([byte[]]$v)
}
function Test-TestModeActive { (Get-RegValue 'HKLM:\SYSTEM\CurrentControlSet\Control' 'SystemStartOptions') -match 'TESTSIGNING' }

# ================================================================ signing (device-local certificate)
$CertSubject = "CN=PS5 Windows Setup local driver signing ($env:COMPUTERNAME)"
$script:SignCert = $null
function Add-MachineCert([string]$Store, [string]$CerPath, [string]$Thumbprint) {
  if (Test-Path "Cert:\LocalMachine\$Store\$Thumbprint") { return }
  $cu = Join-Path $env:SystemRoot 'System32\certutil.exe'
  if (Test-Path -LiteralPath $cu) {
    $out = (& $cu -addstore -f $Store $CerPath 2>&1) -join ' | '
    if ($LASTEXITCODE -eq 0 -and (Test-Path "Cert:\LocalMachine\$Store\$Thumbprint")) { return }
    Log "  certutil -addstore $Store exit $LASTEXITCODE ($out) - trying .NET X509Store"
  }
  $x = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $CerPath
  $st = New-Object Security.Cryptography.X509Certificates.X509Store $Store, 'LocalMachine'
  try { $st.Open('ReadWrite'); $st.Add($x) } finally { $st.Close() }
}
function Get-SignCert {
  if ($script:SignCert) { return $script:SignCert }
  if ($PSCmdlet.ShouldProcess('Cert:\LocalMachine\My', 'create device-local code-signing certificate')) {
    $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\LocalMachine\My `
           -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(30)
    $cer = Join-Path $Data "sign-$($c.Thumbprint).cer"; Export-Certificate -Cert $c -FilePath $cer | Out-Null
    foreach ($store in 'Root', 'TrustedPublisher') {
      Add-MachineCert $store $cer $c.Thumbprint
      Add-Journal ([ordered]@{ c = 'Signing'; t = 'cert'; store = $store; thumb = $c.Thumbprint }) }
    Log "  device-local signing certificate $($c.Thumbprint) (Root + TrustedPublisher, key not exportable)"
    $script:SignCert = $c }
  return $script:SignCert
}
function Remove-SignKeys {   # no private key of ours may stay on the machine
  foreach ($c in @(Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -like 'CN=PS5 Windows Setup local driver signing*' })) {
    if ($PSCmdlet.ShouldProcess($c.Thumbprint, 'delete signing certificate + private key')) { Remove-Item -LiteralPath "Cert:\LocalMachine\My\$($c.Thumbprint)" -DeleteKey; Log "  private signing key $($c.Thumbprint) deleted" } }
  $script:SignCert = $null
}
function Install-ShippedCert {   # v0.4: public certificate of the release signing key (payload\certs\pswloader-signing.cer); no private key is shipped
  if ($script:ShippedCertDone) { return }; $script:ShippedCertDone = $true
  $cer = Join-Path $Payload 'certs\pswloader-signing.cer'
  if (-not (Test-Path -LiteralPath $cer)) { Log '  no shipped signing certificate in payload\certs (older package: device-local catalogs)'; return }
  $x = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer
  foreach ($store in 'Root', 'TrustedPublisher') {
    if (Test-Path "Cert:\LocalMachine\$store\$($x.Thumbprint)") { continue }
    if ($PSCmdlet.ShouldProcess("$store $($x.Subject)", 'trust release signing certificate')) {
      Add-MachineCert $store $cer $x.Thumbprint
      Add-Journal ([ordered]@{ c = 'Signing'; t = 'cert'; store = $store; thumb = $x.Thumbprint }) } }
  Log "  release signing certificate trusted: $($x.Subject) $($x.Thumbprint.Substring(0,8))"
}
function New-SignedCatalog([string]$Dir, [string]$CatName) {
  $cat = Join-Path $Dir $CatName
  Get-ChildItem -LiteralPath $Dir -Filter *.cat -Recurse | Remove-Item -Force
  $tmp = Join-Path $Data "cat-$PID.cat"; Remove-Item $tmp -ErrorAction SilentlyContinue
  $mk = Join-Path $Payload 'tools\makecat.exe'; $st = Join-Path $Payload 'tools\signtool.exe'
  if ((Test-Path $mk) -and (Test-Path $st)) {   # optional fallback, only if someone put the SDK tools there (not shipped)
    $cdf = Join-Path $Data "cat-$PID.cdf"
    $lines = @('[CatalogHeader]', "Name=$tmp", 'ResultDir=', 'PublicVersion=0x0000001', 'EncodingType=0x00010001', 'CATATTR1=0x10010001:OSAttr:2:10.0', '[CatalogFiles]')
    $i = 0; Get-ChildItem -LiteralPath $Dir -File -Recurse | ForEach-Object { $i++; $lines += "<hash>f$i=$($_.FullName)" }
    Set-Content $cdf $lines -Encoding ascii; Invoke-Tool $mk @($cdf) | Out-Null
  } else { New-FileCatalog -Path $Dir -CatalogFilePath $tmp -CatalogVersion 2 | Out-Null }
  Move-Item $tmp $cat -Force
  $s = Set-AuthenticodeSignature -FilePath $cat -Certificate (Get-SignCert) -HashAlgorithm SHA256
  if ($s.Status -ne 'Valid') { throw "catalog signature $CatName status $($s.Status): $($s.StatusMessage)" }
  $t = Test-FileCatalog -Path $Dir -CatalogFilePath $cat -ErrorAction SilentlyContinue
  Log "  catalog $CatName signed ($($s.SignerCertificate.Thumbprint.Substring(0,8))), Test-FileCatalog=$t"
}

# ================================================================ PnP driver packages
function Get-InfCatalogName([string]$Inf) { ((Select-String -LiteralPath $Inf -Pattern '^\s*CatalogFile\s*=\s*(\S+)').Matches | Select-Object -First 1).Groups[1].Value }
function Test-Ps5PwrBound {   # v0.5.2: is ps5pwr already the function driver of 104D:9107 (then: update at the next start)
  [bool]@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { @($_.HardwareID) -like 'PCI\VEN_104D&DEV_9107*' -and $_.Service -eq 'ps5pwr' }).Count
}
function Install-DriverPackage([string]$Component, [string]$PkgRel, [string]$InfName, [scriptblock]$Prepare = $null, [string]$ReinstallAtBootHwId = '') {
  $files = @((Get-Manifest).files | Where-Object { $_.path -like "$PkgRel/*" })
  if (-not $files.Count) { throw "payload has no files under $PkgRel" }
  $stage = Join-Path $Data "stage\$(Split-Path -Leaf $PkgRel)"
  if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
  foreach ($f in $files) { $src = Test-PayloadFile $f; $dst = Join-Path $stage ($f.path.Substring($PkgRel.Length + 1) -replace '/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null; Copy-Item -LiteralPath $src $dst -Force }
  if ($Prepare) { & $Prepare $stage }
  $inf = Join-Path $stage $InfName
  $catName = Get-InfCatalogName $inf; $shipped = Join-Path $stage $catName
  Install-ShippedCert
  if ($catName -and (Test-Path -LiteralPath $shipped) -and (Get-AuthenticodeSignature -LiteralPath $shipped).Status -eq 'Valid') {
    Log "  catalog $catName shipped and validly signed ($((Get-AuthenticodeSignature -LiteralPath $shipped).SignerCertificate.Subject)) - used as is" }
  else { if ($catName -and (Test-Path -LiteralPath $shipped)) { LogWarn "shipped catalog $catName not valid here - falling back to a device-local catalog" }
    New-SignedCatalog $stage $catName }
  if (-not $PSCmdlet.ShouldProcess($inf, 'pnputil /add-driver /install')) { return }
  # v0.5.2: -ReinstallAtBootHwId = add the package only and mark the bound device(s) for reinstall at the next start
  $atBoot = @(if ($ReinstallAtBootHwId) { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { @($_.HardwareID) -like "$ReinstallAtBootHwId*" } })
  $r = if ($atBoot.Count) { Invoke-Tool pnputil.exe @('/add-driver', $inf) 300 } else { Invoke-Tool pnputil.exe @('/add-driver', $inf, '/install') 300 }
  foreach ($dev in $atBoot) {
    $ek = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)"
    $cf = [uint32](Get-RegValue $ek 'ConfigFlags'); Set-ItemProperty -LiteralPath $ek -Name ConfigFlags -Value ([int]($cf -bor 0x20)) -Type DWord
    Log "  $($dev.InstanceId): ConfigFlags 0x$('{0:x}' -f $cf) -> 0x$('{0:x}' -f ($cf -bor 0x20)) (reinstall at the next start, no device restart now)" }
  if ($atBoot.Count) { $script:RestartTwice = $true
    Log "  ${InfName}: the new driver is installed at the next start and LOADED only at the start after that - restart twice (check: ps5pwr.ps1 -Version)" }
  $oem = ([regex]::Matches($r[1], '(?i)\boem\d+\.inf\b') | Select-Object -First 1).Value
  Log "  pnputil: $(($r[1] -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 3) -join ' | ')"
  if ($r[0] -notin 0, 3010, 259) { throw "pnputil failed ($($r[0])) for $InfName" }
  if ($r[0] -eq 3010) { $script:RebootNeeded = $true }
  if ($oem) { Add-Journal ([ordered]@{ c = $Component; t = 'driver'; oem = $oem; inf = $InfName }) }
  $script:RebootNeeded = $true
}
function Get-AuthenticodeHash([string]$Path) {   # v0.4.2: PE image hash as Authenticode (without CheckSum, Security entry, cert table)
  try { $b = [IO.File]::ReadAllBytes($Path) } catch { return $null }
  if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
  $pe = [BitConverter]::ToInt32($b, 0x3C); if ($pe -lt 0 -or $pe + 0x100 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { return $null }
  $opt = $pe + 24; $magic = [BitConverter]::ToUInt16($b, $opt)
  $chk = $opt + 64; $sec = $opt + $(if ($magic -eq 0x20B) { 144 } else { 128 })   # data directory 4 = certificate table
  $cOff = [int][BitConverter]::ToUInt32($b, $sec); $cLen = [int][BitConverter]::ToUInt32($b, $sec + 4)
  $end = if ($cOff -gt 0 -and $cOff -le $b.Length) { $cOff } else { $b.Length }
  $h = [Security.Cryptography.SHA256]::Create()
  [void]$h.TransformBlock($b, 0, $chk, $null, 0)
  [void]$h.TransformBlock($b, $chk + 4, $sec - $chk - 4, $null, 0)
  [void]$h.TransformBlock($b, $sec + 8, $end - $sec - 8, $null, 0)
  if ($cOff -gt 0 -and $cOff + $cLen -lt $b.Length) { [void]$h.TransformBlock($b, $cOff + $cLen, $b.Length - $cOff - $cLen, $null, 0) }
  [void]$h.TransformFinalBlock([byte[]]::new(0), 0, 0)
  return (-join ($h.Hash | ForEach-Object { $_.ToString('X2') }))
}
function Test-DriverCurrent([string]$Service, [string]$SysRel) {   # the service image is our payload build (file or code identical)
  $f = Get-PayloadEntry $SysRel; $img = Get-ServiceImageFile $Service
  if (-not ($f -and $img)) { return $false }
  if (Test-FileHashIs $img $f.sha256) { return $true }
  # v0.4.2: same code with another signature (e.g. signed on the console vs. on the host) - no reinstall needed
  $a = Get-AuthenticodeHash $img; $p = Get-AuthenticodeHash (Join-Path $Payload $SysRel)
  if ($a -and $p -and $a -eq $p) { Log "  ${Service}: installed image differs in its signature only (code hash $($a.Substring(0, 16))) - current"; return $true }
  return $false
}

# ================================================================ components
$CS = 'HKLM:\SYSTEM\CurrentControlSet'
$DiskClass = "$CS\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
function Get-NvmeInstanceKeys {
  $out = @()
  $pciRoot = "$CS\Enum\PCI"
  if (Test-Path $pciRoot) {
    foreach ($dev in Get-ChildItem $pciRoot -ErrorAction SilentlyContinue) {
      foreach ($inst in Get-ChildItem $dev.PSPath -ErrorAction SilentlyContinue) {
        $cids = @((Get-ItemProperty $inst.PSPath -Name CompatibleIDs -ErrorAction SilentlyContinue).CompatibleIDs)
        $hids = @((Get-ItemProperty $inst.PSPath -Name HardwareID -ErrorAction SilentlyContinue).HardwareID)
        if ((($cids + $hids) -join "`n") -match '(?i)PCI\\CC_010802') { $out += "$CS\Enum\PCI\$($dev.PSChildName)\$($inst.PSChildName)" }
      }
    }
  }
  return @($out)
}
function Find-EspVbios {   # the firmware exports the VBIOS at every boot: \PS5VBIOS.BIN on the boot ESP (or a stick)
  $cands = @()
  $free = [char[]](70..90) | Where-Object { -not (Test-Path "$($_):\") } | Select-Object -Last 1
  if ($free) { $null = Invoke-Tool mountvol.exe @("$($free):", '/S'); $cands += "$($free):\PS5VBIOS.BIN" }
  $cands += @(Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -and $_.FileSystemType -match 'FAT' } | ForEach-Object { "$($_.DriveLetter):\PS5VBIOS.BIN" })
  try {
    foreach ($c in $cands) { if (Test-Path -LiteralPath $c) { $b = [IO.File]::ReadAllBytes($c)
        if ($b.Length -ge 32768 -and $b[0] -eq 0x55 -and $b[1] -eq 0xAA) { $t = Join-Path $Data 'ps5-vbios.bin'; [IO.File]::WriteAllBytes($t, $b); return @($c, $t) }
        Log "  ignoring $c (size $($b.Length), no 55AA ROM header)" } }
  } finally { if ($free) { $null = Invoke-Tool mountvol.exe @("$($free):", '/D') } }
  return $null
}

$Comp = [ordered]@{}
$Comp.Preflight = @{ Default = $true; Phase = 1
  Detect = { $r = [ordered]@{}
    $r.Elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $r.Ps5Gpu = @(Get-PciDevice 'PCI\VEN_1002&DEV_13*' | Where-Object { $_.InstanceId -match 'DEV_13(FC|FE|DB|DC|DA|F9|FA|FB)' }).Count -gt 0
    $r.Ps5Icc = @(Get-PciDevice 'PCI\VEN_104D&DEV_9107*').Count -gt 0
    $r.Build22631 = [Environment]::OSVersion.Version.Build -ge 22631
    $sb = $false; try { $sb = Confirm-SecureBootUEFI } catch { }; $r.SecureBootOff = -not $sb
    $r.FreeSpace2GB = (Get-PSDrive C).Free -gt 2GB
    $r }
  Install = { $r = & $Comp.Preflight.Detect; $bad = @($r.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object Key)
    if ($bad.Count -and -not $Force) { $script:PreflightFailed = $true; throw "preflight failed: $($bad -join ', ') (not a PS5 or unsupported Windows; -Force overrides)" } }
}
$Comp.Boot = @{ Default = $true; Phase = 1   # BCD as on the reference install; bcdedit keywords are not localized
  Detect = { [ordered]@{ TestSigning = ((Get-BcdCurrentElement '16000049') -join ',') -eq '1'
                         NoDynamicTick = ((Get-BcdCurrentElement '260000a5') -join ',') -eq '1'
                         IgnoreAllFailures = (($b = Get-BcdCurrentElement '250000e0') -and $b[0] -eq 1)
                         TestModeActive = Test-TestModeActive } }
  Install = {
    foreach ($s in @(@('testsigning', 'on', '16000049'), @('disabledynamictick', 'yes', '260000a5'), @('bootstatuspolicy', 'IgnoreAllFailures', '250000e0'))) {
      $old = Get-BcdCurrentElement $s[2]
      $want = if ($s[2] -eq '250000e0') { $old -and $old[0] -eq 1 } else { ($old -join ',') -eq '1' }
      if (-not $want -and $PSCmdlet.ShouldProcess('BCD {current}', "$($s[0]) $($s[1])")) {
        $r = Invoke-Tool bcdedit.exe @('/set', '{current}', $s[0], $s[1]); if ($r[0] -ne 0) { throw "bcdedit /set $($s[0]) failed: $($r[1])" }
        Add-Journal ([ordered]@{ c = 'Boot'; t = 'bcd'; id = '{current}'; name = $s[0]; old = $(if ($null -eq $old) { 'absent' } else { $old -join ',' }) })
        $script:RebootNeeded = $true } }
  }
}
$Comp.Gpu = @{ Default = $true; Phase = 2   # bc250kmd v10.14 (release set v1.0, ps5gpu.inf + shipped catalog, VBIOS in System32\drivers), EnableDisplayModes=0 via generation override (ps5-postboot)
  Detect = { [ordered]@{ KmdCurrent = Test-DriverCurrent 'bc250kmd' 'drivers/bc250kmd/bc250kmd.sys'
                         DisplayModesFactory = (Get-RegValue "$CS\Services\bc250kmd\Parameters\DefaultsV2" 'EnableDisplayModes') -eq 2
                         DesktopRouter = (Get-RegValue 'HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter' 'DwmForceCpu') -eq 0
                         TdrDelay10 = (Get-RegValue "$CS\Control\GraphicsDrivers" 'TdrDelay') -eq 10 } }
  Install = {
    if (-not (& $Comp.Gpu.Detect).KmdCurrent -or $Force) {
      $vb = Find-EspVbios
      if (-not $vb) { throw 'VBIOS not found: the firmware writes \PS5VBIOS.BIN to the boot ESP at every start. Boot once with PSWloader and run the setup again.' }
      Log "  VBIOS from $($vb[0]) sha256 $((Get-FileHash $vb[1] -Algorithm SHA256).Hash.Substring(0,16))"
      $rom = [IO.File]::ReadAllBytes($vb[1]); $len = if ($rom.Length -ge 3) { [int]$rom[2] * 512 } else { 0 }   # v0.4: same check as the KMD (BC250_ATOM_CHECKSUM)
      $sum = 0; if ($len -ge 512 -and $len -le $rom.Length) { for ($k = 0; $k -lt $len; $k++) { $sum = ($sum + $rom[$k]) % 256 } }
      if ($rom.Length -lt 3 -or $rom[0] -ne 0x55 -or $rom[1] -ne 0xAA -or $len -lt 512 -or $len -gt $rom.Length -or $sum -ne 0) {
        throw "VBIOS $($vb[0]) is not a valid ROM image (signature/length/checksum, sum=$sum, image $len of $($rom.Length) bytes). Start the console once with PSWloader firmware r5.19 or newer, then run the setup again." }
      Log "  VBIOS image $len bytes, checksum ok"
      # v0.4 (release set v1.0): the VBIOS goes to System32\drivers (KMD v10.14 reads it there), NOT into the package -
      # the shipped ps5gpu.cat covers every package file, and an edit or an extra file would break it
      $vbDst = Join-Path $env:SystemRoot 'System32\drivers\ps5-vbios.bin'
      if ($PSCmdlet.ShouldProcess($vbDst, 'copy console VBIOS')) { Copy-Item -LiteralPath $vb[1] $vbDst -Force; Log "  VBIOS copied to $vbDst" }
      $gpuInf = if (Test-Path -LiteralPath (Join-Path $Payload 'drivers\bc250kmd\ps5gpu.inf')) { 'ps5gpu.inf' } else { 'bc250kmd.inf' }
      Install-DriverPackage Gpu 'drivers/bc250kmd' $gpuInf {
        param($stage)
        if ($gpuInf -eq 'bc250kmd.inf') {   # older package without a shipped catalog: previous way (VBIOS in the package; DefaultsV2 stays factory)
          Copy-Item $vb[1] (Join-Path $stage 'ps5-vbios.bin') -Force
          Remove-Item (Join-Path $stage 'LICENSE.amdgpu') -ErrorAction SilentlyContinue } }
    }
    Set-RegValue Gpu "$CS\Services\bc250kmd\Parameters\DefaultsV2" 'EnableDisplayModes' 2 DWord | Out-Null   # catalog factory value; KMD refuses to start otherwise (mode 0 = generation override)
    foreach ($kv in @(@('DwmForceCpu', 0), @('RequireKmdSwitches', 0), @('DirectFlipFront', 0))) { Set-RegValue Gpu 'HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter' $kv[0] $kv[1] DWord | Out-Null }
    Set-RegValue Gpu "$CS\Control\GraphicsDrivers" 'TdrDelay' 10 DWord | Out-Null; Set-RegValue Gpu "$CS\Control\GraphicsDrivers" 'TdrDdiDelay' 10 DWord | Out-Null
    foreach ($d in Get-PciDevice 'PCI\VEN_1002&DEV_13EA*') {   # HDMI audio: the inbox INF writes MSISupported=0 -> Code 12
      Set-RegValue Gpu "$CS\Enum\$($d.InstanceId)\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties" 'MSISupported' 1 DWord | Out-Null }
  }
}
$Comp.Power = @{ Default = $true; Phase = 2   # ps5pwr from the payload (release set: v1.12.0 = v1.10.3 + boost HOLD / above-stock request with readback, or v1.10.3; checked by payload hash) + CLI where ThermalManager v1.6.7 looks for it
  Detect = { [ordered]@{ DriverCurrent = Test-DriverCurrent 'ps5pwr' 'drivers/ps5pwr/ps5pwr.sys'
                         Cli = Test-FileHashIs 'C:\BC250\drivers\ps5pwr\v1.10\ps5pwr.ps1' (Get-PayloadEntry 'tools/ps5pwr/ps5pwr.ps1').sha256 } }
  Install = { if (-not (Test-DriverCurrent 'ps5pwr' 'drivers/ps5pwr/ps5pwr.sys') -or $Force) { Install-DriverPackage Power 'drivers/ps5pwr' 'ps5pwr.inf' -ReinstallAtBootHwId $(if (Test-Ps5PwrBound) { 'PCI\VEN_104D&DEV_9107' } else { '' }) }
              Install-PayloadFile Power 'tools/ps5pwr/ps5pwr.ps1' 'C:\BC250\drivers\ps5pwr\v1.10\ps5pwr.ps1' | Out-Null }
}
$Comp.SelfCheck = @{ Default = $true; Phase = 2   # v0.5.6: support tool - C:\PSWloader\selfcheck\PSWSelfCheck.cmd writes one zip with every log to ...\results
  Detect = { [ordered]@{ Script = Test-FileHashIs 'C:\PSWloader\selfcheck\PSWSelfCheck.ps1' (Get-PayloadEntry 'tools/selfcheck/PSWSelfCheck.ps1').sha256
                         Launcher = Test-FileHashIs 'C:\PSWloader\selfcheck\PSWSelfCheck.cmd' (Get-PayloadEntry 'tools/selfcheck/PSWSelfCheck.cmd').sha256
                         Watch = Test-FileHashIs 'C:\PSWloader\selfcheck\PSWWatch.ps1' (Get-PayloadEntry 'tools/selfcheck/PSWWatch.ps1').sha256
                         WatchTask = [bool](Get-ScheduledTask -TaskName 'PSWloader Watch' -ErrorAction SilentlyContinue) } }
  Install = { Install-PayloadFile SelfCheck 'tools/selfcheck/PSWSelfCheck.ps1' 'C:\PSWloader\selfcheck\PSWSelfCheck.ps1' | Out-Null
              Install-PayloadFile SelfCheck 'tools/selfcheck/PSWSelfCheck.cmd' 'C:\PSWloader\selfcheck\PSWSelfCheck.cmd' | Out-Null
              Install-PayloadFile SelfCheck 'tools/selfcheck/PSWWatch.ps1' 'C:\PSWloader\selfcheck\PSWWatch.ps1' | Out-Null   # v0.5.7: background log, at startup and every minute
              Register-Task SelfCheck 'PSWloader Watch' (New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\PSWloader\selfcheck\PSWWatch.ps1"') `
                @((New-ScheduledTaskTrigger -AtStartup), (New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 1))) `
                (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable) }
}
$Comp.PowerButton = @{ Default = $true; Phase = 2   # listener for ps5pwr v1.10 power-button presses -> Windows power-button action
  Detect = { [ordered]@{ Exe = Test-FileHashIs 'C:\ps5-work\ps5-powerbutton.exe' (Get-PayloadEntry 'scripts/ps5-powerbutton.exe').sha256
                         Task = [bool](Get-ScheduledTask -TaskName 'PSW-PowerButton' -ErrorAction SilentlyContinue) } }
  Install = {
    Install-PayloadFile PowerButton 'scripts/ps5-powerbutton.exe' 'C:\ps5-work\ps5-powerbutton.exe' | Out-Null
    Remove-LegacyTask 'PS5-PowerButton'
    Register-Task PowerButton 'PSW-PowerButton' (New-ScheduledTaskAction -Execute 'C:\ps5-work\ps5-powerbutton.exe') (New-ScheduledTaskTrigger -AtStartup) `
      (New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartInterval (New-TimeSpan -Minutes 1) -RestartCount 999)
  }
}
$Comp.Network = @{ Default = $true; Phase = 2   # ps5gbe 1.0.10.0 (0382452a), DHCP (no static address for users)
  Detect = { [ordered]@{ DriverCurrent = Test-DriverCurrent 'ps5gbe' 'drivers/ps5gbe/ps5gbe.sys' } }
  Install = { if (-not (Test-DriverCurrent 'ps5gbe' 'drivers/ps5gbe/ps5gbe.sys') -or $Force) { Install-DriverPackage Network 'drivers/ps5gbe' 'ps5gbe.inf' } }
}
$Comp.WiFi = @{ Default = $true; Phase = 2      # ps5wifi 1.0.0 (IW620 1B4B:2B56, NDIS 6.20 Native 802.11)
  Detect = { [ordered]@{ DriverCurrent = Test-DriverCurrent 'ps5wifi' 'drivers/ps5wifi/ps5wifi.sys' } }
  Install = { if ((Test-Path (Join-Path $Payload 'drivers/ps5wifi/ps5wifi.inf')) -and (-not (Test-DriverCurrent 'ps5wifi' 'drivers/ps5wifi/ps5wifi.sys') -or $Force)) { Install-DriverPackage WiFi 'drivers/ps5wifi' 'ps5wifi.inf' } }
}
$Comp.Storage = @{ Phase = 2
  # only when the internal SanDisk (15B7:5063) is present; values as on the reference install (boot-start filters, MSI limit 1).
  # Skipped when the filters already came in as INF packages through Windows Setup (winpe-drivers\): never twice (GPT shift).
  Default = $false   # set below from the hardware
  Detect = {
    $r = [ordered]@{}
    $uf = @(Get-RegValue $DiskClass 'UpperFilters'); $r.UpperFiltersFirst = ($uf.Count -gt 0 -and $uf[0] -ieq 'ps5m2flt')
    $r.m2fltBoot = ((Get-RegValue "$CS\Services\ps5m2flt" 'Start') -eq 0)
    $r.nvmsiBoot = ((Get-RegValue "$CS\Services\ps5nvmsi" 'Start') -eq 0)
    $r.Files = (Test-FileHashIs "$env:SystemRoot\System32\drivers\ps5m2flt.sys" (Get-PayloadEntry 'drivers/ps5m2flt/ps5m2flt.sys').sha256) -and
               (Test-FileHashIs "$env:SystemRoot\System32\drivers\ps5nvmsi.sys" (Get-PayloadEntry 'drivers/ps5nvmsi/ps5nvmsi.sys').sha256)
    $r.NoStartOverride = -not (@('ps5m2flt', 'ps5nvmsi', 'stornvme') | Where-Object { Test-Path "$CS\Services\$_\StartOverride" })
    $inst = @(Get-NvmeInstanceKeys)
    $r.LowerFilters = ($inst.Count -eq 1 -and ([string[]](Get-RegValue $inst[0] 'LowerFilters')) -contains 'ps5nvmsi')
    $r.MsiLimit1 = ($inst.Count -eq 1 -and (Get-RegValue "$($inst[0])\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties" 'MessageNumberLimit') -eq 1)
    $r }
  Install = {
    $drv = (Invoke-Tool pnputil.exe @('/enum-drivers'))[1]
    if ($drv -match '(?i)ps5m2flt\.inf|ps5nvmsi\.inf') { Log '  storage filters already installed as INF packages (Windows Setup path) - skipped'; return }
    $inst = @(Get-NvmeInstanceKeys)
    if ($inst.Count -ne 1) { throw "expected exactly one SanDisk 15B7:5063 instance, found $($inst.Count)" }
    $lf = @(Get-RegValue $inst[0] 'LowerFilters') | Where-Object { $_ }
    if ($lf.Count -and -not ($lf.Count -eq 1 -and $lf[0] -ieq 'ps5nvmsi')) { throw "NVMe instance has foreign LowerFilters: $($lf -join ',')" }
    Install-PayloadFile Storage 'drivers/ps5m2flt/ps5m2flt.sys' "$env:SystemRoot\System32\drivers\ps5m2flt.sys" | Out-Null
    Install-PayloadFile Storage 'drivers/ps5nvmsi/ps5nvmsi.sys' "$env:SystemRoot\System32\drivers\ps5nvmsi.sys" | Out-Null
    $sv = "$CS\Services\ps5m2flt"
    foreach ($kv in @(@('Type', 1, 'DWord'), @('Start', 0, 'DWord'), @('ErrorControl', 1, 'DWord'), @('Group', 'System Bus Extender', 'String'),
                      @('ImagePath', 'System32\drivers\ps5m2flt.sys', 'ExpandString'), @('DisplayName', 'PS5 M.2 Nested GPT Overlay Filter', 'String'))) { Set-RegValue Storage $sv $kv[0] $kv[1] $kv[2] | Out-Null }
    $sv = "$CS\Services\ps5nvmsi"
    foreach ($kv in @(@('Type', 1, 'DWord'), @('Start', 0, 'DWord'), @('ErrorControl', 1, 'DWord'), @('Group', 'SCSI miniport', 'String'),
                      @('ImagePath', 'System32\drivers\ps5nvmsi.sys', 'ExpandString'), @('DisplayName', 'ps5nvmsi', 'String'))) { Set-RegValue Storage $sv $kv[0] $kv[1] $kv[2] | Out-Null }
    foreach ($kv in @(@('InstalledBy', 'install-ps5nvmsi', 'String'), @('MsiAddrTemplate', 0xFEE0000C, 'DWord'), @('MsiDataTemplate', 0x4900, 'DWord'),
                      @('MsiDestKind', 2, 'DWord'), @('OrigLowerFiltersPresent', 0, 'DWord'))) { Set-RegValue Storage "$sv\Parameters" $kv[0] $kv[1] $kv[2] | Out-Null }
    foreach ($s in 'ps5m2flt', 'ps5nvmsi', 'stornvme') { Remove-RegKeyJournaled Storage "$CS\Services\$s\StartOverride" }
    $uf = @(@(Get-RegValue $DiskClass 'UpperFilters') | ForEach-Object { $_ } | Where-Object { $_ -and $_.Trim() -ine 'ps5m2flt' })
    Set-RegValue Storage $DiskClass 'UpperFilters' ([string[]](@('ps5m2flt') + $uf)) MultiString | Out-Null
    Set-RegValue Storage $inst[0] 'LowerFilters' ([string[]]@('ps5nvmsi')) MultiString | Out-Null
    $scsiClass = "$CS\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}"
    $clf = @(@(Get-RegValue $scsiClass 'LowerFilters') | ForEach-Object { $_ } | Where-Object { $_ -and $_.Trim() -ine 'ps5nvmsi' })
    if ($clf.Count -gt 0) { Set-RegValue Storage $scsiClass 'LowerFilters' ([string[]]$clf) MultiString | Out-Null }
    elseif ($null -ne (Get-RegValue $scsiClass 'LowerFilters')) { Remove-ItemProperty -Path $scsiClass -Name 'LowerFilters' -ErrorAction SilentlyContinue }
    $msi = "$($inst[0])\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties"
    Set-RegValue Storage $msi 'MSISupported' 1 DWord | Out-Null; Set-RegValue Storage $msi 'MessageNumberLimit' 1 DWord | Out-Null
    $script:RebootNeeded = $true
  }
  Guard = { if ((Get-BootDiskIsSandisk) -and $Mode -eq 'Uninstall') { throw 'refusing: Windows runs from the SanDisk; removing the storage filters would make it unbootable (0x7B)' } }
}
$Comp.Cmos = @{ Default = $true; Phase = 2   # ps5cmos v1.5 (6ccae2fb, HW-confirmed 16.11.): RTC compatibility for the CPUID SDK (3DMark SystemInfo)
  Detect = { [ordered]@{ ServiceSystemStart = ((Get-RegValue "$CS\Services\ps5cmos" 'Start') -eq 1 -and (Get-RegValue "$CS\Services\ps5cmos" 'Type') -eq 1)
                         FileCurrent = Test-FileHashIs "$env:SystemRoot\System32\drivers\ps5cmos.sys" (Get-PayloadEntry 'drivers/ps5cmos/ps5cmos.sys').sha256
                         NoCpuz165Stub = -not ((Get-RegValue "$CS\Services\cpuz165" 'ImagePath') -match 'ps5-blocked|ps5-work\\cmos') } }
  Install = {
    if (Install-PayloadFile Cmos 'drivers/ps5cmos/ps5cmos.sys' "$env:SystemRoot\System32\drivers\ps5cmos.sys") { $script:RebootNeeded = $true }
    $exists = (Invoke-Tool sc.exe @('query', 'ps5cmos'))[0] -eq 0
    if ($PSCmdlet.ShouldProcess('ps5cmos', 'service system start')) {
      if ($exists) { Invoke-Tool sc.exe @('config', 'ps5cmos', 'type=', 'kernel', 'start=', 'system', 'error=', 'normal', 'binPath=', 'System32\drivers\ps5cmos.sys') | Out-Null }
      else { Invoke-Tool sc.exe @('create', 'ps5cmos', 'type=', 'kernel', 'start=', 'system', 'error=', 'normal', 'binPath=', 'System32\drivers\ps5cmos.sys', 'DisplayName=', '"PS5 RTC compatibility"') | Out-Null
             Add-Journal ([ordered]@{ c = 'Cmos'; t = 'service'; name = 'ps5cmos' }); $script:RebootNeeded = $true } }
    $qc = Invoke-Tool sc.exe @('qc', 'cpuz165')
    if ($qc[0] -eq 0 -and $qc[1] -match 'ps5-blocked|ps5-work\\cmos' -and $PSCmdlet.ShouldProcess('cpuz165', 'remove old stub service')) {
      Invoke-Tool sc.exe @('sdset', 'cpuz165', 'D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)') | Out-Null
      Invoke-Tool sc.exe @('delete', 'cpuz165') | Out-Null; Add-Journal ([ordered]@{ c = 'Cmos'; t = 'removedservice'; name = 'cpuz165'; qc = $qc[1] }) }
  }
}
$Comp.Names = @{ Default = $true; Phase = 2   # "AMD Oberon (PS5 Custom GPU)" (set-hw-names.ps1; the CPU name comes from the firmware)
  Detect = { $d = @(Get-PciDevice 'PCI\VEN_1002&DEV_13*' | Where-Object { $_.InstanceId -match 'DEV_13(FC|FE|DB|DC|DA|F9|FA|FB)' }); [ordered]@{ GpuName = ($d.Count -eq 1 -and (Get-RegValue "$CS\Enum\$($d[0].InstanceId)" 'FriendlyName') -eq 'AMD Oberon (PS5 Custom GPU)') } }
  Install = {
    if ((& $Comp.Names.Detect).GpuName) { return }
    $s = Install-PayloadFile Names 'scripts/set-hw-names.ps1' 'C:\ps5-work\set-hw-names.ps1'
    $g = @(Get-PciDevice 'PCI\VEN_1002&DEV_13*' | Where-Object { $_.InstanceId -match 'DEV_13(FC|FE|DB|DC|DA|F9|FA|FB)' })
    if (-not $g.Count -or -not (Get-RegValue "$CS\Enum\$($g[0].InstanceId)" 'Driver')) { Log '  GPU has no driver key yet - names follow on the next run (PSW-BootConfirm does not rename)'; return }
    if ($PSCmdlet.ShouldProcess('GPU names', 'set-hw-names apply')) {
      $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ps5-work\set-hw-names.ps1 -Mode apply 2>&1
      Log "  set-hw-names: exit $LASTEXITCODE $(($o | Select-Object -Last 2) -join ' | ')"
      if ($LASTEXITCODE -eq 0) { Add-Journal ([ordered]@{ c = 'Names'; t = 'script'; file = 'C:\ps5-work\set-hw-names.ps1'; args = @('-Mode', 'revert') }) } }
  }
}
$Comp.System = @{ Default = $true; Phase = 2   # CrashControl (A1), PnP boot wait (R22), no driver replacement by Windows Update (R26)
  Detect = { [ordered]@{ AutoReboot0 = (Get-RegValue "$CS\Control\CrashControl" 'AutoReboot') -eq 0
                         KernelDump = (Get-RegValue "$CS\Control\CrashControl" 'CrashDumpEnabled') -eq 2
                         BootPartitionWait = (Get-RegValue "$CS\Control\PnP" 'PollBootPartitionTimeout') -eq 0x1d4c0
                         NoWuDrivers = (Get-RegValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' 'ExcludeWUDriversInQualityUpdate') -eq 1 } }
  Install = {
    foreach ($kv in @(@('AutoReboot', 0), @('DisplayParameters', 1), @('CrashDumpEnabled', 2))) { Set-RegValue System "$CS\Control\CrashControl" $kv[0] $kv[1] DWord | Out-Null }
    Set-RegValue System "$CS\Control\PnP" 'PollBootPartitionTimeout' 0x1d4c0 DWord | Out-Null
    Set-RegValue System 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' 'ExcludeWUDriversInQualityUpdate' 1 DWord | Out-Null
    Set-RegValue System 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching' 'SearchOrderConfig' 0 DWord | Out-Null
  }
}
function Get-PowerValue([string]$Sub, [string]$Setting) {   # last two hex values of powercfg /q = AC, DC (language independent)
  $h = [regex]::Matches(((Invoke-Tool powercfg.exe @('/q', 'SCHEME_CURRENT', $Sub, $Setting))[1]), '0x[0-9a-fA-F]{8}') | Select-Object -Last 2 | ForEach-Object { [Convert]::ToInt32($_.Value, 16) }
  return , @($h) }
$UsbSub = '2a737441-1930-4402-8d77-b2bebba308a3'; $UsbLpm = 'd4e98f31-5ffe-4ce1-be31-1b38b384c009'   # v0.5.4
$PowerSettings = @(@('SUB_SLEEP', 'STANDBYIDLE', 0), @('SUB_VIDEO', 'VIDEOIDLE', 0), @('SUB_SLEEP', 'HIBERNATEIDLE', 0), @('SUB_PROCESSOR', 'PROCTHROTTLEMAX', 100),
                   @($UsbSub, '48e6b7a6-50f5-4782-a5d4-53bb8f07e226', 0), @($UsbSub, $UsbLpm, 0))   # v0.5.4: USB selective suspend off, USB 3 link power management (U1/U2) off
$Comp.PowerPlan = @{ Default = $true; Phase = 2   # never sleep / screen off, hibernate + fast startup off
  Detect = { $r = [ordered]@{}; foreach ($s in $PowerSettings) { $v = Get-PowerValue $s[0] $s[1]; $r[$s[1]] = ($v.Count -eq 2 -and $v[0] -eq $s[2] -and $v[1] -eq $s[2]) }
             $r.HibernateOff = -not (Test-Path "$env:SystemDrive\hiberfil.sys" -ErrorAction SilentlyContinue); $r }
  Install = {
    Invoke-Tool powercfg.exe @('-attributes', $UsbSub, $UsbLpm, '-ATTRIB_HIDE') | Out-Null   # v0.5.4: the LPM setting is hidden by default; powercfg /q lists it only when visible
    foreach ($s in $PowerSettings) { $v = Get-PowerValue $s[0] $s[1]
      if ($v.Count -eq 2 -and $v[0] -eq $s[2] -and $v[1] -eq $s[2]) { continue }
      if ($PSCmdlet.ShouldProcess("power $($s[1])", "AC/DC $($s[2])")) {
        Invoke-Tool powercfg.exe @('/setacvalueindex', 'SCHEME_CURRENT', $s[0], $s[1], "$($s[2])") | Out-Null
        Invoke-Tool powercfg.exe @('/setdcvalueindex', 'SCHEME_CURRENT', $s[0], $s[1], "$($s[2])") | Out-Null
        Add-Journal ([ordered]@{ c = 'PowerPlan'; t = 'power'; sub = $s[0]; name = $s[1]; old = $v }) } }
    Invoke-Tool powercfg.exe @('/setactive', 'SCHEME_CURRENT') | Out-Null
    if (-not (& $Comp.PowerPlan.Detect).HibernateOff -and $PSCmdlet.ShouldProcess('hibernate', 'off')) {
      Invoke-Tool powercfg.exe @('/hibernate', 'off') | Out-Null; Add-Journal ([ordered]@{ c = 'PowerPlan'; t = 'hibernate'; old = 'on' }) }
  }
}
$Comp.Thermal = @{ Default = $true; Phase = 2   # ThermalManager v1.6.7 (BAPM init + boost window in standard/oc, temperature-led fan), guard v18.3 (file name kept: thermal-guard-v17.ps1), supervisor v1.2, fan target task
  Detect = { $r = [ordered]@{}
    foreach ($f in Get-PayloadFiles Thermal) { $r[(Split-Path -Leaf $f.path)] = Test-FileHashIs "C:\ps5-work\$(Split-Path -Leaf $f.path)" $f.sha256 }
    foreach ($t in 'PSW-FanTarget', 'PSW-ThermalManager', 'PSW-ThermalGuard') { $r[$t] = [bool](Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) }
    $r }
  Install = {
    foreach ($f in Get-PayloadFiles Thermal) { Install-PayloadFile Thermal $f.path "C:\ps5-work\$(Split-Path -Leaf $f.path)" | Out-Null }
    $ps = 'powershell.exe'; $sx = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $t = New-ScheduledTaskTrigger -AtStartup
    foreach ($o in 'PS5-FanTarget-60C', 'PS5-ThermalManager', 'PS5-TimeSpy-ThermalGuard-v17') { Remove-LegacyTask $o }
    Register-Task Thermal 'PSW-FanTarget' (New-ScheduledTaskAction -Execute 'C:\ps5-work\fan-target-startup-fast.exe') $t `
      (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 4) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries)
    $t = New-ScheduledTaskTrigger -AtLogOn; $t.Delay = 'PT120S'
    Register-Task Thermal 'PSW-ThermalManager' (New-ScheduledTaskAction -Execute $ps -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ps5-work\ps5-thermal-manager.ps1 -OcMaxMhz 2230') $t $sx
    Register-Task Thermal 'PSW-ThermalGuard' (New-ScheduledTaskAction -Execute $ps -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ps5-work\thermal-supervisor.ps1') (New-ScheduledTaskTrigger -AtStartup) $sx
  }
}
$Comp.Tasks = @{ Default = $true; Phase = 2   # one boot-confirm task instead of T1+T2 (+ A1 / EnableDisplayModes=0 per generation)
  Detect = { [ordered]@{ Script = Test-FileHashIs 'C:\ps5-work\ps5-postboot.ps1' (Get-PayloadEntry 'scripts/ps5-postboot.ps1').sha256
                         Task = [bool](Get-ScheduledTask -TaskName 'PSW-BootConfirm' -ErrorAction SilentlyContinue) } }
  Install = {
    Install-PayloadFile Tasks 'scripts/ps5-postboot.ps1' 'C:\ps5-work\ps5-postboot.ps1' | Out-Null
    $t1 = New-ScheduledTaskTrigger -AtLogOn; $t1.Delay = 'PT90S'; $t2 = New-ScheduledTaskTrigger -AtStartup; $t2.Delay = 'PT4M'
    Remove-LegacyTask 'PS5-BootConfirm'
    Register-Task Tasks 'PSW-BootConfirm' (New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ps5-work\ps5-postboot.ps1') @($t1, $t2) `
      (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries)
  }
}
$Comp.VCRedist = @{ Default = $true; Phase = 2   # v0.5.0: Microsoft Visual C++ 2015-2022 Redistributable x64 + x86, offline (payload\vcredist)
  Detect = { $r = [ordered]@{}
    foreach ($a in 'x64', 'x86') {
      $e = Get-PayloadEntry "vcredist/VC_redist.$a.exe"; $want = if ($e -and $e.version) { [version]$e.version } else { [version]'14.0' }
      $k = if ($a -eq 'x86') { 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x86' } else { 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' }
      $have = $null; try { $have = [version]("$(Get-RegValue $k 'Version')".TrimStart('v')) } catch { }
      $r[$a] = ((Get-RegValue $k 'Installed') -eq 1) -and $have -and $have -ge $want }
    $r }
  Install = {
    foreach ($a in 'x64', 'x86') {
      if ((& $Comp.VCRedist.Detect)[$a]) { continue }
      $f = Test-PayloadFile (Get-PayloadEntry "vcredist/VC_redist.$a.exe")
      if ($PSCmdlet.ShouldProcess("VC_redist.$a.exe", 'install /quiet')) {
        $pr = Start-Process -FilePath $f -ArgumentList '/install', '/quiet', '/norestart' -Wait -PassThru
        Log "  VC_redist.$a.exe exit $($pr.ExitCode)"
        if ($pr.ExitCode -eq 3010) { $script:RebootNeeded = $true }
        elseif ($pr.ExitCode -ne 0 -and $pr.ExitCode -ne 1638) { LogWarn "VC_redist.$a.exe exit $($pr.ExitCode)" } } }
    # no journal entry: the runtime is shared with other programs and stays installed on uninstall
  }
}
$Comp.Watermark = @{ Default = $true; Phase = 2   # v0.5.0: the test-mode text on the desktop is hidden (user choice), -Restore undoes it
  Detect = { [ordered]@{ Script = Test-FileHashIs "$env:ProgramData\PSWloader\psw-hide-testmode.ps1" (Get-PayloadEntry 'scripts/psw-hide-testmode.ps1').sha256
                         Task = [bool](Get-ScheduledTask -TaskName 'PSW-HideTestMode' -ErrorAction SilentlyContinue) } }
  Install = {
    $dst = "$env:ProgramData\PSWloader\psw-hide-testmode.ps1"
    Install-PayloadFile Watermark 'scripts/psw-hide-testmode.ps1' $dst | Out-Null
    Register-Task Watermark 'PSW-HideTestMode' (New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$dst`"") `
      (New-ScheduledTaskTrigger -AtStartup) (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries)
    if ($PSCmdlet.ShouldProcess('test-mode text', 'hide now')) {
      $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $dst 2>&1
      Log "  psw-hide-testmode: exit $LASTEXITCODE $(($o | Select-Object -Last 2) -join ' | ')"
      Add-Journal ([ordered]@{ c = 'Watermark'; t = 'script'; file = $dst; args = @('-Restore') }) }
  }
}
$Comp.BootMenu = @{ Default = $true; Phase = 2   # v0.5.5 (user request): Start-menu entry "PSWloader UEFI Boot Manager" -> confirm -> shutdown /r /fw
  Detect = { [ordered]@{ Script = Test-FileHashIs "$env:ProgramData\PSWloader\psw-uefi-bootmanager.ps1" (Get-PayloadEntry 'scripts/psw-uefi-bootmanager.ps1').sha256
                         Shortcut = Test-Path "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\PSWloader UEFI Boot Manager.lnk" } }
  Install = {
    $dst = "$env:ProgramData\PSWloader\psw-uefi-bootmanager.ps1"
    Install-PayloadFile BootMenu 'scripts/psw-uefi-bootmanager.ps1' $dst | Out-Null
    $lnk = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\PSWloader UEFI Boot Manager.lnk"
    if ($PSCmdlet.ShouldProcess($lnk, 'create Start-menu shortcut (runs as administrator)')) {
      $sh = New-Object -ComObject WScript.Shell; $s = $sh.CreateShortcut($lnk)
      $s.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
      $s.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$dst`""
      $s.IconLocation = "$env:SystemRoot\System32\shell32.dll,27"; $s.Description = 'Restart into the PSWloader UEFI Boot Manager'
      $s.WindowStyle = 7; $s.Save()
      $b = [IO.File]::ReadAllBytes($lnk); $b[0x15] = $b[0x15] -bor 0x20; [IO.File]::WriteAllBytes($lnk, $b)   # LinkFlags RunAsUser = run as administrator
      Add-Journal ([ordered]@{ c = 'BootMenu'; t = 'file'; path = $lnk; backup = $null }); Log "  shortcut $lnk" }
    # pinned to Start for accounts created from now on (first install: SetupComplete runs before the first sign-in);
    # Windows 11 takes the pinned list of the default profile once per new account. An existing layout file is kept.
    $lay = "$env:SystemDrive\Users\Default\AppData\Local\Microsoft\Windows\Shell\LayoutModification.json"
    if (-not (Test-Path $lay) -and $PSCmdlet.ShouldProcess($lay, 'pin the Boot Manager entry for new accounts')) {
      New-Item -ItemType Directory -Force (Split-Path $lay) | Out-Null
      $pins = @(
        @{ desktopAppLink = '%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs\PSWloader UEFI Boot Manager.lnk' },
        @{ desktopAppLink = '%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs\Microsoft Edge.lnk' },
        @{ packagedAppId = 'windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel' },
        @{ desktopAppId = 'Microsoft.Windows.Explorer' },
        @{ packagedAppId = 'Microsoft.WindowsStore_8wekyb3d8bbwe!App' },
        @{ packagedAppId = 'Microsoft.WindowsTerminal_8wekyb3d8bbwe!App' },
        @{ packagedAppId = 'Microsoft.WindowsNotepad_8wekyb3d8bbwe!App' })
      (@{ pinnedList = $pins } | ConvertTo-Json -Depth 4) | Set-Content -Path $lay -Encoding UTF8
      Add-Journal ([ordered]@{ c = 'BootMenu'; t = 'file'; path = $lay; backup = $null }); Log "  Start pin for new accounts: $lay" }
  }
}
$Comp.Remote = @{ Default = $false; Phase = 2   # OPTIONAL (test stick: -Include Remote): OpenSSH server + key from payload\remote\authorized_keys
  Detect = { [ordered]@{ Sshd = [bool](Get-Service sshd -ErrorAction SilentlyContinue); Key = Test-Path "$env:ProgramData\ssh\administrators_authorized_keys" } }
  Install = {
    $keySrc = Join-Path $Payload 'remote\authorized_keys'
    if (-not (Test-Path $keySrc)) { throw 'Remote: payload\remote\authorized_keys missing (public key only)' }
    if (-not (Get-Service sshd -ErrorAction SilentlyContinue)) {
      $zip = Test-PayloadFile (Get-PayloadEntry 'remote/OpenSSH-Win64.zip'); $dst = "$env:ProgramFiles\OpenSSH"
      Expand-Archive -LiteralPath $zip -DestinationPath "$Data\openssh" -Force
      New-Item -ItemType Directory -Force $dst | Out-Null; Copy-Item "$Data\openssh\OpenSSH-Win64\*" $dst -Recurse -Force
      & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$dst\install-sshd.ps1" | Out-Null
      Add-Journal ([ordered]@{ c = 'Remote'; t = 'script'; file = "$dst\uninstall-sshd.ps1"; args = @() }) }
    Set-Service sshd -StartupType Automatic
    if (-not (Get-NetFirewallRule -Name sshd-in -ErrorAction SilentlyContinue)) {
      New-NetFirewallRule -Name sshd-in -DisplayName 'OpenSSH Server (sshd)' -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow | Out-Null
      Add-Journal ([ordered]@{ c = 'Remote'; t = 'fwrule'; name = 'sshd-in' }) }
    Start-Service sshd   # creates C:\ProgramData\ssh
    $ak = "$env:ProgramData\ssh\administrators_authorized_keys"; Copy-Item $keySrc $ak -Force
    Invoke-Tool icacls.exe @($ak, '/inheritance:r', '/grant', '*S-1-5-32-544:F', '/grant', '*S-1-5-18:F') | Out-Null
    Restart-Service sshd; Log '  sshd running, administrators_authorized_keys set'
    $ipf = Join-Path $Payload 'remote\static-ip.txt'   # optional, test stick only: "192.0.2.10/24 192.0.2.1 192.0.2.1" (address/prefix gateway dns)
    if (Test-Path $ipf) {
      $ip, $gw, $dns = (Get-Content $ipf -TotalCount 1).Trim() -split '\s+'; $addr, $len = $ip -split '/'
      $nic = Get-NetAdapter | Where-Object { $_.DriverFileName -match 'ps5gbe' -or $_.InterfaceDescription -match 'PS5' } | Select-Object -First 1
      if (-not $nic) { Log '  static IP: no ps5gbe adapter yet - skipped' }
      elseif (-not (Get-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $addr -ErrorAction SilentlyContinue)) {
        Set-NetIPInterface -InterfaceIndex $nic.ifIndex -Dhcp Disabled
        Get-NetIPAddress -InterfaceIndex $nic.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
        New-NetIPAddress -InterfaceIndex $nic.ifIndex -IPAddress $addr -PrefixLength ([int]$len) -DefaultGateway $gw | Out-Null
        Set-DnsClientServerAddress -InterfaceIndex $nic.ifIndex -ServerAddresses $dns
        Add-Journal ([ordered]@{ c = 'Remote'; t = 'note'; name = "static IP $ip on $($nic.Name) (undo: DHCP by hand)" }); Log "  static IP $ip gw $gw dns $dns on $($nic.Name)" } }
  }
}
$Comp.Monitor = @{ Default = $true; Phase = 2   # monitor name + modes from the maker's monitor INF: Windows Update, ONLY class Monitor (other WU drivers stay blocked)
  Detect = { $m = @(Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue); [ordered]@{ MonitorDriver = ($m.Count -eq 0 -or @($m | Where-Object FriendlyName -match '^Generic( PnP)? Monitor$').Count -eq 0) } }
  Install = {
    # v0.5.1: the WU search/download/install runs in a background job; without a signed-in user (SetupComplete) it gets
    # a hard 60 s limit, then the job is stopped and the generic monitor name stays (a later PSWSetup run retries).
    $limit = if ($FromSetupComplete -or -not @(Get-Process explorer -ErrorAction SilentlyContinue).Count) { 60 } else { 600 }
    $apply = $PSCmdlet.ShouldProcess('monitor drivers', 'download + install from Windows Update')
    $job = Start-Job -ArgumentList $apply -ScriptBlock { param($apply)
      try { $s = New-Object -ComObject Microsoft.Update.Session; $r = $s.CreateUpdateSearcher().Search("IsInstalled=0 and Type='Driver'")
        $col = New-Object -ComObject Microsoft.Update.UpdateColl
        foreach ($u in $r.Updates) { if ($u.DriverClass -eq 'Monitor') { [void]$col.Add($u); "I  monitor driver offered: $($u.Title)" } }
        if ($col.Count -eq 0) { 'I  no monitor driver offered by Windows Update (generic monitor name stays; harmless)'; return }
        if ($apply) {
          $d = $s.CreateUpdateDownloader(); $d.Updates = $col; [void]$d.Download()
          $i = $s.CreateUpdateInstaller(); $i.Updates = $col; $ir = $i.Install()
          for ($k = 0; $k -lt $col.Count; $k++) { $rc = $ir.GetUpdateResult($k).ResultCode; "I  $($col.Item($k).Title) -> result $rc"; if ($rc -ne 2) { "Wmonitor driver $($col.Item($k).Title) result $rc" } } } }
      catch { "Wmonitor driver via Windows Update skipped: $($_.Exception.Message) (offline? harmless)" } }
    $done = Wait-Job $job -Timeout $limit
    foreach ($line in @(Receive-Job $job -ErrorAction SilentlyContinue)) { if ("$line".StartsWith('W')) { LogWarn "$line".Substring(1) } else { Log "$line".Substring(1) } }
    if (-not $done) { LogWarn "monitor driver via Windows Update stopped after $limit s (slow or no network; generic monitor name stays, run PSWSetup again later)"; Stop-Job $job }
    Remove-Job $job -Force
  }
}
$Comp.Signing = @{ Default = $true; Phase = 2   # runs last: deletes our private signing key (certificates stay trusted)
  Detect = { [ordered]@{ NoPrivateKey = -not @(Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -like 'CN=PS5 Windows Setup local driver signing*' }).Count } }
  Install = { Remove-SignKeys }
}
$Comp.Storage.Default = @(Get-NvmeInstanceKeys).Count -gt 0
function Get-BootDiskIsSandisk { try { $d = Get-Partition -DriveLetter C | Get-Disk; return ($d.BusType -eq 'NVMe' -or $d.FriendlyName -match 'SanDisk|Optimus') } catch { return $false } }

# ================================================================ uninstall = replay the journal backwards
function Undo-Journal([string[]]$Only) {
  $j = @(Get-Journal); [array]::Reverse($j); $keep = @()
  foreach ($e in $j) {
    if ($Only.Count -and $e.c -notin $Only) { $keep += $e; continue }
    if ($e.c -eq 'Storage') { & $Comp.Storage.Guard }
    try {
      switch ($e.t) {
        'reg' { if ($null -eq $e.old) { Remove-ItemProperty -LiteralPath $e.rk -Name $e.name -ErrorAction SilentlyContinue } else { New-ItemProperty -LiteralPath $e.rk -Name $e.name -Value $e.old -PropertyType $e.oldKind -Force | Out-Null } }
        'key' { Remove-Item -LiteralPath $e.rk -Recurse -Force -ErrorAction SilentlyContinue }
        'delkey' { New-Item -Path $e.rk -Force | Out-Null; foreach ($p in $e.oldValues.PSObject.Properties) { New-ItemProperty -LiteralPath $e.rk -Name $p.Name -Value $p.Value.v -PropertyType $p.Value.k -Force | Out-Null } }
        'bcd' { if ($e.old -eq 'absent') { Invoke-Tool bcdedit.exe @('/deletevalue', $e.id, $e.name) | Out-Null } elseif ($e.name -ne 'bootstatuspolicy') { Invoke-Tool bcdedit.exe @('/set', $e.id, $e.name, $(if ($e.old -eq '1') { 'on' } else { 'off' })) | Out-Null } }
        'service' { Invoke-Tool sc.exe @('config', $e.name, 'start=', 'disabled') | Out-Null; Invoke-Tool sc.exe @('delete', $e.name) | Out-Null }
        'file' { Move-Item -LiteralPath $e.path "$($e.path).removed-$(Get-Date -Format yyyyMMddHHmmss)" -Force -ErrorAction SilentlyContinue
                 if ($e.backup -and (Test-Path -LiteralPath $e.backup)) { Move-Item -LiteralPath $e.backup $e.path -Force } }
        'driver' { Invoke-Tool pnputil.exe @('/delete-driver', $e.oem, '/uninstall', '/force') 300 | Out-Null }
        'cert' { Remove-Item -LiteralPath "Cert:\LocalMachine\$($e.store)\$($e.thumb)" -ErrorAction SilentlyContinue }
        'task' { Unregister-ScheduledTask -TaskName $e.name -Confirm:$false -ErrorAction SilentlyContinue }
        'script' { if (Test-Path -LiteralPath $e.file) { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $e.file @($e.args) | Out-Null } }
        'power' { Invoke-Tool powercfg.exe @('/setacvalueindex', 'SCHEME_CURRENT', $e.sub, $e.name, "$($e.old[0])") | Out-Null; Invoke-Tool powercfg.exe @('/setdcvalueindex', 'SCHEME_CURRENT', $e.sub, $e.name, "$($e.old[1])") | Out-Null }
        'hibernate' { Invoke-Tool powercfg.exe @('/hibernate', 'on') | Out-Null }
        'fwrule' { Remove-NetFirewallRule -Name $e.name -ErrorAction SilentlyContinue }
        'removedservice' { Log "  note: $($e.name) is not recreated (the SDK creates it on demand)" }
      }
      Log "  undone $($e.c) $($e.t) $($e.rk)$($e.name)$($e.path)$($e.oem)"
    } catch { Log "  UNDO FAILED $($e.c) $($e.t): $($_.Exception.Message)"; $keep += $e }
  }
  [array]::Reverse($keep); ConvertTo-Json -InputObject @($keep) -Depth 6 | Set-Content -LiteralPath $JournalFile -Encoding UTF8
}

# ================================================================ SYSTEM hop (admin process -> one-shot SYSTEM task, log mirrored)
function Invoke-AsSystem {
  if (-not $LogPath) { $script:LogPath = New-RunLog }
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Mode', $Mode, '-LogPath', "`"$LogPath`"", '-Root', "`"$Root`"")
  if ($Components.Count) { $argList += @('-Components', ($Components -join ',')) }
  if ($Include.Count) { $argList += @('-Include', ($Include -join ',')) }
  if ($Force) { $argList += '-Force' }; if ($Quiet) { $argList += '-Quiet' }
  $name = 'PSWSetup-Run'
  $a = New-ScheduledTaskAction -Execute "$PSHOME\powershell.exe" -Argument ($argList -join ' ')
  $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
  Register-ScheduledTask -TaskName $name -Action $a -Principal $p -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 1) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries) -Force | Out-Null
  $pos = if (Test-Path $LogPath) { (Get-Item $LogPath).Length } else { 0 }
  Start-ScheduledTask -TaskName $name; Start-Sleep -Seconds 2
  do { Start-Sleep -Milliseconds 700
       if (Test-Path $LogPath) { $fs = [IO.File]::Open($LogPath, 'Open', 'Read', 'ReadWrite'); $fs.Position = $pos; $sr = New-Object IO.StreamReader($fs)
         $txt = $sr.ReadToEnd(); $pos = $fs.Position; $sr.Close(); if ($txt) { Write-Host -NoNewline $txt } }
       $state = (Get-ScheduledTask -TaskName $name).State } while ($state -eq 'Running' -or $state -eq 'Queued')
  $rc = (Get-ScheduledTaskInfo -TaskName $name).LastTaskResult
  Unregister-ScheduledTask -TaskName $name -Confirm:$false
  exit $rc
}

# ================================================================ main
$Components = @($Components -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Include = @($Include -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$bad = @(($Components + $Include) | Where-Object { $_ -notin $Comp.Keys }); if ($bad.Count) { throw "unknown component(s): $($bad -join ', ') - known: $($Comp.Keys -join ', ')" }
$isSystem = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -eq 'S-1-5-18'
if ($Mode -ne 'Detect' -and -not $isSystem -and -not $NoSystemHop -and -not $WhatIfPreference) { Invoke-AsSystem }
if (-not $LogPath -and $Mode -ne 'Detect') { $LogPath = New-RunLog }
$sel = if ($Components.Count) { $Components } else { @($Comp.Keys | Where-Object { $Comp[$_].Default -or $_ -in $Include }) }
Log "PSWloader Setup $SetupVersion mode=$Mode components=$($sel -join ',') system=$isSystem resume=$Resume log=$LogPath"
if ($Resume) { Log '==== continuing after the phase-1 restart ====' }
Write-SystemReport
$rc = 0
try {
  switch ($Mode) {
    'Detect' { foreach ($c in $sel) { $r = & $Comp[$c].Detect; $ok = -not ($r.Values -contains $false)
        Log ('{0,-10} {1,-8} {2}' -f $c, $(if ($ok) { 'ok' } else { 'missing' }), (($r.GetEnumerator() | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ')) } }
    'Install' {
      if (-not $Resume -and $Root -ne "$Data\installer" -and $PSCmdlet.ShouldProcess("$Data\installer", 'copy setup for the resume after reboot')) {
        if (Test-Path "$Data\installer") { Remove-Item "$Data\installer" -Recurse -Force }
        New-Item -ItemType Directory -Force "$Data\installer" | Out-Null
        foreach ($d in 'engine', 'payload') { Copy-Item (Join-Path $Root $d) "$Data\installer\$d" -Recurse -Force }
        Get-ChildItem $Root -File | Copy-Item -Destination "$Data\installer" -Force }
      $phase = if (Test-TestModeActive) { 2 } else { 1 }
      Log "phase $phase (test signing active in this boot: $(Test-TestModeActive))"
      foreach ($c in $sel) {
        if ($phase -eq 1 -and $Comp[$c].Phase -ne 1) { continue }
        $r = & $Comp[$c].Detect; $ok = -not ($r.Values -contains $false)
        if ($ok -and -not $Force -and $c -ne 'Preflight') { Log "skip $c (already installed)"; continue }
        Log "install $c"; if ($Comp[$c].Guard) { & $Comp[$c].Guard }; & $Comp[$c].Install }
      if ($phase -eq 1) {
        $resumeArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$Data\installer\engine\PSWSetup.ps1`" -Mode Install -Resume -Root `"$Data\installer`" -LogPath `"$LogPath`""
        if ($Include.Count) { $resumeArgs += " -Include $($Include -join ',')" }; if ($Components.Count) { $resumeArgs += " -Components $($Components -join ',')" }
        $t = New-ScheduledTaskTrigger -AtStartup; $t.Delay = 'PT45S'
        Register-Task Setup 'PSWSetup-Resume' (New-ScheduledTaskAction -Execute "$PSHOME\powershell.exe" -Argument $resumeArgs) $t `
          (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 1) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries)
        Log 'PHASE 1 DONE - restart Windows; setup continues by itself after the restart (log: this file)'; $rc = 3010
        if ($Quiet -and -not $NoRestart) { Invoke-Tool shutdown.exe @('/r', '/t', '20', '/c', 'PSWloader Setup: phase 2 after restart') | Out-Null }
      } else {
        Remove-SignKeys
        # Install silent boot-time auto-update checker + version metadata for future updates
        try {
          $chkSrc = Join-Path $Payload 'updater\PSWUpdateCheck.ps1'
          if (Test-Path -LiteralPath $chkSrc) {
            $updDir = Join-Path $Data 'updater'
            New-Item -ItemType Directory -Path $updDir -Force | Out-Null
            $chkDst = Join-Path $updDir 'PSWUpdateCheck.ps1'
            Copy-Item -LiteralPath $chkSrc -Destination $chkDst -Force
            $cfgPath = Join-Path $Data 'updater.json'
            if (-not (Test-Path -LiteralPath $cfgPath)) {
              [ordered]@{ autoCheck = $true; repo = 'mr-Native/PSWloader' } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $cfgPath -Encoding UTF8
            }
            $uAct = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
              -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$chkDst`""
            $uTrg = New-ScheduledTaskTrigger -AtLogOn; $uTrg.Delay = 'PT25S'
            $uPrn = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
            $uSet = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
            Register-ScheduledTask -TaskName 'PSWloader Update Check' -Action $uAct -Trigger $uTrg -Principal $uPrn -Settings $uSet -Force -ErrorAction Stop | Out-Null
            Log '  auto-update checker installed (PSWloader Update Check)'
          }
          $pkgVer = Join-Path $Payload 'version.json'
          $verDst = Join-Path $Data 'version.json'
          if ((Test-Path -LiteralPath $pkgVer) -and -not (Test-Path -LiteralPath $verDst)) {
            Copy-Item -LiteralPath $pkgVer -Destination $verDst -Force
          }
        } catch { LogWarn "auto-update checker setup skipped: $($_.Exception.Message)" }
        foreach ($rt in 'PSWSetup-Resume', 'PS5Setup-Resume') { if (Get-ScheduledTask -TaskName $rt -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $rt -Confirm:$false; Log '  resume task removed' } }
        Log 'final check:'; foreach ($c in $sel) { $r = & $Comp[$c].Detect; $miss = @($r.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object Key)
          Log ('  {0,-10} {1}' -f $c, $(if ($miss.Count) { 'missing: ' + ($miss -join ',') } else { 'ok' }))
          if ($miss.Count -and $c -ne 'Monitor') { LogWarn "$c still incomplete after install: $($miss -join ',') (driver package not newer than the installed one? see pnputil lines above)" } }
        if ($script:RebootNeeded) { Log $(if ($script:RestartTwice) { 'DONE - restart Windows TWICE: the first start installs the updated power driver, the second loads it' } else { 'DONE - restart Windows once more to load the drivers' }); $rc = 3010
          if (($Resume -or $Quiet) -and -not $NoRestart) { Invoke-Tool shutdown.exe @('/r', '/t', $(if ($Quiet) { '30' } else { '120' }), '/c', 'PSWloader Setup: restart to load the drivers') | Out-Null } }
        else { Log 'DONE - nothing to change' } }
    }
    'Uninstall' { Undo-Journal $Components; Log 'uninstall done - restart required'; $rc = 3010 }
  }
} catch {
  LogError $_.Exception.Message; Log "  at $($_.InvocationInfo.PositionMessage -replace '\s+', ' ')"
  try { Remove-SignKeys } catch { }
  $rc = if ($script:PreflightFailed) { 2 } else { 1 }
}
Log "==== SUMMARY: mode=$Mode exit=$rc warnings=$($script:Warnings.Count) errors=$($script:Errors.Count) ===="
foreach ($w in $script:Warnings) { Log "  warning: $w" }; foreach ($e in $script:Errors) { Log "  error: $e" }
if ($LogPath) { Log "  report problems with this log file: $LogPath" }
exit $rc
