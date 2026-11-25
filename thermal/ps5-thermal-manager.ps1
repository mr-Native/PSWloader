# ps5-thermal-manager.ps1 v1.8.0 (24.11.2026; v1.8.0 = with ps5pwr 1.17+ (caps 0x2000) the driver runs the PS5 Linux fan curve:
#   no fan target writes here any more (event fan_driver_curve), fan_status telemetry stays)
# ps5-thermal-manager.ps1 v1.7.0 (18.11.2026; v1.7.0 = real OC through ps5pwr 1.14.0 (caps 0x200), clock regulator, no curve:
#   in oc, REG_DWORDs under HKLM\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters decide (all 0 = today's oc): OcGfxMhz
#   (ForceGfxFreq 2231..2600, 0 = off), OcGfxMv (ForceGfxVid 700..1129 mV before the clock, 0 = no voltage), OcCpuBoostMhz
#   (SetMaxCpuBoostClk 3200..4000, 0 = off), OcCpuVidScale (signed, -50..-1, 0 = off). Applied once per Windows boot (marker
#   oc-boot.txt, deleted when taken back); while forced the RequestGfxclk regulator rests. Thermal: Tctl >= TargetC -> forced
#   clock -25 MHz per poll down to 2230, below that Unforce (the normal regulator takes over); Tctl <= TargetC-4 -> +25 MHz per
#   10 s back up to OcGfxMhz; Tctl >= GuardC-3 -> Unforce + CpuOcReset at once, no OC for the rest of this run. TM stop and a
#   preset change away from oc: Unforce + CpuOcReset. Events oc_force / oc_back / oc_up / oc_unforce / oc_skip / oc_plan)
# ps5-thermal-manager.ps1 v1.6.15 (17.11.2026; v1.6.15 = boost window HOLD no longer automatic in oc, opt-in -Hold (-NoHold still
#   accepted). HW 17.11. ~18:37 with ps5pwr 1.12.1: HOLD gave Time Spy 7755 (GPU avg 2049) - the SMU ignores the GPU floor and
#   splits one fixed budget, worse than without a window (8468-8573); RequestGfxclk 2300 refused (resp 0xFF). oc = standard clocks,
#   only the higher temperature limits 86/66/92)
# ps5-thermal-manager.ps1 v1.6.14 (17.11.2026; v1.6.14 = with ps5pwr 1.12.1+ BAPM and HOLD come from the capabilities of the
#   LOADED driver (VERSION IOCTL caps 0x40 / 0x80), not from the file hash - the v1.12.0 HW test sent HOLD to a still loaded
#   v1.10.3 (error 87) because the copied file kept its old time stamp; event driver_mismatch when file and loaded differ;
#   hash C33FEE0A = 1.12.1; boost presets computed after the capability check)
# ps5-thermal-manager.ps1 v1.6.13 (17.11.2026; v1.6.13 = user-approved OC package: with ps5pwr v1.12.0 (image 4816F4A9) the oc
#   preset opens the SMU boost window HOLD (-BoostEnterHold: GPU 2230 held, CPU up to 3500) by itself, ONCE per Windows boot
#   (marker boost-boot.txt; a TM restart in the same boot does not send it again); -NoHold opts out, -OcBoost/-BoostWindow
#   keep their old meaning; BAPM init also with v1.12.0; settings file header '# PSWloader settings v1' (firmware r5.24))
# ps5-thermal-manager.ps1 v1.6.12 (17.11.2026; v1.6.12 = settings key diag also accepts 'full' (firmware r5.20.6: system check
#   with the ICC fan/temperature reads); nothing else changed)
# ps5-thermal-manager.ps1 v1.6.11 (17.11.2026; v1.6.11 = -BoostExit (= UniversalModeExit 0x23, the same
#   message that capped the GPU at ~2000) only if THIS boot really opened the window: marker boost-boot.txt (LastBootUpTime)
#   written on a successful BoostEnter, deleted after BoostExit. Before, -SetPreset/-SetSetting to standard/efficiency sent
#   0x23 although the window is off by default. The window decision uses the preset's BAPM plan, not the bapm_skip result,
#   so -BoostWindow still opens it on a TM restart in the same boot)
# ps5-thermal-manager.ps1 v1.6.10 (17.11.2026; v1.6.10 = -BapmInit only ONCE per Windows boot (marker bapm-boot.txt next to the log
#   = LastBootUpTime of the successful init). Every init starts with UniversalModeExit; a second init in the same boot (TM restart,
#   preset change) left the GPU at ~2000 MHz although 2225 was requested: Time Spy oc 7962 vs standard after a cold boot 8422/8504)
# ps5-thermal-manager.ps1 v1.6.9 (17.11.2026; v1.6.9 = settings keys refresh=native|max (r5.16) and diag=off|once (r5.20.1,
#   ps5-settings.h v1.3) are kept when the file is rewritten; before, a preset change dropped refresh=max. Lines with keys this
#   script does not know are passed through unchanged (before the crc32 line), so future firmware keys survive too)
# ps5-thermal-manager.ps1 v1.6.8 (17.11.2026; v1.6.8 = SMU boost window off by default, -BoostWindow opts in; -NoBoostWindow stays
#   accepted. Time Spy standard: BAPM + window 7739 (GPU avg 2049 MHz), BAPM without window 8504 (GPU avg 2216 MHz, CPU 6396))
# ps5-thermal-manager.ps1 v1.6.7 (17.11.2026; v1.6.7 = proportional fan on raw Tctl every poll: EMC target = preset - 1.5 *
#   (Tctl - (TargetC-12)), floor 55 C; down at once, up 1 C per 6 s. v1.6.6 reacted too late: Tctl 70 -> 97 C in 5 s)
# ps5-thermal-manager.ps1 v1.6.6 (17.11.2026; v1.6.6 = closed-loop fan on Tctl: the EMC target moves between 55 C and the quiet
#   preset value - down 3 C per 6 s while Tctl >= TargetC-6, up 1 C per 6 s while Tctl <= TargetC-14 (Time Spy was stopped by the
#   guard at Tctl 91 C while the EMC held its SoC sensor at 70 C with 22-40 % duty))
# ps5-thermal-manager.ps1 v1.6.5 (17.11.2026; v1.6.5 = -NoBoostWindow (BAPM init yes, SMU boost window no) for the A/B series;
#   every -GfxMeasuredSeconds (5) one 'gfx_measured' row with the KMD's real GPU clock (GRBM counters: GfxClkMeasuredMHz,
#   GfxClkSetPointMHz, GfxBusyPermille from the active state generation's Ps5Modes key; read-only))
# ps5-thermal-manager.ps1 v1.6.4 (17.11.2026; v1.6.4 = fan targets 72/70/66 C (eff/standard/oc): with 76/76/78 the thermal guard stopped
#   Time Spy at Tctl 92.5 C - the EMC fan servo reacts too late for combined CPU+GPU load; oc cools hardest for its clocks)
# ps5-thermal-manager.ps1 v1.6.3 (17.11.2026; v1.6.3 = with ps5pwr v1.10.3 (BAPM init as PS5 Linux mp1_init): in standard and oc the
#   start runs -BapmInit once and then opens the SMU boost window (UniversalModeEntry, CPU 3200..3500 / GPU 2000..2230 MHz, the
#   GameOS range); -NoBapm switches both off; efficiency unchanged. Undo of the BAPM weights = restart.)
# ps5-thermal-manager.ps1 v1.6.2 (17.11.2026; v1.6.2 = fan targets 76/76/78 C, gpu 80/83/86, guard 88/90/92 after the v1.6.1 load test
#   (fan ramped gradually, but Tctl spikes to 98 C at fan 80 were too close to the limit); knows ps5pwr v1.10.2 release C5D351EF)
# ps5-thermal-manager.ps1 v1.6.1 (17.11.2026; v1.6.1 = fan targets raised after the 3-min GPU load test: at 72 C the SoC (77-85 C under
#   full load) never reached the target and the fan ran at 100 %; now eff fan 79 / gpu 82 / guard 88, standard 80 / 85 / 91, oc 80 / 88 / 93)
# ps5-thermal-manager.ps1 v1.6.0 (17.11.2026; v1.6.0 = temperature-led fan: the EMC fan servo target now sits BELOW the GPU throttle target
#   and the guard per preset (fan quiet until ~70 C instead of full speed above 50/60 C under load), Default = GameOS GPU clock 2230 MHz:
#   efficiency 1800 MHz fan 70 / gpu 76 / guard 84; standard 2230 fan 72 / gpu 80 / guard 88; oc 2230+ fan 74 / gpu 84 / guard 90).
# ps5-thermal-manager.ps1 v1.5.1 (17.11.2026; v1.5.1 = hash table + 2424E686 = 1.10.2 (release, no VERSION IOCTL) and
#   C4F91EFC = 1.11.2; hash_matches compared as [version] (1.10 == 1.10.0). v1.5.0 = capabilities from the LOADED driver via IOCTL_PS5PWR_VERSION
#   0x80026428 (ps5pwr v1.11.1+: {size,major,minor,patch,caps}): boost = caps&0x08, fan min 50 = caps&0x10, fan_status poll
#   only with caps&0x20; older drivers (ERROR_INVALID_FUNCTION) or no handle -> hash table as before; hash stays a cross-check.
#   Hash table + 55B42814 = 1.11.1. v1.4.3 = ps5pwr v1.11 known (image 73F87F16, hotplug H2; same CLI,
#   same path v1.10). v1.4.2 = ps5pwr v1.10.1 known (image 58C96659, hotplug fix; same CLI,
#   same path v1.10). v1.4.1 = ps5pwr v1.10 known (image 12133625, power button; same
#   CLI/IOCTLs as v1.9), driver path v1.10 > v1.9 > v1.8.2 > v1.7 > v1.6. v1.4.0 = ps5pwr v1.9 (image 70C1D6EA, boost + fan 50..100 like
#   v1.8.2; driver path v1.9 > v1.8.2 > v1.7 > v1.6) and fan telemetry: every FanPollSeconds (5) ps5pwr -Fan ->
#   event fan_status {duty_percent, zone0_mainsoc, zone1..3, sensor18}; no RPM exists. Older drivers: logged once, off.
#   v1.3.9: v1.3.9 = settings key pstates=0|1 (r5.12 / ps5-settings.h v1.1:
#   line written only when 1, so older files stay byte-identical). v1.3.8: v1.3.8 = loaded-driver detection by the SHA256 of the ps5pwr image
#   in the service ImagePath (known: d7107a77 = v1.8.2) and "file not newer than the last boot" (staged != loaded); GFXCLK op 4
#   is NOT used (GetGfxFrequency 0x37 is absent on the base PS5, resp 0xFE). A BoostEnter answer resp 1 also proves v1.8+.
#   New -OcMaxMhz (overrides MaxMhz only in the oc preset, 500..2230) for the OC step series. -OcBoost sends BoostEnter at
#   every start in the oc preset (the SMU forgets it on reboot). v1.3.7: v1.3.7 = ps5pwr v1.8.2: driver C:\BC250\drivers\ps5pwr\v1.8.2 (fallback
#   v1.7, v1.6); fan target range follows the loaded driver (v1.3.8: image hash, not op 4); oc fan 50;
#   optional -OcBoost: -BoostEnter at start in the oc preset, -BoostExit when -SetPreset/-SetSetting leaves oc and in finally.
#   Default off. MaxMhz oc stays 2100. v1.3.6: v1.3.6 = -SetPreset / -SetSetting preset=... also writes the guard
#   limit of the new preset to -GuardFile at once (guard v18 re-reads it on change; review). v1.3.5: v1.3.5 = oc MaxMhz 2100 instead of 2225 (review: without the
#   UniversalModeEntry the SMU caps at ~2075, 2225 only moves the operating point); back to 2225 only after the boost test
#   with ps5pwr v1.8.1. v1.3.4: v1.3.4 = control on the median of the last 3 Tctl samples (one
#   spike 64.5 -> 72.8 -> 63.8 C no longer cuts the clock) and a temperature deadband of +-TempDeadbandC (default 1.0) around
#   TargetC (efficiency run 11:00 chattered +-25..50 MHz every 2 s at 66-69 C). Samples log raw + filtered. History cleared after a
#   sensor error, mean of 2 during warm-up (review). v1.3.3: v1.3.3 = -SetSetting key=value for every PS5UEFI.CFG key
#   (preset efficiency|standard|oc, lang en|de, fastboot 0|1, bootorder internal|usb; whitelist only, same atomic write).
#   v1.3.2: v1.3.2 = r5.7 settings file \PS5UEFI.CFG (ps5-settings.h v1: text +
#   crc32 IEEE over every byte before the crc32 line). Read order: PS5UEFI.CFG > .NEW > .BAK (each only with valid crc) >
#   legacy PS5POWER.CFG > UEFI variable > standard. New: -SetPreset efficiency|standard|oc writes PS5UEFI.CFG exactly like
#   the firmware (keeps lang/fastboot/bootorder; NEW -> read back + parse -> CFG renamed to BAK -> NEW renamed to CFG) and
#   exits without starting the controller. v1.3.1: v1.3.1 = fan target clamped to ps5pwr's 60..100 C, oc fan 55 -> 60
#   (ps5pwr rejected 55 -> endless fan_retry, readback). v1.3: v1.3 = power presets efficiency|standard|oc (OC-Mode; 'power' = alias of oc): read at start from
#   \PS5POWER.CFG on the boot ESP (read-only, via the volume path, no drive letter) or else the UEFI variable
#   Ps5PowerPreset {5efc5ae0-b753-4f7a-a662-e34d15a6d8e5} (UINT32 0/1/2), default standard; -Preset overrides both.
#   The preset only fills parameters that were NOT given explicitly (old calls keep working). New: CPU cap
#   (PROCTHROTTLEMAX ceiling) + boost (PERFBOOSTMODE), guard limit written to C:\ps5-work\thermal-guard-limit.txt for
#   thermal-supervisor.ps1 v1.1, rule guard >= max(TargetC+4, CpuHighC) (F14). Original boost/CPU restored in finally.)
# v1.2 (15.11.2026; v1.2 = review: start <= 2000 MHz like PS5 Linux,
#   CPU back to 100 % in finally, one fan attempt per round instead of a blocking retry loop)
# Heat management for PS5 native Windows. Runs as SYSTEM via scheduled task at LOGON (+120 s), never at boot:
# an early ICC/SMU request while the system disk is still settling is avoided on purpose.
#  1. fan: sets the MainSoC fan target (default 60 C) through ps5pwr and verifies the readback; again every 10 min
#  2. GPU: with ps5pwr v1.7 (-GfxClk) an integral controller like PS5 Linux gpufreq.c: every poll the clock limit
#     moves by GainMhzPerC per degree off TargetC, clamped to MinMhz..MaxMhz; a request is sent only when it moves
#     by DeadbandMhz or more. Without v1.7 this stage is skipped (logged once).
#  3. CPU: only if Tctl still reaches CpuHighC, the maximum processor state (power plan, AC+DC) steps down by 10 %
#     to CpuMinPercent; below CpuLowC it steps back up.
# No process is killed. Log: C:\ps5-work\thermal-manager.jsonl (changes + one sample per minute).
# Revert: Unregister-ScheduledTask PS5-ThermalManager; ps5pwr.ps1 -SetGfxClk 2230;
#         powercfg /setacvalueindex scheme_current sub_processor PROCTHROTTLEMAX 100 (and /setdcvalueindex)
param([int]$FanTargetC = 60, [int]$TargetC = 83, [int]$MinMhz = 1000, [int]$MaxMhz = 2000, [int]$GainMhzPerC = 25,
      [int]$DeadbandMhz = 25, [int]$CpuHighC = 88, [int]$CpuLowC = 78, [int]$CpuMinPercent = 70, [int]$PollSeconds = 2,
      [string]$Driver = 'C:\BC250\drivers\ps5pwr\v1.10\ps5pwr.ps1', [switch]$OcBoost, [switch]$Hold, [switch]$NoHold, [switch]$NoBapm, [switch]$NoBoostWindow, [switch]$BoostWindow, [int]$GfxMeasuredSeconds = 5, [int]$FanPollSeconds = 5, [int]$OcMaxMhz = 0, [string]$Log = 'C:\ps5-work\thermal-manager.jsonl',
      [ValidateSet('', 'efficiency', 'standard', 'oc', 'power')][string]$Preset = '', [int]$CpuMaxPercent = 100,
      [ValidateRange(-1, 4)][int]$CpuBoost = -1, [int]$GuardC = 0, [string]$GuardFile = 'C:\ps5-work\thermal-guard-limit.txt',
      [ValidateSet('', 'efficiency', 'standard', 'oc')][string]$SetPreset = '', [string[]]$SetSetting = @(), [double]$TempDeadbandC = 1.0)
