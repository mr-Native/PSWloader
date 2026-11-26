# install-ps5nvmsi.ps1 v0.5 (14.-16.11.2026) - attach / detach the way-(b1) filter ps5nvmsi to exactly the
# internal-M.2 instance PCI\VEN_15B7&DEV_5063 (device-level LowerFilters, NOT a class filter), test without reboot.
# Run as SYSTEM (Enum keys are SYSTEM-only; run_ps_as_system v2). The NVMe function is its own device (not the GPU/xHCI
# 1002:13FC), so `pnputil /restart-device` of just this instance is allowed; the USB boot disk is untouched.
#   -Mode check    read-only: instance, problem code, LowerFilters, service, parameters, driver file hash
#   -Mode apply    copy C:\ps5-work\ps5nvmsi.sys -> System32\drivers, service ps5nvmsi (kernel, demand start),
#                  Parameters MsiAddrTemplate/MsiDataTemplate/MsiDestKind (REQUIRED, from the ps5msiprobe result,
#                  checked like the driver checks them), LowerFilters += ps5nvmsi on the instance, restart it
#   -Mode revert   LowerFilters -= ps5nvmsi (back to the saved original), restart the instance; service/file stay (inert)
#   -Mode purge    after revert: delete the ps5nvmsi service key and System32\drivers\ps5nvmsi.sys - only if this script
#                  created them (marker) and no LowerFilters entry references ps5nvmsi any more (v0.3)
# v0.5: -UseMsi writes Parameters\MsiUseMsi=1 (ps5nvmsi v0.8: real MSI capability instead of MSI-X); without it 0.
# v0.4: -DestKind 2 (ps5nvmsi v0.7: HAL message 1:1) - DM free, data bit 11 allowed only with DM = 1 (logical).
# v0.3 (v0.2 re-review): apply refuses a ps5nvmsi service key or driver file that this script did not create
# (Parameters\InstalledBy marker) instead of overwriting it; purge makes apply fully reversible.
# v0.2 (way-b1 review): LowerFilters is MERGED, never overwritten - the original value (or its absence) is
# saved once under Parameters (OrigLowerFiltersPresent / OrigLowerFilters) before the first change; apply appends
# ps5nvmsi to the original list, revert removes only ps5nvmsi and must arrive exactly at the saved original (readback).
# Output: C:\ps5-work\install-ps5nvmsi.txt. Exit 0 done, 2 precondition, 3 registry/restart failed.
param([ValidateSet('check','apply','revert','purge')][string]$Mode = 'check',
      [string]$AddrTemplate = '', [string]$DataTemplate = '', [ValidateSet(-1,0,1,2)][int]$DestKind = -1, [switch]$UseMsi)
