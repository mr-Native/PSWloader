@echo off
rem psw-drivers.cmd v3.1 (PSWloader v1.0; v3.1: "only the installer drive" asks for a cold start through GameOS instead of Refresh - a drive added while Setup runs stopped the installation at about 22 % (HW 26.11.); v3.0: full-screen "Checking the system" over Windows Setup until the test-signing check is done; when test signing was off (an NTFS stick: the firmware can only prepare a FAT32 stick) PSWloader restarts the console by itself (wpeutil reboot) instead of asking for it; v2.9: pswsvc 1.12 usbwatch - USB disks that appear later get idle power-down off and a port cycle too (HW 26.11.: the Realtek SSD came up after usbcycle and Setup stopped writing to it at 24 %), empty card readers are not restarted; v2.8: when test signing was off the script stops after switching it on and the status window shows one restart message (it used to go on scanning USB disks); v2.7: status window in the look of the PSWloader windows (PSW\pswui.exe), this console is hidden while it shows the same lines; v2.6: disk count in every language (issue #16, Russian diskpart counted 0 disks); v2.5: welcome line, status lines and a clear hint when test signing was off (issue #9: no disks); v2.4: logs of the last 10 Setup starts in PSW\logs; v2.3: psw-wim.cmd in the background - removes the corner text in the stick's boot.wim for every
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
rem v2.5: the window shows who it is and what happens (it used to stay empty, users could not tell whether it hung)
set "PSWVER=v1.2"
title PSWloader %PSWVER% - Windows Setup
echo.
echo   Welcome to PSWloader %PSWVER%
echo.
echo   Loading the PS5 drivers for Windows Setup ...
set "SRC=%~d0"
set "LOG=X:\psw-drivers.log"
echo psw-drivers v2.7 src=%SRC% > %LOG%
rem v2.7: status window in the look of the PSWloader windows (PSW\pswui.exe, plain Win32 - Windows PE has no PowerShell). This
rem console keeps working as before and is hidden while the window shows its lines; without pswui.exe nothing changes.
set "UI=X:\psw-ui.txt"
rem v3.0: F = full screen "Checking the system ..." until the test-signing check below is done
> "%UI%" echo F
>> "%UI%" echo HWelcome to PSWloader %PSWVER%
>> "%UI%" echo SLoading the PS5 drivers for Windows Setup
if exist "%SRC%\PSW\pswui.exe" ( start "" "%SRC%\PSW\pswui.exe" "%UI%" "PSWloader %PSWVER% - Windows Setup" & echo pswui started >> %LOG% )
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
    set "BCDOK="
    echo TESTSIGNING is OFF in WinPE - enabling testsigning in stick BCD for next boot >> %LOG%
    echo.
    echo   Test signing was off in this start, so Windows Setup cannot see the disks yet.
    echo   It is switched on now. Close Windows Setup with X and confirm: the console
    echo   restarts into Windows Setup, and the disks are there.
    echo.
    for %%B in ("%SRC%\efi\microsoft\boot\bcd" "%SRC%\boot\bcd") do if exist %%B (
      bcdedit /store %%B /set {default} testsigning on >> %LOG% 2>&1
      if not errorlevel 1 set "BCDOK=1"
      echo bcdedit %%B exit !ERRORLEVEL! >> %LOG%
    )
    copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
    rem v3.0 (user 26.11.2026): nothing else is useful in this start - full-screen restart notice and PSWloader restarts the
    rem console itself; the next start of the stick has test signing and lists the drives (a FAT32 stick never gets here:
    rem the firmware switches test signing on before Windows Boot Manager starts)
    rem only once and only when the stick's boot entry was changed - never a restart loop
    if defined BCDOK if not exist "%SRC%\PSW\restarted-for-testsigning.txt" (
      > "%SRC%\PSW\restarted-for-testsigning.txt" echo %DATE% %TIME%
      >> "%UI%" echo R
      echo restart needed - testsigning enabled, restarting with wpeutil >> %LOG%
      copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
      ping -n 8 127.0.0.1 > nul 2>&1
      wpeutil reboot >> %LOG% 2>&1
      ping -n 30 127.0.0.1 > nul 2>&1
    )
    echo no automatic restart ^(BCD changed: !BCDOK!, restarted before: see PSW\restarted-for-testsigning.txt^) >> %LOG%
    rem only when the restart could not be started: the old notice in the small window
    > "%UI%" echo HRestart needed
    >> "%UI%" echo WTest signing was off in this start, so Windows Setup cannot see the disks.
    >> "%UI%" echo WPSWloader has switched it on. Close Windows Setup with its X and confirm:
    >> "%UI%" echo Wthe console restarts into Windows Setup, and the disks are there.
    >> "%UI%" echo E
    exit /b 0
  )
)
copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
>> "%UI%" echo N
if "!NVRC!"=="0" del /q "%SRC%\PSW\restarted-for-testsigning.txt" > nul 2>&1
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
  rem v2.9: for 30 minutes, a USB disk that appears later (a USB SSD whose bridge came up late) gets the same treatment
  set "UW=x" & if defined STICKDISK set "UW=!STICKDISK!"
  if not exist "%SRC%\PSW\logs" mkdir "%SRC%\PSW\logs" > nul 2>&1
  start "" /b cmd /c ""%SRC%\PSW\pswsvc.exe" usbwatch !UW! >> "%SRC%\PSW\logs\usbwatch.log" 2>&1"
  echo usbwatch started >> %LOG%
  echo   Preparing the USB disks ...
  >> "%UI%" echo SPreparing the USB disks
  "%SRC%\PSW\pswsvc.exe" start pswdisk X:\PSWDRV\pswdisk\pswdisk.sys >> %LOG% 2>&1
  echo pswsvc start pswdisk exit !ERRORLEVEL! >> %LOG%
  rem v1.6: the stick detection above already started the Virtual Disk Service, which keeps the bus type it read before
  rem pswdisk ran (USB) - stop it, so diskpart and Windows Setup read the disks again with pswdisk active
  "%SRC%\PSW\pswsvc.exe" stop vds >> %LOG% 2>&1
  reg query HKLM\SYSTEM\CurrentControlSet\Services\pswdisk >> %LOG% 2>&1
  copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
)
>> "%UI%" echo SLooking for the disks
(echo rescan) > X:\psw-rescan.txt
diskpart /s X:\psw-rescan.txt >> %LOG% 2>&1
rem v1.3: what Windows now reports (disk list with type column via list disk + list volume); no per-disk loop: detail
rem disk on an empty card reader can block
copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
(echo list disk& echo list volume) > X:\psw-ld.txt
diskpart /s X:\psw-ld.txt >> %LOG% 2>&1
rem v2.5 (issue #10): the target must be a second disk - Windows cannot be installed onto the drive Setup runs from,
rem and that drive stays a USB disk for Setup. Count the other disks of 1 GB or more and say so in the window when there
rem is none. v2.6 (issue #16): the status can be two words and the unit is translated (Russian: "Disk 1  V seti  465 Gbait"),
rem so the size is the first number whose next word starts with G or T (GB, Go, Gbait, TB, To ...), wherever it stands.
(echo list disk) > X:\psw-ld2.txt
diskpart /s X:\psw-ld2.txt > X:\psw-ld2.out 2>&1
set "OTHER=0"
for /f "usebackq tokens=1-9" %%a in ("X:\psw-ld2.out") do (
  set "DN=%%b" & set "DNUM=" & set /a "DNUM=DN" 2> nul
  if "!DNUM!"=="!DN!" if not "!DN!"=="!STICKDISK!" (
    set "PREV=" & set "BIG="
    for %%t in (%%c %%d %%e %%f %%g %%h %%i) do (
      set "T=%%t"
      if defined PREV if /i "!T:~0,1!"=="G" set "BIG=1"
      if defined PREV if /i "!T:~0,1!"=="T" set "BIG=1"
      set "PREV=" & set "N=" & set /a "N=T" 2> nul
      if "!N!"=="!T!" set "PREV=1"
    )
    if defined BIG set /a OTHER+=1
  )
)
echo other disks of 1 GB or more: !OTHER! >> %LOG%
rem the diskpart call above started the Virtual Disk Service again - stop it as after pswdisk (v1.6), so it does not
rem keep polling the disks while Windows Setup copies the files
if exist "%SRC%\PSW\pswsvc.exe" "%SRC%\PSW\pswsvc.exe" stop vds >> %LOG% 2>&1
if defined STICKDISK if "!OTHER!"=="0" (
  echo.
  echo   Only the installer drive was found. Windows cannot be installed onto the drive
  echo   it starts from. Connect a second, empty drive ^(USB SSD/HDD or the internal M.2;
  echo   no partitions, no formatting^), switch the console off and start PSWloader again
  echo   from GameOS.
  echo.
  >> "%UI%" echo WOnly the installer drive was found. Windows cannot be installed onto
  >> "%UI%" echo Wthe drive it starts from. Connect a second, empty drive ^(USB SSD/HDD
  >> "%UI%" echo Wor the internal M.2; no partitions, no formatting^), switch the console
  >> "%UI%" echo Woff and start PSWloader again from GameOS.
)
rem v2.3: corner text out of the stick's boot.wim (first start only; background, Setup does not wait)
if exist "%SRC%\PSW\psw-wim.cmd" if not exist "%SRC%\PSW\watermark-patched.txt" start "" /b cmd /c "%SRC%\PSW\psw-wim.cmd" > nul 2>&1
copy /y %LOG% "%SRC%\PSW\psw-drivers.log" > nul 2>&1 & copy /y %LOG% "!HIST!" > nul 2>&1
set "UIW=0"
if "!NVRC!"=="577" set "UIW=1"
if defined STICKDISK if "!OTHER!"=="0" set "UIW=1"
if "!UIW!"=="0" >> "%UI%" echo DReady - go on in the Windows Setup window.
if "!UIW!"=="1" >> "%UI%" echo E
if "!NVRC!"=="577" ( echo   Restart needed - see above. ) else if defined STICKDISK if "!OTHER!"=="0" ( echo   Plug in a second drive - see above. ) else ( echo   Ready - go on in the Windows Setup window. This window can stay open. )
exit /b 0
