@echo off
rem SetupComplete.cmd v1.1 (PSWloader v1.0; v1.1: -FromSetupComplete = 60 s limit for the monitor driver search) - Windows Setup runs this file once as SYSTEM at the end of the installation,
rem before the first sign-in (official mechanism: sources\$OEM$\$$\Setup\Scripts on the setup stick). It starts
rem C:\PSWSetup\PSWSetup.exe (copied there from sources\$OEM$\$1\PSWSetup) without dialogs and without a restart: all PS5
rem drivers and tools are installed, the drivers become active at the next start of the console.
rem Log: C:\Windows\Setup\Scripts\psw-setupcomplete.log; PSWSetup's own log: C:\PSWSetup\logs\setup_psw<date-time>.log
set "LOG=%SystemRoot%\Setup\Scripts\psw-setupcomplete.log"
echo %DATE% %TIME% PSWloader SetupComplete v1.2 start >> "%LOG%"
if not exist "%SystemDrive%\PSWSetup\PSWSetup.exe" ( echo PSWSetup.exe not found - nothing to do >> "%LOG%" & exit /b 0 )
start "" /wait "%SystemDrive%\PSWSetup\PSWSetup.exe" /quiet -NoRestart -FromSetupComplete
set "RC=%ERRORLEVEL%"
echo %DATE% %TIME% PSWSetup exit %RC% >> "%LOG%"
if exist "%SystemRoot%\Setup\Scripts\psw-firstlogin-status.ps1" (
  reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce /v PSWSetupStatus /t REG_SZ /d "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"%SystemRoot%\Setup\Scripts\psw-firstlogin-status.ps1\" -ExitCode %RC%" /f >> "%LOG%" 2>&1
)
exit /b 0
