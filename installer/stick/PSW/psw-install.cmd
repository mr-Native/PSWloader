@echo off
rem psw-install.cmd v1.7 (17.11.2026; v1.7: no findstr (Windows Setup's Windows PE has none): number, bus and text checks in plain cmd
rem (:isnum, :busof, :hastext); bus SATA counts as USB (pswdisk reports USB disks as SATA, the PS5 has no SATA disks); v1.6: PSWSetup from sources\$OEM$\$1\PSWSetup (release stick layout, old PSWSetup\ still accepted)
rem and SetupComplete.cmd into the new Windows, so PSWSetup runs by itself before the first sign-in as with Windows Setup; v1.5: PSWloader names - PSW\ folder, PSWSetup\, C:\PSWSetup, psw-install.log; v1.4 v1.4: /from-setup = started by the stick's autounattend.xml as soon as Windows Setup boots
rem (title, explanation, waits for a key before closing on cancel or failure); v1.3: the internal PS5 M.2 can be chosen as target by hand (extra confirmation INTERNAL),
rem PS5 storage drivers are injected into the new Windows (needed to boot from the internal M.2), neutral certificate name;
rem v1.2: step 0 loads ps5nvmsi + ps5m2flt from PSW\drivers so the internal M.2 is listed
rem too (never a target: only USB disks are accepted, SanDisk/Optimus refused even with /force-wipe); v1.1.1: "empty" only when POSITIVELY confirmed (diskpart ok + no partition rows +
rem the en/de "no partitions" message), anything unclear counts as NOT empty (review); v1.1 F29: a disk with ANY partition is refused - /auto never takes it, by hand only with
rem /force-wipe (then the number is typed twice + YES); v1.0.1: bcdedit/xcopy results checked before DONE) - installs Windows 11 from this stick onto a disk of your choice, without the Windows
rem Setup GUI (Setup refuses USB target disks; the internal M.2 needs the PS5 storage drivers). Start it in Windows Setup: press Shift+F10, then type
rem     D:\PSW\psw-install.cmd        (use the stick's drive letter)
rem Steps: pick the disk -> erase it (GPT: EFI 300 MB, MSR 16 MB, Windows) -> DISM applies "Windows 11 Pro" from
rem sources\install.wim / install.esd / install.swm (any build) -> boot files (bcdboot) -> boot options for the PS5
rem (test signing, no dynamic tick) -> copies PSWSetup\ to C:\PSWSetup -> restart.
rem Options (used by the test answer file, not needed by hand):
rem   /auto MODEL MIN_GB MAX_GB   pick the disk without asking: exactly one non-removable USB disk whose "detail disk" text
rem                               contains MODEL and whose size is MIN_GB..MAX_GB (never SanDisk/Optimus), else power off
rem   /unattend FILE              copies FILE to Windows\Panther\unattend.xml (first-start settings)
rem   /edition "NAME"             image name, default "Windows 11 Pro"
rem   /from-setup                 started by autounattend.xml on this stick (no Shift+F10 needed); otherwise like by hand
rem   /force-wipe                 by hand only: allow a disk that already has partitions (Linux, Windows, data) to be erased.
rem                               Without it the script refuses every disk that is not empty. Never used with /auto.
rem Log: X:\psw-install.log, copied to the stick as PSW\psw-install.log
setlocal enabledelayedexpansion
set "SRC=%~d0"
set "LOG=X:\psw-install.log"
set "SETUP=0" & set "AUTO=0" & set "FORCE=0" & set "MODEL=" & set "MIN_GB=0" & set "MAX_GB=0" & set "UNATTEND=" & set "EDITION=Windows 11 Pro"
:args
if "%~1"=="" goto args_done
if /i "%~1"=="/auto" ( set "AUTO=1" & set "MODEL=%~2" & set "MIN_GB=%~3" & set "MAX_GB=%~4" & shift & shift & shift & shift & goto args )
if /i "%~1"=="/unattend" ( set "UNATTEND=%~2" & shift & shift & goto args )
if /i "%~1"=="/force-wipe" ( set "FORCE=1" & shift & goto args )
if /i "%~1"=="/from-setup" ( set "SETUP=1" & shift & goto args )
if /i "%~1"=="/edition" ( set "EDITION=%~2" & shift & shift & goto args )
echo Unknown option %1 & goto fail
:args_done
if "%AUTO%%FORCE%"=="11" ( echo /force-wipe is not allowed with /auto & goto fail )
if "%SETUP%"=="1" (
  title PSWloader - Windows installation
  echo ==========================================================================
  echo  PSWloader - install Windows 11 on this console
  echo  You choose the target disk in the next step. Nothing is changed before you
  echo  confirm it. To stop now, close this window: the console turns off.
  echo ==========================================================================
  echo.
)
echo psw-install v1.5 src=%SRC% setup=%SETUP% auto=%AUTO% model="%MODEL%" window=%MIN_GB%..%MAX_GB% edition="%EDITION%" > %LOG%

rem ---------------------------------------------------------------- 1. find the image (wim / esd / swm)
set "IMG=" & set "SWM="
if exist "%SRC%\sources\install.wim" set "IMG=%SRC%\sources\install.wim"
if exist "%SRC%\sources\install.esd" set "IMG=%SRC%\sources\install.esd"
if exist "%SRC%\sources\install.swm" ( set "IMG=%SRC%\sources\install.swm" & set "SWM=%SRC%\sources\install*.swm" )
if not defined IMG ( echo No sources\install.wim/.esd/.swm on %SRC% - this script must run from the Windows stick. & goto fail )
echo image %IMG% >> %LOG%
set "INDEX="
set "CUR="
dism /English /Get-WimInfo /WimFile:"%IMG%" > X:\psw-wiminfo.txt 2>&1
type X:\psw-wiminfo.txt >> %LOG%
for /f "usebackq tokens=1,* delims=:" %%a in ("X:\psw-wiminfo.txt") do (
  set "K=%%a" & set "V=%%b"
  for /f "tokens=1" %%k in ("!K!") do set "K=%%k"
  if defined V for /f "tokens=*" %%v in ("!V!") do set "V=%%v"
  if /i "!K!"=="Index" set "CUR=!V!"
  if /i "!K!"=="Name" if /i "!V!"=="%EDITION%" if not defined INDEX set "INDEX=!CUR!"
)
if not defined INDEX ( echo Image "%EDITION%" not found in %IMG% - see X:\psw-wiminfo.txt & goto fail )
echo edition "%EDITION%" = index %INDEX% >> %LOG%

rem ---------------------------------------------------------------- 2. pick the disk (step 0: PS5 storage drivers first)
call :loaddrivers
(echo list disk) > X:\psw-ld.txt
diskpart /s X:\psw-ld.txt > X:\psw-ld.out
type X:\psw-ld.out >> %LOG%
if "%AUTO%"=="1" goto pick_auto
echo.
type X:\psw-ld.out
echo.
echo Which disk gets Windows? EVERYTHING ON IT WILL BE ERASED.
set /p "TARGET=Disk number: "
call :isnum "!TARGET!"
if not "!ISNUM!"=="1" ( echo Not a number. & goto fail )
(echo select disk !TARGET!& echo detail disk& echo list partition) > X:\psw-dd.txt
diskpart /s X:\psw-dd.txt
rem v1.3: USB disks and the internal PS5 M.2 (bus NVMe) are valid targets; any other bus is refused. The internal M.2 needs
rem the extra word INTERNAL, because it may hold the system that runs today.
set "TUSB=0" & set "TNVME=0"
diskpart /s X:\psw-dd.txt > X:\psw-dd-t.out 2>&1
call :busof X:\psw-dd-t.out
if /i "!BUS!"=="USB" set "TUSB=1"
if /i "!BUS!"=="SATA" set "TUSB=1"
if /i "!BUS!"=="NVMe" set "TNVME=1"
if not "!TUSB!!TNVME!"=="10" if not "!TUSB!!TNVME!"=="01" ( echo Disk !TARGET! is neither a USB disk nor the internal M.2 - not a valid target. Nothing changed. & echo REFUSED bus >> %LOG% & goto end )
if "!TNVME!"=="1" (
  echo.
  echo Disk !TARGET! is the INTERNAL M.2 of the console. Installing there replaces whatever is on it.
  set /p "INT=Type INTERNAL to allow the internal M.2 as target: "
  if not "!INT!"=="INTERNAL" ( echo Cancelled, nothing changed. & echo internal M.2 not confirmed >> %LOG% & goto end )
  echo internal M.2 confirmed >> %LOG%
)
call :checkempty !TARGET!
if not "!EMPTY!"=="1" if not "%FORCE%"=="1" (
  echo.
  echo Disk !TARGET! is NOT confirmed empty: it has partitions ^(maybe another system: Linux, Windows or data^) or could not be checked. Nothing changed.
  echo Only if you are sure it may be erased: run the script again with /force-wipe
  echo DISK NOT CONFIRMED EMPTY, refused without /force-wipe >> %LOG%
  goto end
)
echo.
echo CHECK THE MODEL, SIZE AND PARTITIONS ABOVE. If this disk holds another system (Linux, Windows, data), STOP NOW.
set /p "AGAIN=Type the disk number again: "
if not "!AGAIN!"=="!TARGET!" ( echo Numbers differ, nothing changed. & goto end )
set /p "OK=Type YES to erase disk !TARGET! and install Windows: "
if not "!OK!"=="YES" ( echo Cancelled, nothing changed. & goto end )
goto picked
:pick_auto
if not defined MODEL ( echo /auto needs MODEL & goto poweroff )
set HITS=0
set "TARGET="
for /f "tokens=2,4,5" %%a in (X:\psw-ld.out) do (
  set N=%%a& set SZ=%%b& set UN=%%c
  call :isnum "!N!"
  if "!ISNUM!"=="1" (
    set GB=
    if /i "!UN!"=="GB" set GB=!SZ!
    if /i "!UN!"=="TB" set /a GB=!SZ!*1024
    (echo select disk !N!& echo detail disk& echo list partition) > X:\psw-dd.txt
    diskpart /s X:\psw-dd.txt > X:\psw-dd-!N!.out
    type X:\psw-dd-!N!.out >> %LOG%
    set USB=0& set BAD=0& set ID=0& set PARTS=0
    rem F29: a disk that already has ANY partition is never taken automatically (it may be another system disk)
    call :checkempty !N!
    if not "!EMPTY!"=="1" ( set "PARTS=1" & set "BAD=1" )
    call :busof X:\psw-dd-!N!.out
    if /i "!BUS!"=="USB" set USB=1
    if /i "!BUS!"=="SATA" set USB=1
    for %%t in (SanDisk Optimus Removable Wechsel) do (
      call :hastext X:\psw-dd-!N!.out "%%t"
      if "!HAS!"=="1" set BAD=1
    )
    call :hastext X:\psw-dd-!N!.out "%MODEL%"
    if "!HAS!"=="1" set ID=1
    set C=0
    if defined GB if !USB!==1 if !BAD!==0 if !ID!==1 if !GB! GEQ %MIN_GB% if !GB! LEQ %MAX_GB% set C=1
    echo disk !N! size=!SZ! !UN! usb=!USB! has-partitions=!PARTS! excluded=!BAD! model=!ID! candidate=!C! >> %LOG%
    if !C!==1 (
      set /a HITS+=1
      set "TARGET=!N!"
    )
  )
)
if not "%HITS%"=="1" ( echo ABORT: %HITS% candidate disks, need exactly 1 >> %LOG% & goto poweroff )
:picked
echo TARGET disk %TARGET% >> %LOG%

rem ---------------------------------------------------------------- 3. partition (free letters for EFI + Windows)
set "S=" & set "W="
for %%L in (S T U V R Q P O N M L K) do if not defined S if not exist %%L:\ set "S=%%L"
for %%L in (W Y Z J I H G) do if not defined W if not exist %%L:\ if not "%%L"=="!S!" set "W=%%L"
(
echo select disk %TARGET%
echo clean
echo convert gpt
echo create partition efi size=300
echo format quick fs=fat32 label=System
echo assign letter=%S%
echo create partition msr size=16
echo create partition primary
echo format quick fs=ntfs label=Windows
echo assign letter=%W%
) > X:\psw-part.txt
type X:\psw-part.txt >> %LOG%
echo Partitioning disk %TARGET% ...
diskpart /s X:\psw-part.txt >> %LOG% 2>&1 || ( echo diskpart failed & goto fail_auto )

rem ---------------------------------------------------------------- 4. apply the image + boot files
echo Applying "%EDITION%" (index %INDEX%) to %W%: - this takes 5-20 minutes ...
if defined SWM (
  dism /Apply-Image /ImageFile:"%IMG%" /SWMFile:"%SWM%" /Index:%INDEX% /ApplyDir:%W%:\ >> %LOG% 2>&1 || ( echo DISM failed & goto fail_auto )
) else (
  dism /Apply-Image /ImageFile:"%IMG%" /Index:%INDEX% /ApplyDir:%W%:\ >> %LOG% 2>&1 || ( echo DISM failed & goto fail_auto )
)
rem v1.3: PS5 storage drivers into the new Windows (boot-start); without them Windows cannot boot from the internal M.2
for %%I in (ps5nvmsi ps5m2flt) do if exist "%SRC%\PSW\drivers\%%I\%%I.inf" (
  dism /Image:%W%:\ /Add-Driver /Driver:"%SRC%\PSW\drivers\%%I\%%I.inf" /ForceUnsigned >> %LOG% 2>&1 && ( echo driver %%I injected >> %LOG% ) || ( echo WARNING driver %%I not injected >> %LOG% )
)
bcdboot %W%:\Windows /s %S%: /f UEFI >> %LOG% 2>&1 || ( echo bcdboot failed & goto fail_auto )
rem PS5 boot options (same values PSWSetup sets): test signing for our drivers, no dynamic tick (freeze ~9 min after start otherwise)
set "BCD=%S%:\EFI\Microsoft\Boot\BCD"
bcdedit /store %BCD% /set {default} testsigning on >> %LOG% 2>&1 || ( echo bcdedit testsigning failed & goto fail_auto )
bcdedit /store %BCD% /set {default} disabledynamictick yes >> %LOG% 2>&1 || ( echo bcdedit disabledynamictick failed & goto fail_auto )
bcdedit /store %BCD% /set {default} bootstatuspolicy IgnoreAllFailures >> %LOG% 2>&1 || ( echo bcdedit bootstatuspolicy failed & goto fail_auto )
bcdedit /store %BCD% /enum {default} >> %LOG% 2>&1

rem ---------------------------------------------------------------- 5. PS5 files for the first start
set "PSWS=%SRC%\sources\$OEM$\$1\PSWSetup"
if not exist "%PSWS%\PSWSetup.exe" set "PSWS=%SRC%\PSWSetup"
set "SC=%SRC%\sources\$OEM$\$$\Setup\Scripts\SetupComplete.cmd"
set "AUTO="
if exist "%PSWS%\PSWSetup.exe" (
  xcopy /e /i /q /y "%PSWS%" %W%:\PSWSetup >> %LOG% 2>&1 || ( echo copying PSWSetup failed & goto fail_auto )
  if not exist %W%:\PSWSetup\PSWSetup.exe ( echo PSWSetup.exe missing after copy & goto fail_auto )
  echo PSWSetup copied to C:\PSWSetup from %PSWS% >> %LOG%
  if exist "%SC%" (
    mkdir %W%:\Windows\Setup\Scripts 2> nul
    copy /y "%SC%" %W%:\Windows\Setup\Scripts\SetupComplete.cmd >> %LOG% 2>&1 && set "AUTO=1"
    echo SetupComplete.cmd copied: PSWSetup runs by itself before the first sign-in >> %LOG%
  )
) else if defined UNATTEND ( echo PSWSetup folder missing on the stick - the test needs it & goto fail_auto )
if defined UNATTEND (
  if not exist "%UNATTEND%" ( echo unattend file %UNATTEND% missing & goto fail_auto )
  mkdir %W%:\Windows\Panther 2> nul
  copy /y "%UNATTEND%" %W%:\Windows\Panther\unattend.xml >> %LOG% 2>&1 || goto fail_auto
  echo unattend copied >> %LOG%
)
echo DONE >> %LOG%
copy /y %LOG% "%SRC%\PSW\psw-install.log" > nul 2>&1
echo.
if defined AUTO ( echo Done. Windows restarts now and installs the PSWloader drivers and tools by itself.
) else echo Done. Windows restarts now. After the first sign-in run C:\PSWSetup\PSWSetup.exe.
if "%AUTO%"=="1" exit /b 0
rem /auto: the answer file restarts after checking DONE in the log (no reboot/shutdown race here)
pause
wpeutil reboot
exit /b 0

:loaddrivers
rem Step 0 (v1.2): make the internal PS5 M.2 visible in WinPE: ps5nvmsi (MSI lower filter) first, then ps5m2flt (nested-GPT
rem overlay). Only loads drivers into this WinPE session (RAM); nothing is written to a disk. If a signing certificate (.cer)
rem exists and certutil is present, the public test certificate is trusted for this WinPE session first.
if not exist "%SRC%\PSW\drivers" ( echo step0: no PSW\drivers folder - internal M.2 stays hidden >> %LOG% & exit /b 0 )
for %%C in (pswloader-signing.cer ps5-test.cer) do if exist "%SRC%\PSW\drivers\%%C" (
  where certutil > nul 2>&1 && (
    certutil -f -addstore Root "%SRC%\PSW\drivers\%%C" >> %LOG% 2>&1
    certutil -f -addstore TrustedPublisher "%SRC%\PSW\drivers\%%C" >> %LOG% 2>&1
  ) || echo step0: certutil not present in this WinPE >> %LOG%
)
for %%I in (ps5nvmsi ps5m2flt) do if exist "%SRC%\PSW\drivers\%%I\%%I.inf" (
  drvload "%SRC%\PSW\drivers\%%I\%%I.inf" >> %LOG% 2>&1
  echo step0: drvload %%I exit !ERRORLEVEL! >> %LOG%
  ping -n 4 127.0.0.1 > nul
)
(echo rescan) > X:\psw-rescan.txt
diskpart /s X:\psw-rescan.txt >> %LOG% 2>&1
exit /b 0

:checkempty
rem sets EMPTY=1 only if diskpart succeeded, shows no partition rows and says "no partitions" (en/de); else EMPTY=0
set "EMPTY=0"
(echo select disk %1& echo list partition) > X:\psw-ce.txt
diskpart /s X:\psw-ce.txt > X:\psw-ce-%1.out 2>&1
if errorlevel 1 ( echo checkempty %1: diskpart error >> %LOG% & exit /b 0 )
set "PFOUND=0"
for /f "usebackq tokens=1,2" %%a in ("X:\psw-ce-%1.out") do if /i "%%a"=="Partition" (
  call :isnum "%%b"
  if "!ISNUM!"=="1" set "PFOUND=1"
)
if "!PFOUND!"=="1" ( echo checkempty %1: partitions found >> %LOG% & exit /b 0 )
call :hastext X:\psw-ce-%1.out "no partitions"
set "NOPART=!HAS!"
rem "keine Partitionen" matches the German Windows PE diskpart text for "no partitions"
call :hastext X:\psw-ce-%1.out "keine Partitionen"
if "!HAS!"=="1" set "NOPART=1"
if not "!NOPART!"=="1" ( echo checkempty %1: no confirmed "no partitions" message >> %LOG% & exit /b 0 )
set "EMPTY=1"
echo checkempty %1: empty >> %LOG%
exit /b 0

:isnum
rem v1.7: ISNUM=1 when %1 is a plain decimal number without sign or leading zero (0 itself counts)
set "ISNUM=0"
set "NUMIN=%~1"
if not defined NUMIN exit /b 0
if "!NUMIN:~0,1!"=="-" exit /b 0
set "NUMCHK="
set /a "NUMCHK=NUMIN" 2> nul
if "!NUMCHK!"=="!NUMIN!" set "ISNUM=1"
exit /b 0

:busof
rem v1.7: BUS = the single word after the colon of a "detail disk" row that is USB, SATA or NVMe (rows "Type : USB",
rem "Typ : USB" in every language), empty when there is none
set "BUS="
for /f "usebackq tokens=1* delims=:" %%a in ("%~1") do for /f "tokens=1,2" %%x in ("%%b") do if "%%y"=="" (
  if /i "%%x"=="USB" set "BUS=USB"
  if /i "%%x"=="SATA" set "BUS=SATA"
  if /i "%%x"=="NVMe" set "BUS=NVMe"
)
exit /b 0

:hastext
rem v1.7: HAS=1 when a line of file %1 contains text %2 (case-insensitive)
set "HAS=0"
set "HT=%~2"
if not defined HT exit /b 0
for /f "usebackq delims=" %%l in ("%~1") do (
  set "HL=%%l"
  if /i not "!HL:%HT%=!"=="!HL!" set "HAS=1"
)
exit /b 0

:fail_auto
if "%AUTO%"=="1" goto poweroff
:fail
echo FAILED >> %LOG%
copy /y %LOG% "%SRC%\PSW\psw-install.log" > nul 2>&1
echo Failed - see X:\psw-install.log (copy on the stick: PSW\psw-install.log).
if "%SETUP%"=="1" ( echo The console turns off when you press a key. & pause > nul )
exit /b 1
:poweroff
echo ABORT - powering off >> %LOG%
copy /y %LOG% "%SRC%\PSW\psw-install.log" > nul 2>&1
wpeutil shutdown
exit /b 1
:end
if "%SETUP%"=="1" ( echo. & echo Nothing was changed. The console turns off when you press a key. & pause > nul )
exit /b 0
