# psw-specialize.ps1 v1.4 (PSWloader v1.0) - called by psw-specialize.cmd in the specialize pass of Windows Setup, before
# the Windows setup screens. Every step logs to %WINDIR%\Setup\Scripts\psw-specialize.log and nothing may wait for input:
#   1. Boot files on the Windows disk itself + BCD testsigning. Windows Setup can put boot files on the EFI partition of
#      ANOTHER disk and create none on the target. Only run bcdboot if the Windows disk has no BCD pointing at this Windows
#      (never wipe a working BCD); always enforce testsigning, disabledynamictick and bootstatuspolicy on {default},
#      {current} and {bootloadersettings} so OOBE Windows Update reboots keep test mode active and never hit 0x7B.
#   2. Clock: if the console clock is earlier than the PSWloader certificate start date, advance it so signatures verify.
#   3. The public PSWloader certificate in LocalMachine Root + TrustedPublisher (via certutil, works under SYSTEM).
#   4. USB selective suspend and USB 3 link power management off (prevents 0x144 on USB SSDs during Setup).
#   5. Protect PS5 boot storage filters before OOBE Windows Updates: exclude driver updates via Windows Update policy,
#      clear StartOverride on stornvme/ps5nvmsi/ps5m2flt, and mirror ps5nvmsi + MSI limit 1 onto the internal NVMe instance.
#   6. Wired network driver ps5gbe with pnputil, at most 120 s; a hanging install (prompt) is ended, Setup goes on.
param([string]$Payload = "$env:SystemDrive\PSWSetup\payload")
$ErrorActionPreference = 'Continue'
$log = "$env:WINDIR\Setup\Scripts\psw-specialize.log"
function L($m) { Write-Output ("ps1 " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " " + $m) }
L "v1.4 start, payload $Payload"
$espType = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'

function Set-PswBcdFlags([string]$StorePath) {
  $base = if ($StorePath) { @('/store', $StorePath) } else { @() }
  $target = if ($StorePath) { '{default}' } else { '{current}' }
  $label = if ($StorePath) { "store $StorePath" } else { 'current' }
  foreach ($pair in @(
      @($target, 'testsigning', 'on'),
      @($target, 'disabledynamictick', 'yes'),
      @($target, 'bootstatuspolicy', 'IgnoreAllFailures'),
      @('{bootloadersettings}', 'testsigning', 'on')
    )) {
    $o = (& bcdedit @base /set $pair[0] $pair[1] $pair[2] 2>&1) -join ' | '
    L "bcdedit ($label) $($pair[0]) $($pair[1])=$($pair[2]) exit $LASTEXITCODE ($o)"
  }
}

