# run-ps5msiprobe.ps1 v1.3 (13.11.2026) - load the READ-ONLY gate driver ps5msiprobe.sys once, collect its
# registry output, unload and remove it again. Run in an elevated session (admin SSH is enough). Test signing is on.
# Expects C:\ps5-work\ps5msiprobe.sys (scp it there). Output: C:\ps5-work\ps5msiprobe.txt (+ stdout).
# v1.3 (v1.9 re-review): pre-cleanup must leave neither the output key nor the service behind (else no
# load at all), the driver must report exactly version 'ps5msiprobe v1.2', and the final stop/delete must succeed -
# every one of these failing is a VERDICT FAIL.
$ErrorActionPreference = 'Continue'
$want = 'ps5msiprobe v1.2'
$sys = 'C:\ps5-work\ps5msiprobe.sys'
if (-not (Test-Path $sys)) { 'ABORT: ' + $sys + ' missing'; exit 2 }
reg delete 'HKLM\SOFTWARE\ps5msiprobe' /f 2>&1 | Out-Null
sc.exe delete ps5msiprobe 2>&1 | Out-Null
$null = sc.exe query ps5msiprobe 2>&1; $qrc = $LASTEXITCODE                    # 1060 = service does not exist
if ((Test-Path 'HKLM:\SOFTWARE\ps5msiprobe') -or $qrc -ne 1060) {
    $msg = "ABORT: pre-cleanup incomplete (output key present $(Test-Path 'HKLM:\SOFTWARE\ps5msiprobe'), sc query rc $qrc - a reboot clears a service marked for deletion)"
    @("=== ps5msiprobe $(Get-Date -Format o)", $msg, 'VERDICT FAIL (pre-cleanup)') | Set-Content C:\ps5-work\ps5msiprobe.txt -Encoding utf8
    $msg; 'VERDICT FAIL (pre-cleanup)'; exit 1
}
# v1.1: sc exit codes are checked and a fresh 'done' value (the key is deleted before) is required.
$c = (sc.exe create ps5msiprobe type= kernel start= demand binPath= $sys 2>&1 | Out-String).Trim(); $crc = $LASTEXITCODE
$s = (sc.exe start ps5msiprobe 2>&1 | Out-String).Trim(); $src = $LASTEXITCODE
$out = @("=== ps5msiprobe $(Get-Date -Format o)", "create rc ${crc}: $c", "start rc ${src}: $s")
$k = $null
for ($i = 0; $i -lt 10 -and -not $k; $i++) { Start-Sleep 1; $k = Get-Item 'HKLM:\SOFTWARE\ps5msiprobe' -ErrorAction SilentlyContinue; if ($k -and $k.GetValue('done') -ne 'ok') { $k = $null } }
if ($crc -ne 0 -or $src -ne 0) { $out += 'VERDICT FAIL (service create/start failed)' }
if ($k) {
    foreach ($n in ($k.GetValueNames() | Sort-Object)) { $out += ('{0} = {1}' -f $n, $k.GetValue($n)) }
    if ($k.GetValue('version') -ne $want) { $out += "VERDICT FAIL: driver version '$($k.GetValue('version'))', expected '$want'" }
}
else { $out += 'VERDICT FAIL: no fresh done=ok (driver did not run or did not finish)' }
$st = (sc.exe stop ps5msiprobe 2>&1 | Out-String).Trim(); $strc = $LASTEXITCODE    # 1062 = was not running
$de = (sc.exe delete ps5msiprobe 2>&1 | Out-String).Trim(); $derc = $LASTEXITCODE
$out += "stop rc ${strc}: $st"
$out += "delete rc ${derc}: $de"
if (($strc -ne 0 -and $strc -ne 1062) -or $derc -ne 0) { $out += 'VERDICT FAIL (cleanup: driver/service may still be present - reboot clears it)' }
$out | Set-Content C:\ps5-work\ps5msiprobe.txt -Encoding utf8
$out
# v1.2: explicit exit code - 0 only with a fresh done=ok and successful sc create/start
if (@($out | Where-Object { $_ -like 'VERDICT FAIL*' }).Count) { exit 1 }
exit 0
