# ps5pwr.ps1 v1.15 (v1.15: -BapmGfxPct <85..100> = BAPM init with the GFX CAC weights scaled (driver op 3, caps 0x800; 100 = stock table); -BoostWindow <gpuMax 2230..2400> <cpuMax 3500..3800> = boost window with a raised ceiling (driver v1.15.0 op 21, caps 0x400), leave with -BoostExit; v1.14: v1.14: OC ops of driver v1.14.0 (caps 0x200): -GfxVid, -SmuFeatures, -ForceGfxFreq <500..2600 MHz>,
#   -ForceGfxVid <700..1129 mV>, -Unforce (clock + voltage back to the SMU); CPU (queue 3): -CpuVoltage, -CoreFreq <0..7>,
#   -SetCpuBoostMax <3200..4000 MHz>, -CpuVidScale <-50..0>, -SetTempLimits <cpu> <gpu> (70..95 C, only lowers), -CpuOcReset
#   (boost 3500, scale 0); every write op is followed by the matching readback; v1.13: -SetHotplug 4 = replay + pick the new sink's mode; -HotplugPick shows the pick (IOCTL 0x90D);
#   -HotplugProgram <mode id> programs the FLAVA3 bridge for that mode (IOCTL 0x90E, test only: the DCN keeps its mode); v1.12.1: -Version = version + capabilities of the LOADED driver (IOCTL 0x90A, v1.12.1+; older drivers ->
#   'older than 1.12.1'); -BoostEnterHold / -AllowAboveStock on a driver without them -> clear message instead of 'ioctl failed 87'; v1.12: -BoostEnterHold = boost window HOLD (GPU 2230 held, CPU up to 3500; driver v1.12.0 op 5);
#   -SetGfxClk 2231..2400 only together with -AllowAboveStock (op 6, request + immediate QueryGfxclk/GetGfxFrequency readback))
# ps5pwr.ps1 v1.10 - control the ps5pwr driver (admin). -Status: print state. -Off / -Reboot: ICC power now.
# -OffAfterUnmount: wait until the ESP (S:) is unmounted by the trial hook, log the status, then ICC power off.
# v1.4: -Hotplug: hotplug state, counters, last EDID event and the driver's log ring (oldest first).
#       -SetHotplug <0..3>: HotplugMode (0 off, 1 log only, 2 replay boot list, 3 replay short list); the driver
#       re-reads it within 1 s, no reboot and no device restart (NEVER restart/remove 104D:9107).
# v1.5: -Thermal reads PS5 Tctl; -Fan reads the MainSoC fan target; -SetFanTargetTemp 60..100 writes only that
#       target via ICC and verifies readback. -ThermalWatchdog logs Tctl and stops 3DMark at the guard limit.
# v1.6: Fan response size is reported; ICC timeouts and short buffers have distinct Win32 errors.
# v1.10: -Button: power-button state (IOCTL 0x909). -SetPowerButton 0|1: Parameters\EnablePowerButton kill switch
#        (default 1); the driver re-reads it within 1 s and switches the ICC button notifications off/on.
param([switch]$Status, [switch]$Off, [switch]$Reboot, [switch]$OffAfterUnmount, [string]$Log = "$PSScriptRoot\last-status.txt",
      [switch]$Hotplug, [ValidateRange(0,4)][int]$SetHotplug = -1, [switch]$Thermal, [switch]$Fan,
      [int]$SetFanTargetTemp = -1, [switch]$ThermalWatchdog, [int]$ThermalLimitC = 85,
      [string]$ThermalLog = "$PSScriptRoot\thermal-watchdog.jsonl",
      [switch]$GfxClk, [int]$SetGfxClk = -1, [switch]$Version, [switch]$HotplugPick, [int]$HotplugProgram = -1, [string]$BridgeRead = '', [switch]$BoostEnter, [switch]$BoostEnterHold, [switch]$AllowAboveStock, [switch]$BoostExit, [switch]$GfxFreq,
      [switch]$Button, [ValidateRange(0,1)][int]$SetPowerButton = -1, [switch]$BapmInit, [switch]$WcCpu,
      [switch]$GfxVid, [switch]$SmuFeatures, [int]$ForceGfxFreq = -1, [int]$ForceGfxVid = -1, [switch]$Unforce,
      [switch]$CpuVoltage, [int]$CoreFreq = -1, [int]$SetCpuBoostMax = -1, [int]$CpuVidScale = 1, [int[]]$SetTempLimits, [switch]$CpuOcReset, [int[]]$BoostWindow, [int]$BapmGfxPct = 0)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Ps5Pwr {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, int il, byte[] o, int ol, out int ret, IntPtr ov);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public const uint STATUS = 0x80026400;   // CTL_CODE(0x8002, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint POWER  = 0x8002a404;   // CTL_CODE(0x8002, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)
  public const uint HOTPLUG = 0x8002640c;  // CTL_CODE(0x8002, 0x903, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint FAN_GET = 0x80026410;  // CTL_CODE(0x8002, 0x904, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint BUTTON = 0x80026424;   // v1.10: CTL_CODE(0x8002, 0x909, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint FAN_STATUS = 0x80026420;  // v1.9: CTL_CODE(0x8002, 0x908, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint FAN_SET = 0x8002a414;  // CTL_CODE(0x8002, 0x905, METHOD_BUFFERED, FILE_WRITE_ACCESS)
  public const uint TCTL = 0x80026418;     // CTL_CODE(0x8002, 0x906, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint GFXCLK = 0x8002a41c;   // CTL_CODE(0x8002, 0x907, METHOD_BUFFERED, FILE_WRITE_ACCESS) v1.7
  public const uint BAPM = 0x8002a430;
  public const uint VERSION = 0x80026428;
  public const uint HP_PICK = 0x80026434;     // v1.13: CTL_CODE(0x8002, 0x90D, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint BRIDGE_READ = 0x8002643c; // v1.18.3: CTL_CODE(0x8002, 0x90F, METHOD_BUFFERED, FILE_READ_ACCESS)
  public const uint HP_PROGRAM = 0x8002a438;  // v1.13: CTL_CODE(0x8002, 0x90E, METHOD_BUFFERED, FILE_WRITE_ACCESS)  // v1.12.1: CTL_CODE(0x8002, 0x90A, METHOD_BUFFERED, FILE_READ_ACCESS)     // v1.10.3: CTL_CODE(0x8002, 0x90C, METHOD_BUFFERED, FILE_WRITE_ACCESS)
  public static byte[] Call(uint code, byte[] input, int outLen) {
    IntPtr h = CreateFileW("\\\\.\\Ps5Pwr", 0xC0000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (h == new IntPtr(-1)) throw new Exception("open failed " + Marshal.GetLastWin32Error());
    try { byte[] o = new byte[Math.Max(outLen, 4)]; int r;
      if (!DeviceIoControl(h, code, input, input == null ? 0 : input.Length, o, o.Length, out r, IntPtr.Zero))
        throw new Exception("ioctl failed " + Marshal.GetLastWin32Error());
      if (r < outLen) throw new Exception("short ioctl result " + r + " < " + outLen);
      return o; } finally { CloseHandle(h); }
  }
}
"@
function Get-Ps5PwrStatus {
  $o = [Ps5Pwr]::Call([Ps5Pwr]::STATUS, $null, 40)
  [ordered]@{ led_result = [BitConverter]::ToInt32($o, 0); s5_seen = [BitConverter]::ToInt32($o, 4);
              shutdown_type = [BitConverter]::ToInt32($o, 8); power_result = [BitConverter]::ToInt32($o, 12);
              started = [BitConverter]::ToInt32($o, 16); sow = [BitConverter]::ToUInt32($o, 20); sor = [BitConverter]::ToUInt32($o, 24);
              mailbox = '0x{0:x}' -f [BitConverter]::ToUInt32($o, 28); doorbell = '0x{0:x}' -f [BitConverter]::ToUInt32($o, 32);
              drained = [BitConverter]::ToUInt32($o, 36); at = (Get-Date).ToString('o') }
}
function Get-Ps5Tctl {
  # v1.8.1: one retry after 50 ms before an error is reported (a single transient failure must not trip a fail-closed guard)
  try { $o = [Ps5Pwr]::Call([Ps5Pwr]::TCTL, $null, 8) } catch { Start-Sleep -Milliseconds 50; $o = [Ps5Pwr]::Call([Ps5Pwr]::TCTL, $null, 8) }
  $raw = [BitConverter]::ToUInt32($o, 0); $mc = [BitConverter]::ToInt32($o, 4)
  if ($raw -eq 0 -or $mc -lt 0 -or $mc -gt 150000) { throw ('Tctl sensor data implausible: raw=0x{0:x8}, milliC={1}' -f $raw, $mc) }
  [ordered]@{ raw = ('0x{0:x8}' -f $raw); milli_celsius = $mc; celsius = [Math]::Round($mc / 1000.0, 1); at = (Get-Date).ToString('o') }
}
function Get-Ps5FanState {
  $o = [Ps5Pwr]::Call([Ps5Pwr]::FAN_GET, $null, 32)
  $cs = for ($i = 0; $i -lt 6; $i++) { [BitConverter]::ToUInt32($o, 8 + 4 * $i) }
  [ordered]@{ target_c = [BitConverter]::ToUInt32($o, 0); reply_bytes = [BitConverter]::ToUInt32($o, 4); curset = @($cs); at = (Get-Date).ToString('o') }
}
if ($Thermal) { Get-Ps5Tctl | ConvertTo-Json; return }
# v1.7: GPU clock through the PS5 SMU. -GfxClk reads (QueryGfxclk), -SetGfxClk 500..2230 requests (RequestGfxclk).
function Invoke-Ps5GfxClk([uint32]$op, [uint32]$mhz) {
  $in = [byte[]]::new(8); [BitConverter]::GetBytes($op).CopyTo($in, 0); [BitConverter]::GetBytes($mhz).CopyTo($in, 4)
  $o = [Ps5Pwr]::Call([Ps5Pwr]::GFXCLK, $in, 24)
  [ordered]@{ status = [BitConverter]::ToInt32($o, 0); resp = [BitConverter]::ToUInt32($o, 4); mhz = [BitConverter]::ToUInt32($o, 8);
              wait_before = [BitConverter]::ToUInt32($o, 12); wait_after = [BitConverter]::ToUInt32($o, 16); poisoned = [BitConverter]::ToUInt32($o, 20); at = (Get-Date).ToString('o') }
}
if ($GfxClk) { Invoke-Ps5GfxClk 0 0 | ConvertTo-Json; return }
function Get-Ps5PwrVersion {   # v1.12.1
  try { $o = [Ps5Pwr]::Call([Ps5Pwr]::VERSION, $null, 20) }
  catch { return [ordered]@{ loaded = 'older than 1.12.1 (no VERSION IOCTL)'; caps = $null; error = $_.Exception.Message } }
  $c = [BitConverter]::ToUInt32($o, 16)
  [ordered]@{ loaded = ('{0}.{1}.{2}' -f [BitConverter]::ToUInt32($o, 4), [BitConverter]::ToUInt32($o, 8), [BitConverter]::ToUInt32($o, 12)); caps = ('0x{0:x2}' -f $c)
              boost = [bool]($c -band 0x08); bapm = [bool]($c -band 0x40); hold_and_above_stock = [bool]($c -band 0x80) }
}
if ($Version) { Get-Ps5PwrVersion | ConvertTo-Json; return }
# v1.13: struct ps5_hp_pick = seq, mode_id, offered_mask, edid_ok, refresh_hz (5x u32), ps5_timing (clock_khz u32, 8x u16,
# hpos/vpos u8 + 2 pad = 24 bytes), edid_bytes u32, edid[512] -> 560 bytes (C_ASSERT in ps5pwr.c)
$Ps5ModeNames = @('1080p60', '1080p120', '1440p60', '2160p60', '3440p60', '3440p100', 'ugreen1440p60', '2560x1440p120', '720p144')
if ($HotplugPick) {
  $o = [Ps5Pwr]::Call([Ps5Pwr]::HP_PICK, $null, 560)
  $u = { param($k) [BitConverter]::ToUInt32($o, $k) }; $w = { param($k) [BitConverter]::ToUInt16($o, $k) }
  $id = & $u 4; $n = & $u 44
  [ordered]@{ seq = & $u 0; mode_id = $id; mode = $(if ($id -lt $Ps5ModeNames.Count) { $Ps5ModeNames[$id] } else { '?' }); offered_mask = ('0x{0:x3}' -f (& $u 8))
              edid_ok = & $u 12; refresh_hz = & $u 16; clock_khz = & $u 20; width = & $w 24; height = & $w 32; edid_bytes = $n
              edid_hex = (($o[48..(47 + [Math]::Min($n, 512))] | ForEach-Object { '{0:x2}' -f $_ }) -join '') } | ConvertTo-Json; return
}
if ($BridgeRead) {   # v1.18.3: -BridgeRead "7072:1,7206:1" - bridge registers (hex address:count), read only
  foreach ($item in $BridgeRead -split ',') { $a, $c = $item.Trim() -split ':'; $reg = [Convert]::ToUInt16($a, 16); $cnt = if ($c) { [int]$c } else { 1 }
    $in = [byte[]]@(($reg -band 0xff), ($reg -shr 8), $cnt, 0)
    $o = [Ps5Pwr]::Call([Ps5Pwr]::BRIDGE_READ, $in, 20)
    '{0:x4}: result {1} data {2}' -f $reg, [BitConverter]::ToInt32($o, 0), (($o[4..(3 + $cnt)] | ForEach-Object { '{0:x2}' -f $_ }) -join ' ') }
  return
}
if ($HotplugProgram -ge 0) {
  $o = [Ps5Pwr]::Call([Ps5Pwr]::HP_PROGRAM, [BitConverter]::GetBytes([uint32]$HotplugProgram), 144)
  $cnt = [BitConverter]::ToUInt32($o, 4)
  [ordered]@{ result = [BitConverter]::ToInt32($o, 0); count = $cnt; last_stage = [BitConverter]::ToUInt32($o, 8); mode_id = [BitConverter]::ToUInt32($o, 12)
              stages = @(for ($i = 0; $i -lt $cnt; $i++) { [BitConverter]::ToUInt32($o, 16 + 4 * $i) })
              results = @(for ($i = 0; $i -lt $cnt; $i++) { [BitConverter]::ToInt32($o, 80 + 4 * $i) }) } | ConvertTo-Json; return
}
function Assert-Ps5Hold {   # v1.12.1: ops 5/6 need the loaded driver to have them
  $v = Get-Ps5PwrVersion
  if (-not $v.hold_and_above_stock) { throw "the loaded ps5pwr driver has no boost HOLD / above-stock request (loaded: $($v.loaded)) - install ps5pwr 1.12.1+ and restart" }
}
# v1.10.3: BAPM init exactly as PS5 Linux mp1_init (-BapmInit), WC CPU mode read only (-WcCpu). Undo = restart.
function Invoke-Ps5Bapm([uint32]$op) {
  $o = [Ps5Pwr]::Call([Ps5Pwr]::BAPM, [BitConverter]::GetBytes($op), 64)
  $u = { param($k) [BitConverter]::ToUInt32($o, $k) }
  [ordered]@{ status = [BitConverter]::ToInt32($o, 0); step = & $u 4; index = & $u 8; resp = & $u 12; wc_before = & $u 16; wc_after = & $u 20
              l3_sent = & $u 24; gfx_sent = & $u 28; param_sent = & $u 32; cpus_ok = & $u 36; cpus_total = & $u 40; poisoned = & $u 44
              dpm_cfg_before = ('{0:X16}' -f [BitConverter]::ToUInt64($o, 48)); dpm_cfg_after = ('{0:X16}' -f [BitConverter]::ToUInt64($o, 56)); at = (Get-Date).ToString('o') }
}
if ($WcCpu) { Invoke-Ps5Bapm 2 | ConvertTo-Json; return }
if ($BapmGfxPct) {   # v1.15: GFX CAC weights scaled -> SMU power estimate lower, same budget
  if ($BapmGfxPct -lt 85 -or $BapmGfxPct -gt 100) { throw 'BapmGfxPct must be 85..100' }
  $v = Get-Ps5PwrVersion
  if (-not $v.caps -or -not ([Convert]::ToUInt32($v.caps.Substring(2), 16) -band 0x800)) { throw "loaded ps5pwr $($v.loaded) has no GFX CAC scale (needs 1.15.0 with caps 0x800)" }
  $in = [byte[]]::new(8); [BitConverter]::GetBytes([uint32]3).CopyTo($in, 0); [BitConverter]::GetBytes([uint32]$BapmGfxPct).CopyTo($in, 4)
  $o = [Ps5Pwr]::Call([Ps5Pwr]::BAPM, $in, 64)
  [ordered]@{ op = 'bapm_gfx_scaled'; gfx_pct = $BapmGfxPct; status = [BitConverter]::ToInt32($o, 0); step = [BitConverter]::ToUInt32($o, 4); index = [BitConverter]::ToUInt32($o, 8)
              resp = [BitConverter]::ToUInt32($o, 12); l3_sent = [BitConverter]::ToUInt32($o, 24); gfx_sent = [BitConverter]::ToUInt32($o, 28); param_sent = [BitConverter]::ToUInt32($o, 32)
              cpus_ok = [BitConverter]::ToUInt32($o, 36); poisoned = [BitConverter]::ToUInt32($o, 44); at = (Get-Date).ToString('o') } | ConvertTo-Json; return
}
# v1.14: OC ops (driver v1.14.0, IOCTL_PS5PWR_GFXCLK ops 7..20). Ranges are checked here and again in the driver.
function Assert-Ps5Oc {
  $v = Get-Ps5PwrVersion
  if (-not $v.caps -or -not ([Convert]::ToUInt32($v.caps.Substring(2), 16) -band 0x200)) { throw "loaded ps5pwr $($v.loaded) has no OC ops (needs 1.14.0)" }
}
function ConvertFrom-Ps5Vid([uint32]$vid) { [Math]::Round(1550 - 6.25 * $vid, 2) }
function Get-Ps5GfxVid { $r = Invoke-Ps5GfxClk 7 0; $r['vid'] = $r.mhz; $r.Remove('mhz'); $r['mv'] = $(if ($r.status -eq 0) { ConvertFrom-Ps5Vid $r.vid } else { $null }); $r }
if ($GfxVid -or $SmuFeatures -or $ForceGfxFreq -ge 0 -or $ForceGfxVid -ge 0 -or $Unforce -or $CpuVoltage -or $CoreFreq -ge 0 -or
    $SetCpuBoostMax -ge 0 -or $CpuVidScale -le 0 -or $SetTempLimits -or $CpuOcReset) {
  Assert-Ps5Oc
  $out = [ordered]@{}
  if ($GfxVid) { $out['gfx_vid'] = Get-Ps5GfxVid }
  if ($SmuFeatures) { $r = Invoke-Ps5GfxClk 8 0; $r['features'] = ('0x{0:x8}' -f $r.mhz); $r.Remove('mhz'); $out['smu_features'] = $r }
  if ($ForceGfxVid -ge 0) {   # voltage first (governor order), then the clock
    if ($ForceGfxVid -lt 700 -or $ForceGfxVid -gt 1129) { throw 'ForceGfxVid must be 700..1129 mV' }
    $r = Invoke-Ps5GfxClk 11 $ForceGfxVid; $r['vid'] = $r.mhz; $r.Remove('mhz'); $r['programmed_mv'] = ConvertFrom-Ps5Vid $r.vid
    $out['force_gfx_vid'] = $r; $out['gfx_vid_after'] = Get-Ps5GfxVid
  }
  if ($ForceGfxFreq -ge 0) {
    if ($ForceGfxFreq -lt 500 -or $ForceGfxFreq -gt 2600) { throw 'ForceGfxFreq must be 500..2600 MHz' }
    $out['force_gfx_freq'] = Invoke-Ps5GfxClk 9 $ForceGfxFreq; $out['gfxclk_after'] = Invoke-Ps5GfxClk 0 0; $out['gfxfreq_after'] = Invoke-Ps5GfxClk 4 0
  }
  if ($Unforce) { $out['unforce'] = Invoke-Ps5GfxClk 13 0; $out['gfxclk_after'] = Invoke-Ps5GfxClk 0 0; $out['gfx_vid_after'] = Get-Ps5GfxVid }
  if ($CpuVoltage) { $r = Invoke-Ps5GfxClk 14 0; $r['value'] = $r.mhz; $r.Remove('mhz'); $out['cpu_voltage_raw'] = $r }
  if ($CoreFreq -ge 0) { if ($CoreFreq -gt 7) { throw 'CoreFreq: core 0..7' }; $r = Invoke-Ps5GfxClk 15 $CoreFreq; $r['core'] = $CoreFreq; $r['freq_mhz'] = $r.mhz; $r.Remove('mhz'); $out['core_freq'] = $r }
  if ($SetTempLimits) {
    if ($SetTempLimits.Count -ne 2 -or ($SetTempLimits | Where-Object { $_ -lt 70 -or $_ -gt 95 })) { throw 'SetTempLimits <cpu> <gpu>, each 70..95 C' }
    $out['cpu_tmax'] = Invoke-Ps5GfxClk 18 $SetTempLimits[0]; $out['gpu_tmax'] = Invoke-Ps5GfxClk 19 $SetTempLimits[1]
  }
  if ($CpuVidScale -le 0) { if ($CpuVidScale -lt -50) { throw 'CpuVidScale must be -50..0' }; $out['cpu_vid_scale'] = Invoke-Ps5GfxClk 17 ([uint32][BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$CpuVidScale), 0)) }
  if ($SetCpuBoostMax -ge 0) { if ($SetCpuBoostMax -lt 3200 -or $SetCpuBoostMax -gt 4000) { throw 'SetCpuBoostMax must be 3200..4000 MHz' }; $out['cpu_boost_max'] = Invoke-Ps5GfxClk 16 $SetCpuBoostMax }
  if ($CpuOcReset) { $out['cpu_oc_reset'] = Invoke-Ps5GfxClk 20 0 }
  if ($SetCpuBoostMax -ge 0 -or $CpuVidScale -le 0 -or $CpuOcReset) { $r = Invoke-Ps5GfxClk 15 0; $r['freq_mhz'] = $r.mhz; $r.Remove('mhz'); $out['core0_freq_after'] = $r }
  $out | ConvertTo-Json -Depth 3; return
}
if ($BoostWindow) {   # v1.15: boost window with a raised ceiling (P3), SMU picks voltage on its own curve
  if ($BoostWindow.Count -ne 2 -or $BoostWindow[0] -lt 2230 -or $BoostWindow[0] -gt 2400 -or $BoostWindow[1] -lt 3500 -or $BoostWindow[1] -gt 3800) { throw 'BoostWindow <gpuMax 2230..2400> <cpuMax 3500..3800>' }
  $v = Get-Ps5PwrVersion
  if (-not $v.caps -or -not ([Convert]::ToUInt32($v.caps.Substring(2), 16) -band 0x400)) { throw "loaded ps5pwr $($v.loaded) has no boost window ceiling (needs 1.15.0)" }
  $r = Invoke-Ps5GfxClk 21 ([uint32](($BoostWindow[0] -shl 16) -bor $BoostWindow[1]))
  $r['gpu_max'] = $BoostWindow[0]; $r['cpu_max'] = $BoostWindow[1]; $r.Remove('mhz')
  [ordered]@{ op = 'boost_window'; result = $r; gfxclk = (Invoke-Ps5GfxClk 0 0); gfxfreq = (Invoke-Ps5GfxClk 4 0) } | ConvertTo-Json -Depth 3; return
}
if ($BapmInit) { [ordered]@{ bapm = (Invoke-Ps5Bapm 1); gfxclk = (Invoke-Ps5GfxClk 0 0) } | ConvertTo-Json -Depth 3; return }
# v1.8: SMU boost window (OC-Mode). -BoostEnter / -BoostExit, each followed by QueryGfxclk + GetGfxFrequency readback.
if ($GfxFreq) { Invoke-Ps5GfxClk 4 0 | ConvertTo-Json; return }
if ($BoostEnter -or $BoostEnterHold -or $BoostExit) {
  $op = if ($BoostEnterHold) { 5 } elseif ($BoostEnter) { 2 } else { 3 }
  if ($op -eq 5) { Assert-Ps5Hold }
  $r = Invoke-Ps5GfxClk $op 0
  [ordered]@{ op = $(switch ($op) { 5 { 'boost_enter_hold' } 2 { 'boost_enter' } default { 'boost_exit' } }); result = $r; gfxclk = (Invoke-Ps5GfxClk 0 0); gfxfreq = (Invoke-Ps5GfxClk 4 0) } | ConvertTo-Json -Depth 3; return
}
if ($SetGfxClk -gt 2230 -and $AllowAboveStock) {   # v1.12: above stock only on explicit request, readback from the driver
  if ($SetGfxClk -gt 2400) { throw 'SetGfxClk above stock: at most 2400 MHz' }
  Assert-Ps5Hold
  $in = [byte[]]::new(8); [BitConverter]::GetBytes([uint32]6).CopyTo($in, 0); [BitConverter]::GetBytes([uint32]$SetGfxClk).CopyTo($in, 4)
  $o = [Ps5Pwr]::Call([Ps5Pwr]::GFXCLK, $in, 32)
  [ordered]@{ op = 'request_above_stock'; status = [BitConverter]::ToInt32($o, 0); resp = [BitConverter]::ToUInt32($o, 4); requested_mhz = [BitConverter]::ToUInt32($o, 8)
              poisoned = [BitConverter]::ToUInt32($o, 20); query_gfxclk_mhz = [BitConverter]::ToUInt32($o, 24); gfx_freq_mhz = [BitConverter]::ToUInt32($o, 28)
              at = (Get-Date).ToString('o') } | ConvertTo-Json; return
}
if ($SetGfxClk -ge 0) {
  if ($SetGfxClk -lt 500 -or $SetGfxClk -gt 2230) { throw 'SetGfxClk must be 500..2230 MHz (2231..2400 only with -AllowAboveStock)' }
  $r = Invoke-Ps5GfxClk 1 $SetGfxClk; $q = Invoke-Ps5GfxClk 0 0
  [ordered]@{ request = $r; readback = $q } | ConvertTo-Json -Depth 3; return
}
function Get-Ps5FanStatus {
  # v1.9: fan duty (0..1024 -> %) and EMC temperature zones 0..3 + sensor 18 (zone 0 = MainSoC). No RPM exists.
  $o = [Ps5Pwr]::Call([Ps5Pwr]::FAN_STATUS, $null, 28)
  $valid = [BitConverter]::ToUInt32($o, 4); $duty = [BitConverter]::ToUInt32($o, 0)
  $names = @('zone0_mainsoc', 'zone1', 'zone2', 'zone3', 'sensor18'); $z = [ordered]@{}
  for ($i = 0; $i -lt 5; $i++) { $z[$names[$i]] = if ($valid -band (2 -shl $i)) { [Math]::Round([BitConverter]::ToInt32($o, 8 + 4 * $i) / 1000.0, 1) } else { $null } }
  [ordered]@{ duty_raw = $(if ($valid -band 1) { $duty } else { $null }); duty_percent = $(if ($valid -band 1) { [Math]::Round($duty * 100.0 / 1024, 1) } else { $null });
              temps_c = $z; at = (Get-Date).ToString('o') }
}
if ($Fan -and $SetFanTargetTemp -lt 0) {
  $st = Get-Ps5FanState
  try { $fs = Get-Ps5FanStatus; $st['duty_percent'] = $fs.duty_percent; $st['duty_raw'] = $fs.duty_raw; $st['temps_c'] = $fs.temps_c } catch { $st['fan_status_error'] = $_.Exception.Message }
  $st | ConvertTo-Json -Depth 4; return
}
if ($SetFanTargetTemp -ge 0) {
  if ($SetFanTargetTemp -lt 50 -or $SetFanTargetTemp -gt 100) { throw 'SetFanTargetTemp must be 50..100 C' }
  $ioctlInput = [BitConverter]::GetBytes([uint32]$SetFanTargetTemp)
  $o = [Ps5Pwr]::Call([Ps5Pwr]::FAN_SET, $ioctlInput, 32)
  $cs = for ($i = 0; $i -lt 6; $i++) { [BitConverter]::ToUInt32($o, 8 + 4 * $i) }
  [ordered]@{ target_c = [BitConverter]::ToUInt32($o, 0); reply_bytes = [BitConverter]::ToUInt32($o, 4); curset = @($cs); verified = ([BitConverter]::ToUInt32($o, 0) -eq $SetFanTargetTemp); at = (Get-Date).ToString('o') } | ConvertTo-Json -Depth 3
  return
}
if ($ThermalWatchdog) {
  if ($ThermalLimitC -lt 70 -or $ThermalLimitC -gt 90) { throw 'ThermalLimitC must be 70..90 C' }
  while ($true) {
    $bench = @(Get-Process -Name '3DMark','TimeSpyDemo' -ErrorAction SilentlyContinue)
    if ($bench.Count -eq 0) { break }
    try {
      $t = Get-Ps5Tctl
      $row = [ordered]@{ at = $t.at; pids = @($bench.Id); tctl_c = $t.celsius; raw = $t.raw }
      ($row | ConvertTo-Json -Compress) | Add-Content -Encoding ascii $ThermalLog
      if ($t.celsius -ge $ThermalLimitC) {
        foreach ($p in $bench) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
        throw ('Thermal guard stopped 3DMark at Tctl {0} C (limit {1} C)' -f $t.celsius, $ThermalLimitC)
      }
    } catch {
      foreach ($p in $bench) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
      [ordered]@{ at = (Get-Date).ToString('o'); pids = @($bench.Id); error = $_.Exception.Message } | ConvertTo-Json -Compress | Add-Content -Encoding ascii $ThermalLog
      throw
    }
    Start-Sleep -Seconds 2
  }
  "Thermal watchdog completed; log: $ThermalLog"; return
}
if ($Status) { Get-Ps5PwrStatus | ConvertTo-Json; return }
if ($SetPowerButton -ge 0) {
  $k = 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters'
  if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null }
  New-ItemProperty -Path $k -Name EnablePowerButton -PropertyType DWord -Value $SetPowerButton -Force | Out-Null
  "EnablePowerButton = $SetPowerButton (active within 1 s)"; Start-Sleep -Milliseconds 1500; $Button = $true
}
if ($Button) {
  $o = [Ps5Pwr]::Call([Ps5Pwr]::BUTTON, $null, 60)
  $u = { param($i) [BitConverter]::ToUInt32($o, 4 * $i) }; $l = { param($i) [BitConverter]::ToInt32($o, 4 * $i) }
  [ordered]@{ version = ('0x{0:x}' -f (& $u 0)); enabled_setting = (& $u 1); notifications_on = (& $u 2); left_s0 = (& $u 3);
              enable_result = (& $l 4); off_result = (& $l 5); enable_tries = (& $l 6); icc_button_notes = (& $u 7);
              short_presses = (& $u 8); long_presses = (& $u 9); drained = (& $u 10); stale_cleared = (& $u 11);
              off_count = (& $u 12); event_ok = (& $u 13); bugcheck_callback = (& $u 14); at = (Get-Date).ToString('o') } | ConvertTo-Json
  return
}
if ($SetHotplug -ge 0) {
  $k = 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters'
  if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null }
  New-ItemProperty -Path $k -Name HotplugMode -PropertyType DWord -Value $SetHotplug -Force | Out-Null
  "HotplugMode = $SetHotplug (active within 1 s)"; Start-Sleep -Milliseconds 1500; $Hotplug = $true
}
if ($Hotplug) {
  $o = [Ps5Pwr]::Call([Ps5Pwr]::HOTPLUG, $null, 12768)
  $u = { param($i) [BitConverter]::ToUInt32($o, 4 * $i) }
  $names = 'notes hdmi unplug edid edid_same edid_diff edid_bad short_note other_event other_service stale poll_err queue_lost storms guard_ignored baseline scheduled replays replay_ok replay_fail replay_abort retries held log_only' -split ' '
  $c = [ordered]@{}; for ($i = 0; $i -lt $names.Count; $i++) { $c[$names[$i]] = [BitConverter]::ToUInt32($o, 64 + 4 * $i) }
  $edidLen = [Math]::Min((& $u 9), 256)
  [ordered]@{
    HotplugMode = (& $u 2); state = @('idle','armed','arm-failed','inactive')[[Math]::Min((& $u 3), 3)]
    mode_source = @('none (replay refused)','Ps5HdmiMode','bridge readback','default 1080p60')[[Math]::Min((& $u 4), 3)]
    mode_id = (& $u 5); vic16_packets = (& $u 6); pending = (& $u 7); boot_edid_bytes = (& $u 8)
    last_edid_bytes = (& $u 9); last_edid_same = (& $u 10); icc_tolerated = (& $u 12); icc_dropped = (& $u 13)
    last_result = [BitConverter]::ToInt32($o, 56); last_stage = (& $u 15)
    bridge = (($o[192..221] | ForEach-Object { '{0:x2}' -f $_ }) -join ' ')
    last_edid = $(if ($edidLen) { (($o[224..(223 + $edidLen)] | ForEach-Object { '{0:x2}' -f $_ }) -join '') } else { '' })
    counters = $c; at = (Get-Date).ToString('o')
  } | ConvertTo-Json -Depth 3
  $lines = for ($i = 0; $i -lt 128; $i++) {
    $b = 480 + 96 * $i; $seq = [BitConverter]::ToUInt32($o, $b)
    if ($seq) { [pscustomobject]@{ seq = $seq; ms = [BitConverter]::ToUInt32($o, $b + 4)
                                   text = [Text.Encoding]::ASCII.GetString($o, $b + 8, 88).TrimEnd([char]0) } }
  }
  $lines | Sort-Object seq | ForEach-Object { '{0,6} {1,10} {2}' -f $_.seq, $_.ms, $_.text }
  return
}
if ($OffAfterUnmount) {
  for ($i = 0; $i -lt 300 -and (Test-Path 'S:\'); $i++) { Start-Sleep -Milliseconds 200 }
  Start-Sleep -Milliseconds 500
  (Get-Ps5PwrStatus | ConvertTo-Json) | Set-Content -Encoding ascii $Log
  $Off = $true
}
if ($Off -or $Reboot) {
  $what = [BitConverter]::GetBytes([uint32]$(if ($Reboot) { 1 } else { 0 }))
  try { [void][Ps5Pwr]::Call([Ps5Pwr]::POWER, $what, 4) } catch { Add-Content -Encoding ascii $Log ("power: " + $_) }
}
