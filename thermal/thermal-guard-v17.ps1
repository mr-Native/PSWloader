# thermal-guard-v18.3.ps1 (17.11.2026; v18.3 = stop only after 3 consecutive samples >= limit (1.5 s) or at once at limit+5 C;
#   single Tctl spikes are logged as thermal_spike - they had stopped Time Spy at 90-92 C while the filtered Tctl was ~70 C)
# thermal-guard-v18.2.ps1 (16.11.2026; v18.2 = single instance: named mutex Global\PS5ThermalGuard - a second
#   guard logs guard_duplicate_exit and quits (16.11. 11:47 two guards ran in parallel and fought over the log file).
#   v18.1: v18.1 = review: Write-ThermalRow serialises inside its try (truly
#   never throws), -ThermalLimitC range 70..95 like the limit file). v18 based on the live v17, sha 53668cd8. Changes:
#  1. Logging can never stop 3DMark: Write-ThermalRow retries 3x (FileShare ReadWrite, 50 ms apart) and otherwise drops the
#     row. v17 appended inside the sensor try-block, so an Add-Content failure (localised
#     "Stream was not readable", someone reading the log) counted as a SENSOR error -> fail-closed kill (11:19:23 FS
#     standard, 11:31:55 OC Time Spy).
#  2. Sensor errors: single errors are only logged (event sensor_error_soft, consecutive count); fail-closed needs >= 3
#     consecutive errors AND >= 1.5 s without a valid reading. Real over-temperature still stops immediately.
#  4. Every error logs exception type + script line (16.11.).
#  3. The limit is re-read from -LimitFile (default C:\ps5-work\thermal-guard-limit.txt, written by ThermalManager v1.3+)
#     whenever the file changes (event limit_changed); -ThermalLimitC is the start/fallback value.
param(
    [ValidateRange(70, 95)][int]$ThermalLimitC = 78,
    [ValidateRange(250, 2000)][int]$PollMilliseconds = 500,
    [string]$ThermalLog = 'C:\ps5-work\thermal-watchdog-timespy-v17.jsonl',
    [ValidateRange(5, 60)][int]$IdleExitSeconds = 15,
    [switch]$SelfCheckOnly,
    [string]$LimitFile = 'C:\ps5-work\thermal-guard-limit.txt',
    [ValidateRange(1, 20)][int]$FailClosedAfterErrors = 3,
    [ValidateRange(500, 10000)][int]$FailClosedAfterMs = 1500
)

$ErrorActionPreference = 'Stop'
$AppRoot = 'C:\Program Files (x86)\Steam\steamapps\common\3DMark Demo\'
$TctlIoctl = [Convert]::ToUInt32('80026418', 16)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Ps5ThermalGuardV17 {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr handle, uint code, byte[] input, int inputLength,
        byte[] output, int outputLength, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

    public static byte[] ReadTctl(uint code) {
        IntPtr handle = CreateFileW("\\\\.\\Ps5Pwr", 0xC0000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle == new IntPtr(-1)) throw new Exception("Ps5Pwr open failed " + Marshal.GetLastWin32Error());
        try {
            byte[] output = new byte[8]; int returned;
            if (!DeviceIoControl(handle, code, null, 0, output, output.Length, out returned, IntPtr.Zero))
                throw new Exception("Tctl ioctl failed " + Marshal.GetLastWin32Error());
            if (returned < 8) throw new Exception("short Tctl ioctl result " + returned);
            return output;
        } finally { CloseHandle(handle); }
    }
}
"@

function Get-3DMarkProcesses {
    $items = @(Get-CimInstance Win32_Process -ErrorAction Stop)
    @($items | Where-Object {
        ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($script:AppRoot, [StringComparison]::OrdinalIgnoreCase)) -or
        ($_.CommandLine -and $_.CommandLine.IndexOf($script:AppRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0) -or
        ($_.Name -match '^(3DMark|TimeSpyDemo)')
    })
}

function Get-Ps5Tctl {
    $bytes = [Ps5ThermalGuardV17]::ReadTctl($script:TctlIoctl)
    $raw = [BitConverter]::ToUInt32($bytes, 0)
    $milliC = [BitConverter]::ToInt32($bytes, 4)
    if ($raw -eq 0 -or $milliC -lt 0 -or $milliC -gt 150000) {
        throw ('implausible Tctl sensor data raw=0x{0:x8} milliC={1}' -f $raw, $milliC)
    }
    [pscustomobject]@{
        raw=('0x{0:x8}' -f $raw)
        milli_celsius=$milliC
        celsius=[Math]::Round($milliC / 1000.0, 1)
        at=(Get-Date).ToString('o')
    }
}

