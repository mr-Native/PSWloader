/* PSWUpdater.c - version-independent updater launcher for PSWloader Windows 11 installations.
 * Reads installed version (C:\ProgramData\PSWloader\version.json) and package metadata
 * (<dir>\payload\version.json), confirms the upgrade, warns if PSWloader.elf also needs
 * updating, runs <dir>\engine\PSWUpdate.ps1 elevated, and prompts to restart Windows.
 * v1.1: blocks a downgrade (package older than the installed version) with a notice, exit code 3.
 * v1.2: the dialogs in the look of the PSWloader windows (engine\PSWDialog.ps1, WPF); a plain message box if that fails;
 *         a progress window (engine\PSWProgress.ps1: percentage + one line per step) instead of the console window. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static char *read_file_utf8(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 65536) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    return buf;
}

static int json_get_string(const char *json, const char *key, wchar_t *out, size_t out_cap)
{
    out[0] = 0;
    if (!json || !key) return 0;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len == 0) return 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, p, (int)len, out, (int)(out_cap - 1));
    if (wlen <= 0) { out[0] = 0; return 0; }
    out[wlen] = 0;
    return 1;
}

static int json_get_bool(const char *json, const char *key)
{
    if (!json || !key) return 0;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return strncmp(p, "true", 4) == 0;
}

/* v1.1: version order for the downgrade block - "v1.0.4" < "v1.0.5-beta" < "v1.0.5" < "v1.1"; numbers first, a release
 * ranks above its own pre-releases. Returns -1/0/1, or 2 when one side is not a version (then nothing is blocked). */
static int parse_ver(const wchar_t *s, long n[4], const wchar_t **pre)
{
    int i = 0;
    n[0] = n[1] = n[2] = n[3] = 0; *pre = NULL;
    while (*s == L' ') s++;
    if (*s == L'v' || *s == L'V') s++;
    if (*s < L'0' || *s > L'9') return 0;
    for (;;) {
        long v = 0;
        if (*s < L'0' || *s > L'9') return 0;
        while (*s >= L'0' && *s <= L'9') v = v * 10 + (*s++ - L'0');
        if (i < 4) n[i++] = v;
        if (*s == L'.') { s++; continue; }
        break;
    }
    if (*s == L'-' && s[1]) { *pre = s + 1; return 1; }
    while (*s == L' ') s++;
    return *s == 0;
}

