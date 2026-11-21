# thermal-supervisor.ps1 v1.2 (16.11.2026; v1.2: the guard runs inside a job object with
#   JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (dies with the supervisor), and at start, guards left over from an earlier supervisor (task
#   stop kills the supervisor, not its Start-Process child) are stopped, so only this supervisor's guard runs). v1.1: guard limit per power preset. Reads C:\ps5-work\thermal-guard-limit.txt
# (written by ps5-thermal-manager v1.3: efficiency 78 / standard 80 / oc 82) before EVERY guard cycle; missing or
# outside 70..95 -> 78 as before. Otherwise unchanged (the guard child restarts after IdleExitSeconds, so a new preset
# applies at the next cycle).
$ErrorActionPreference = 'Continue'
$SupervisorLog = 'C:\ps5-work\thermal-supervisor.jsonl'
$Guard = 'C:\ps5-work\thermal-guard-v17.ps1'
$WatchLog = 'C:\ps5-work\thermal-watchdog-timespy-live.jsonl'
$PowerShell = Join-Path $PSHOME 'powershell.exe'
$LimitFile = 'C:\ps5-work\thermal-guard-limit.txt'

function Write-SupervisorRow([string]$Event, [int]$ExitCode, [string]$Detail) {
    $row = [ordered]@{
        at = (Get-Date).ToString('o')
        event = $Event
        child_exit = $ExitCode
        detail = $Detail
    }
    $row | ConvertTo-Json -Compress | Add-Content -LiteralPath $SupervisorLog -Encoding ascii
}

# v1.2: job object - when this supervisor ends (task stop, kill), Windows closes the job handle and kills the guard
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Ps5GuardJob {
    [StructLayout(LayoutKind.Sequential)] struct BASIC { public long a; public long b; public uint LimitFlags; public UIntPtr c; public UIntPtr d; public uint e; public UIntPtr f; public uint g; public uint h; }
    [StructLayout(LayoutKind.Sequential)] struct IOC { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)] struct EXT { public BASIC Basic; public IOC Io; public UIntPtr p1, p2, p3, p4; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateJobObjectW(IntPtr a, string n);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr j, int c, ref EXT i, uint l);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr j, IntPtr p);
    public static IntPtr Create() {
        IntPtr j = CreateJobObjectW(IntPtr.Zero, null);
        EXT x = new EXT(); x.Basic.LimitFlags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if (j == IntPtr.Zero || !SetInformationJobObject(j, 9, ref x, (uint)Marshal.SizeOf(typeof(EXT)))) return IntPtr.Zero;
        return j;
    }
    public static bool Assign(IntPtr j, IntPtr process) { return j != IntPtr.Zero && AssignProcessToJobObject(j, process); }
}
"@
$Job = [Ps5GuardJob]::Create()
if ($Job -eq [IntPtr]::Zero) { Write-SupervisorRow 'job_create_failed' 1 'guard will not die with the supervisor' }

# v1.2: orphaned guards from an earlier supervisor out (command line points at the guard script, parent is not alive)
try {
    $guards = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match [regex]::Escape($Guard) })
    foreach ($g in $guards) {
        if ($g.ParentProcessId -ne $PID -and -not (Get-Process -Id $g.ParentProcessId -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq 'powershell' })) {
            Stop-Process -Id $g.ProcessId -Force -ErrorAction SilentlyContinue
            Write-SupervisorRow 'orphan_guard_stopped' 0 "pid $($g.ProcessId) parent $($g.ParentProcessId)"
        }
    }
} catch { Write-SupervisorRow 'orphan_scan_error' 1 $_.Exception.Message }

while ($true) {
    $limit = 78
    try { $v = [int]((Get-Content -LiteralPath $LimitFile -TotalCount 1 -ErrorAction Stop).Trim()); if ($v -ge 70 -and $v -le 95) { $limit = $v } } catch { }
    try {
        $child = Start-Process -FilePath $PowerShell -WindowStyle Hidden -PassThru -ArgumentList @(
            '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', $Guard, '-ThermalLimitC', "$limit", '-PollMilliseconds', '500',
            '-ThermalLog', $WatchLog, '-IdleExitSeconds', '15'
        )
        if (-not [Ps5GuardJob]::Assign($Job, $child.Handle)) { Write-SupervisorRow 'job_assign_failed' 1 "pid $($child.Id)" }
        $child.WaitForExit()
        Write-SupervisorRow 'guard_cycle_exit' ([int]$child.ExitCode) "restarting watcher (limit $limit)"
    } catch {
        Write-SupervisorRow 'guard_launch_error' 1 $_.Exception.Message
    }
    Start-Sleep -Seconds 2
}
