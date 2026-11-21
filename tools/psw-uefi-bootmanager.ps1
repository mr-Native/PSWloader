# psw-uefi-bootmanager.ps1 v1.0 (PSWloader v1.0) - "PSWloader UEFI Boot Manager" in the Start menu.
# Asks once, then restarts Windows into the firmware UI (shutdown /r /fw: Windows sets OsIndications BOOT_TO_FW_UI). With
# PSWloader firmware r5.25.4+ built with warm re-entry the console does not go through GameOS: the firmware starts again and
# opens its Boot Manager directly. Started by the Start-menu shortcut (runs as administrator; /fw needs it).
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.Application]::EnableVisualStyles()
$title = 'PSWloader UEFI Boot Manager'
$text = "Windows will restart and open the PSWloader UEFI Boot Manager.`n`n" +
        "Open programs are closed - save your work first.`n`n" +
        'Restart now?'
$answer = [System.Windows.Forms.MessageBox]::Show($text, $title, [System.Windows.Forms.MessageBoxButtons]::YesNo,
                                                 [System.Windows.Forms.MessageBoxIcon]::Question,
                                                 [System.Windows.Forms.MessageBoxDefaultButton]::Button2)
if ($answer -ne [System.Windows.Forms.DialogResult]::Yes) { exit 0 }
$out = & shutdown.exe /r /fw /t 0 2>&1
if ($LASTEXITCODE -ne 0) {
  [System.Windows.Forms.MessageBox]::Show("The restart into the UEFI Boot Manager did not start:`n`n$($out -join ' ')`n`n" +
    'It needs administrator rights and PSWloader firmware r5.25.4 or newer.', $title,
    [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Warning) | Out-Null
  exit 1
}
