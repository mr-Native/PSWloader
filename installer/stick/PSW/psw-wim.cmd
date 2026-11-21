@echo off
rem psw-wim.cmd v1.0 (PSWloader v1.0, 18 November 2026) - started in the background by psw-drivers.cmd on the first start of a
rem setup stick. Windows PE reads the corner text of Windows (test mode, build line) from shell32.dll.mui before any script
rem runs (HW 18.11.), so it is removed in the stick's sources\boot.wim for every later start: the shell32.dll.mui files of
rem this Windows PE are blanked by pswsvc patchmui (same rules as tools\psw-hide-watermark.py) and put into a COPY of
rem boot.wim with DISM (images 1 and 2, robocopy /B because the files belong to TrustedInstaller); only when both commits
rem succeeded, the copy replaces boot.wim by renaming - an interrupted run leaves the original untouched. The original is
rem kept as sources\boot.wim.psw-orig. Marker: PSW\watermark-patched.txt. Log: X:\psw-wim.log, copied to PSW\psw-wim.log.
setlocal enabledelayedexpansion
set "SRC=%~d0"
set "LOG=X:\psw-wim.log"
set "W=%SRC%\sources\boot.wim"
set "N=%SRC%\sources\boot.psw-new.wim"
if exist "%SRC%\PSW\watermark-patched.txt" ( echo psw-wim: already patched > %LOG% & exit /b 0 )
if not exist "%W%" ( echo psw-wim: no sources\boot.wim > %LOG% & exit /b 0 )
echo psw-wim v1.0 %DATE% %TIME% > %LOG%
rd /s /q X:\pswwm 2> nul
md X:\pswwm
set "CNT=0"
for /d %%L in (X:\Windows\System32\??-??) do (
  set "IN=%%L\shell32.dll.mui"
  if exist "%%L\shell32.dll.mui.psw-orig" set "IN=%%L\shell32.dll.mui.psw-orig"
  if exist "!IN!" (
    md "X:\pswwm\%%~nxL"
    "%SRC%\PSW\pswsvc.exe" patchmui "!IN!" "X:\pswwm\%%~nxL\shell32.dll.mui" >> %LOG% 2>&1 && set /a CNT+=1
  )
)
if "!CNT!"=="0" ( echo no shell32.dll.mui found >> %LOG% & goto fail )
del /f /q "%N%" > nul 2>&1
copy /y "%W%" "%N%" >> %LOG% 2>&1 || ( echo copy of boot.wim failed - not enough space? >> %LOG% & goto fail )
attrib -r -h -s "%N%"
rd /s /q X:\pswmnt 2> nul
md X:\pswmnt
for %%I in (1 2) do (
  dism /Mount-Image /ImageFile:"%N%" /Index:%%I /MountDir:X:\pswmnt >> %LOG% 2>&1 || goto faildel
  for /d %%L in (X:\pswwm\*) do if exist "X:\pswmnt\Windows\System32\%%~nxL\shell32.dll.mui" (
    robocopy "%%L" "X:\pswmnt\Windows\System32\%%~nxL" shell32.dll.mui /B /IS /IT /R:0 /W:0 /NJH /NJS /NP >> %LOG% 2>&1
    if errorlevel 8 ( echo robocopy failed for %%~nxL >> %LOG% & dism /Unmount-Image /MountDir:X:\pswmnt /Discard >> %LOG% 2>&1 & goto faildel )
  )
  dism /Unmount-Image /MountDir:X:\pswmnt /Commit >> %LOG% 2>&1 || ( dism /Unmount-Image /MountDir:X:\pswmnt /Discard >> %LOG% 2>&1 & goto faildel )
  echo image %%I patched >> %LOG%
)
attrib -r -h -s "%W%"
if exist "%SRC%\sources\boot.wim.psw-orig" ( del /f /q "%W%" ) else ( ren "%W%" boot.wim.psw-orig )
ren "%N%" boot.wim || ( echo rename failed >> %LOG% & goto fail )
echo patched %DATE% %TIME% > "%SRC%\PSW\watermark-patched.txt"
echo done - the corner text is gone from the next start of this stick >> %LOG%
copy /y %LOG% "%SRC%\PSW\psw-wim.log" > nul 2>&1
exit /b 0
:faildel
del /f /q "%N%" > nul 2>&1
:fail
echo FAILED - boot.wim unchanged >> %LOG%
copy /y %LOG% "%SRC%\PSW\psw-wim.log" > nul 2>&1
exit /b 1
