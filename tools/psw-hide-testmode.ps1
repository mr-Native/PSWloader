# psw-hide-testmode.ps1 v2.0 (PSWloader) - hides the corner text of the desktop (cosmetic; test signing stays on). v2.0 (18.11.2026,
# user: no text at all): the whole corner group is blanked - test mode, the "(Build ...)" lines, "%ws Build %ws" and the
# evaluation / licence / secure boot / system requirement notes of the same block - same rules as tools\psw-hide-watermark.py
# (HW 18.11.: in Windows Setup this removed every corner text). v1.0 blanked only the test-mode word.
# Runs at every start as SYSTEM (scheduled task PSW-HideTestMode). For every language resource file listed in $Targets
# (shell32.dll.mui of every installed language) that still contains the watermark entry as a stand-alone string, it writes a copy with the text replaced by spaces of the same length, keeps
# the original as <file>.psw-orig (once per original hash) and swaps the files (rename works while the file is in use;
# the new text appears after the next sign-in). A Windows update that replaces the file is patched again at the next start.
# Undo: -Restore (puts every .psw-orig back). Log: C:\ProgramData\PSWloader\logs\hide-testmode.log (last 200 lines).
param([switch]$Restore)
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