$ErrorActionPreference = 'Stop'
foreach ($cand in @($Driver, 'C:\BC250\drivers\ps5pwr\v1.9\ps5pwr.ps1', 'C:\BC250\drivers\ps5pwr\v1.8.2\ps5pwr.ps1', 'C:\BC250\drivers\ps5pwr\v1.7\ps5pwr.ps1', 'C:\BC250\drivers\ps5pwr\v1.6\ps5pwr.ps1')) { if (Test-Path $cand) { $Driver = $cand; break } }
# v1.3.8: capability of the LOADED ps5pwr: SHA256 of the image named in the service ImagePath, and the file must not be
# newer than the last boot (swap scripts stage the new image in place; it only loads at the next boot).
$Ps5PwrKnown = @{ 'D7107A772CE9A0D69D675F687CCC128329AFB045A36D36FC5592476DE6B24017' = [version]'1.8.2'
                 '70C1D6EA47B0AFDB5C8D12AF9ACC5DB8AA8F10A18CDCDC99EE2F76CAFBC5D258' = [version]'1.9'
                 'C5D351EF3FB40BB561C06F5855F2DF031123F5FAAE4C86F2BE4AC8834C7AE772' = [version]'1.10.2'   # v1.6.2: release set v1.0-rc1 (PSWloader signing)
                 'F53A6738D3F24F4D4721AC27086621C125D794F7AEC0E11C9B0669EC96292418' = [version]'1.10.3'   # v1.6.3: BAPM init (IOCTL 0x90C), PSWloader signing
                 '1213362520048A3AD1DE12DFFB7134DAEE2BB134A543B9A4BEF0608858D09A63' = [version]'1.10'
                 '58C96659B0360D0F35CF5D603F3375C9C74600BB63D27007FC039963FE697604' = [version]'1.10.1'
                 '73F87F16BBD53D3EF325BB79EFCABFAF68C6DECCAA1B4D4BED5F877ABC765F75' = [version]'1.11'
                 '55B4281424DDF60A81695A534F669B59E831AFA4FC837A4DE7EB14DD2DACC744' = [version]'1.11.1'
                 '2424E686CBE80F5A1C3F05ACFFAD212024D19D0CBEDC0CEB679F47D47742B06B' = [version]'1.10.2'
                 'C4F91EFCA88AFE76F03CE37979EC2DA1A62C4F3A77E8A7C67D51043C23783809' = [version]'1.11.2'
                 '4816F4A9ACD8CCFB4B96764C3464A7B25CFBE00B12BB2DCF91E29AB3D2E032E3' = [version]'1.12.0'
                 'C33FEE0AEE29C6FFA54F18FA42DB43DF3BF1A5603789D8CBB05858CF2DA57AFF' = [version]'1.12.1'
                 '2B77A1CDD3E5676FB9C188FB143C4AEAA3ADBD6CAC4289B5B39990F589B3E34F' = [version]'1.14.0' }   # v1.7.0   # v1.6.14   # v1.6.13: PSWloader-signed release image
