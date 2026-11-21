/* ps5-powerbutton.exe v1.0 (16.11.2026) - user-mode listener for the PS5 power button (ps5pwr v1.10, README "Listener").
 * Runs as SYSTEM (scheduled task PS5-PowerButton, at startup). Loop: OpenEvent(SYNCHRONIZE, Global\Ps5PowerButton), retry
 * every 5 s while the driver is not there -> WaitForSingleObject -> run the Windows power-button action of the active power
 * scheme (AC value of SUB_BUTTONS / PBUTTONACTION): 0 nothing, 3 shut down; 1 sleep / 2 hibernate fall back to shut down
 * (PS5 has no working Sx); 4 display off is logged only (a SYSTEM process cannot turn off the user's display).
 * Debounce: presses within 10 s after a shutdown was started are ignored. Single instance (Global\Ps5PowerButtonListener).
 * Log: C:\ps5-work\powerbutton.log. Kill switch stays in the driver (ps5pwr\Parameters\EnablePowerButton=0). */
#include <windows.h>
#include <powrprof.h>
#include <stdio.h>

static const GUID SUB_BUTTONS_G = {0x4f971e89, 0xeebd, 0x4455, {0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47}};
static const GUID PBUTTONACTION_G = {0x7648efa3, 0xdd9c, 0x4e3e, {0xb5, 0x66, 0x50, 0xf9, 0x29, 0x38, 0x62, 0x80}};

static void logf_(const char *fmt, ...)
{
    CreateDirectoryA("C:\\ps5-work", NULL);
    FILE *f = fopen("C:\\ps5-work\\powerbutton.log", "a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

static DWORD button_action(void)
{
    GUID *scheme = NULL; DWORD v = 3;   /* PS5 default: shut down */
    if (PowerGetActiveScheme(NULL, &scheme) == ERROR_SUCCESS) {
        if (PowerReadACValueIndex(NULL, scheme, &SUB_BUTTONS_G, &PBUTTONACTION_G, &v) != ERROR_SUCCESS) v = 3;
        LocalFree(scheme);
    }
    return v;
}

static int enable_shutdown_privilege(void)
{
    HANDLE tok; TOKEN_PRIVILEGES tp = {0};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return 0;
    LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok); return ok;
}

static void do_shutdown(void)
{
    DWORD rc = InitiateShutdownW(NULL, NULL, 0, SHUTDOWN_POWEROFF | SHUTDOWN_FORCE_OTHERS,
                                 SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
    logf_("shutdown requested rc=%lu", rc);
    if (rc != ERROR_SUCCESS && rc != ERROR_SHUTDOWN_IN_PROGRESS) {   /* fallback: the stock tool */
        STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi; char cmd[] = "shutdown.exe /s /t 0 /d p:0:0";
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); logf_("fallback shutdown.exe started"); }
        else logf_("fallback shutdown.exe failed %lu", GetLastError());
    }
}

int main(void)
{
    HANDLE single = CreateMutexW(NULL, TRUE, L"Global\\Ps5PowerButtonListener");
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) { logf_("another listener runs - exit"); return 0; }
    logf_("listener v1.0 start, shutdown privilege=%d", enable_shutdown_privilege());
    ULONGLONG shutdown_at = 0; int waiting_logged = 0;
    for (;;) {
        HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, L"Global\\Ps5PowerButton");
        if (!ev) { if (!waiting_logged) { logf_("event not available (%lu) - driver not loaded or button off; retry every 5 s", GetLastError()); waiting_logged = 1; } Sleep(5000); continue; }
        logf_("event opened, waiting for presses"); waiting_logged = 0;
        for (;;) {
            DWORD w = WaitForSingleObject(ev, INFINITE);
            if (w != WAIT_OBJECT_0) { logf_("wait failed %lu - reopen", GetLastError()); break; }
            ULONGLONG now = GetTickCount64();
            if (shutdown_at && now - shutdown_at < 10000) { logf_("press ignored (shutdown already started %llu ms ago)", now - shutdown_at); continue; }
            DWORD act = button_action();
            logf_("press: PBUTTONACTION(AC)=%lu", act);
            switch (act) {
            case 0: logf_("action: do nothing"); break;
            case 4: logf_("action: turn off display - not possible from SYSTEM, ignored"); break;
            case 1: case 2: logf_("action %lu (sleep/hibernate) not supported on PS5 -> shut down", act); /* fall through */
            default: shutdown_at = now; do_shutdown(); break;
            }
        }
        CloseHandle(ev);
        Sleep(1000);
    }
}
