@echo off
rem psw-specialize.cmd v1.3 (PSWloader v1.0) - runs once in the specialize pass of Windows Setup (autounattend.xml), before the
rem Windows setup screens (region, network, account) of the new Windows. Nothing here may wait for input.
rem   1. sets OOBE\BypassNRO, so a Microsoft account is optional ("I don't have internet" / local account);
rem   2. psw-specialize.ps1: boot files onto the Windows disk itself (EFI partition made if missing) and its entry removed
rem      from other disks, clock fixed if it is before the certificate date, PSWloader certificate trusted, USB power saving
rem      off, wired network driver ps5gbe (at most 120 s). v1.3: certificate and driver moved into the .ps1 (HW 18.11.:
rem      pnputil stopped at a "publisher could not be verified" prompt in v1.2 and Setup waited until a USB crash).
rem Everything else comes from PSWSetup after the installation. Log: %%WINDIR%%\Setup\Scripts\psw-specialize.log
setlocal
set "LOG=%WINDIR%\Setup\Scripts\psw-specialize.log"
echo psw-specialize v1.3 %DATE% %TIME% > "%LOG%"
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\OOBE /v BypassNRO /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0psw-specialize.ps1" >> "%LOG%" 2>&1
echo done >> "%LOG%"
exit /b 0