$LoadedPs5Pwr = [ordered]@{ path = $null; sha256 = $null; version = $null; staged_not_loaded = $null }
try {
  $ip = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr' -ErrorAction Stop).ImagePath
  $img = ($ip -replace '^\\SystemRoot', $env:SystemRoot) -replace '^\\\?\?\\', ''
  if ($img -notmatch '^[A-Za-z]:') { $img = Join-Path $env:SystemRoot $img }
  $fi = Get-Item -LiteralPath $img -ErrorAction Stop
  $LoadedPs5Pwr.path = $img; $LoadedPs5Pwr.sha256 = (Get-FileHash -LiteralPath $img -Algorithm SHA256).Hash.ToUpper()
  $LoadedPs5Pwr.version = $Ps5PwrKnown[$LoadedPs5Pwr.sha256]
  $LoadedPs5Pwr.staged_not_loaded = ($fi.LastWriteTime -gt (Get-CimInstance Win32_OperatingSystem).LastBootUpTime)
} catch { }
$DriverHasBoost = ($LoadedPs5Pwr.version -and $LoadedPs5Pwr.version -ge [version]'1.8.2' -and -not $LoadedPs5Pwr.staged_not_loaded)
$DriverHasBapm = ($LoadedPs5Pwr.version -in @([version]'1.10.3', [version]'1.12.0', [version]'1.12.1') -and -not $LoadedPs5Pwr.staged_not_loaded)
$DriverHasHold = ($LoadedPs5Pwr.version -eq [version]'1.12.0' -and -not $LoadedPs5Pwr.staged_not_loaded)   # v1.6.13: GFXCLK op 5   # v1.6.3: only the 1.10.3 image has IOCTL 0x90C
if (-not $BoostWindow) { $NoBoostWindow = [switch]$true }   # v1.6.8: window is opt-in
$FanMinC = if ($DriverHasBoost) { 50 } else { 60 }   # loaded v1.8.2+: 50..100, else 60..100
$DriverFanCurve = $false
# v1.5.0: ask the loaded driver itself (no ICC, works without a started device). Falls back to the hash result above.
$LoadedPs5Pwr.caps = $null; $LoadedPs5Pwr.reported = $null; $LoadedPs5Pwr.caps_source = 'hash'
try {
  if (-not ('P5.Ver' -as [type])) {
    Add-Type -Namespace P5 -Name Ver -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, int il, byte[] o, int ol, out int r, IntPtr ov);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
'@ }
  $vh = [P5.Ver]::CreateFileW('\\.\Ps5Pwr', [uint32]2147483648, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
  if ($vh -ne [IntPtr](-1)) {
    try { $vo = New-Object byte[] 20; $vr = 0
      if ([P5.Ver]::DeviceIoControl($vh, [uint32]2147640360, $null, 0, $vo, 20, [ref]$vr, [IntPtr]::Zero) -and $vr -ge 20) {
        $LoadedPs5Pwr.reported = '{0}.{1}.{2}' -f [BitConverter]::ToUInt32($vo, 4), [BitConverter]::ToUInt32($vo, 8), [BitConverter]::ToUInt32($vo, 12)
        $LoadedPs5Pwr.caps = [BitConverter]::ToUInt32($vo, 16); $LoadedPs5Pwr.caps_source = 'ioctl' }
    } finally { [void][P5.Ver]::CloseHandle($vh) } }
} catch { }
if ($LoadedPs5Pwr.caps_source -eq 'ioctl') {
  $DriverHasBoost = [bool]($LoadedPs5Pwr.caps -band 0x08)
  $FanMinC = if ($LoadedPs5Pwr.caps -band 0x10) { 50 } else { 60 }
  if (-not ($LoadedPs5Pwr.caps -band 0x20)) { $FanPollSeconds = 0 }
  $DriverFanCurve = [bool]($LoadedPs5Pwr.caps -band 0x2000)   # v1.8.0: ps5pwr 1.17+ drives the fan itself (PS5 Linux curve)
  $hv = $LoadedPs5Pwr.version; $rv = [version]$LoadedPs5Pwr.reported
  $LoadedPs5Pwr.hash_matches = ($hv -and $hv.Major -eq $rv.Major -and $hv.Minor -eq $rv.Minor -and [Math]::Max($hv.Build, 0) -eq [Math]::Max($rv.Build, 0))   # cross-check: loaded == file
  if ($rv -ge [version]'1.12.1') {   # v1.6.14: caps 0x40 BAPM, 0x80 HOLD + above-stock request (meaning fixed from 1.12.1 on)
    $DriverHasBapm = [bool]($LoadedPs5Pwr.caps -band 0x40); $DriverHasHold = [bool]($LoadedPs5Pwr.caps -band 0x80) }
} elseif ($LoadedPs5Pwr.version -and $LoadedPs5Pwr.version -ge [version]'1.12.1') {
  # v1.6.14: the file at ImagePath is 1.12.1+ but the loaded driver has no VERSION IOCTL -> an older driver still runs
  $DriverHasHold = $false; $DriverHasBapm = $false; $LoadedPs5Pwr.staged_not_loaded = $true
}
if ($LoadedPs5Pwr.version -eq [version]'1.12.0') { $DriverHasHold = $false }
$DriverHasOc = ($LoadedPs5Pwr.caps_source -eq 'ioctl' -and [bool]($LoadedPs5Pwr.caps -band 0x200))   # v1.7.0: OC ops of ps5pwr 1.14.0   # v1.6.14: 1.12.0 cannot prove it is loaded - HOLD only with 1.12.1+
$BoostPresets = if ($DriverHasBapm -and -not $NoBapm -and -not $NoBoostWindow) { @('oc', 'power', 'standard') } else { @('oc', 'power') }   # v1.6.3: presets that keep the boost window; v1.6.14: after the capability check
# --- v1.3 presets ------------------------------------------------------------------------------------------------
# oc: 2225 is the REQUEST; the SMU caps it at ~2075 MHz effective (16.11.: 2100 -> 2050, 2225 -> 2075 readback).
# OcFmaxMhz = hook for raising the SMU ceiling above 2075 (under investigation): 0 = off (default). When > 0 and the
# driver script offers -SetGfxFmax, it is called ONCE at start before the controller runs; result logged ('oc_fmax').
$PresetTable = @{
  efficiency = @{ MaxMhz = 1800; TargetC = 80; FanTargetC = 72; CpuMaxPercent = 90;  CpuBoost = 0; CpuHighC = 82; CpuLowC = 77; GuardC = 88 }   # v1.6: quiet, moderate
  standard   = @{ MaxMhz = 2230; TargetC = 83; FanTargetC = 70; CpuMaxPercent = 100; CpuBoost = 2; CpuHighC = 85; CpuLowC = 80; GuardC = 90 }   # v1.6: 'Default' = GameOS GPU clock
  oc         = @{ MaxMhz = 2230; TargetC = 86; FanTargetC = 66; CpuMaxPercent = 100; CpuBoost = 2; CpuHighC = 87; CpuLowC = 83; GuardC = 92
                  OcFmaxMhz = 0 }
}
function Get-Crc32([byte[]]$Bytes, [int]$Count) {   # CRC-32 IEEE 802.3 (zlib / gBS->CalculateCrc32)
  if (-not $script:CrcTable) { $script:CrcTable = [uint32[]]::new(256)
    for ($i = 0; $i -lt 256; $i++) { $c = [uint32]$i; for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = [uint32]([uint32]3988292384 -bxor ($c -shr 1)) } else { $c = [uint32]($c -shr 1) } }; $script:CrcTable[$i] = $c } }
  $crc = [uint32]::MaxValue
  for ($i = 0; $i -lt $Count; $i++) { $crc = [uint32]($script:CrcTable[[int](($crc -bxor [uint32]$Bytes[$i]) -band [uint32]255)] -bxor ($crc -shr 8)) }
  return [uint32]($crc -bxor [uint32]::MaxValue)
}
function ConvertFrom-Ps5Settings([byte[]]$b) {   # mirrors ps5_settings_parse(): $null unless the crc32 line matches
  $r = [ordered]@{ preset = 'standard'; lang = 'en'; fastboot = '0'; bootorder = 'internal'; pstates = '0'; refresh = 'native'; diag = ''; extra = @() }
  $known = 'preset', 'lang', 'fastboot', 'bootorder', 'pstates', 'refresh', 'diag'
  $i = 0; $n = $b.Length
  while ($i -lt $n) {
    $start = $i; while ($i -lt $n -and $b[$i] -ne 10) { $i++ }; $end = $i; if ($i -lt $n) { $i++ }
    if ($end -gt $start -and $b[$end - 1] -eq 13) { $end-- }
    $line = [Text.Encoding]::ASCII.GetString($b, $start, $end - $start)
    $eq = $line.IndexOf('='); if ($eq -lt 0 -or $line.StartsWith('#')) { continue }
    $k = $line.Substring(0, $eq); $v = $line.Substring($eq + 1).TrimEnd(' ', "`t")
    if ($k -ceq 'crc32') { if ($v -match '^[0-9A-Fa-f]{8}$' -and [Convert]::ToUInt32($v, 16) -eq (Get-Crc32 $b $start)) { return $r } else { return $null } }
    if ($k -ceq 'preset' -and $v -cin 'efficiency', 'standard', 'oc') { $r.preset = $v }
    elseif ($k -ceq 'lang' -and $v -cin 'en', 'de') { $r.lang = $v }
    elseif ($k -ceq 'fastboot' -and $v -cin '0', '1') { $r.fastboot = $v }
    elseif ($k -ceq 'bootorder' -and $v -cin 'internal', 'usb') { $r.bootorder = $v }
    elseif ($k -ceq 'pstates' -and $v -cin '0', '1') { $r.pstates = $v }
    elseif ($k -ceq 'refresh' -and $v -cin 'native', 'max') { $r.refresh = $v }
    elseif ($k -ceq 'diag' -and $v -cin 'once', 'off', 'full') { $r.diag = $v }
    elseif ($k -cnotin $known) { $r.extra += $line }   # unknown key: kept verbatim
  }
  return $null
}
function ConvertTo-Ps5Settings($s) {   # mirrors ps5_settings_format(): CRLF, upper-case hex
  $t = "# PSWloader settings v1`r`npreset=$($s.preset)`r`nlang=$($s.lang)`r`nfastboot=$($s.fastboot)`r`nbootorder=$($s.bootorder)`r`n"
  if ($s.pstates -eq '1') { $t += "pstates=1`r`n" }   # ps5-settings.h v1.1: only when 1
  if ($s.refresh -eq 'max') { $t += "refresh=max`r`n" }   # v1.2: only when max
  if ($s.diag -cin 'once', 'off', 'full') { $t += "diag=$($s.diag)`r`n" }   # v1.3: only when set
  foreach ($x in @($s.extra)) { if ($x) { $t += "$x`r`n" } }   # v1.6.9: unknown keys passed through
  $b = [Text.Encoding]::ASCII.GetBytes($t)
  return [Text.Encoding]::ASCII.GetBytes($t + ('crc32={0:X8}' -f (Get-Crc32 $b $b.Length)) + "`r`n")
}
function Get-EspRoots([switch]$BootDiskOnly) {   # \\?\Volume{..} roots of the ESPs, boot disk first
  $esp = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'; $cands = @()
  try { $d = (Get-Partition -DriveLetter C -ErrorAction Stop).DiskNumber; $cands += Get-Partition -DiskNumber $d | Where-Object { $_.GptType -eq $esp } } catch { }
  if (-not $BootDiskOnly) { try { $cands += Get-Partition -ErrorAction Stop | Where-Object { $_.GptType -eq $esp } } catch { } }
  @($cands | ForEach-Object { $_.AccessPaths | Where-Object { $_ -like '\\?\Volume*' } | Select-Object -First 1 } | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\') } | Select-Object -Unique)
}
function Read-PresetFile {   # r5.7 order: PS5UEFI.CFG > .NEW > .BAK (crc checked) > legacy PS5POWER.CFG
  $roots = Get-EspRoots
  foreach ($root in $roots) { foreach ($f in 'PS5UEFI.CFG', 'PS5UEFI.NEW', 'PS5UEFI.BAK') {
      try { $s = ConvertFrom-Ps5Settings ([IO.File]::ReadAllBytes("$root\$f")) } catch { $s = $null }
      if ($s) { return @($s.preset, "$f $root") } } }
  foreach ($root in $roots) {
    try { $t = [IO.File]::ReadAllText("$root\PS5POWER.CFG") } catch { continue }
    if ($t -match 'preset=(efficiency|standard|oc|power)\b') { return @($Matches[1].ToLower(), "PS5POWER.CFG (legacy) $root") } }
  return $null
}
$Ps5SettingValues = [ordered]@{ preset = @('efficiency', 'standard', 'oc'); lang = @('en', 'de'); fastboot = @('0', '1'); bootorder = @('internal', 'usb'); pstates = @('0', '1'); refresh = @('native', 'max'); diag = @('off', 'once', 'full') }
function Write-Ps5Settings([hashtable]$Changes) {   # same sequence as the firmware (Ps5BootWindows.c r5.7)
  foreach ($k in $Changes.Keys) { if (-not $Ps5SettingValues.Contains($k) -or $Ps5SettingValues[$k] -cnotcontains $Changes[$k]) {
      throw "invalid setting $k=$($Changes[$k]) - allowed: $(($Ps5SettingValues.Keys | ForEach-Object { "$_=" + ($Ps5SettingValues[$_] -join '|') }) -join ', ')" } }
  $roots = @(Get-EspRoots -BootDiskOnly); if ($roots.Count -ne 1) { throw "expected exactly one ESP on the boot disk, found $($roots.Count)" }
  $r = $roots[0]; $cur = $null
  foreach ($f in 'PS5UEFI.CFG', 'PS5UEFI.NEW', 'PS5UEFI.BAK') { try { $cur = ConvertFrom-Ps5Settings ([IO.File]::ReadAllBytes("$r\$f")) } catch { }; if ($cur) { break } }
  if (-not $cur) { $cur = [ordered]@{ preset = 'standard'; lang = 'en'; fastboot = '0'; bootorder = 'internal'; pstates = '0'; refresh = 'native'; diag = ''; extra = @() } }
  $diff = @($Changes.Keys | Where-Object { $cur[$_] -cne $Changes[$_] })
  if (-not $diff.Count -and ([IO.File]::Exists("$r\PS5UEFI.CFG"))) { return "unchanged ($(($Changes.Keys | ForEach-Object { "$_=$($Changes[$_])" }) -join ' '))" }
  foreach ($k in $Changes.Keys) { $cur[$k] = $Changes[$k] }
  $bytes = ConvertTo-Ps5Settings $cur
  if ([IO.File]::Exists("$r\PS5UEFI.NEW")) { [IO.File]::Delete("$r\PS5UEFI.NEW") }
  $fs = [IO.File]::Open("$r\PS5UEFI.NEW", 'CreateNew', 'Write'); $fs.Write($bytes, 0, $bytes.Length); $fs.Flush($true); $fs.Close()
  $back = ConvertFrom-Ps5Settings ([IO.File]::ReadAllBytes("$r\PS5UEFI.NEW"))
  if (-not $back -or @($Changes.Keys | Where-Object { $back[$_] -cne $Changes[$_] }).Count) { throw 'read back of PS5UEFI.NEW failed - CFG left untouched' }
  if ([IO.File]::Exists("$r\PS5UEFI.BAK")) { [IO.File]::Delete("$r\PS5UEFI.BAK") }
  if ([IO.File]::Exists("$r\PS5UEFI.CFG")) { [IO.File]::Move("$r\PS5UEFI.CFG", "$r\PS5UEFI.BAK") }
  [IO.File]::Move("$r\PS5UEFI.NEW", "$r\PS5UEFI.CFG")
  return "written preset=$($cur.preset) lang=$($cur.lang) fastboot=$($cur.fastboot) bootorder=$($cur.bootorder) pstates=$($cur.pstates) refresh=$($cur.refresh) diag=$($cur.diag) to $r\PS5UEFI.CFG"
}
function Write-PresetFile([string]$p) { Write-Ps5Settings @{ preset = $p } }
function Read-PresetVar {    # UEFI variable Ps5PowerPreset (volatile, set by 1360uefi), needs SeSystemEnvironmentPrivilege
  try {
    if (-not ('P5.Fw' -as [type])) {
      Add-Type -Namespace P5 -Name Fw -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern uint GetFirmwareEnvironmentVariableExW(string n, string g, byte[] b, uint s, out uint a);
[DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr t);
[DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool LookupPrivilegeValueW(string s, string n, out long l);
[DllImport("advapi32.dll", SetLastError=true)] public static extern bool AdjustTokenPrivileges(IntPtr t, bool d, ref TP n, uint len, IntPtr p, IntPtr r);
[DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
[StructLayout(LayoutKind.Sequential, Pack=4)] public struct TP { public uint Count; public long Luid; public uint Attr; }
'@
    }
    $tok = [IntPtr]::Zero; $luid = 0L
    [void][P5.Fw]::OpenProcessToken([P5.Fw]::GetCurrentProcess(), 0x28, [ref]$tok)
    [void][P5.Fw]::LookupPrivilegeValueW($null, 'SeSystemEnvironmentPrivilege', [ref]$luid)
    $tp = New-Object P5.Fw+TP; $tp.Count = 1; $tp.Luid = $luid; $tp.Attr = 2
    [void][P5.Fw]::AdjustTokenPrivileges($tok, $false, [ref]$tp, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    $buf = New-Object byte[] 4; $attr = 0
    $n = [P5.Fw]::GetFirmwareEnvironmentVariableExW('Ps5PowerPreset', '{5efc5ae0-b753-4f7a-a662-e34d15a6d8e5}', $buf, 4, [ref]$attr)
    if ($n -eq 4) { $v = [BitConverter]::ToUInt32($buf, 0); if ($v -le 2) { return @(@('efficiency', 'standard', 'oc')[$v], 'uefi variable') } }
  } catch { }
  return $null
}
function Get-PowerIndex([string]$alias) {   # current AC/DC index from powercfg (locale-independent: the two hex values)
  $h = @(powercfg /query scheme_current sub_processor $alias 2>$null | Select-String -Pattern '0x[0-9a-fA-F]{8}' -AllMatches |
         ForEach-Object { $_.Matches } | ForEach-Object { $_.Value })
  if ($h.Count -ge 2) { return @([Convert]::ToInt32($h[-2], 16), [Convert]::ToInt32($h[-1], 16)) } else { return $null }
}
function Write-GuardForPreset([string]$p) {   # v1.3.6: same rule as at start: max(guard, target+4, cpu_high)
  if ($p -eq 'power') { $p = 'oc' }
  $t = $PresetTable[$p]; if (-not $t) { return $null }
  $g = [Math]::Max($t.GuardC, [Math]::Max($t.TargetC + 4, $t.CpuHighC))
  try { Set-Content -LiteralPath $GuardFile -Value $g -Encoding ascii; return $g } catch { return $null }
}
function Get-BootId { try { (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o') } catch { '' } }   # v1.6.11
$boostMark = Join-Path (Split-Path -Parent $Log) 'boost-boot.txt'
function Test-BoostMark { $id = Get-BootId; [bool]($id -and (Test-Path $boostMark) -and ((Get-Content $boostMark -Raw -EA SilentlyContinue) -replace '\s', '') -eq $id) }
$ocMark = Join-Path (Split-Path -Parent $Log) 'oc-boot.txt'   # v1.7.0: OC applied in this boot (boot id; deleted when taken back)
function Test-OcMark { $id = Get-BootId; [bool]($id -and (Test-Path $ocMark) -and (((Get-Content $ocMark -Raw -EA SilentlyContinue) -split "`n")[0] -replace '\s', '') -eq $id) }
function Exit-Oc([string]$why) {   # Unforce GPU clock/voltage + CPU boost/curve back to the SMU; only if this boot applied OC
  if (-not (Test-OcMark)) { return $null }
  $r = [ordered]@{ why = $why }
  try { $r.unforce = (& $Driver -Unforce | ConvertFrom-Json).unforce } catch { $r.unforce_error = $_.Exception.Message }
  try { $r.cpu_reset = (& $Driver -CpuOcReset | ConvertFrom-Json).cpu_oc_reset } catch { $r.cpu_reset_error = $_.Exception.Message }
  Remove-Item $ocMark -EA SilentlyContinue; return $r
}
function Get-OcSetting([string]$name) {   # REG_DWORD under ps5pwr\Parameters, 0 when absent; signed (OcCpuVidScale -50 = 0xFFFFFFCE)
  try { $v = [int64](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters' -Name $name -EA Stop).$name } catch { return 0 }
  if ($v -gt [int32]::MaxValue) { $v -= 4294967296 }; if ($v -lt [int32]::MinValue) { return 0 }; return [int32]$v
}
function Exit-Boost { if (Test-BoostMark) { try { & $Driver -BoostExit | Out-Null } catch { }; Remove-Item $boostMark -EA SilentlyContinue; return $true }; return $false }
function W2($o) { $o['at'] = (Get-Date).ToString('o'); ($o | ConvertTo-Json -Compress -Depth 4) | Add-Content -LiteralPath $Log -Encoding ascii }   # v1.7.0: before W exists
if ($SetSetting.Count) {
  $ch = @{}; foreach ($kv in ($SetSetting -split ',')) { if ($kv -notmatch '^\s*(\w+)\s*=\s*(\S+)\s*$') { throw "bad -SetSetting '$kv' (key=value)" }; $ch[$Matches[1].ToLower()] = $Matches[2] }
  if ($SetPreset) { $ch['preset'] = $SetPreset }
  $m = Write-Ps5Settings $ch
  $gl = if ($ch.ContainsKey('preset')) { Write-GuardForPreset $ch['preset'] } else { $null }
  $bx = if ($ch.ContainsKey('preset') -and $ch['preset'] -notin $BoostPresets -and $DriverHasBoost) { Exit-Boost } else { $false }
  if ($ch.ContainsKey('preset') -and $ch['preset'] -ne 'oc') { $ox = Exit-Oc 'preset'; if ($ox) { W2 ([ordered]@{ event = 'oc_unforce'; reason = 'preset'; result = $ox }) } }   # v1.7.0
  ([ordered]@{ event = 'set_setting'; changes = $ch; result = $m; guard_c = $gl; boost_exit = $bx; at = (Get-Date).ToString('o') } | ConvertTo-Json -Compress) | Add-Content -LiteralPath $Log -Encoding ascii; $m; return }
if ($SetPreset) { $m = Write-PresetFile $SetPreset; $gl = Write-GuardForPreset $SetPreset
  $bx = if ($SetPreset -notin $BoostPresets -and $DriverHasBoost) { Exit-Boost } else { $false }
  if ($SetPreset -ne 'oc') { $ox = Exit-Oc 'preset'; if ($ox) { W2 ([ordered]@{ event = 'oc_unforce'; reason = 'preset'; result = $ox }) } }   # v1.7.0
  ([ordered]@{ event = 'set_preset'; preset = $SetPreset; result = $m; guard_c = $gl; boost_exit = $bx; at = (Get-Date).ToString('o') } | ConvertTo-Json -Compress) | Add-Content -LiteralPath $Log -Encoding ascii; $m; return }
$src = 'parameter'
if (-not $Preset) { $r = Read-PresetFile; if (-not $r) { $r = Read-PresetVar }; if ($r) { $Preset = $r[0]; $src = $r[1] } else { $Preset = 'standard'; $src = 'default' } }
if ($Preset -eq 'power') { $Preset = 'oc' }
$OcFmaxMhz = 0
foreach ($k in $PresetTable[$Preset].Keys) { if (-not $PSBoundParameters.ContainsKey($k)) { Set-Variable -Name $k -Value $PresetTable[$Preset][$k] } }
$ocMaxApplied = $null
if ($Preset -eq 'oc' -and $OcMaxMhz -gt 0) { $MaxMhz = [Math]::Max(500, [Math]::Min(2230, $OcMaxMhz)); $ocMaxApplied = $MaxMhz }
$fanWanted = $FanTargetC; $FanTargetC = [Math]::Max($FanMinC, [Math]::Min(100, $FanTargetC))   # driver range ($FanMinC..100)
$FanTargetPreset = $FanTargetC; $lastFanAdj = [datetime]::MinValue   # v1.6.6: quiet upper fan target of the preset
$FanHotC = $TargetC - 6; $FanCoolC = $TargetC - 14; $FanFloorC = [Math]::Max($FanMinC, 55)
$guardWanted = $GuardC; $GuardC = [Math]::Max($GuardC, [Math]::Max($TargetC + 4, $CpuHighC))
$CpuMaxPercent = [Math]::Max($CpuMinPercent, [Math]::Min(100, $CpuMaxPercent))
$origBoost = Get-PowerIndex 'PERFBOOSTMODE'
# -------------------------------------------------------------------------------------------------------------------
# v1.3.7: driver resolved right after param() (v1.8.2 > v1.7 > v1.6)
function W($o) { $o['at'] = (Get-Date).ToString('o'); ($o | ConvertTo-Json -Compress) | Add-Content -LiteralPath $Log -Encoding ascii }
# KMD GPU clock measurement (bc250kmd v10+): REG_DWORDs GfxClkMeasuredMHz / GfxClkSetPointMHz / GfxBusyPermille in
# <state>\StateV2\Generations\<Active>\Data\Ps5Modes. StateV2 is located once (Services\bc250kmd, else the GPU's Enum key),
# Active is re-read on every call, so a generation change is followed at once. Read-only.
$script:KmdStateV2 = $null; $script:KmdSearchAt = [datetime]::MinValue
function Find-KmdStateV2 {
  foreach ($root in 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd', 'HKLM:\SYSTEM\CurrentControlSet\Enum\PCI') {
    $k = Get-ChildItem -LiteralPath $root -Recurse -ErrorAction SilentlyContinue |
         Where-Object { $_.PSChildName -eq 'StateV2' -and $null -ne $_.GetValue('Active') } | Select-Object -First 1
    if ($k) { return $k.PSPath }
  }
  return $null
}
function Get-KmdGfx {
  if (-not $script:KmdStateV2) {
    if (((Get-Date) - $script:KmdSearchAt).TotalSeconds -lt 60) { return $null }
    $script:KmdSearchAt = Get-Date; $script:KmdStateV2 = Find-KmdStateV2; if (-not $script:KmdStateV2) { return $null }
  }
  try {
    $act = (Get-ItemProperty -LiteralPath $script:KmdStateV2 -Name Active -ErrorAction Stop).Active
    $m = Get-ItemProperty -LiteralPath "$($script:KmdStateV2)\Generations\$act\Data\Ps5Modes" -ErrorAction Stop
    return [ordered]@{ gfx_measured_mhz = $m.GfxClkMeasuredMHz; gfx_setpoint_mhz = $m.GfxClkSetPointMHz; gfx_busy_permille = $m.GfxBusyPermille; generation = $act }
  } catch { $script:KmdStateV2 = $null; return $null }
}
function Set-CpuMax([int]$p) {
  powercfg /setacvalueindex scheme_current sub_processor PROCTHROTTLEMAX $p | Out-Null
  powercfg /setdcvalueindex scheme_current sub_processor PROCTHROTTLEMAX $p | Out-Null
  powercfg /setactive scheme_current | Out-Null
}
function Set-Fan {   # one attempt; the main loop retries every 30 s until verified, then every 10 min
  try {
    $s = & $Driver -SetFanTargetTemp $FanTargetC | ConvertFrom-Json
    if ($s.verified -and [int]$s.target_c -eq $FanTargetC) { W ([ordered]@{ event = 'fan_set'; target_c = $FanTargetC }); return $true }
    W ([ordered]@{ event = 'fan_unverified'; target_c = $s.target_c })
  } catch { W ([ordered]@{ event = 'fan_retry'; error = $_.Exception.Message }) }
  return $false
}
function Request-Gpu([int]$mhz) {
  try { $r = & $Driver -SetGfxClk $mhz | ConvertFrom-Json; return $r }
  catch { W ([ordered]@{ event = 'gpu_error'; mhz = $mhz; error = $_.Exception.Message }); return $null }
}
$gpu = $false; $gpuSet = 0; $limit = [double]$MaxMhz
try { $q = & $Driver -GfxClk | ConvertFrom-Json; if ($q.status -eq 0) { $gpu = $true; $gpuSet = [int]$q.mhz } } catch { }
if ($gpu -and $gpuSet -gt 0) { $limit = [double][Math]::Min($gpuSet, $MaxMhz) }
$cpu = $CpuMaxPercent; Set-CpuMax $cpu
if ($CpuBoost -ge 0) { powercfg /setacvalueindex scheme_current sub_processor PERFBOOSTMODE $CpuBoost | Out-Null
  powercfg /setdcvalueindex scheme_current sub_processor PERFBOOSTMODE $CpuBoost | Out-Null; powercfg /setactive scheme_current | Out-Null }
try { Set-Content -LiteralPath $GuardFile -Value $GuardC -Encoding ascii } catch { }
W ([ordered]@{ event = 'preset'; driver = $Driver; ps5pwr = $LoadedPs5Pwr; fan_min_c = $FanMinC; oc_max_mhz = $ocMaxApplied; preset = $Preset; source = $src; max_mhz = $MaxMhz; target_c = $TargetC; fan_c = $FanTargetC
  cpu_max = $CpuMaxPercent; cpu_boost = $CpuBoost; cpu_high_c = $CpuHighC; cpu_low_c = $CpuLowC; guard_c = $GuardC
  guard_raised_from = $(if ($guardWanted -ne $GuardC) { $guardWanted } else { $null }); boost_before = $origBoost; oc_fmax_mhz = $OcFmaxMhz
  fan_clamped_from = $(if ($fanWanted -ne $FanTargetC) { $fanWanted } else { $null }) })
$boostOn = $false
$bapmBoost = ($DriverHasBapm -and -not $NoBapm -and $Preset -in 'standard', 'oc')   # v1.6.3
W ([ordered]@{ event = 'bapm_plan'; preset = $Preset; driver_has_bapm = $DriverHasBapm; no_bapm = [bool]$NoBapm; no_boost_window = [bool]$NoBoostWindow })   # v1.6.5
$bapmMark = Join-Path (Split-Path -Parent $Log) 'bapm-boot.txt'   # v1.6.10
$bootId = Get-BootId
$bapmWanted = $bapmBoost   # v1.6.11: the window follows the preset's plan, not the skip
if ($bapmBoost -and $bootId -and (Test-Path $bapmMark) -and ((Get-Content $bapmMark -Raw -EA SilentlyContinue) -replace '\s', '') -eq $bootId) {
  W ([ordered]@{ event = 'bapm_skip'; reason = 'already initialised in this boot'; boot = $bootId }); $bapmBoost = $false
}
if ($bapmBoost) {   # v1.6.3: BAPM weights exactly as PS5 Linux mp1_init, once per boot (v1.6.10), before the boost window
  try { $ba = & $Driver -BapmInit | ConvertFrom-Json; W ([ordered]@{ event = $(if ($ba.bapm.status -eq 0) { 'bapm_init' } else { 'bapm_init_failed' }); result = $ba })
        if ($ba.bapm.status -eq 0 -and $bootId) { Set-Content -Path $bapmMark -Value $bootId -Encoding ASCII } }
  catch { W ([ordered]@{ event = 'bapm_init_error'; error = $_.Exception.Message }) }
}
$holdWanted = ($Preset -eq 'oc' -and $Hold -and $DriverHasHold -and -not $NoHold)   # v1.6.13; v1.6.15: opt-in -Hold
if ($holdWanted -and (Test-BoostMark)) {
  W ([ordered]@{ event = 'oc_hold_skip'; reason = 'boost window already opened in this boot' }); $holdWanted = $false; $boostOn = $true
} elseif ($holdWanted -or ($Preset -eq 'oc' -and $OcBoost) -or ($bapmWanted -and -not $NoBoostWindow)) {   # v1.6.5: -NoBoostWindow = BAPM only
   # v1.3.8: every start (the SMU forgets the window on reboot); v1.6.3: also standard; v1.6.13: oc = HOLD once per boot
  try {
    $b = if ($holdWanted) { & $Driver -BoostEnterHold | ConvertFrom-Json } else { & $Driver -BoostEnter | ConvertFrom-Json }
    $boostOn = ($b.result.status -eq 0 -and $b.result.resp -eq 1)
    if ($boostOn -and $bootId) { Set-Content -Path $boostMark -Value $bootId -Encoding ASCII }   # v1.6.11
    W ([ordered]@{ event = $(if ($boostOn) { $(if ($holdWanted) { 'oc_hold_enter' } else { 'oc_boost_enter' }) } else { 'oc_boost_refused' }); result = $b })
    if ($boostOn -and $FanMinC -gt 50) {
      # BoostEnter is supported by v1.8/v1.8.1 too; it does not prove FAN_SET accepts 50 C.
      # Keep the 60 C clamp unless the loaded image hash above identifies v1.8.2.
      W ([ordered]@{ event = 'fan_range_unproven'; fan_min_c = $FanMinC; fan_c = $FanTargetC; ps5pwr = $LoadedPs5Pwr })
    }
  } catch { W ([ordered]@{ event = 'oc_boost_error'; error = $_.Exception.Message }) }
}
if ($OcFmaxMhz -gt 0) {   # optional OC step (off by default); only if the driver script has the switch
  if ((Get-Command $Driver).Parameters.ContainsKey('SetGfxFmax')) {
    try { $o = & $Driver -SetGfxFmax $OcFmaxMhz | ConvertFrom-Json; W ([ordered]@{ event = 'oc_fmax'; mhz = $OcFmaxMhz; result = $o }) }
    catch { W ([ordered]@{ event = 'oc_fmax_error'; mhz = $OcFmaxMhz; error = $_.Exception.Message }) }
  } else { W ([ordered]@{ event = 'oc_fmax_unsupported'; mhz = $OcFmaxMhz; driver = $Driver }) }
}
W ([ordered]@{ event = 'start'; driver = $Driver; gpu_control = $gpu; gpu_now_mhz = $gpuSet; target_c = $TargetC; max_mhz = $MaxMhz; fan_c = $FanTargetC ; ps5pwr = $LoadedPs5Pwr; driver_has_hold = $DriverHasHold; driver_has_bapm = $DriverHasBapm })   # v1.6.14
if ($LoadedPs5Pwr.staged_not_loaded -or ($LoadedPs5Pwr.caps_source -eq 'ioctl' -and -not $LoadedPs5Pwr.hash_matches)) { W ([ordered]@{ event = 'driver_mismatch'; ps5pwr = $LoadedPs5Pwr; note = 'file at ImagePath and loaded driver differ - restart (cold start) after a driver update' }) }
# v1.7.0: real OC (only oc preset, only ps5pwr 1.14.0 + CLI v1.14, only with registry values)
$OcGfxMhz = Get-OcSetting 'OcGfxMhz'; $OcGfxMv = Get-OcSetting 'OcGfxMv'; $OcCpuBoostMhz = Get-OcSetting 'OcCpuBoostMhz'; $OcCpuVidScale = Get-OcSetting 'OcCpuVidScale'
$ocCli = (Get-Command $Driver).Parameters.ContainsKey('ForceGfxFreq')
$ocWanted = ($Preset -eq 'oc' -and ($OcGfxMhz -gt 0 -or $OcCpuBoostMhz -gt 0 -or $OcCpuVidScale -lt 0))
W ([ordered]@{ event = 'oc_plan'; wanted = $ocWanted; driver_has_oc = $DriverHasOc; cli_has_oc = $ocCli; gfx_mhz = $OcGfxMhz; gfx_mv = $OcGfxMv; cpu_boost_mhz = $OcCpuBoostMhz; cpu_vid_scale = $OcCpuVidScale })
$ocGpu = $false; $ocCur = 0; $ocDead = $false; $lastOcUp = [datetime]::MinValue; $ocHanded = $null
if ($ocWanted -and $DriverHasOc -and $ocCli) {
  if ($OcGfxMhz -gt 0 -and ($OcGfxMhz -lt 2231 -or $OcGfxMhz -gt 2600)) { W ([ordered]@{ event = 'oc_refused'; reason = 'OcGfxMhz must be 2231..2600'; gfx_mhz = $OcGfxMhz }); $OcGfxMhz = 0 }
  if ($OcGfxMv -ne 0 -and ($OcGfxMv -lt 700 -or $OcGfxMv -gt 1129)) { W ([ordered]@{ event = 'oc_refused'; reason = 'OcGfxMv must be 700..1129'; gfx_mv = $OcGfxMv }); $OcGfxMv = 0 }
  $ocKey = "$OcGfxMhz/$OcGfxMv/$OcCpuBoostMhz/$OcCpuVidScale"
  if ((Test-OcMark) -and ((Get-Content $ocMark -EA SilentlyContinue | Select-Object -Skip 1 -First 1) -eq $ocKey)) {
    W ([ordered]@{ event = 'oc_skip'; reason = 'applied in this boot'; key = $ocKey }); $ocGpu = ($OcGfxMhz -gt 0); $ocCur = $OcGfxMhz
  } else {
    $res = [ordered]@{}
    try {
      if ($OcGfxMhz -gt 0) {
        $a = @{ ForceGfxFreq = $OcGfxMhz }; if ($OcGfxMv -gt 0) { $a.ForceGfxVid = $OcGfxMv }   # the CLI sends the voltage first, then the clock
        $res.gpu = & $Driver @a | ConvertFrom-Json
        $ocGpu = ($res.gpu.force_gfx_freq.status -eq 0 -and ($OcGfxMv -eq 0 -or $res.gpu.force_gfx_vid.status -eq 0)); if ($ocGpu) { $ocCur = $OcGfxMhz }
      }
      if ($OcCpuBoostMhz -gt 0 -or $OcCpuVidScale -lt 0) {
        $a = @{}; if ($OcCpuVidScale -lt 0) { $a.CpuVidScale = $OcCpuVidScale }; if ($OcCpuBoostMhz -gt 0) { $a.SetCpuBoostMax = $OcCpuBoostMhz }
        $res.cpu = & $Driver @a | ConvertFrom-Json
      }
    } catch { $res.error = $_.Exception.Message }
    $bootIdOc = Get-BootId
    if ($bootIdOc) { Set-Content -Path $ocMark -Value @($bootIdOc, $ocKey) -Encoding ASCII }   # set even after a partial result: the way back runs
    W ([ordered]@{ event = 'oc_force'; gpu_forced = $ocGpu; mhz = $ocCur; mv = $OcGfxMv; result = $res })
  }
} elseif ($ocWanted) { W ([ordered]@{ event = 'oc_unavailable'; driver_has_oc = $DriverHasOc; cli_has_oc = $ocCli; driver = $Driver }) }
if ($DriverFanCurve) { $fanOk = $false; W ([ordered]@{ event = 'fan_driver_curve'; ps5pwr = $LoadedPs5Pwr }) }   # v1.8.0: no fan target writes
else { $fanOk = Set-Fan }
$lastFan = Get-Date; $lastSample = Get-Date
$fanPoll = ($FanPollSeconds -gt 0); $lastFanPoll = [datetime]::MinValue; $fanErr = 0
$lastGfxMeas = [datetime]::MinValue   # v1.6.5
$hist = New-Object System.Collections.Generic.List[double]
try {
while ($true) {
  try { $raw = [double](& $Driver -Thermal | ConvertFrom-Json).celsius } catch { W ([ordered]@{ event = 'tctl_error'; error = $_.Exception.Message }); $hist.Clear(); Start-Sleep -Seconds 10; continue }   # stale samples out (review)
  $hist.Add($raw); if ($hist.Count -gt 3) { $hist.RemoveAt(0) }
  $sorted = @($hist | Sort-Object); $t = if ($sorted.Count -eq 2) { ($sorted[0] + $sorted[1]) / 2 } else { $sorted[[int][Math]::Floor($sorted.Count / 2)] }   # median of 3 (warm-up: 1 value / mean of 2)
  if ($ocGpu) {   # v1.7.0: forced clock instead of the RequestGfxclk regulator
    if ($t -ge $GuardC - 3) {
      $ox = Exit-Oc 'guard'; $ocGpu = $false; $ocDead = $true; $ocHanded = $null
      W ([ordered]@{ event = 'oc_unforce'; reason = 'guard'; tctl_c = $t; guard_c = $GuardC; mhz = $ocCur; result = $ox })
    } elseif ($t -ge $TargetC) {
      if ($ocCur -gt 2230) {
        $to = [Math]::Max(2230, $ocCur - 25); $r = try { & $Driver -ForceGfxFreq $to | ConvertFrom-Json } catch { $null }
        W ([ordered]@{ event = 'oc_back'; tctl_c = $t; from = $ocCur; to = $to; status = $r.force_gfx_freq.status; readback = $r.gfxclk_after.mhz }); if ($r -and $r.force_gfx_freq.status -eq 0) { $ocCur = $to }
      } else {
        $r = try { & $Driver -Unforce | ConvertFrom-Json } catch { $null }; $ocGpu = $false; $ocHanded = Get-Date
        W ([ordered]@{ event = 'oc_unforce'; reason = 'target'; tctl_c = $t; mhz = $ocCur; status = $r.unforce.status })
      }
    } elseif ($t -le $TargetC - 4 -and $ocCur -lt $OcGfxMhz -and ((Get-Date) - $lastOcUp).TotalSeconds -ge 10) {
      $to = [Math]::Min($OcGfxMhz, $ocCur + 25); $r = try { & $Driver -ForceGfxFreq $to | ConvertFrom-Json } catch { $null }; $lastOcUp = Get-Date
      W ([ordered]@{ event = 'oc_up'; tctl_c = $t; from = $ocCur; to = $to; status = $r.force_gfx_freq.status }); if ($r -and $r.force_gfx_freq.status -eq 0) { $ocCur = $to }
    }
  } elseif ($ocHanded -and -not $ocDead -and $t -le $TargetC - 4 -and ((Get-Date) - $ocHanded).TotalSeconds -ge 30) {
    # handed back at the target, cool again for 30 s: force 2230 and climb with oc_up
    $r = try { & $Driver -ForceGfxFreq 2230 | ConvertFrom-Json } catch { $null }
    if ($r -and $r.force_gfx_freq.status -eq 0) { $ocGpu = $true; $ocCur = 2230; $ocHanded = $null; $lastOcUp = Get-Date }
    else { $ocHanded = Get-Date }
    W ([ordered]@{ event = 'oc_force'; reason = 'resume'; tctl_c = $t; mhz = 2230; status = $r.force_gfx_freq.status })
  } elseif (-not $ocDead -and (Test-OcMark) -and $t -ge $GuardC - 3) {   # CPU-only OC (or GPU handed back): the guard rule still holds
    $ox = Exit-Oc 'guard'; $ocDead = $true; W ([ordered]@{ event = 'oc_unforce'; reason = 'guard'; tctl_c = $t; guard_c = $GuardC; result = $ox })
  }
  if ($gpu -and -not $ocGpu) {
    $err = $TargetC - $t; if ([Math]::Abs($err) -le $TempDeadbandC) { $err = 0 }
    $limit = [Math]::Max($MinMhz, [Math]::Min($MaxMhz, $limit + $err * $GainMhzPerC))
    $want = [int]([Math]::Floor($limit / 25) * 25)
    if ([Math]::Abs($want - $gpuSet) -ge $DeadbandMhz -or ($want -eq $MaxMhz -and $gpuSet -ne $MaxMhz)) {
      $r = Request-Gpu $want
      if ($r -and $r.request.status -eq 0) { W ([ordered]@{ event = 'gpu_set'; tctl_c = $t; tctl_raw_c = $raw; from = $gpuSet; to = $want; readback = $r.readback.mhz }); $gpuSet = $want }
      elseif ($r) { W ([ordered]@{ event = 'gpu_refused'; tctl_c = $t; mhz = $want; status = $r.request.status; resp = $r.request.resp; poisoned = $r.request.poisoned }); if ($r.request.poisoned) { $gpu = $false } }
    }
  }
  $newCpu = $cpu
  if ($t -ge $CpuHighC -and $cpu -gt $CpuMinPercent) { $newCpu = [Math]::Max($CpuMinPercent, $cpu - 10) }
  elseif ($t -le $CpuLowC -and $cpu -lt $CpuMaxPercent) { $newCpu = [Math]::Min($CpuMaxPercent, $cpu + 10) }
  if ($newCpu -ne $cpu) { Set-CpuMax $newCpu; W ([ordered]@{ event = 'cpu_max'; tctl_c = $t; from = $cpu; to = $newCpu }); $cpu = $newCpu }
  # v1.6.7 proportional fan on raw Tctl (v1.6.6 was too slow: Tctl rose 70 -> 97 C in 5 s at the start of Time Spy while the
  # EMC servo still saw 72 C). Every poll: wanted EMC target = preset - 1.5 * (Tctl_raw - (TargetC - 12)), clamped to
  # [55 .. preset]. Going down (more fan) is applied at once; going up (quieter) by 1 C per 6 s at most.
  if ($fanOk) {
    $tFan = [Math]::Max($raw, $t)
    $ideal = [int][Math]::Round($FanTargetPreset - 1.5 * [Math]::Max(0, $tFan - ($TargetC - 12)))
    $ideal = [Math]::Max($FanFloorC, [Math]::Min($FanTargetPreset, $ideal))
    $fanWant = $FanTargetC
    if ($ideal -lt $FanTargetC) { $fanWant = $ideal }
    elseif ($ideal -gt $FanTargetC -and ((Get-Date) - $lastFanAdj).TotalSeconds -ge 6) { $fanWant = $FanTargetC + 1 }
    if ($fanWant -ne $FanTargetC) {
      $from = $FanTargetC; $FanTargetC = $fanWant; $fanOk = Set-Fan; $lastFan = Get-Date; $lastFanAdj = Get-Date
      W ([ordered]@{ event = 'fan_adjust'; tctl_c = $t; tctl_raw_c = $raw; from = $from; to = $FanTargetC; ideal = $ideal; preset_c = $FanTargetPreset })
    }
  }
  if ($fanPoll -and ((Get-Date) - $lastFanPoll).TotalSeconds -ge $FanPollSeconds) {   # v1.4.0 fan telemetry
    $lastFanPoll = Get-Date
    try {
      $fs = & $Driver -Fan | ConvertFrom-Json
      if ($null -ne $fs.duty_percent) {
        W ([ordered]@{ event = 'fan_status'; duty_percent = $fs.duty_percent; zone0_mainsoc = $fs.temps_c.zone0_mainsoc; zone1 = $fs.temps_c.zone1
          zone2 = $fs.temps_c.zone2; zone3 = $fs.temps_c.zone3; sensor18 = $fs.temps_c.sensor18; target_c = $fs.target_c; tctl_c = $t })
      } else { W ([ordered]@{ event = 'fan_status_unsupported'; error = $fs.fan_status_error; driver = $Driver }); $fanPoll = $false }
    } catch { W ([ordered]@{ event = 'fan_status_error'; error = $_.Exception.Message }); $fanErr++; if ($fanErr -ge 5) { $fanPoll = $false } }
  }
  if (((Get-Date) - $lastSample).TotalSeconds -ge 60) { W ([ordered]@{ event = 'sample'; tctl_c = $t; tctl_raw_c = $raw; gpu_mhz = $gpuSet; cpu_max = $cpu; oc_mhz = $(if ($ocGpu) { $ocCur } else { $null }) }); $lastSample = Get-Date }
  if (-not $DriverFanCurve -and ((Get-Date) - $lastFan).TotalSeconds -ge ($(if ($fanOk) { 600 } else { 30 }))) { $fanOk = Set-Fan; $lastFan = Get-Date }
  if ($GfxMeasuredSeconds -gt 0 -and ((Get-Date) - $lastGfxMeas).TotalSeconds -ge $GfxMeasuredSeconds) {   # v1.6.5
    $lastGfxMeas = Get-Date; $g = Get-KmdGfx
    if ($g) { $g['event'] = 'gfx_measured'; $g['tctl_c'] = $t; W $g }
  }
  Start-Sleep -Seconds $PollSeconds
}
} finally {
  Set-CpuMax 100
  if ($boostOn) { try { & $Driver -BoostExit | Out-Null; Remove-Item $boostMark -EA SilentlyContinue; W ([ordered]@{ event = 'oc_boost_exit' }) } catch { } }
  $ox = Exit-Oc 'stop'; if ($ox) { W ([ordered]@{ event = 'oc_unforce'; reason = 'stop'; result = $ox }) }   # v1.7.0
  if ($CpuBoost -ge 0 -and $origBoost) { powercfg /setacvalueindex scheme_current sub_processor PERFBOOSTMODE $origBoost[0] | Out-Null
    powercfg /setdcvalueindex scheme_current sub_processor PERFBOOSTMODE $origBoost[1] | Out-Null; powercfg /setactive scheme_current | Out-Null }
  W ([ordered]@{ event = 'stop'; cpu_max = 100; gpu_mhz = $gpuSet; boost_restored = $origBoost })
}
