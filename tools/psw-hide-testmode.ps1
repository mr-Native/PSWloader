# psw-hide-testmode.ps1 v2.3 (PSWloader) - hides the corner text of the desktop (cosmetic; test signing stays on). v2.0 (18.11.2026,
# user: no text at all): the whole corner group is blanked - test mode, the "(Build ...)" lines, "%ws Build %ws" and the
# evaluation / licence / secure boot / system requirement notes of the same block - same rules as tools\psw-hide-watermark.py
# (HW 18.11.: in Windows Setup this removed every corner text). v1.0 blanked only the test-mode word.
# Runs at every start as SYSTEM (scheduled task PSW-HideTestMode). For every language resource file listed in $Targets
# (shell32.dll.mui of every installed language) that still contains the watermark entry as a stand-alone string, it writes a copy with the text replaced by spaces of the same length, keeps
# the original as <file>.psw-orig (once per original hash) and swaps the files (rename works while the file is in use;
# the new text appears after the next sign-in). A Windows update that replaces the file is patched again at the next start.
# Undo: -Restore (puts every .psw-orig back). Log: C:\ProgramData\PSWloader\logs\hide-testmode.log (last 200 lines).
# v2.1 (24.11.2026, user: "Windows 11 Pro" still stood in the corner): the edition line is not a shell32 string - Explorer
# loads it with BrandingLoadStringForEdition("Basebrd", 12, ...) from the same basebrd entry that winver, WMI and Settings use,
# so it cannot be blanked in a file. Explorer shows that line only while one byte in shell32's data section is 1 (Windows
# itself only ever clears it). -Edition (task PSW-HideEdition, at every sign-in, as the signed-in user, no admin rights)
# finds that byte in System32\shell32.dll by its code pattern - the only reference to "Kernel-BrandingInfo", a byte compare of
# a data byte right before it, the "Basebrd" argument right after it, start value 1 - and clears it in every Explorer of
# the session, then repaints the desktop. Nothing on disk changes; no match (other shell32 build) = nothing is touched.
# It keeps watching (every 15 s) so a restarted Explorer gets it too. Log: %LOCALAPPDATA%\PSWloader\hide-edition.log.
param([switch]$Restore, [switch]$Edition)
if ($Edition) {
  $ErrorActionPreference = 'Stop'
  $ELog = if (([Security.Principal.WindowsIdentity]::GetCurrent()).IsSystem) { 'C:\ProgramData\PSWloader\logs\hide-edition-system.log' } else { Join-Path $env:LOCALAPPDATA 'PSWloader\hide-edition.log' }
  New-Item -ItemType Directory (Split-Path $ELog) -Force | Out-Null
  function EL($m) { try { "$(Get-Date -Format s) $m" | Add-Content $ELog; $c = Get-Content $ELog; if ($c.Count -gt 100) { $c[-100..-1] | Set-Content $ELog } } catch { } }
  $PswSrc = @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class PswEdition {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int a, bool i, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out IntPtr r);
  [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out IntPtr r);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr g, int f);
  static byte[] d; static int nsec, sect;
  static int I32(int o) { return BitConverter.ToInt32(d, o); }
  static int Off(int rva) { for (int i = 0; i < nsec; i++) { int s = sect + 40 * i, va = I32(s + 12), rs = I32(s + 16), ra = I32(s + 20);
      if (rva >= va && rva < va + rs) return rva - va + ra; } return -1; }
  static int Rva(int off, out uint ch) { for (int i = 0; i < nsec; i++) { int s = sect + 40 * i, va = I32(s + 12), rs = I32(s + 16), ra = I32(s + 20);
      if (off >= ra && off < ra + rs) { ch = (uint)I32(s + 36); return off - ra + va; } } ch = 0; return -1; }
  static int StrRva(string t) { byte[] n = Encoding.Unicode.GetBytes(t + "\0"); int hit = -1;
    for (int i = 0; i + n.Length <= d.Length; i += 2) { int k = 0; while (k < n.Length && d[i + k] == n[k]) k++;
      if (k == n.Length) { uint ch; int r = Rva(i, out ch); if (r >= 0 && (ch & 0x20000000) == 0) { if (hit >= 0) return -2; hit = r; } } }
    return hit; }
  // returns "cmpRva cmpLen flagRva" or an error text starting with "no:"
  public static string Find(string path) {
    d = System.IO.File.ReadAllBytes(path); int pe = I32(0x3c); nsec = BitConverter.ToUInt16(d, pe + 6);
    sect = pe + 24 + BitConverter.ToUInt16(d, pe + 20);
    int sKbi = StrRva("Kernel-BrandingInfo"), sBrd = StrRva("Basebrd");
    if (sKbi < 0 || sBrd < 0) return "no: strings " + sKbi + " " + sBrd;
    string res = null;
    for (int i = 0; i + 7 <= d.Length; i++) {
      if (d[i] != 0x48 || d[i + 1] != 0x8D || (d[i + 2] & 0xC7) != 0x05) continue;
      uint ch; int r = Rva(i, out ch); if (r < 0 || (ch & 0x20000000) == 0 || r + 7 + I32(i + 3) != sKbi) continue;
      bool brd = false;   // the "Basebrd" argument of BrandingLoadStringForEdition follows within 160 bytes
      for (int k = i + 7; k < i + 160 && k + 7 <= d.Length; k++)
        if (d[k] == 0x48 && d[k + 1] == 0x8D && (d[k + 2] & 0xC7) == 0x05) { uint c2; int r2 = Rva(k, out c2); if (r2 + 7 + I32(k + 3) == sBrd) brd = true; }
      if (!brd) continue;
      for (int j = i - 2; j >= i - 48; j--) {   // byte compare of a RIP-relative data byte followed by a conditional jump
        int len = 0, disp = 0;
        if (d[j] == 0x80 && d[j + 1] == 0x3D && d[j + 6] == 0) { len = 7; disp = j + 2; }
        else if ((d[j] == 0x40 || d[j] == 0x44) && d[j + 1] == 0x38 && (d[j + 2] & 0xC7) == 0x05) { len = 7; disp = j + 3; }
        else if (d[j] == 0x38 && (d[j + 1] & 0xC7) == 0x05 && (d[j - 1] & 0xF0) != 0x40) { len = 6; disp = j + 2; }   // with REX: next j
        if (len == 0) continue;
        byte n0 = d[j + len], n1 = d[j + len + 1];
        if (!(n0 == 0x74 || n0 == 0x75 || (n0 == 0x0F && (n1 == 0x84 || n1 == 0x85)))) continue;
        uint c3; int cr = Rva(j, out c3); int flag = cr + len + I32(disp); int fo = Off(flag);
        if (fo < 0 || d[fo] != 1) continue;
        if (res != null) return "no: several matches";
        res = cr + " " + len + " " + flag; break;
      }
    }
    return res ?? "no: pattern not found";
  }
  // 0 = cleared now, 1 = was already clear, negative = not touched
  public static int Clear(int pid, long baseAddr, int cmpRva, int cmpLen, int flagRva) {
    IntPtr h = OpenProcess(0x0438, false, pid); if (h == IntPtr.Zero) return -1;
    try {
      IntPtr r; byte[] code = new byte[cmpLen];
      if (!ReadProcessMemory(h, new IntPtr(baseAddr + cmpRva), code, cmpLen, out r)) return -2;
      int fo = Off(cmpRva); for (int k = 0; k < cmpLen; k++) if (code[k] != d[fo + k]) return -3;   // loaded image = the scanned file
      byte[] f = new byte[1]; if (!ReadProcessMemory(h, new IntPtr(baseAddr + flagRva), f, 1, out r)) return -4;
      if (f[0] == 0) return 1; if (f[0] != 1) return -5;
      f[0] = 0; return WriteProcessMemory(h, new IntPtr(baseAddr + flagRva), f, 1, out r) ? 0 : -6;
    } finally { CloseHandle(h); }
  }
}
'@
  # v2.3 (HW 24.11.: Add-Type ran csc.exe for seconds while the sign-in kept the CPU busy - the line showed ~7 s): the helper is
  # compiled once into C:\ProgramData\PSWloader\cache\PswEdition-<hash>.dll and only loaded after that (milliseconds).
  # Only the SYSTEM run writes there: it owns the folder (SYSTEM + Administrators full, Users read) and a DLL is loaded only
  # when SYSTEM or Administrators own it - a file another user put there is never loaded (it would run as SYSTEM).
  $CacheDir = 'C:\ProgramData\PSWloader\cache'
  $hash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($PswSrc))).Replace('-', '').Substring(0, 12)
  $dll = Join-Path $CacheDir "PswEdition-$hash.dll"
  $sysRun = ([Security.Principal.WindowsIdentity]::GetCurrent()).IsSystem
  function Trusted($f) { try { $o = (Get-Acl -LiteralPath $f).Owner; return $o -eq 'NT AUTHORITY\SYSTEM' -or $o -eq 'BUILTIN\Administrators' -or $o -match '\\(SYSTEM|Administratoren|Administrators)$' } catch { return $false } }
  if ($sysRun) {
    try {
      New-Item -ItemType Directory $CacheDir -Force | Out-Null
      & takeown.exe /f $CacheDir /a | Out-Null
      & icacls.exe $CacheDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
      Get-ChildItem $CacheDir -File | Where-Object { -not (Trusted $_.FullName) } | Remove-Item -Force
    } catch { }
  }
  if ((Test-Path $dll) -and (Trusted $dll)) { try { Add-Type -Path $dll } catch { } }
  if (-not ('PswEdition' -as [type]) -and $sysRun) {
    try { $tmp = Join-Path $CacheDir "PswEdition-$hash-$PID.dll"
          Add-Type -TypeDefinition $PswSrc -OutputAssembly $tmp -OutputType Library
          Move-Item $tmp $dll -Force -ErrorAction SilentlyContinue
          Add-Type -Path $(if (Test-Path $dll) { $dll } else { $tmp }) } catch { }
  }
  if (-not ('PswEdition' -as [type])) { Add-Type -TypeDefinition $PswSrc }
  $s32 = Join-Path $env:SystemRoot 'System32\shell32.dll'
  $hit = [PswEdition]::Find($s32); $ver = (Get-Item $s32).VersionInfo.FileVersion
  if ($hit -like 'no:*') { EL "shell32 $ver : $hit - edition line left as it is"; exit 0 }
  $cmp, $len, $flag = $hit -split ' ' | ForEach-Object { [int]$_ }
  EL "shell32 $ver : flag at RVA 0x$('{0:X}' -f $flag) (compare at 0x$('{0:X}' -f $cmp))"
  # v2.2 (user 24.11.: the line showed for ~5 s after sign-in): as SYSTEM (task PSW-HideEditionSys, from system start) the
  # flag is cleared in every session as soon as an Explorer has loaded shell32 - a process start event, then a check every
  # 25 ms - usually before the desktop is painted. As the user (task PSW-HideEdition, at sign-in) it checks every 250 ms for
  # the first minute, then every 15 s, and repaints the desktop once (the line may have been painted already).
  $sys = ([Security.Principal.WindowsIdentity]::GetCurrent()).IsSystem
  $sess = (Get-Process -Id $PID).SessionId; $done = @{}
  function Patch-Explorer($p) {
    try {   # an Explorer that ends in between is simply skipped
      $key = "$($p.Id)/$($p.StartTime.Ticks)"; if ($done[$key]) { return $true }
      $p.Refresh(); $m = $p.Modules | Where-Object { $_.FileName -eq $s32 } | Select-Object -First 1
      if (-not $m) { return $false }   # shell32 not loaded yet
      $rc = [PswEdition]::Clear($p.Id, [int64]$m.BaseAddress, $cmp, $len, $flag); $done[$key] = $true
      EL "explorer $($p.Id) (session $($p.SessionId)): $(switch ($rc) { 0 {'edition line hidden'} 1 {'already hidden'} default {"not touched ($rc)"} })"
      return $true
    } catch { return $false }
  }
  if ($sys) {
    foreach ($p in [Diagnostics.Process]::GetProcessesByName('explorer')) { [void](Patch-Explorer $p) }
    Register-CimIndicationEvent -Query "SELECT ProcessID FROM Win32_ProcessStartTrace WHERE ProcessName='explorer.exe'" -SourceIdentifier PswExplorer
    while ($true) {
      $ev = Wait-Event -SourceIdentifier PswExplorer -Timeout 3600
      if (-not $ev) { continue }
      $xpid = [int]$ev.SourceEventArgs.NewEvent.ProcessID; Remove-Event -EventIdentifier $ev.EventIdentifier
      for ($t = 0; $t -lt 600; $t++) {   # up to 15 s
        try { $p = [Diagnostics.Process]::GetProcessById($xpid) } catch { break }
        if (Patch-Explorer $p) { break }
        Start-Sleep -Milliseconds 25
      }
    }
  }
  $first = $true; $t0 = Get-Date
  while ($true) {
    foreach ($p in @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $sess)) { [void](Patch-Explorer $p) }
    if ($first -and $done.Count) { $first = $false; Start-Sleep -Milliseconds 300; [void][PswEdition]::RedrawWindow([IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero, 0x0485) }
    if (((Get-Date) - $t0).TotalSeconds -lt 60) { Start-Sleep -Milliseconds 250 } else { Start-Sleep -Seconds 15 }
  }
}
$ErrorActionPreference = 'Stop'
$LogDir = 'C:\ProgramData\PSWloader\logs'; New-Item -ItemType Directory $LogDir -Force | Out-Null
$Log = Join-Path $LogDir 'hide-testmode.log'
function L($m) { "$(Get-Date -Format s) $m" | Add-Content $Log; $c = Get-Content $Log; if ($c.Count -gt 200) { $c[-200..-1] | Set-Content $Log } }
$Needles = 'Testmodus', 'Test Mode', 'Mode test', 'Modo de prueba', ('Modalit' + [char]0xE0 + ' test')
function EntryLen([byte[]]$b, [int]$k) { if ($k + 1 -ge $b.Length) { return 0 }; return [int]$b[$k] + 256 * [int]$b[$k + 1] }
function EntryText([byte[]]$b, [int]$k) { $n = EntryLen $b $k; if ($k + 2 + 2 * $n -gt $b.Length) { return '' }; return [Text.Encoding]::Unicode.GetString($b, $k + 2, 2 * $n) }
function Blank([byte[]]$b, [int]$k) { $n = EntryLen $b $k; for ($c = 0; $c -lt $n; $c++) { $b[$k + 2 + 2 * $c] = 32; $b[$k + 3 + 2 * $c] = 0 } }
function Find-Entries([byte[]]$b, [string]$text) {   # offsets of string-table entries whose text is exactly $text
  $u = [Text.Encoding]::Unicode.GetString($b, 0, $b.Length - ($b.Length % 2)); $out = @(); $i = $u.IndexOf($text, [StringComparison]::Ordinal)
  while ($i -ge 1) { if ((EntryLen $b (2 * $i - 2)) -eq $text.Length) { $out += (2 * $i - 2) }; $i = $u.IndexOf($text, $i + 1, [StringComparison]::Ordinal) }
  return $out
}
function Hide-Corner([byte[]]$b) {   # returns the number of blanked entries; works on $b in place
  $o = [byte[]]$b.Clone(); $hits = 0
  foreach ($n in $Needles) { foreach ($k in (Find-Entries $o $n)) {
    Blank $b $k; $hits++; $p = $k; $last = -1
    for ($e = 0; $e -lt 16 -and $p + 2 -le $o.Length; $e++) {
      $len = EntryLen $o $p; if ($len -and (EntryText $o $p).Contains('(Build %ws')) { Blank $b $p; $hits++; $last = $p }; $p += 2 + 2 * $len }
    if ($last -ge 0) { $q = $last + 2 + 2 * (EntryLen $o $last); if (EntryLen $o $q) { Blank $b $q; $hits++ } } } }
  foreach ($k in (Find-Entries $o '%ws Build %ws')) { $p = $k
    for ($e = 0; $e -lt 16 -and $p + 2 -le $o.Length; $e++) { $len = EntryLen $o $p; if (-not $len) { break }; Blank $b $p; $hits++; $p += 2 + 2 * $len } }
  return $hits
}
$Targets = @(Get-ChildItem C:\Windows\System32 -Directory | ? Name -match '^[a-z]{2}-[A-Z]{2}$' | % { Join-Path $_.FullName 'shell32.dll.mui' } | ? { Test-Path $_ })
function Own($f) { & takeown.exe /f $f /a | Out-Null; & icacls.exe $f /grant '*S-1-5-32-544:F' | Out-Null }
foreach ($f in $Targets) {
  try {
    if ($Restore) { if (Test-Path "$f.psw-orig") { Own $f; Move-Item $f "$f.psw-del" -Force; Move-Item "$f.psw-orig" $f -Force; L "restored $f" }; continue }
    $b = [IO.File]::ReadAllBytes($f); $hit = (Hide-Corner $b) -gt 0
    if (-not $hit) { L "ok (no watermark text) $f"; continue }
    Own $f
    if (-not (Test-Path "$f.psw-orig")) { Copy-Item $f "$f.psw-orig" -Force }
    [IO.File]::WriteAllBytes("$f.psw-new", $b)
    Get-ChildItem "$(Split-Path $f)\*.psw-del" -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue
    Move-Item $f "$f.psw-del" -Force; Move-Item "$f.psw-new" $f -Force
    L "patched $f (original kept as .psw-orig)"
  } catch { L "ERROR $f : $($_.Exception.Message)" }
}
