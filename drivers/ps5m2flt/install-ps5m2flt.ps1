# install-ps5m2flt.ps1 - Register ps5m2flt.sys as a BOOT_START DiskDrive UpperFilter below PartMgr
param(
    [string]$SysPath = "$PSScriptRoot\ps5m2flt.sys",
    [string]$WindowsRoot = "C:\Windows",
    [string]$SystemHiveRoot = "HKLM:\SYSTEM\CurrentControlSet"
)

$ErrorActionPreference = "Stop"

$DstSys = Join-Path $WindowsRoot "System32\drivers\ps5m2flt.sys"
Copy-Item -Path $SysPath -Destination $DstSys -Force

$SvcKey = "$SystemHiveRoot\Services\ps5m2flt"
if (-not (Test-Path $SvcKey)) {
    New-Item -Path $SvcKey -Force | Out-Null
}
New-ItemProperty -Path $SvcKey -Name "Type"         -PropertyType DWord        -Value 1 -Force | Out-Null
New-ItemProperty -Path $SvcKey -Name "Start"        -PropertyType DWord        -Value 0 -Force | Out-Null
New-ItemProperty -Path $SvcKey -Name "ErrorControl" -PropertyType DWord        -Value 1 -Force | Out-Null
New-ItemProperty -Path $SvcKey -Name "Group"        -PropertyType String       -Value "System Bus Extender" -Force | Out-Null
New-ItemProperty -Path $SvcKey -Name "ImagePath"    -PropertyType ExpandString -Value "System32\drivers\ps5m2flt.sys" -Force | Out-Null
New-ItemProperty -Path $SvcKey -Name "DisplayName"  -PropertyType String       -Value "PS5 M.2 Nested GPT Overlay Filter" -Force | Out-Null

# An old StartOverride\0=3 takes precedence over Start=0 and prevents the
# overlay filter from loading in the boot phase. This filter must be present
# before Windows mounts the nested-GPT boot disk, so keep the boot override at 0.
$StartOverrideKey = Join-Path $SvcKey "StartOverride"
New-Item -Path $StartOverrideKey -Force | Out-Null
New-ItemProperty -Path $StartOverrideKey -Name "0" -PropertyType DWord -Value 0 -Force | Out-Null
if ((Get-ItemPropertyValue -Path $StartOverrideKey -Name "0") -ne 0) {
    throw "StartOverride\0 did not verify as boot-start"
}

$DiskClassKey = "$SystemHiveRoot\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
$CurFilters = @((Get-ItemProperty -Path $DiskClassKey -Name "UpperFilters" -ErrorAction SilentlyContinue).UpperFilters)
$Filtered = @($CurFilters | Where-Object { $_ -and $_ -ne "ps5m2flt" })
if ($Filtered.Count -eq 0) {
    $NewFilters = @("ps5m2flt", "PartMgr")
} else {
    $NewFilters = @("ps5m2flt") + $Filtered
}
Set-ItemProperty -Path $DiskClassKey -Name "UpperFilters" -Type MultiString -Value $NewFilters

Write-Output ("installed={0} UpperFilters={1}" -f $DstSys, ($NewFilters -join ","))