$ErrorActionPreference = 'Stop'
$out = 'C:\ps5-work\install-ps5nvmsi.txt'
New-Item -ItemType Directory -Force C:\ps5-work | Out-Null
function W($m) { Add-Content -Path $out -Value $m; $m }
"=== install-ps5nvmsi v0.5 $Mode $(Get-Date -Format o)" | Set-Content $out
$dev = @(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_15B7&DEV_5063*')
if ($dev.Count -ne 1) { W "ABORT: expected one present 15B7:5063, found $($dev.Count)"; exit 2 }
$id = $dev[0].InstanceId
$enum = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
$svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5nvmsi'
$par = "$svc\Parameters"
function LF { $v = (Get-ItemProperty $enum -Name LowerFilters -ErrorAction SilentlyContinue).LowerFilters; if ($null -eq $v) { $null } else { [string[]]@($v) } }
function Show($l) { if ($null -eq $l) { '(absent)' } elseif (-not $l.Count) { '(empty)' } else { $l -join ',' } }
function State($tag) {
    $d = Get-PnpDevice -InstanceId $id
    $sv = Get-ItemProperty $svc -ErrorAction SilentlyContinue
    $pp = Get-ItemProperty $par -ErrorAction SilentlyContinue
    $fh = if (Test-Path C:\Windows\System32\drivers\ps5nvmsi.sys) { (Get-FileHash C:\Windows\System32\drivers\ps5nvmsi.sys).Hash.Substring(0, 8) } else { '-' }
    W ("[{0}] {1} status={2} problem={3} LowerFilters={4} service={5} file={6} addr={7} data={8} kind={9} orig={10}" -f $tag, $id, $d.Status, [int]$d.Problem,
       (Show (LF)), $(if ($sv) { "start $($sv.Start)" } else { '-' }), $fh,
       $(if ($pp -and $null -ne $pp.MsiAddrTemplate) { '0x{0:X8}' -f $pp.MsiAddrTemplate } else { '-' }),
       $(if ($pp -and $null -ne $pp.MsiDataTemplate) { '0x{0:X8}' -f $pp.MsiDataTemplate } else { '-' }),
       $(if ($pp -and $null -ne $pp.MsiDestKind) { $pp.MsiDestKind } else { '-' }),
       $(if ($pp -and $null -ne $pp.OrigLowerFiltersPresent) { if ($pp.OrigLowerFiltersPresent) { Show ([string[]]@($pp.OrigLowerFilters)) } else { '(absent)' } } else { 'not saved' }))
}
function Restart {
    $r = (pnputil /restart-device "$id" 2>&1 | Out-String).Trim(); $rc = $LASTEXITCODE
    W ("pnputil /restart-device rc {0}: {1}" -f $rc, ($r -replace '\s+', ' ')) | Out-Null
    if ($rc -eq 3010) { W 'REBOOT_REQUIRED: the change takes effect at the next boot' | Out-Null; return }
    if ($rc -ne 0) { W 'ABORT: restart failed'; exit 3 }
    Start-Sleep 15
}
function Same($a, $b) { if ($null -eq $a -or $null -eq $b) { return ($null -eq $a) -and ($null -eq $b) }; return ($a -join "`n") -ceq ($b -join "`n") }
State 'before'
if ($Mode -eq 'check') { W 'check done (nothing changed)'; exit 0 }
try {
    if ($Mode -eq 'apply') {
        if (-not (Test-Path C:\ps5-work\ps5nvmsi.sys)) { W 'ABORT: C:\ps5-work\ps5nvmsi.sys missing'; exit 2 }
        # the MSI format from ps5msiprobe - the same rules the driver applies (FormatConsistent)
        try { $a = [Convert]::ToUInt32($AddrTemplate, 16); $dt = [Convert]::ToUInt32($DataTemplate, 16) } catch { W 'ABORT: -AddrTemplate / -DataTemplate (hex) required'; exit 2 }
        if ($DestKind -lt 0) { W 'ABORT: -DestKind 0|1|2 required'; exit 2 }
        # 64-bit literals (L): a bare 0xFFF00000 is a NEGATIVE Int32 in PowerShell and breaks -band/-ne on UInt32
        $dm = ($a -shr 2) -band 1
        $dmask = if ($DestKind -eq 2) { 0xFFFF30FFL } else { 0xFFFF38FFL }
        if (($a -band 0xFFF00000L) -ne 0xFEE00000L -or ($a -band 0x000FFFF3L) -ne 0 -or ($DestKind -lt 2 -and $dm -ne $DestKind) -or
            ($dt -band $dmask) -ne 0 -or (($dt -shr 8) -band 7) -gt 1 -or (($dt -band 0x800L) -and $dm -ne 1)) { W ('ABORT: inconsistent format addr 0x{0:X8} data 0x{1:X8} kind {2}' -f $a, $dt, $DestKind); exit 2 }
        # v0.3: never overwrite a service/driver this script did not create
        $ours = (Test-Path $par) -and ((Get-ItemProperty $par -ErrorAction SilentlyContinue).InstalledBy -eq 'install-ps5nvmsi')
        if ((Test-Path $svc) -and -not $ours) { W 'ABORT: a ps5nvmsi service key exists that this script did not create - inspect it first'; exit 2 }
        if ((Test-Path C:\Windows\System32\drivers\ps5nvmsi.sys) -and -not $ours) { W 'ABORT: System32\drivers\ps5nvmsi.sys exists without our service marker - inspect it first'; exit 2 }
        Copy-Item C:\ps5-work\ps5nvmsi.sys C:\Windows\System32\drivers\ps5nvmsi.sys -Force
        if (-not (Test-Path $svc)) { New-Item $svc -Force | Out-Null }
        New-ItemProperty $svc -Name Type -PropertyType DWord -Value 1 -Force | Out-Null
        New-ItemProperty $svc -Name Start -PropertyType DWord -Value 3 -Force | Out-Null
        New-ItemProperty $svc -Name ErrorControl -PropertyType DWord -Value 1 -Force | Out-Null
        New-ItemProperty $svc -Name ImagePath -PropertyType ExpandString -Value 'System32\drivers\ps5nvmsi.sys' -Force | Out-Null
        if (-not (Test-Path $par)) { New-Item $par -Force | Out-Null }
        New-ItemProperty $par -Name InstalledBy -PropertyType String -Value 'install-ps5nvmsi' -Force | Out-Null
        # REG_DWORD wants Int32: reinterpret the bits (a checked [int32] cast throws above 0x7FFFFFFF)
        New-ItemProperty $par -Name MsiAddrTemplate -PropertyType DWord -Value ([BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]$a), 0)) -Force | Out-Null
        New-ItemProperty $par -Name MsiDataTemplate -PropertyType DWord -Value ([BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]$dt), 0)) -Force | Out-Null
        New-ItemProperty $par -Name MsiDestKind -PropertyType DWord -Value $DestKind -Force | Out-Null
        New-ItemProperty $par -Name MsiUseMsi -PropertyType DWord -Value ([int][bool]$UseMsi) -Force | Out-Null
        $cur = LF
        $pp = Get-ItemProperty $par
        if ($null -eq $pp.OrigLowerFiltersPresent) {                        # save the original exactly once
            if ($null -ne $cur -and $cur -contains 'ps5nvmsi') { W 'ABORT: ps5nvmsi already in LowerFilters but no saved original'; exit 3 }
            New-ItemProperty $par -Name OrigLowerFiltersPresent -PropertyType DWord -Value ([int]($null -ne $cur)) -Force | Out-Null
            if ($null -ne $cur) { New-ItemProperty $par -Name OrigLowerFilters -PropertyType MultiString -Value $cur -Force | Out-Null }
        }
        $base = if ($null -eq $cur) { [string[]]@() } else { [string[]]@($cur | Where-Object { $_ -ne 'ps5nvmsi' }) }
        $want = [string[]]@($base + 'ps5nvmsi')
        Set-ItemProperty $enum -Name LowerFilters -Type MultiString -Value $want
        if (-not (Same (LF) $want)) { W "ABORT: LowerFilters readback '$(Show (LF))', wanted '$(Show $want)'"; exit 3 }
        W "attached: LowerFilters $(Show $cur) -> $(Show $want) (readback ok), format addr 0x$('{0:X8}' -f $a) data 0x$('{0:X8}' -f $dt) kind $DestKind"
    } elseif ($Mode -eq 'purge') {
        $pp = Get-ItemProperty $par -ErrorAction SilentlyContinue
        if (-not $pp -or $pp.InstalledBy -ne 'install-ps5nvmsi') { W 'ABORT: service not created by this script (no marker) - nothing purged'; exit 2 }
        if ($null -ne $pp.OrigLowerFiltersPresent) { W 'ABORT: still attached (saved original present) - run -Mode revert first'; exit 2 }
        $refs = @(Get-ChildItem HKLM:\SYSTEM\CurrentControlSet\Enum\PCI -Recurse -ErrorAction SilentlyContinue |
                  Where-Object { @((Get-ItemProperty $_.PSPath -Name LowerFilters, UpperFilters -ErrorAction SilentlyContinue).PSObject.Properties |
                                   Where-Object { $_.Name -in 'LowerFilters', 'UpperFilters' -and @($_.Value) -contains 'ps5nvmsi' }).Count })
        if ($refs.Count) { W "ABORT: ps5nvmsi still referenced by $($refs.Count) PCI instance(s) - nothing purged"; exit 3 }
        Remove-Item $svc -Recurse -Force
        Remove-Item C:\Windows\System32\drivers\ps5nvmsi.sys -Force -ErrorAction SilentlyContinue
        if ((Test-Path $svc) -or (Test-Path C:\Windows\System32\drivers\ps5nvmsi.sys)) { W 'ABORT: purge incomplete (file in use? a reboot unloads it)'; exit 3 }
        W 'purged: service key and driver file removed (readback ok)'
        State 'after'; exit 0
    } else {
        $pp = Get-ItemProperty $par -ErrorAction SilentlyContinue
        if (-not $pp -or $null -eq $pp.OrigLowerFiltersPresent) { W 'ABORT: no saved original LowerFilters - revert by hand'; exit 3 }
        $orig = if ($pp.OrigLowerFiltersPresent) { [string[]]@($pp.OrigLowerFilters) } else { $null }
        $cur = LF
        $rest = if ($null -eq $cur) { $null } else { [string[]]@($cur | Where-Object { $_ -ne 'ps5nvmsi' }) }
        if (-not (Same $rest $orig) -and -not ($null -eq $orig -and $null -ne $rest -and -not $rest.Count)) {
            W "ABORT: LowerFilters without ps5nvmsi '$(Show $rest)' differs from the saved original '$(Show $orig)' - someone else changed it, revert by hand"; exit 3
        }
        if ($null -eq $orig) { Remove-ItemProperty $enum -Name LowerFilters -ErrorAction SilentlyContinue }
        else { Set-ItemProperty $enum -Name LowerFilters -Type MultiString -Value $orig }
        if (-not (Same (LF) $orig)) { W "ABORT: LowerFilters readback '$(Show (LF))', wanted the original '$(Show $orig)'"; exit 3 }
        Remove-ItemProperty $par -Name OrigLowerFiltersPresent, OrigLowerFilters -ErrorAction SilentlyContinue
        W "detached: LowerFilters back to the original $(Show $orig) (readback ok)"
    }
} catch { W ("ABORT: " + $_.Exception.Message); exit 3 }
Restart
State 'after'
exit 0