static int ver_cmp(const wchar_t *a, const wchar_t *b)
{
    long x[4], y[4]; const wchar_t *pa, *pb;
    if (!parse_ver(a, x, &pa) || !parse_ver(b, y, &pb)) return 2;
    for (int i = 0; i < 4; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    if (!pa && !pb) return 0;
    if (!pa) return 1;
    if (!pb) return -1;
    int c = wcscmp(pa, pb);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static void read_last_result(wchar_t *status, size_t status_cap,
                             wchar_t *version, size_t ver_cap,
                             wchar_t *logpath, size_t log_cap)
{
    status[0] = version[0] = logpath[0] = 0;
    FILE *f = _wfopen(L"C:\\PSWSetup\\Update\\last-result.txt", L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[1024] = {0};
    if (fgetws(line, 1024, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\r' || line[len - 1] == L'\n')) line[--len] = 0;
        wchar_t *p1 = wcschr(line, L'|');
        if (p1) {
            *p1++ = 0;
            wcsncpy(status, line, status_cap - 1);
            wchar_t *p2 = wcschr(p1, L'|');
            if (p2) {
                *p2++ = 0;
                wcsncpy(version, p1, ver_cap - 1);
                wcsncpy(logpath, p2, log_cap - 1);
            } else {
                wcsncpy(version, p1, ver_cap - 1);
            }
        }
    }
    fclose(f);
}

/* v1.2: dialog through engine\PSWDialog.ps1 (spec file: title bar, heading, button 1, button 2, text - UTF-8);
 * returns 1 = first button, 2 = second button or closed; a plain message box (mb flags) when the WPF dialog cannot run */
static wchar_t g_dir[MAX_PATH];
static int pswdlg(const wchar_t *bar, const wchar_t *head, const wchar_t *text, const wchar_t *b1, const wchar_t *b2, UINT mb)
{
    wchar_t script[MAX_PATH], tmp[MAX_PATH], file[MAX_PATH], ps[MAX_PATH], cmd[2048];
    swprintf(script, MAX_PATH, L"%ls\\engine\\PSWDialog.ps1", g_dir);
    if (GetFileAttributesW(script) != INVALID_FILE_ATTRIBUTES && GetTempPathW(MAX_PATH, tmp)) {
        swprintf(file, MAX_PATH, L"%lspswdlg-%lu.txt", tmp, (unsigned long)GetCurrentProcessId());
        static wchar_t spec[8192];
        swprintf(spec, 8192, L"%ls\n%ls\n%ls\n%ls\n%ls\n", bar, head, b1, b2 ? b2 : L"", text);
        int len = WideCharToMultiByte(CP_UTF8, 0, spec, -1, NULL, 0, NULL, NULL);
        char *u8 = len > 0 ? (char *)malloc((size_t)len) : NULL;
        FILE *f = u8 ? _wfopen(file, L"wb") : NULL;
        if (f) {
            WideCharToMultiByte(CP_UTF8, 0, spec, -1, u8, len, NULL, NULL);
            fwrite(u8, 1, (size_t)(len - 1), f);
            fclose(f);
            GetSystemDirectoryW(ps, MAX_PATH);
            wcscat(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
            swprintf(cmd, 2048, L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%ls\" -Spec \"%ls\"", ps, script, file);
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            DWORD rc = 9;
            if (CreateProcessW(ps, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &rc);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            DeleteFileW(file);
            free(u8);
            if (rc == 1 || rc == 2) return (int)rc;
        } else free(u8);
    }
    static wchar_t m[8192];
    swprintf(m, 8192, L"%ls\n\n%ls", head, text);
    int r = MessageBoxW(NULL, m, bar, mb);
    return (r == IDYES || r == IDOK) ? 1 : 2;
}

static void restart_windows(const wchar_t *why)
{
    wchar_t sd[MAX_PATH], sc[256];
    GetSystemDirectoryW(sd, MAX_PATH);
    wcscat(sd, L"\\shutdown.exe");
    swprintf(sc, 256, L"shutdown.exe /r /t 3 /c \"%ls\"", why);
    STARTUPINFOW s2 = { sizeof(s2) };
    PROCESS_INFORMATION p2;
    if (CreateProcessW(sd, sc, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &s2, &p2)) {
        CloseHandle(p2.hProcess);
        CloseHandle(p2.hThread);
    }
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t dir[MAX_PATH], engine[MAX_PATH], pkg_ver_path[MAX_PATH];
    wchar_t ps[MAX_PATH], cmd[8192], extra[4096] = L"";
    int quiet = 0, no_reboot = 0;

    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    wcscpy(g_dir, dir);

    swprintf(engine, MAX_PATH, L"%ls\\engine\\PSWUpdate.ps1", dir);
    if (GetFileAttributesW(engine) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL,
            L"engine\\PSWUpdate.ps1 was not found next to the updater.\n"
            L"Please extract the entire PSWloader Updater archive before running it.",
            L"PSWloader Updater", MB_ICONERROR);
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/quiet")) { quiet = 1; wcscat(extra, L" -Quiet"); }
        else if (!_wcsicmp(argv[i], L"/noreboot")) { no_reboot = 1; }
        else if (wcslen(extra) + wcslen(argv[i]) + 4 < 4096) {
            wcscat(extra, L" \""); wcscat(extra, argv[i]); wcscat(extra, L"\"");
        }
    }

    /* Read target & installed versions + payloadChanged flag */
    wchar_t target_ver[64] = L"latest";
    wchar_t payload_ver[64] = L"";
    wchar_t cur_ver[64] = L"";
    int payload_changed = 0;

    swprintf(pkg_ver_path, MAX_PATH, L"%ls\\payload\\version.json", dir);
    char *pkg_json = read_file_utf8(pkg_ver_path);
    if (pkg_json) {
        json_get_string(pkg_json, "version", target_ver, 64);
        if (!target_ver[0]) wcscpy(target_ver, L"latest");
        payload_changed = json_get_bool(pkg_json, "payloadChanged");
        if (!json_get_string(pkg_json, "payloadVersion", payload_ver, 64))
            wcsncpy(payload_ver, target_ver, 63);
        free(pkg_json);
    }

    char *cur_json = read_file_utf8(L"C:\\ProgramData\\PSWloader\\version.json");
    if (cur_json) {
        json_get_string(cur_json, "version", cur_ver, 64);
        free(cur_json);
    }

    wchar_t title[128];
    swprintf(title, 128, L"PSWloader Updater %ls", target_ver);

    /* v1.1: never downgrade - an older updater would put older drivers, scripts and firmware files on a newer install */
    if (cur_ver[0] && ver_cmp(target_ver, cur_ver) == -1) {
        if (!quiet) {
            wchar_t msg[1024];
            swprintf(msg, 1024,
                L"PSWloader %ls is installed on this PC. This updater is for the older version %ls.\n\n"
                L"A downgrade is not supported, nothing was changed.\n\n"
                L"Download the current updater from:\nhttps://github.com/mr-Native/PSWloader/releases",
                cur_ver, target_ver);
            if (pswdlg(title, L"A newer PSWloader is installed", msg, L"Open releases", L"Close", MB_OK | MB_ICONWARNING) == 1)
                ShellExecuteW(NULL, L"open", L"https://github.com/mr-Native/PSWloader/releases", NULL, NULL, SW_SHOWNORMAL);
        }
        return 3;
    }

    if (!quiet) {
        wchar_t prompt[1024], head[128];
        int same = cur_ver[0] && _wcsicmp(cur_ver, target_ver) == 0;
        if (same) {
            swprintf(head, 128, L"Reinstall PSWloader %ls", target_ver);
            swprintf(prompt, 1024,
                L"PSWloader %ls is already installed.\n\n"
                L"Reinstall it and check all drivers and tools?",
                target_ver);
        } else if (cur_ver[0]) {
            swprintf(head, 128, L"Update PSWloader to %ls", target_ver);
            swprintf(prompt, 1024,
                L"Update PSWloader from %ls to %ls?\n\n"
                L"Drivers, tools and the firmware for the next start are updated. Windows restarts at the end.",
                cur_ver, target_ver);
        } else {
            swprintf(head, 128, L"Update PSWloader to %ls", target_ver);
            swprintf(prompt, 1024,
                L"Update your PSWloader installation to %ls?\n\n"
                L"Drivers, tools and the firmware for the next start are updated. Windows restarts at the end.",
                target_ver);
        }
        if (pswdlg(title, head, prompt, same ? L"Reinstall" : L"Update", L"Cancel", MB_YESNO | MB_ICONQUESTION) != 1) {
            return 0;
        }

        if (payload_changed) {
            wchar_t pmsg[1024];
            swprintf(pmsg, 1024,
                L"This update also needs PSWloader.elf %ls on your PS5.\n\n"
                L"Replace PSWloader.elf in your Payload Manager after the restart at the end of this update.",
                payload_ver[0] ? payload_ver : target_ver);
            if (pswdlg(title, L"PSWloader.elf needs an update too", pmsg, L"Continue", L"Cancel",
                       MB_YESNO | MB_ICONWARNING) != 1) {
                return 0;
            }
        }
    }

    GetSystemDirectoryW(ps, MAX_PATH);
    wcscat(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    swprintf(cmd, 8192, L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -File \"%ls\" -Root \"%ls\"%ls",
             ps, engine, dir, extra);

    /* v1.2: progress window - PSWUpdate.ps1 appends steps to the progress file, "END" closes the window */
    wchar_t prog_file[] = L"C:\\PSWSetup\\Update\\progress.txt", prog_script[MAX_PATH], prog_cmd[2048];
    PROCESS_INFORMATION ppi = { 0 };
    int prog = 0;
    swprintf(prog_script, MAX_PATH, L"%ls\\engine\\PSWProgress.ps1", dir);
    if (!quiet && GetFileAttributesW(prog_script) != INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(L"C:\\PSWSetup", NULL);
        CreateDirectoryW(L"C:\\PSWSetup\\Update", NULL);
        HANDLE h = CreateFileW(prog_file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        swprintf(prog_cmd, 2048, L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%ls\" -Path \"%ls\" -Version \"%ls\"",
                 ps, prog_script, prog_file, target_ver);
        STARTUPINFOW psi = { sizeof(psi) };
        prog = CreateProcessW(ps, prog_cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &psi, &ppi) ? 1 : 0;
    }
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(ps, cmd, NULL, NULL, FALSE, prog ? CREATE_NO_WINDOW : 0, NULL, dir, &si, &pi)) {
        if (prog) { TerminateProcess(ppi.hProcess, 0); CloseHandle(ppi.hProcess); CloseHandle(ppi.hThread); }
        if (!quiet) pswdlg(title, L"PSWloader update failed", L"Windows PowerShell could not be started.", L"Close", NULL, MB_ICONERROR);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (prog) {   /* the last step stays visible a moment, then "END" closes the progress window */
        Sleep(1500);
        FILE *pf = _wfopen(prog_file, L"ab");
        if (pf) { fputs("END\r\n", pf); fclose(pf); }
        if (WaitForSingleObject(ppi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(ppi.hProcess, 0);
        CloseHandle(ppi.hProcess);
        CloseHandle(ppi.hThread);
    }

    wchar_t st[32], ver[64], logpath[MAX_PATH], text[2048];
    read_last_result(st, 32, ver, 64, logpath, MAX_PATH);
    if (!ver[0]) wcsncpy(ver, target_ver, 63);
    if (!logpath[0]) wcscpy(logpath, L"C:\\PSWSetup\\Update\\");

    if (quiet) return (int)rc;

    if (rc == 0) {
        swprintf(text, 2048,
            L"The update to %ls completed successfully.\n\n"
            L"Restart now to finish the update.\n\n"
            L"Log: %ls",
            ver, logpath);
        wchar_t hd[128];
        swprintf(hd, 128, L"PSWloader updated to %ls", ver);
        if (!no_reboot && pswdlg(title, hd, text, L"Restart now", L"Later", MB_YESNO | MB_ICONINFORMATION) == 1)
            restart_windows(L"PSWloader update completed");
    } else if (rc == 1) {
        swprintf(text, 2048,
            L"The update to %ls finished, but some components could not be updated.\n\n"
            L"Restart now to apply the updated components, then check the log.\n\n"
            L"Log: %ls",
            ver, logpath);
        if (!no_reboot && pswdlg(title, L"PSWloader update needs a look", text, L"Restart now", L"Later", MB_YESNO | MB_ICONWARNING) == 1)
            restart_windows(L"PSWloader update completed with warnings");
    } else {
        swprintf(text, 2048,
            L"The update to %ls failed (exit code %lu). Nothing needs a restart.\n\n"
            L"Log: %ls",
            ver, (unsigned long)rc, logpath);
        if (pswdlg(title, L"PSWloader update failed", text, L"Open log", L"Close", MB_OK | MB_ICONERROR) == 1)
            ShellExecuteW(NULL, L"open", logpath, NULL, NULL, SW_SHOWNORMAL);
    }

    return (int)rc;
}
