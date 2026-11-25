/* PSWUpdater.c - version-independent updater launcher for PSWloader Windows 11 installations.
 * Reads installed version (C:\ProgramData\PSWloader\version.json) and package metadata
 * (<dir>\payload\version.json), confirms the upgrade, warns if PSWloader.elf also needs
 * updating, runs <dir>\engine\PSWUpdate.ps1 elevated, and prompts to restart Windows.
 * v1.1: blocks a downgrade (package older than the installed version) with a notice, exit code 3. */
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

int wmain(int argc, wchar_t **argv)
{
    wchar_t dir[MAX_PATH], engine[MAX_PATH], pkg_ver_path[MAX_PATH];
    wchar_t ps[MAX_PATH], cmd[8192], extra[4096] = L"";
    int quiet = 0, no_reboot = 0;

    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

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
    swprintf(title, 128, L"PSWloader Updater (%ls)", target_ver);

    /* v1.1: never downgrade - an older updater would put older drivers, scripts and firmware files on a newer install */
    if (cur_ver[0] && ver_cmp(target_ver, cur_ver) == -1) {
        if (!quiet) {
            wchar_t msg[1024];
            swprintf(msg, 1024,
                L"PSWloader %ls is installed on this PC.\n\n"
                L"This updater is for the older version %ls. A downgrade is not supported, nothing was changed.\n\n"
                L"Please download the current updater from:\nhttps://github.com/mr-Native/PSWloader/releases",
                cur_ver, target_ver);
            MessageBoxW(NULL, msg, title, MB_OK | MB_ICONWARNING);
        }
        return 3;
    }

    if (!quiet) {
        wchar_t prompt[1024];
        if (cur_ver[0] && _wcsicmp(cur_ver, target_ver) == 0) {
            swprintf(prompt, 1024,
                L"PSWloader %ls is already installed.\n\n"
                L"Do you want to reinstall and verify %ls?",
                target_ver, target_ver);
        } else if (cur_ver[0]) {
            swprintf(prompt, 1024,
                L"Do you want to update PSWloader from %ls to %ls?",
                cur_ver, target_ver);
        } else {
            swprintf(prompt, 1024,
                L"Do you want to update your PSWloader installation to %ls?",
                target_ver);
        }
        if (MessageBoxW(NULL, prompt, title, MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return 0;
        }

        if (payload_changed) {
            wchar_t pmsg[1024];
            swprintf(pmsg, 1024,
                L"This update also requires updating PSWloader.elf on your PS5 to %ls.\n\n"
                L"You can replace PSWloader.elf in your Payload Manager after the reboot "
                L"at the end of this update.\n\n"
                L"Continue with the update?",
                payload_ver[0] ? payload_ver : target_ver);
            if (MessageBoxW(NULL, pmsg, L"PSWloader Payload Update Required",
                            MB_YESNO | MB_ICONWARNING) != IDYES) {
                return 0;
            }
        }
    }

    GetSystemDirectoryW(ps, MAX_PATH);
    wcscat(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    swprintf(cmd, 8192, L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -File \"%ls\" -Root \"%ls\"%ls",
             ps, engine, dir, extra);

    wprintf(L"PSWloader Updater (%ls)\n%ls\n\n", target_ver, cmd);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(ps, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        if (!quiet) MessageBoxW(NULL, L"Could not start Windows PowerShell.", title, MB_ICONERROR);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    wchar_t st[32], ver[64], logpath[MAX_PATH], text[2048];
    read_last_result(st, 32, ver, 64, logpath, MAX_PATH);
    if (!ver[0]) wcsncpy(ver, target_ver, 63);
    if (!logpath[0]) wcscpy(logpath, L"C:\\PSWSetup\\Update\\");

    if (quiet) return (int)rc;

    if (rc == 0) {
        swprintf(text, 2048,
            L"PSWloader update to %ls completed successfully!\n\n"
            L"Log file:\n%ls\n\n"
            L"The system must be restarted to complete the update.\n"
            L"Do you want to restart now?",
            ver, logpath);
        if (!no_reboot &&
            MessageBoxW(NULL, text, title, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            wchar_t sd[MAX_PATH];
            GetSystemDirectoryW(sd, MAX_PATH);
            wcscat(sd, L"\\shutdown.exe");
            wchar_t sc[] = L"shutdown.exe /r /t 3 /c \"PSWloader update completed\"";
            STARTUPINFOW s2 = { sizeof(s2) };
            PROCESS_INFORMATION p2;
            if (CreateProcessW(sd, sc, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &s2, &p2)) {
                CloseHandle(p2.hProcess);
                CloseHandle(p2.hThread);
            }
        }
    } else if (rc == 1) {
        swprintf(text, 2048,
            L"PSWloader update to %ls completed with warnings.\n"
            L"Some components could not be updated.\n\n"
            L"Please check the log file:\n%ls\n\n"
            L"The system must be restarted to apply the updated components.\n"
            L"Do you want to restart now?",
            ver, logpath);
        if (!no_reboot &&
            MessageBoxW(NULL, text, title, MB_YESNO | MB_ICONWARNING) == IDYES) {
            wchar_t sd[MAX_PATH];
            GetSystemDirectoryW(sd, MAX_PATH);
            wcscat(sd, L"\\shutdown.exe");
            wchar_t sc[] = L"shutdown.exe /r /t 3 /c \"PSWloader update completed with warnings\"";
            STARTUPINFOW s2 = { sizeof(s2) };
            PROCESS_INFORMATION p2;
            if (CreateProcessW(sd, sc, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &s2, &p2)) {
                CloseHandle(p2.hProcess);
                CloseHandle(p2.hThread);
            }
        }
    } else {
        swprintf(text, 2048,
            L"PSWloader update to %ls failed (exit code %lu).\n\n"
            L"Please check the log file for details:\n%ls",
            ver, (unsigned long)rc, logpath);
        MessageBoxW(NULL, text, title, MB_OK | MB_ICONERROR);
    }

    return (int)rc;
}
