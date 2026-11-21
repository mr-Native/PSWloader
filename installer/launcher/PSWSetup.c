/* PSWSetup.exe v0.3.0 (17.11.2026; v0.3.0: PSWloader names; v0.2.0 16.11.) - launcher of "PSWloader Setup". Runs, from its own folder:
 *   powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<dir>\engine\PSWSetup.ps1" -Mode Install [options]
 * Options: /quiet (no dialogs, restarts by itself), /detect, /uninstall; anything else is passed to the engine verbatim
 * (e.g. -Include Remote). Elevation via manifest (requireAdministrator). Exit code = engine exit code (0, 3010, 1, 2). */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static void msg(const wchar_t *text, UINT flags, int quiet) { if (!quiet) MessageBoxW(NULL, text, L"PSWloader Setup", flags); }

int wmain(int argc, wchar_t **argv)
{
    wchar_t dir[MAX_PATH], engine[MAX_PATH], ps[MAX_PATH], cmd[8192], extra[4096] = L"";
    const wchar_t *mode = L"Install"; int quiet = 0;
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    swprintf(engine, MAX_PATH, L"%ls\\engine\\PSWSetup.ps1", dir);
    if (GetFileAttributesW(engine) == INVALID_FILE_ATTRIBUTES) { msg(L"engine\\PSWSetup.ps1 not found next to PSWSetup.exe.\nCopy the whole PSWSetup folder.", MB_ICONERROR, 0); return 2; }
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/quiet")) { quiet = 1; wcscat(extra, L" -Quiet"); }
        else if (!_wcsicmp(argv[i], L"/detect")) mode = L"Detect";
        else if (!_wcsicmp(argv[i], L"/uninstall")) mode = L"Uninstall";
        else if (wcslen(extra) + wcslen(argv[i]) + 4 < 4096) { wcscat(extra, L" \""); wcscat(extra, argv[i]); wcscat(extra, L"\""); }
    }
    GetSystemDirectoryW(ps, MAX_PATH); wcscat(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    swprintf(cmd, 8192, L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -File \"%ls\" -Mode %ls%ls", ps, engine, mode, extra);
    wprintf(L"PSWloader Setup 0.3.0\n%ls\n\n", cmd);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(ps, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { msg(L"Could not start Windows PowerShell.", MB_ICONERROR, quiet); return 1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (!wcscmp(mode, L"Detect")) { if (!quiet) { wprintf(L"\nPress Enter to close."); getwchar(); } return (int)rc; }
    if (rc == 3010) {
        if (quiet) return (int)rc;   /* the engine schedules the restart itself in quiet mode */
        if (MessageBoxW(NULL, L"Step done. Windows must restart.\nAfter the restart the setup continues by itself (log: C:\\PSWSetup\\logs\\).\n\nRestart now?",
                        L"PSWloader Setup", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            wchar_t sd[MAX_PATH]; GetSystemDirectoryW(sd, MAX_PATH); wcscat(sd, L"\\shutdown.exe");
            wchar_t sc[] = L"shutdown.exe /r /t 5"; STARTUPINFOW s2 = { sizeof s2 }; PROCESS_INFORMATION p2;
            if (CreateProcessW(sd, sc, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &s2, &p2)) { CloseHandle(p2.hProcess); CloseHandle(p2.hThread); }
        }
    } else if (rc == 0) msg(L"Done. Everything is installed.\nLog: C:\\PSWSetup\\logs\\", MB_ICONINFORMATION, quiet);
    else if (rc == 2) msg(L"This PC is not supported (needs the PSWloader firmware, Windows 11, Secure Boot off).\nDetails: C:\\PSWSetup\\logs\\", MB_ICONWARNING, quiet);
    else msg(L"Setup stopped with an error.\nDetails: C:\\PSWSetup\\logs\\", MB_ICONERROR, quiet);
    return (int)rc;
}
