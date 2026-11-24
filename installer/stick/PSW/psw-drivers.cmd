@echo off
rem psw-drivers.cmd v2.4 (PSWloader v1.0; v2.4: logs of the last 10 Setup starts in PSW\logs; v2.3: psw-wim.cmd in the background - removes the corner text in the stick's boot.wim for every
rem later start (Windows PE reads it before any script); v2.2: pswsvc v1.8 hidetestmode first - hides the "Test Mode" text in Windows Setup's screen corner (RAM copy of
rem shell32.dll.mui only); v2.1: pswsvc v1.7 keepawake keeps the setup stick awake in the background (Setup stalled at its first
rem read of install.swm after the stick had idled in the dialogs - USB selective suspend, powercfg has no effect here); v2.0: TRIM off for this Windows Setup session - with pswdisk reporting a USB SSD as SATA, Windows
rem sends TRIM the ATA way and a USB-NVMe bridge (RTL9210B) hangs: HW 18.11. formatting with pswdisk hung, with TRIM off it took 1 s; v1.9: pswsvc v1.6 usbcycle also turns off idle power-down in each USB disk's Device Parameters - HW 18.11.:
rem the cause is USB selective suspend, a USB SSD does not come back from idle, powercfg does not reach it here; v1.8: after that the USB port of every USB disk except the setup stick is cycled (pswsvc v1.5 usbcycle),
rem so its link is negotiated again with U1/U2 off - HW 18.11.: the setting alone did not reach a disk that was already running; v1.7: USB selective suspend and USB 3 link power management (U1/U2) off first - with them on a USB SSD
rem on the console's AMD xHCI stalls under load and Windows Setup stops at "copying Windows files"; v1.6: stop the Virtual Disk Service after pswdisk starts; v1.5: pswsvc usbreset first; v1.4: PSWloader v1.0; v1.4: setup stick disk number read from a diskpart output FILE, two independent ways
rem (A: for /f over the file, cmd only; B: findstr, if present) + number check - v1.3 found nothing in the German Windows PE; v1.3: Windows Setup's Windows PE has no sc.exe and no certutil - pswdisk is
rem started with PSW\pswsvc.exe (service control manager API), certutil only if present; v1.2: the stick's disk number -> pswdisk ExcludeDisk, the stick stays a USB disk;
rem v1.1: pswdisk + X:\PSWDRV) - called in the background by the stick's autounattend.xml when Windows Setup
rem starts. Trusts the public PSWloader certificate for this Windows Setup session (RAM only) and loads the PS5 storage
rem drivers (ps5nvmsi: interrupts through the console's MSI proxy, ps5m2flt: internal M.2 layout), so Windows Setup lists
rem the internal M.2. v1.1: copies the drivers to X:\PSWDRV (offlineServicing DriverPaths) and starts pswdisk, which
rem reports USB disks as internal SATA disks, so Windows Setup accepts a USB SSD as target. Changes nothing on any disk. Needs test signing in the stick's boot entry (set by PSWloader
rem firmware r5.22+ before Windows Boot Manager starts). Log: X:\psw-drivers.log, copied to PSW\psw-drivers.log.
setlocal enabledelayedexpansion
set "SRC=%~d0"
set "LOG=X:\psw-drivers.log"
echo psw-drivers v2.4 src=%SRC% > %LOG%
rem v2.4: the logs of the last 10 Setup starts are kept in PSW\logs\psw-drivers-N.log (a failed start used to be
rem overwritten by the next one, for example after a restart)
if not exist "%SRC%\PSW\logs" md "%SRC%\PSW\logs" > nul 2>&1
set "RUNN=0"
if exist "%SRC%\PSW\logs\count.txt" set /p RUNN=<"%SRC%\PSW\logs\count.txt"
set /a RUNN=(RUNN+1) %% 10
> "%SRC%\PSW\logs\count.txt" echo !RUNN!
set "HIST=%SRC%\PSW\logs\psw-drivers-!RUNN!.log"
echo setup start !RUNN! of 0-9, %DATE% %TIME% >> %LOG%
if exist "%SRC%\PSW\pswsvc.exe" ( "%SRC%\PSW\pswsvc.exe" hidetestmode >> %LOG% 2>&1 )
rem v2.0: no TRIM in Windows Setup (RAM registry of Windows PE only; the installed Windows keeps TRIM)
fsutil behavior set DisableDeleteNotify 1 >> %LOG% 2>&1
reg add HKLM\SYSTEM\CurrentControlSet\Control\FileSystem /v DisableDeleteNotification /t REG_DWORD /d 1 /f >> %LOG% 2>&1
echo trim off exit !ERRORLEVEL! >> %LOG%
rem v1.7: USB power saving off for this Windows Setup session (settings 48e6b7a6 = selective suspend, d4e98f31 = USB 3 LPM)
(
  powercfg -attributes 2a737441-1930-4402-8d77-b2bebba308a3 d4e98f31-5ffe-4ce1-be31-1b38b384c009 -ATTRIB_HIDE >> %LOG% 2>&1
  for %%S in (48e6b7a6-50f5-4782-a5d4-53bb8f07e226 d4e98f31-5ffe-4ce1-be31-1b38b384c009) do (
    powercfg /setacvalueindex SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 %%S 0 >> %LOG% 2>&1
    powercfg /setdcvalueindex SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 %%S 0 >> %LOG% 2>&1
  )
  powercfg /setactive SCHEME_CURRENT >> %LOG% 2>&1
  echo usb power saving off exit !ERRORLEVEL! >> %LOG%
)
if exist "%SRC%\PSW\pswsvc.exe" if exist "%SRC%\PSW\drivers\pswloader-signing.cer" (
  "%SRC%\PSW\pswsvc.exe" addcert "%SRC%\PSW\drivers\pswloader-signing.cer" >> %LOG% 2>&1
  echo pswsvc addcert exit !ERRORLEVEL! >> %LOG%
)
where certutil > nul 2>&1 && for %%C in ("%SRC%\PSW\drivers\pswloader-signing.cer") do if exist %%C (
  certutil -f -addstore Root %%C >> %LOG% 2>&1
  certutil -f -addstore TrustedPublisher %%C >> %LOG% 2>&1
)
for %%I in (ps5nvmsi) do if exist "%SRC%\PSW\drivers\%%I\%%I.inf" (
  drvload "%SRC%\PSW\drivers\%%I\%%I.inf" >> %LOG% 2>&1
  echo drvload %%I exit !ERRORLEVEL! >> %LOG%
  ping -n 3 127.0.0.1 > nul
)
if exist "%SRC%\PSW\pswsvc.exe" if exist "%SRC%\PSW\drivers\ps5nvmsi\ps5nvmsi.sys" if exist "%SRC%\PSW\drivers\ps5m2flt\ps5m2flt.sys" (
  "%SRC%\PSW\pswsvc.exe" nvmeinit "%SRC%\PSW\drivers\ps5nvmsi\ps5nvmsi.sys" "%SRC%\PSW\drivers\ps5m2flt\ps5m2flt.sys" >> %LOG% 2>&1
  set "NVRC=!ERRORLEVEL!"
  echo pswsvc nvmeinit exit !NVRC! >> %LOG%
  if "!NVRC!"=="0" (
    start "" /b "%SRC%\PSW\pswsvc.exe" nvmewatch %SRC:~0,1% X:\Windows\System32\drivers\ps5nvmsi.sys X:\Windows\System32\drivers\ps5m2flt.sys > nul 2>&1
    echo nvmewatch started >> %LOG%
  )
  if "!NVRC!"=="577" (
    echo TESTSIGNING is OFF in WinPE - enabling testsigning in stick BCD for next boot >> %LOG%
    for %%B in ("%SRC%\efi\microsoft\boot\bcd" "%SRC%\boot\bcd") do if exist %%B (
      bcdedit /store %%B /set {default} testsigning on >> %LOG% 2>&1
      echo bcdedit %%B exit !ERRORLEVEL! >> %LOG%
    )
    copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
  )
)
copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
rem v1.5: restart USB disks that report 0 bytes (stuck USB SSD bridges), before pswdisk and before Setup lists the disks
if exist "%SRC%\PSW\pswsvc.exe" ( "%SRC%\PSW\pswsvc.exe" usbreset >> %LOG% 2>&1 )
xcopy /e /i /y "%SRC%\PSW\drivers" X:\PSWDRV\ >> %LOG% 2>&1
if exist X:\PSWDRV\pswdisk\pswdisk.sys (
  rem the disk that holds this stick: the "*" row of "detail volume", same layout in every language: * Disk N
  (echo select volume %SRC:~0,1%& echo detail volume) > X:\psw-vol.txt
  diskpart /s X:\psw-vol.txt > X:\psw-vol.out 2>&1
  set "STICKDISK="
  rem way A: the selected disk row starts with "*" - first token "*", third token the number (word 2 = Disk/Datentraeger/... - matches the localized diskpart output, for example German)
  for /f "usebackq tokens=1-3" %%a in ("X:\psw-vol.out") do if "%%a"=="*" if not defined STICKDISK set "STICKDISK=%%c"
  rem way B: findstr (not every Windows PE has it)
  if not defined STICKDISK for /f "tokens=3" %%c in ('findstr /b /c:"* " X:\psw-vol.out 2^>nul') do if not defined STICKDISK set "STICKDISK=%%c"
  rem only a plain number counts
  if defined STICKDISK ( set /a "STICKNUM=STICKDISK" 2> nul & if not "!STICKNUM!"=="!STICKDISK!" ( echo setup stick: "!STICKDISK!" is not a disk number >> %LOG% & set "STICKDISK=" ) )
  echo ---- detail volume of the stick: >> %LOG%
  type X:\psw-vol.out >> %LOG%
  if defined STICKDISK (
    reg add HKLM\SYSTEM\CurrentControlSet\Services\pswdisk /v ExcludeDisk /t REG_DWORD /d !STICKDISK! /f >> %LOG% 2>&1
    echo setup stick = disk !STICKDISK! - left as USB disk >> %LOG%
    rem v1.8/v1.9: idle power-down off and a port cycle for every other USB disk, the stick is never cycled
    "%SRC%\PSW\pswsvc.exe" usbcycle !STICKDISK! >> %LOG% 2>&1
    rem v2.1: one unbuffered sector read per second from the stick for as long as Windows Setup runs, no window
    start "" /b "%SRC%\PSW\pswsvc.exe" keepawake !STICKDISK! > nul 2>&1
    echo keepawake started for disk !STICKDISK! >> %LOG%
  ) else echo setup stick disk not found - all USB disks are reported as internal >> %LOG%
  "%SRC%\PSW\pswsvc.exe" start pswdisk X:\PSWDRV\pswdisk\pswdisk.sys >> %LOG% 2>&1
  echo pswsvc start pswdisk exit !ERRORLEVEL! >> %LOG%
  rem v1.6: the stick detection above already started the Virtual Disk Service, which keeps the bus type it read before
  rem pswdisk ran (USB) - stop it, so diskpart and Windows Setup read the disks again with pswdisk active
  "%SRC%\PSW\pswsvc.exe" stop vds >> %LOG% 2>&1
  reg query HKLM\SYSTEM\CurrentControlSet\Services\pswdisk >> %LOG% 2>&1
  copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
)
(echo rescan) > X:\psw-rescan.txt
diskpart /s X:\psw-rescan.txt >> %LOG% 2>&1
rem v1.3: what Windows now reports (disk list with type column via list disk + list volume); no per-disk loop: detail
rem disk on an empty card reader can block
copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
(echo list disk& echo list volume) > X:\psw-ld.txt
diskpart /s X:\psw-ld.txt >> %LOG% 2>&1
rem v2.3: corner text out of the stick's boot.wim (first start only; background, Setup does not wait)
if exist "%SRC%\PSW\psw-wim.cmd" if not exist "%SRC%\PSW\watermark-patched.txt" start "" /b cmd /c "%SRC%\PSW\psw-wim.cmd" > nul 2>&1
copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
exit /b 0
