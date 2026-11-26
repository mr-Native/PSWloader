/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PSWEdition.exe v1.0.1 (25.11.2026: polls every 2 s after the first 5 minutes, 50 ms before) / v1.0 (24.11.2026, PSWloader v1.1) - hides the edition line ("Windows 11 Pro") of the desktop corner text.
 * psw-hide-testmode.ps1 2.1-2.3 did this in PowerShell; during the boot PowerShell itself needed ~30 s to start, so the
 * line showed ~9 s after the desktop appeared (HW 24.11.). This native program starts in milliseconds (task
 * PSW-HideEditionSys, SYSTEM, at system start) and clears the flag in every Explorer as soon as shell32 is loaded.
 *
 * Explorer draws the edition line only while one byte in shell32's data section is 1 (Windows itself only ever clears it).
 * The byte is found in System32\shell32.dll by its code pattern: the only reference to the string "Kernel-BrandingInfo",
 * a byte compare of a RIP-relative data byte right before it, the "Basebrd" argument of BrandingLoadStringForEdition
 * right after it, start value 1 in the file. No match (another shell32 build) = nothing is touched. In memory the compare
 * bytes must equal the file and the byte must be 0 or 1. Nothing on disk changes.
 * Log: C:\ProgramData\PSWloader\logs\hide-edition-native.log */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static unsigned char *d; static DWORD dlen; static int nsec, sect;
static int i32(int o) { int v; memcpy(&v, d + o, 4); return v; }
static int u16(int o) { return d[o] | d[o + 1] << 8; }
static int off_of(int rva) { for (int i = 0; i < nsec; i++) { int s = sect + 40 * i, va = i32(s + 12), rs = i32(s + 16), ra = i32(s + 20);
    if (rva >= va && rva < va + rs) return rva - va + ra; } return -1; }
static int rva_of(int off, unsigned *ch) { for (int i = 0; i < nsec; i++) { int s = sect + 40 * i, va = i32(s + 12), rs = i32(s + 16), ra = i32(s + 20);
    if (off >= ra && off < ra + rs) { *ch = (unsigned)i32(s + 36); return off - ra + va; } } *ch = 0; return -1; }