# 1. boot files on the Windows disk + BCD testsigning
try {
  $sysLetter = $env:SystemDrive.TrimEnd(':')
  $sysPart = Get-Partition -DriveLetter $sysLetter -ErrorAction Stop
  $own = Get-Partition -DiskNumber $sysPart.DiskNumber | Where-Object { $_.GptType -eq $espType } | Select-Object -First 1
  if (-not $own) {
    $dp = Join-Path $env:TEMP 'psw-esp.txt'
    Set-Content -Path $dp -Encoding ascii -Value @("select volume $sysLetter", 'shrink desired=300 minimum=300', "select disk $($sysPart.DiskNumber)",
      'create partition efi size=300', 'format quick fs=fat32 label=System')
    $o = (& diskpart /s $dp 2>&1) -join ' | '; L "no EFI partition on disk $($sysPart.DiskNumber) - diskpart: $o"
    Remove-Item $dp -ErrorAction SilentlyContinue
    $own = Get-Partition -DiskNumber $sysPart.DiskNumber | Where-Object { $_.GptType -eq $espType } | Select-Object -First 1
  }
  if ($own) {
    $addedLetter = $false
    if (-not $own.DriveLetter) {
      Add-PartitionAccessPath -InputObject $own -AssignDriveLetter -ErrorAction SilentlyContinue
      $own = Get-Partition -DiskNumber $own.DiskNumber -PartitionNumber $own.PartitionNumber
      if ($own.DriveLetter) { $addedLetter = $true }
    }
    if ($own.DriveLetter) {
      $ownBcd = "$($own.DriveLetter):\EFI\Microsoft\Boot\BCD"
      $needBcdboot = $true
      if (Test-Path -LiteralPath $ownBcd) {
        $curEnum = (& bcdedit /store $ownBcd /enum '{default}' 2>&1) -join "`n"
        if ($LASTEXITCODE -eq 0 -and $curEnum -match "(?im)\bpartition=$([regex]::Escape($sysLetter)):") {
          $needBcdboot = $false
          L "disk $($own.DiskNumber) EFI partition $($own.DriveLetter): already has a BCD for $sysLetter`: - skipping bcdboot"
        }
      }
      if ($needBcdboot) {
        $o = (& bcdboot "$env:SystemDrive\Windows" /s "$($own.DriveLetter):" /f UEFI 2>&1) -join ' | '
        L "bcdboot to disk $($own.DiskNumber) EFI partition $($own.DriveLetter): exit $LASTEXITCODE ($o)"
      }
      if (Test-Path -LiteralPath $ownBcd) { Set-PswBcdFlags $ownBcd }
      if ($addedLetter) { Remove-PartitionAccessPath -InputObject $own -AccessPath "$($own.DriveLetter):\" -ErrorAction SilentlyContinue }
    } else { L "own EFI partition has no drive letter - skipped" }
    foreach ($esp in (Get-Partition | Where-Object { $_.GptType -eq $espType -and $_.DiskNumber -ne $sysPart.DiskNumber })) {
      $vol = ($esp.AccessPaths | Where-Object { $_ -like '\\?\Volume*' } | Select-Object -First 1)
      if (-not $vol) { continue }
      $store = Join-Path $vol 'EFI\Microsoft\Boot\BCD'
      if (-not (Test-Path -LiteralPath $store)) { continue }
      $text = (& bcdedit /store $store /enum osloader /v) -join "`n"
      foreach ($blk in ($text -split "`n`n")) {
        if ($blk -match "(?m)^identifier\s+(\{[0-9a-fA-F-]+\})") {
          $id = $Matches[1]
          if ($blk -match "(?m)^device\s+partition=$sysLetter`:") {
            $r = & bcdedit /store $store /delete $id 2>&1; L "removed $id (this Windows) from the BCD on disk $($esp.DiskNumber): $r"
          }
        }
      }
      $left = (& bcdedit /store $store /enum osloader /v) -join "`n"
      if ($left -match "(?m)^identifier\s+(\{[0-9a-fA-F-]+\})") { $r = & bcdedit /store $store /default $Matches[1] 2>&1; L "default on disk $($esp.DiskNumber) -> $($Matches[1]): $r" }
    }
  } else { L "no EFI partition on disk $($sysPart.DiskNumber) and none could be made - boot files stay where Setup put them" }
  Set-PswBcdFlags ''
} catch { L ("boot files error: " + $_.Exception.Message) }

# 2. + 3. clock and certificate
$cer = Join-Path $Payload 'certs\pswloader-signing.cer'
$trusted = $false
try {
  $x = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer
  $now = Get-Date
  L "clock $($now.ToString('s')), certificate valid from $($x.NotBefore.ToString('s'))"
  if ($now -lt $x.NotBefore.AddHours(1)) { Set-Date -Date $x.NotBefore.AddHours(1) | Out-Null; L "clock was before the certificate date - set to $((Get-Date).ToString('s'))" }
  $cu = Join-Path $env:SystemRoot 'System32\certutil.exe'
  foreach ($s in 'Root', 'TrustedPublisher') {
    if (-not (Test-Path "Cert:\LocalMachine\$s\$($x.Thumbprint)")) {
      $added = $false
      if (Test-Path -LiteralPath $cu) {
        $co = (& $cu -addstore -f $s $cer 2>&1) -join ' | '
        $added = ($LASTEXITCODE -eq 0 -and (Test-Path "Cert:\LocalMachine\$s\$($x.Thumbprint)"))
        L "certutil -addstore $s exit $LASTEXITCODE ($co)"
      }
      if (-not $added) {
        $st = New-Object Security.Cryptography.X509Certificates.X509Store $s, 'LocalMachine'
        try { $st.Open('ReadWrite'); $st.Add($x) } finally { $st.Close() }
      }
    }
  }
  $trusted = (Test-Path "Cert:\LocalMachine\Root\$($x.Thumbprint)") -and (Test-Path "Cert:\LocalMachine\TrustedPublisher\$($x.Thumbprint)")
  L "certificate $($x.Thumbprint.Substring(0,8)) trusted: $trusted"
} catch { L ("certificate error: " + $_.Exception.Message) }

# 4. USB power saving off
$usb = '2a737441-1930-4402-8d77-b2bebba308a3'
& powercfg -attributes $usb d4e98f31-5ffe-4ce1-be31-1b38b384c009 -ATTRIB_HIDE 2>&1 | Out-Null
foreach ($s in '48e6b7a6-50f5-4782-a5d4-53bb8f07e226', 'd4e98f31-5ffe-4ce1-be31-1b38b384c009') {
  & powercfg /setacvalueindex SCHEME_CURRENT $usb $s 0 2>&1 | Out-Null; & powercfg /setdcvalueindex SCHEME_CURRENT $usb $s 0 2>&1 | Out-Null }
& powercfg /setactive SCHEME_CURRENT 2>&1 | Out-Null
L "usb power saving off: exit $LASTEXITCODE"

# 5. protect PS5 boot storage filters before OOBE Windows Updates
try {
  $wuPol = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'
  if (-not (Test-Path -LiteralPath $wuPol)) { New-Item -Path $wuPol -Force | Out-Null }
  New-ItemProperty -Path $wuPol -Name 'ExcludeWUDriversInQualityUpdate' -Value 1 -PropertyType DWord -Force | Out-Null
  $cs = 'HKLM:\SYSTEM\CurrentControlSet'
  foreach ($svc in 'stornvme', 'ps5nvmsi', 'ps5m2flt') {
    $ov = "$cs\Services\$svc\StartOverride"
    if (Test-Path -LiteralPath $ov) { Remove-Item -LiteralPath $ov -Recurse -Force -ErrorAction SilentlyContinue; L "removed $ov" }
  }
  foreach ($svc in 'ps5nvmsi', 'ps5m2flt') {
    if (Test-Path -LiteralPath "$cs\Services\$svc") {
      New-ItemProperty -Path "$cs\Services\$svc" -Name 'Start' -Value 0 -PropertyType DWord -Force | Out-Null
    }
  }
  if (Test-Path -LiteralPath "$cs\Services\ps5m2flt") {
    $diskClass = "$cs\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
    $uf = @((Get-ItemProperty -Path $diskClass -Name 'UpperFilters' -ErrorAction SilentlyContinue).UpperFilters | ForEach-Object { $_ } | Where-Object { $_ -and $_.Trim() -ine 'ps5m2flt' })
    New-ItemProperty -Path $diskClass -Name 'UpperFilters' -Value ([string[]](@('ps5m2flt') + $uf)) -PropertyType MultiString -Force | Out-Null
  }
  if (Test-Path -LiteralPath "$cs\Services\ps5nvmsi") {
    $nvmeDevs = @(Get-ChildItem "$cs\Enum\PCI" -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like 'VEN_15B7&DEV_5063*' } | ForEach-Object { Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue })
    foreach ($inst in $nvmeDevs) {
      $lf = @((Get-ItemProperty -Path $inst.PSPath -Name 'LowerFilters' -ErrorAction SilentlyContinue).LowerFilters | ForEach-Object { $_ } | Where-Object { $_ -and $_.Trim() -ine 'ps5nvmsi' })
      New-ItemProperty -Path $inst.PSPath -Name 'LowerFilters' -Value ([string[]](@('ps5nvmsi') + $lf)) -PropertyType MultiString -Force | Out-Null
      $msi = Join-Path $inst.PSPath 'Device Parameters\Interrupt Management\MessageSignaledInterruptProperties'
      if (-not (Test-Path -LiteralPath $msi)) { New-Item -Path $msi -Force | Out-Null }
      New-ItemProperty -Path $msi -Name 'MSISupported' -Value 1 -PropertyType DWord -Force | Out-Null
      New-ItemProperty -Path $msi -Name 'MessageNumberLimit' -Value 1 -PropertyType DWord -Force | Out-Null
      L "protected internal NVMe instance $($inst.PSChildName) (LowerFilters=ps5nvmsi, MSI limit=1)"
    }
    if ($nvmeDevs.Count -gt 0) {
      # Migrate cleanly off SCSIAdapter class LowerFilters so ps5nvmsi is never attached twice to the same NVMe stack
      $scsiClass = "$cs\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}"
      $clf = @((Get-ItemProperty -Path $scsiClass -Name 'LowerFilters' -ErrorAction SilentlyContinue).LowerFilters | ForEach-Object { $_ } | Where-Object { $_ -and $_.Trim() -ine 'ps5nvmsi' })
      if ($clf.Count -gt 0) {
        New-ItemProperty -Path $scsiClass -Name 'LowerFilters' -Value ([string[]]$clf) -PropertyType MultiString -Force | Out-Null
      } else {
        Remove-ItemProperty -Path $scsiClass -Name 'LowerFilters' -ErrorAction SilentlyContinue
      }
      L "removed ps5nvmsi from SCSIAdapter class LowerFilters (now device-instance only)"
    }
  }
  L "boot storage filter protection done"
} catch { L ("storage protection error: " + $_.Exception.Message) }

# 6. network driver, never waiting for input
$inf = Join-Path $Payload 'drivers\ps5gbe\ps5gbe.inf'
if (-not $trusted) { L "network driver skipped: certificate not trusted (it would ask for confirmation)" }
elseif (-not (Test-Path -LiteralPath $inf)) { L "network driver skipped: $inf missing" }
else {
  $out = Join-Path $env:TEMP 'psw-pnputil.txt'
  $p = Start-Process pnputil.exe -ArgumentList "/add-driver `"$inf`" /install" -NoNewWindow -PassThru -RedirectStandardOutput $out
  if ($p.WaitForExit(120000)) { L "pnputil ps5gbe exit $($p.ExitCode): $(((Get-Content $out -ErrorAction SilentlyContinue) | Where-Object { $_.Trim() } | Select-Object -Last 3) -join ' | ')" }
  else {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Get-Process drvinst -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    L "pnputil ps5gbe did not finish in 120 s (waiting for a prompt?) - ended; install the driver from PSW\drivers\installdrivers"
  }
}
L "done"