function Stop-3DMarkProcessTree {
    $stopped = [System.Collections.Generic.List[object]]::new()
    $deadline = (Get-Date).AddSeconds(6)
    do {
        $items = @(Get-3DMarkProcesses)
        if ($items.Count -eq 0) { break }
        # End the render worker first, then the Java/3DMark controllers that could create another worker.
        $ordered = @($items | Sort-Object @{Expression={ if ($_.Name -match 'TimeSpy') { 0 } else { 1 } }})
        foreach ($item in $ordered) {
            try {
                Stop-Process -Id ([int]$item.ProcessId) -Force -ErrorAction Stop
                $stopped.Add([pscustomobject]@{ pid=[int]$item.ProcessId; name=[string]$item.Name })
            } catch { }
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)

    [pscustomobject]@{
        stopped = @($stopped)
        remaining = @((Get-3DMarkProcesses | ForEach-Object {
            [pscustomobject]@{ pid=[int]$_.ProcessId; name=[string]$_.Name }
        }))
    }
}

function Write-ThermalRow([System.Collections.IDictionary]$Row) {   # v18.1: never throws (serialisation included)
    try { $bytes = [Text.Encoding]::ASCII.GetBytes(($Row | ConvertTo-Json -Compress -Depth 4) + "`r`n") } catch { return }
    for ($i = 0; $i -lt 3; $i++) {
        try {
            $fs = [IO.File]::Open($script:ThermalLog, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
            try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Close() }
            return
        } catch { Start-Sleep -Milliseconds 50 }
    }
}
function Update-Limit {   # v18: re-read the limit file when it changes
    try {
        if (-not (Test-Path -LiteralPath $script:LimitFile)) { return }
        $w = (Get-Item -LiteralPath $script:LimitFile).LastWriteTimeUtc
        if ($w -eq $script:LimitStamp) { return }
        $script:LimitStamp = $w
        $v = [int]((Get-Content -LiteralPath $script:LimitFile -TotalCount 1).Trim())
        if ($v -ge 70 -and $v -le 95 -and $v -ne $script:ThermalLimitC) {
            Write-ThermalRow ([ordered]@{ at=(Get-Date).ToString('o'); event='limit_changed'; from=$script:ThermalLimitC; to=$v; file=$script:LimitFile })
            $script:ThermalLimitC = $v
        }
    } catch { }
}

if ($SelfCheckOnly) {
    $sample = Get-Ps5Tctl
    $processes = @(Get-3DMarkProcesses)
    [ordered]@{ self_check='ok'; tctl=$sample; matching_benchmark_processes=@($processes | ForEach-Object {
        [pscustomobject]@{ pid=[int]$_.ProcessId; name=[string]$_.Name }
    }) } | ConvertTo-Json -Depth 4
    return
}

$logDirectory = Split-Path -Parent $ThermalLog
if ($logDirectory -and -not (Test-Path -LiteralPath $logDirectory)) {
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
}
if (-not $SelfCheckOnly) {   # v18.2: only one guard at a time
    $script:GuardMutex = New-Object System.Threading.Mutex($false, 'Global\PS5ThermalGuard')
    $owned = $false
    try { $owned = $script:GuardMutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $owned = $true }
    if (-not $owned) {
        Write-ThermalRow ([ordered]@{ at=(Get-Date).ToString('o'); event='guard_duplicate_exit'; pid=$PID; limit_c=$ThermalLimitC })
        exit 0
    }
}
$script:LimitStamp = $null; Update-Limit
$script:hotCount = 0   # v18.3: consecutive samples >= limit
Write-ThermalRow ([ordered]@{
    at=(Get-Date).ToString('o'); event='guard_started'; limit_c=$ThermalLimitC;
    poll_ms=$PollMilliseconds; app_root=$AppRoot
})

$seenBenchmark = $false
$idleSince = $null
$sensorErrors = 0
$lastGood = Get-Date
while ($true) {
    $processes = @(Get-3DMarkProcesses)
    if ($processes.Count -eq 0) {
        if ($seenBenchmark) {
            if ($null -eq $idleSince) { $idleSince = Get-Date }
            elseif (((Get-Date) - $idleSince).TotalSeconds -ge $IdleExitSeconds) { break }
        }
        Start-Sleep -Milliseconds $PollMilliseconds
        continue
    }
    $seenBenchmark = $true
    $idleSince = $null
    $pidList = @($processes | ForEach-Object { [int]$_.ProcessId })
    $nameList = @($processes | ForEach-Object { [string]$_.Name } | Select-Object -Unique)

    Update-Limit
    $reading = $null
    try { $reading = Get-Ps5Tctl } catch {
        $sensorErrors++
        $sinceGood = ((Get-Date) - $lastGood).TotalMilliseconds
        Write-ThermalRow ([ordered]@{ at=(Get-Date).ToString('o'); event='sensor_error_soft'; count=$sensorErrors;
            since_good_ms=[int]$sinceGood; error=$_.Exception.Message; type=$_.Exception.GetType().FullName; line=$_.InvocationInfo.ScriptLineNumber })
        if ($sensorErrors -ge $FailClosedAfterErrors -and $sinceGood -ge $FailClosedAfterMs) {
            $stop = Stop-3DMarkProcessTree
            Write-ThermalRow ([ordered]@{
                at=(Get-Date).ToString('o'); event='sensor_error_fail_closed'; pids=$pidList; names=$nameList;
                error=$_.Exception.Message; consecutive=$sensorErrors; since_good_ms=[int]$sinceGood; stopped=$stop.stopped; remaining=$stop.remaining
            })
            exit 3
        }
        Start-Sleep -Milliseconds $PollMilliseconds
        continue
    }
    $sensorErrors = 0; $lastGood = Get-Date
    try {
        Write-ThermalRow ([ordered]@{
            at=(Get-Date).ToString('o'); event='sample'; pids=$pidList; names=$nameList;
            tctl_c=$reading.celsius; raw=$reading.raw; limit_c=$ThermalLimitC
        })
        # v18.3: Tctl jumps for a moment on every load step (Zen 2); one sample over the limit stopped Time Spy at 90.2-92.5 C
        # while the median-filtered Tctl stayed near 70 C. Stop only when the limit holds for 3 samples in a row (1.5 s at 500 ms)
        # or immediately at limit + 5 C. Short spikes are logged as thermal_spike.
        if ($reading.celsius -ge $ThermalLimitC) { $script:hotCount++ } else { $script:hotCount = 0 }
        if ($script:hotCount -ge 1 -and $script:hotCount -lt 3 -and $reading.celsius -lt ($ThermalLimitC + 5)) {
            Write-ThermalRow ([ordered]@{ at=(Get-Date).ToString('o'); event='thermal_spike'; tctl_c=$reading.celsius; limit_c=$ThermalLimitC; consecutive=$script:hotCount })
        }
        if ($script:hotCount -ge 3 -or $reading.celsius -ge ($ThermalLimitC + 5)) {
            $stop = Stop-3DMarkProcessTree
            Write-ThermalRow ([ordered]@{
                at=(Get-Date).ToString('o'); event='thermal_limit'; pids=$pidList; names=$nameList;
                tctl_c=$reading.celsius; limit_c=$ThermalLimitC; stopped=$stop.stopped; remaining=$stop.remaining
            })
            exit 2
        }
    } catch {
        # v18: anything but the sensor read is logged, not fatal - EXCEPT a pending over-temperature stop, which is retried
        Write-ThermalRow ([ordered]@{ at=(Get-Date).ToString('o'); event='guard_internal_error'; error=$_.Exception.Message;
            type=$_.Exception.GetType().FullName; line=$_.InvocationInfo.ScriptLineNumber; command=$_.InvocationInfo.MyCommand.Name })
        if ($reading -and $reading.celsius -ge $ThermalLimitC) {
            try { Stop-3DMarkProcessTree | Out-Null } catch { try { Get-Process 3DMark*, TimeSpy* -ErrorAction SilentlyContinue | Stop-Process -Force } catch { } }
            exit 2
        }
    }
    Start-Sleep -Milliseconds $PollMilliseconds
}

Write-ThermalRow ([ordered]@{ at=(Get-Date).ToString('o'); event='idle_exit'; idle_seconds=$IdleExitSeconds })
