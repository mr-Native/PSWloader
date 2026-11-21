# psw-firstlogin-status.ps1 v1.0 (PSWloader v1.0) - shown once on first Desktop sign-in after SetupComplete.cmd
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
  $text = "Automatic PS5 driver installation finished with an error (exit code $ExitCode).`n`nLog: $logPath`n`nOpen the log folder now? (You can re-run C:\PSWSetup\PSWSetup.exe as Administrator anytime.)"
  $res = [System.Windows.Forms.MessageBox]::Show($text, 'PSWloader Setup', [System.Windows.Forms.MessageBoxButtons]::YesNo, [System.Windows.Forms.MessageBoxIcon]::Warning)
  if ($res -eq [System.Windows.Forms.DialogResult]::Yes) {
    Start-Process explorer.exe -ArgumentList "`"$logDir`""
  }
}
