# psw-setupcomplete.ps1 v1.2 (PSWloader v1.1; v1.2: progress window removed again - Windows does not show it over "Just a moment..." (user 25.11.); v1.1: progress window) - started by SetupComplete.cmd as SYSTEM before the first sign-in.
# Runs C:\PSWSetup\PSWSetup.exe /quiet with an overall limit (user 25.11.2026: a manual install stayed on "Just a moment..."
# for more than 25 minutes; SetupComplete waited for PSWSetup without any limit). After the limit the whole process tree is
# ended and Windows setup goes on to the sign-in. When PSWSetup did not finish cleanly, the support zip is made at once
# (PSWSelfCheck, also from the PSWSetup payload when the support tool was not installed yet), so the first sign-in can
# point to it (psw-firstlogin-status.ps1). Exit code: PSWSetup's, or 1460 (ERROR_TIMEOUT) after the limit.
$ErrorActionPreference = 'Continue'
$Log = "$env:SystemRoot\Setup\Scripts\psw-setupcomplete.log"
function L([string]$m) { "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m" | Add-Content -Path $Log }
$LimitMin = 20
$Exe = "$env:SystemDrive\PSWSetup\PSWSetup.exe"

if (-not (Test-Path $Exe)) { L 'PSWSetup.exe not found - nothing to do'; exit 0 }

# v1.2: started directly (CreateProcess), never through the Windows shell - a shell start of VC_redist never returned
# before the first sign-in (PSWSetup 0.5.12)
function Start-Direct([string]$File, [string]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo $File, $Arguments
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    [System.Diagnostics.Process]::Start($psi)
}
$p = Start-Direct $Exe '/quiet -NoRestart -FromSetupComplete'
if ($p.WaitForExit($LimitMin * 60000)) {
    $rc = $p.ExitCode
} else {
    L "PSWSetup still running after $LimitMin minutes - stopped with its child processes, Windows setup goes on"
    & taskkill.exe /T /F /PID $p.Id 2>&1 | ForEach-Object { L "  $_" }
    $rc = 1460
}
L "PSWSetup exit $rc"

if ($rc -ne 0 -and $rc -ne 3010) {
    $sc = 'C:\PSWloader\selfcheck\PSWSelfCheck.ps1'
    if (-not (Test-Path $sc)) { $sc = "$env:SystemDrive\PSWSetup\payload\tools\selfcheck\PSWSelfCheck.ps1" }
    if (Test-Path $sc) {
        L "creating the support zip (up to 5 minutes): $sc"
        $q = Start-Direct "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" "-NoProfile -ExecutionPolicy Bypass -File `"$sc`" -NoExplorer"
        if ($q.WaitForExit(5 * 60000)) { $z = Get-ChildItem 'C:\PSWloader\selfcheck\results' -Filter 'PSW-selfcheck-*.zip' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
                                         L "support zip done (exit $($q.ExitCode)): $(if ($z) { $z.FullName } else { 'no zip found' }) - please attach it to a GitHub issue" }
        else { & taskkill.exe /T /F /PID $q.Id 2>&1 | Out-Null; L 'support zip stopped after 5 minutes' }
    } else { L 'support zip: PSWSelfCheck.ps1 not found' }
}
exit $rc