static int str_rva(const wchar_t *t) {   /* the only UTF-16 occurrence (with terminator) outside code; -1 none, -2 several */
  size_t n = (wcslen(t) + 1) * 2; int hit = -1;
  for (DWORD i = 0; i + n <= dlen; i += 2) if (memcmp(d + i, t, n) == 0) { unsigned ch; int r = rva_of((int)i, &ch);
      if (r >= 0 && !(ch & 0x20000000)) { if (hit >= 0) return -2; hit = r; } }
  return hit;
}
static int g_cmp, g_len, g_flag;
static const char *find_flag(void) {
  if (dlen < 0x400 || d[0] != 'M' || d[1] != 'Z') return "not a PE file";
  int pe = i32(0x3c); nsec = u16(pe + 6); sect = pe + 24 + u16(pe + 20);
  int skbi = str_rva(L"Kernel-BrandingInfo"), sbrd = str_rva(L"Basebrd"); int found = 0;
  if (skbi < 0 || sbrd < 0) return "strings not found";
  for (DWORD i = 48; i + 7 <= dlen; i++) {
    if (d[i] != 0x48 || d[i + 1] != 0x8D || (d[i + 2] & 0xC7) != 0x05) continue;
    unsigned ch; int r = rva_of((int)i, &ch); if (r < 0 || !(ch & 0x20000000) || r + 7 + i32(i + 3) != skbi) continue;
    int brd = 0;
    for (DWORD k = i + 7; k < i + 160 && k + 7 <= dlen; k++)
      if (d[k] == 0x48 && d[k + 1] == 0x8D && (d[k + 2] & 0xC7) == 0x05) { unsigned c2; int r2 = rva_of((int)k, &c2); if (r2 + 7 + i32(k + 3) == sbrd) brd = 1; }
    if (!brd) continue;
    for (DWORD j = i - 2; j >= i - 48; j--) {
      int len = 0, disp = 0;
      if (d[j] == 0x80 && d[j + 1] == 0x3D && d[j + 6] == 0) { len = 7; disp = j + 2; }
      else if ((d[j] == 0x40 || d[j] == 0x44) && d[j + 1] == 0x38 && (d[j + 2] & 0xC7) == 0x05) { len = 7; disp = j + 3; }
      else if (d[j] == 0x38 && (d[j + 1] & 0xC7) == 0x05 && (d[j - 1] & 0xF0) != 0x40) { len = 6; disp = j + 2; }
      if (!len) continue;
      unsigned char n0 = d[j + len], n1 = d[j + len + 1];
      if (!(n0 == 0x74 || n0 == 0x75 || (n0 == 0x0F && (n1 == 0x84 || n1 == 0x85)))) continue;
      unsigned c3; int cr = rva_of((int)j, &c3); int flag = cr + len + i32(disp); int fo = off_of(flag);
      if (fo < 0 || d[fo] != 1) continue;
      if (found) return "several matches";
      g_cmp = cr; g_len = len; g_flag = flag; found = 1; break;
    }
  }
  return found ? NULL : "pattern not found";
}
static FILE *lg;
static void L(const char *fmt, ...) {
  SYSTEMTIME t; GetLocalTime(&t); va_list ap; if (!lg) return;
  fprintf(lg, "%04d-%02d-%02dT%02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_start(ap, fmt); vfprintf(lg, fmt, ap); va_end(ap); fputc('\n', lg); fflush(lg);
}
/* 0 cleared now, 1 already clear, 2 shell32 not loaded yet, <0 not touched */
static int clear_in(DWORD pid, const wchar_t *s32) {
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
  if (!h) return -1;
  HMODULE mods[1024]; DWORD need = 0; int rc = 2;
  if (K32EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_64BIT)) {
    for (DWORD m = 0; m < need / sizeof(HMODULE) && m < 1024; m++) {
      wchar_t name[MAX_PATH]; if (!K32GetModuleFileNameExW(h, mods[m], name, MAX_PATH) || _wcsicmp(name, s32)) continue;
      unsigned char *base = (unsigned char *)mods[m], code[8], f; SIZE_T got;
      int fo = off_of(g_cmp);
      if (!ReadProcessMemory(h, base + g_cmp, code, g_len, &got) || got != (SIZE_T)g_len) { rc = -2; break; }
      if (memcmp(code, d + fo, g_len)) { rc = -3; break; }
      if (!ReadProcessMemory(h, base + g_flag, &f, 1, &got)) { rc = -4; break; }
      if (f == 0) { rc = 1; break; }
      if (f != 1) { rc = -5; break; }
      f = 0; rc = WriteProcessMemory(h, base + g_flag, &f, 1, &got) ? 0 : -6; break;
    }
  }
  CloseHandle(h); return rc;
}
int wmain(void) {
  wchar_t s32[MAX_PATH]; GetSystemDirectoryW(s32, MAX_PATH); wcscat(s32, L"\\shell32.dll");
  CreateDirectoryW(L"C:\\ProgramData\\PSWloader", NULL); CreateDirectoryW(L"C:\\ProgramData\\PSWloader\\logs", NULL);
  lg = _wfopen(L"C:\\ProgramData\\PSWloader\\logs\\hide-edition-native.log", L"a");
  if (lg && ftell(lg) > 65536) { fclose(lg); lg = _wfopen(L"C:\\ProgramData\\PSWloader\\logs\\hide-edition-native.log", L"w"); }
  HANDLE f = CreateFileW(s32, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) { L("shell32.dll not readable"); return 1; }
  dlen = GetFileSize(f, NULL); d = HeapAlloc(GetProcessHeap(), 0, dlen); DWORD rd = 0;
  if (!d || !ReadFile(f, d, dlen, &rd, NULL) || rd != dlen) { L("shell32.dll read failed"); return 1; }
  CloseHandle(f);
  const char *why = find_flag();
  if (why) { L("shell32 (%lu bytes): %s - edition line left as it is", dlen, why); return 0; }
  L("start: flag at RVA 0x%X (compare at 0x%X)", g_flag, g_cmp);
  struct { DWORD pid; ULONGLONG created; } done[64]; int ndone = 0; ULONGLONG t0 = GetTickCount64();
  for (;;) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe)) do {
      if (_wcsicmp(pe.szExeFile, L"explorer.exe")) continue;
      HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID); FILETIME c, x, k, u; ULONGLONG cr = 0;
      if (q) { if (GetProcessTimes(q, &c, &x, &k, &u)) cr = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime; CloseHandle(q); }
      int seen = 0; for (int i = 0; i < ndone; i++) if (done[i].pid == pe.th32ProcessID && done[i].created == cr) seen = 1;
      if (seen) continue;
      int rc = clear_in(pe.th32ProcessID, s32);
      if (rc == 2) continue;   /* shell32 not loaded yet: next round */
      DWORD sess = 0; ProcessIdToSessionId(pe.th32ProcessID, &sess);
      L("explorer %lu (session %lu): %s (%d)", pe.th32ProcessID, sess, rc == 0 ? "edition line hidden" : rc == 1 ? "already hidden" : "not touched", rc);
      if (ndone == 64) ndone = 0;
      done[ndone].pid = pe.th32ProcessID; done[ndone].created = cr; ndone++;
    } while (Process32NextW(snap, &pe));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    Sleep(GetTickCount64() - t0 < 300000 ? 50 : 2000);  /* fast for the first 5 minutes (boot, sign-in); v1.0.1: then 2 s (was 250 ms -
                                                         * a process snapshot 4x per second cost ~1 % CPU around the clock) */
  }
}
