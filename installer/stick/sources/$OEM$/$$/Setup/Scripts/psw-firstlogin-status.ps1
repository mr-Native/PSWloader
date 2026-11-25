# psw-firstlogin-status.ps1 v1.1 (PSWloader v1.1; v1.1: on an error or the 20-minute limit (exit 1460) it names the support zip
# psw-setupcomplete.ps1 made and asks to attach it to a GitHub issue) - shown once on first Desktop sign-in after SetupComplete.cmd
param([int]$ExitCode = 0)
Add-Type -AssemblyName System.Windows.Forms
$logDir = "$env:SystemDrive\PSWSetup\logs"
$latest = Get-ChildItem -LiteralPath $logDir -Filter 'setup_psw*.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$logPath = if ($latest) { $latest.FullName } else { $logDir }

if ($ExitCode -eq 0 -or $ExitCode -eq 3010) {
  $text = "All PS5 drivers and tools were installed successfully.`n`nLog: $logPath`n`nRestart now to activate all drivers (GPU, Power, Ethernet)?"
  $res = [System.Windows.Forms.MessageBox]::Show($text, 'PSWloader Setup', [System.Windows.Forms.MessageBoxButtons]::YesNo, [System.Windows.Forms.MessageBoxIcon]::Information)
  if ($res -eq [System.Windows.Forms.DialogResult]::Yes) {
    Start-Process "$env:SystemRoot\System32\shutdown.exe" -ArgumentList '/r /t 3' -WindowStyle Hidden
  }
} else {
  $zip = Get-ChildItem -LiteralPath 'C:\PSWloader\selfcheck\results' -Filter 'PSW-selfcheck-*.zip' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  $what = if ($ExitCode -eq 1460) { "did not finish within 20 minutes and was stopped" } else { "finished with an error (exit code $ExitCode)" }
  $report = if ($zip) { "Support zip: $($zip.FullName)" } else { "Log: $logPath`nCreate a support zip with C:\PSWloader\selfcheck\PSWSelfCheck.cmd" }
  $text = "The automatic PS5 driver installation $what.`n`n$report`n`nPlease attach the zip to an issue: https://github.com/mr-Native/PSWloader/issues`nYou can run C:\PSWSetup\PSWSetup.exe as Administrator again at any time.`n`nOpen the folder and the issues page now?"
  $res = [System.Windows.Forms.MessageBox]::Show($text, 'PSWloader Setup', [System.Windows.Forms.MessageBoxButtons]::YesNo, [System.Windows.Forms.MessageBoxIcon]::Warning)
  if ($res -eq [System.Windows.Forms.DialogResult]::Yes) {
    $dir = if ($zip) { $zip.DirectoryName } else { $logDir }
    Start-Process explorer.exe -ArgumentList "`"$dir`""
    Start-Process 'https://github.com/mr-Native/PSWloader/issues'
  }
}
