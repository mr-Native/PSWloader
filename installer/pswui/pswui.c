/* pswui.c v1.4 (PSWloader v1.2; v1.4: the install start is the $WINDOWS.~BT folder Setup creates on the target (it applies the image to $WINDOWS.~BT\NewOS first); v1.3: full screen while the system is checked, restart screen; v1.2: stays open (no close button), shows when Windows Setup starts installing; v1.1: fonts that exist in Windows PE) - status window for Windows Setup (Windows PE) in the look of the PSWloader windows.
 * Windows PE has no PowerShell and no .NET, so the WPF windows of the installed system cannot be used there; this is plain
 * Win32 + GDI, static, no runtime DLLs. psw-drivers.cmd keeps doing its work in its console and only reports here:
 *
 *   pswui.exe <status file> [console title prefix]      window; the console with that title is hidden while this runs
 *   pswui.exe <status file> --bmp <out.bmp>             render the current state into a picture (test)
 *
 * Status file (appended line by line, the first character is the kind):
 *   H<text>  heading                      S<text>  step started (the step before it is done)
 *   W<text>  warning line                 D<text>  done: all steps finished
 *   E        the script has ended (with warnings, so without a D line): the last step counts as done
 *   F        full screen "Checking the system ..." over Windows Setup (v1.3, written first)
 *   N        back to the small window: Windows Setup can be used (v1.3)
 *   R        full screen "Restart needed to detect the drives" - the script restarts the console (v1.3)
 *   X        close now
 * v1.2 (user 26.11.2026): the window has no close button and stays open while Windows Setup runs. When Setup starts to
 * install - a \Windows\System32 folder appears on a disk after this window started - it shows "Windows is being installed"
 * and whether PSWloader prepared everything without errors.
 * Open source, part of PSWloader (https://github.com/mr-Native/PSWloader). */
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>

#define W0 640
#define MAXL 24
#define C_BG   RGB(0x1B, 0x1B, 0x1B)
#define C_FOOT RGB(0x15, 0x15, 0x15)
#define C_EDGE RGB(0x38, 0x38, 0x38)
#define C_TEXT RGB(0xF2, 0xF2, 0xF0)
#define C_DIM  RGB(0xB4, 0xB4, 0xAE)
#define C_BLUE RGB(0x3A, 0x86, 0xE0)
#define C_OK   RGB(0x7B, 0xC6, 0x7E)
#define C_WARN RGB(0xE8, 0xA3, 0x3D)

static wchar_t gFile[MAX_PATH], gTitle[128];
static wchar_t gHead[160] = L"PSWloader";
static struct { wchar_t kind; wchar_t text[200]; } gLine[MAXL];
static int gLines, gDone, gClose, gWarn, gEnded, gScale = 96, gH = 300;
static DWORD gDoneTick, gLastSize = (DWORD)-1, gStartTick;
static FILETIME gStartFt; static int gInstalling, gPoll, gFull, gRestart;
static HWND gConsole;

static int S(int v) { return MulDiv(v, gScale, 96); }

static int load(void)   /* 1 = changed */
{
    HANDLE h = CreateFileW(gFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    if (sz == gLastSize || sz > 65536) { CloseHandle(h); return 0; }
    static char raw[65537]; static wchar_t w[65537];
    if (!ReadFile(h, raw, sz, &rd, NULL)) { CloseHandle(h); return 0; }
    CloseHandle(h); gLastSize = sz; raw[rd] = 0;
    int n = MultiByteToWideChar(CP_OEMCP, 0, raw, (int)rd, w, 65536); w[n] = 0;   /* cmd "echo >>" writes the OEM code page */
    gLines = 0; gDone = 0; gWarn = 0; gFull = 0; gRestart = 0;
    for (wchar_t *p = w; *p;) {
        wchar_t *e = p; while (*e && *e != L'\r' && *e != L'\n') e++;
        wchar_t k = *p; int len = (int)(e - p) - 1;
        while (len > 0 && p[len] == L' ') len--;   /* "echo text >> file" leaves a trailing blank */
        if (len > 199) len = 199;
        if (k == L'X') gClose = 1;
        else if (k == L'E') gEnded = 1;
        else if (k == L'F') gFull = 1;
        else if (k == L'N') gFull = 0;
        else if (k == L'R') { gFull = 1; gRestart = 1; }
        else if (k == L'H' && len > 0) { if (len > 159) len = 159; lstrcpynW(gHead, p + 1, len + 1); }
        else if ((k == L'S' || k == L'W' || k == L'D') && len > 0 && gLines < MAXL) {
            gLine[gLines].kind = k; lstrcpynW(gLine[gLines].text, p + 1, len + 1); gLines++;
            if (k == L'D') { if (!gDone) gDoneTick = GetTickCount(); gDone = 1; }
            if (k == L'W') gWarn = 1;
        }
        p = e; while (*p == L'\r' || *p == L'\n') p++;
    }
    return 1;
}

/* v1.1: Windows Setup's Windows PE has no regular "Segoe UI" (only Bold, Italic, Light, Semilight, Semibold ...): asking for
 * "Segoe UI" at weight 400 gave the italic face there (test in Windows Setup 26300). Every candidate is realised and only
 * taken when the face is the one asked for and upright; Tahoma is in every Windows and Windows PE. */
static HFONT try_font(int px, int weight, const wchar_t *face)
{
    HFONT f = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    if (!f) return NULL;
    HDC dc = GetDC(NULL); HGDIOBJ o = SelectObject(dc, f); TEXTMETRICW tm; wchar_t real[64] = L"";
    GetTextMetricsW(dc, &tm); GetTextFaceW(dc, 64, real); SelectObject(dc, o); ReleaseDC(NULL, dc);
    if (tm.tmItalic || lstrcmpiW(real, face)) { DeleteObject(f); return NULL; }
    return f;
}
static HFONT font(int px, int weight)
{
    static const wchar_t *reg[] = { L"Segoe UI", L"Segoe UI Semilight", L"Tahoma", L"Microsoft Sans Serif" };
    static const wchar_t *bld[] = { L"Segoe UI Semibold", L"Segoe UI", L"Tahoma" };
    HFONT f;
    if (weight >= 600) { for (int i = 0; i < 3; i++) if ((f = try_font(px, i == 0 ? 600 : 700, bld[i])) != NULL) return f; }
    else { for (int i = 0; i < 4; i++) if ((f = try_font(px, 400, reg[i])) != NULL) return f; }
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Tahoma");
}
static void fill(HDC dc, int x, int y, int w, int h, COLORREF c) { RECT r = { x, y, x + w, y + h }; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, const wchar_t *t)
{
    RECT r = { x, y, x + w, y + S(40) }; HGDIOBJ o = SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS); SelectObject(dc, o);
}
/* small shapes drawn 4x larger and scaled down (GDI has no anti-aliasing) */
static void smooth(HDC dc, int x, int y, int size, int kind, COLORREF c, COLORREF bg)
{
    int n = size * 4; HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, n, n); HGDIOBJ ob = SelectObject(m, b);
    fill(m, 0, 0, n, n, bg);
    HBRUSH br = CreateSolidBrush(c); HGDIOBJ obr = SelectObject(m, br); HPEN np = (HPEN)GetStockObject(NULL_PEN); HGDIOBJ op = SelectObject(m, np);
    if (kind == 0 || kind == 1) Ellipse(m, 0, 0, n + 1, n + 1);   /* 0 = logo circle, 1 = dot */
    LOGBRUSH lb = { BS_SOLID, kind == 0 ? bg : c, 0 };
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (kind == 0 ? n * 32 / 300 : n * 14 / 100), &lb, 0, NULL);
    SelectObject(m, pen);
    if (kind == 0) {   /* two chevrons, coordinates of the 30x30 logo */
        POINT a[3] = { { n * 100 / 300, n * 90 / 300 }, { n * 155 / 300, n * 150 / 300 }, { n * 100 / 300, n * 210 / 300 } };
        POINT d[3] = { { n * 160 / 300, n * 90 / 300 }, { n * 215 / 300, n * 150 / 300 }, { n * 160 / 300, n * 210 / 300 } };
        Polyline(m, a, 3); Polyline(m, d, 3);
    } else if (kind == 2) {   /* check mark */
        POINT a[3] = { { n * 18 / 100, n * 52 / 100 }, { n * 42 / 100, n * 76 / 100 }, { n * 84 / 100, n * 26 / 100 } }; Polyline(m, a, 3);
    } else if (kind == 3) {   /* warning: exclamation mark */
        POINT a[2] = { { n / 2, n * 16 / 100 }, { n / 2, n * 58 / 100 } }; Polyline(m, a, 2);
        POINT d[2] = { { n / 2, n * 84 / 100 }, { n / 2, n * 85 / 100 } }; Polyline(m, d, 2);
    } else if (kind == 4) {   /* close cross */
        POINT a[2] = { { n * 20 / 100, n * 20 / 100 }, { n * 80 / 100, n * 80 / 100 } }; Polyline(m, a, 2);
        POINT d[2] = { { n * 80 / 100, n * 20 / 100 }, { n * 20 / 100, n * 80 / 100 } }; Polyline(m, d, 2);
    }
    SelectObject(m, op); SelectObject(m, obr); DeleteObject(pen); DeleteObject(br);
    SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL); StretchBlt(dc, x, y, size, size, m, 0, 0, n, n, SRCCOPY);
    SelectObject(m, ob); DeleteObject(b); DeleteDC(m);
}

/* v1.2: Windows Setup has started to install when it creates its work folder on a disk after this window started.
 * v1.4 (HW 26.11.: still "Ready" at 11 %): Setup applies the image to <target>:\$WINDOWS.~BT\NewOS first and moves it later,
 * so the sign is the $WINDOWS.~BT folder (created by "Creating new system"), \Windows\System32 counts too. Drives with
 * an older Windows or an older $WINDOWS.~BT do not count (creation time before this window). */
static int setup_installing(void)
{
    DWORD m = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; c++) {
        if (!(m & (1u << (c - L'A'))) || c == L'X') continue;
        wchar_t root[4] = { c, L':', L'\\', 0 }, dir[32];
        UINT t = GetDriveTypeW(root); if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        for (int v = 0; v < 2; v++) {
            wsprintfW(dir, v ? L"%c:\\Windows\\System32" : L"%c:\\$WINDOWS.~BT", c);
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(dir, GetFileExInfoStandard, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                CompareFileTime(&a.ftCreationTime, &gStartFt) > 0) return 1;
        }
    }
    return 0;
}

static int height(void) { int rows = gInstalling ? 3 : (gLines < 3 ? 3 : gLines); return S(30 + 30 + 18 + 36 + 14 + rows * 26 + 20 + 46); }

/* v1.3: full screen over Windows Setup while the system is checked, and the restart screen */
static void paint_full(HDC dc, int w, int h)
{
    HFONT fBrand = font(34, 700), fHead = font(30, 600), fLine = font(17, 400);
    fill(dc, 0, 0, w, h, C_BG);
    int bw = S(560), x = (w - bw) / 2, y = h / 2 - S(130);
    smooth(dc, x, y, S(52), 0, C_BLUE, C_BG);
    SIZE sz; HGDIOBJ o = SelectObject(dc, fBrand); GetTextExtentPoint32W(dc, L"PSW", 3, &sz); SelectObject(dc, o);
    text(dc, fBrand, C_TEXT, x + S(68), y + S(4), S(300), L"PSW"); text(dc, fBrand, C_BLUE, x + S(68) + sz.cx, y + S(4), S(300), L"loader");
    y += S(52 + 40);
    text(dc, fHead, C_TEXT, x, y, bw + S(200), gRestart ? L"Restart needed to detect the drives" : L"Checking the system ...");
    y += S(54);
    text(dc, fLine, C_DIM, x, y, bw + S(260), gRestart ? L"PSWloader restarts the console now. Windows Setup then starts again" : L"PSWloader prepares Windows Setup for the PS5. This takes a moment.");
    if (gRestart) { y += S(28); text(dc, fLine, C_DIM, x, y, bw + S(260), L"and lists the drives."); }
    y += S(46);
    int dots = (int)((GetTickCount() / 400) % 4);   /* simple activity mark */
    for (int i = 0; i < 3; i++) smooth(dc, x + i * S(22), y, S(10), 1, i < dots ? C_BLUE : RGB(0x3A, 0x3A, 0x3A), C_BG);
    DeleteObject(fBrand); DeleteObject(fHead); DeleteObject(fLine);
}

static void paint(HDC dc, int w, int h)
{
    if (gFull) { paint_full(dc, w, h); return; }
    HFONT fBrand = font(20, 700), fHead = font(22, 600), fLine = font(14, 400), fBold = font(14, 600), fFoot = font(12, 400);
    fill(dc, 0, 0, w, h, C_BG);
    int x = S(36), y = S(30);
    smooth(dc, x, y, S(30), 0, C_BLUE, C_BG);
    SIZE sz; HGDIOBJ o = SelectObject(dc, fBrand); GetTextExtentPoint32W(dc, L"PSW", 3, &sz); SelectObject(dc, o);
    text(dc, fBrand, C_TEXT, x + S(40), y + S(1), S(200), L"PSW"); text(dc, fBrand, C_BLUE, x + S(40) + sz.cx, y + S(1), S(200), L"loader");
    if (gInstalling) {   /* v1.2: Windows Setup installs now */
        y += S(30 + 18); text(dc, fHead, C_TEXT, x, y, w - 2 * x, L"Windows is being installed");
        y += S(36 + 14);
        if (gWarn) { smooth(dc, x, y + S(3), S(14), 3, C_WARN, C_BG); text(dc, fBold, C_TEXT, x + S(28), y, w - 2 * x - S(28), L"PSWloader prepared Windows Setup with notes - see PSW\\psw-drivers.log."); }
        else { smooth(dc, x, y + S(3), S(14), 2, C_OK, C_BG); text(dc, fLine, C_DIM, x + S(28), y, w - 2 * x - S(28), L"PSWloader prepared everything without errors."); }
        y += S(26); smooth(dc, x, y + S(3), S(14), 2, C_OK, C_BG); text(dc, fLine, C_DIM, x + S(28), y, w - 2 * x - S(28), L"Windows Setup started the installation as planned.");
        y += S(26); smooth(dc, x + S(2), y + S(5), S(10), 1, C_BLUE, C_BG); text(dc, fBold, C_TEXT, x + S(28), y, w - 2 * x - S(28), L"Copying and installing Windows ...");
    } else {
    y += S(30 + 18); text(dc, fHead, C_TEXT, x, y, w - 2 * x, gHead);
    y += S(36 + 14);
    for (int i = 0; i < gLines; i++, y += S(26)) {
        int last = (i == gLines - 1), k = gLine[i].kind;
        if (k == L'W') { if (i == 0 || gLine[i - 1].kind != L'W') smooth(dc, x, y + S(3), S(14), 3, C_WARN, C_BG);   /* mark on the first line of a note */
                         text(dc, fBold, C_TEXT, x + S(28), y, w - 2 * x - S(28), gLine[i].text); }
        else if (k == L'D' || !last || gDone || gEnded) { smooth(dc, x, y + S(3), S(14), 2, C_OK, C_BG); text(dc, k == L'D' ? fBold : fLine, k == L'D' ? C_TEXT : C_DIM, x + S(28), y, w - 2 * x - S(28), gLine[i].text); }
        else { smooth(dc, x + S(2), y + S(5), S(10), 1, C_BLUE, C_BG); text(dc, fBold, C_TEXT, x + S(28), y, w - 2 * x - S(28), gLine[i].text); }
    }
    }
    fill(dc, 0, h - S(46), w, S(46), C_FOOT); fill(dc, 0, h - S(46), w, 1, RGB(0x2C, 0x2C, 0x2C));
    text(dc, fFoot, RGB(0x9A, 0x9A, 0x95), x, h - S(46) + S(14), w - 2 * x,
         gInstalling ? L"The console restarts by itself during the installation. Do not switch it off."
         : gWarn ? L"The log is in PSW\\psw-drivers.log on the installer drive."
         : gDone ? L"Go on in the Windows Setup window. This window shows when the installation starts."
         : L"Please wait - Windows Setup goes on when this is finished.");
    HBRUSH eb = CreateSolidBrush(C_EDGE); RECT r = { 0, 0, w, h }; FrameRect(dc, &r, eb); DeleteObject(eb);
    DeleteObject(fBrand); DeleteObject(fHead); DeleteObject(fLine); DeleteObject(fBold); DeleteObject(fFoot);
}

static int gWasFull = -1;
static void fit(HWND hw)
{
    if (gFull) {   /* v1.3: the whole screen */
        if (gWasFull == 1) return;
        gWasFull = 1; gH = -1;
        SetWindowPos(hw, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_NOACTIVATE); return;
    }
    int h = height(); if (h == gH && gWasFull == 0) return; gH = h; gWasFull = 0;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(hw, HWND_TOPMOST, wa.left + S(24), wa.bottom - gH - S(24), S(W0), gH, SWP_NOACTIVATE);   /* v1.1: bottom left - Setup's controls are above, "Next" is at the right */
}

static LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (load()) { fit(hw); InvalidateRect(hw, NULL, FALSE); }
        else if (gFull) InvalidateRect(hw, NULL, FALSE);   /* v1.3: activity dots */
        if (gConsole == NULL && gTitle[0]) {   /* hide the console once it is there (its text is shown here) */
            for (HWND c = FindWindowW(L"ConsoleWindowClass", NULL); c; c = FindWindowExW(NULL, c, L"ConsoleWindowClass", NULL)) {
                wchar_t t[160]; GetWindowTextW(c, t, 160);
                if (wcsstr(t, gTitle)) { gConsole = c; ShowWindow(c, SW_HIDE); break; } }
        }
        if (!gInstalling && ++gPoll % 8 == 0 && setup_installing()) { gInstalling = 1; fit(hw); InvalidateRect(hw, NULL, FALSE); }   /* every 2 s */
        if (gClose) DestroyWindow(hw);   /* v1.2: otherwise the window stays open while Windows Setup runs */
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps); RECT r; GetClientRect(hw, &r);
        HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, r.right, r.bottom); HGDIOBJ o = SelectObject(m, b);
        paint(m, r.right, r.bottom); BitBlt(dc, 0, 0, r.right, r.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, o); DeleteObject(b); DeleteDC(m); EndPaint(hw, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTCAPTION;   /* the whole window moves it; no close button (v1.2) */
    case WM_DESTROY:
        if (gConsole && IsWindow(gConsole)) ShowWindow(gConsole, SW_SHOWNOACTIVATE);   /* never leave the console hidden */
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static int save_bmp(const wchar_t *path)
{
    HDC sdc = GetDC(NULL); int w = gFull ? 1280 : S(W0), h = gFull ? 720 : height();
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    void *bits = NULL; HDC m = CreateCompatibleDC(sdc); HBITMAP b = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!b) return 3;
    HGDIOBJ o = SelectObject(m, b); paint(m, w, h); GdiFlush();
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof(fh) + sizeof(BITMAPINFOHEADER) + w * h * 4), 0, 0, sizeof(fh) + sizeof(BITMAPINFOHEADER) };
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); DWORD wr;
    if (f == INVALID_HANDLE_VALUE) return 4;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL); WriteFile(f, &bi.bmiHeader, sizeof(BITMAPINFOHEADER), &wr, NULL); WriteFile(f, bits, (DWORD)(w * h * 4), &wr, NULL);
    CloseHandle(f); SelectObject(m, o); DeleteObject(b); DeleteDC(m); ReleaseDC(NULL, sdc); return 0;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show)
{
    (void)hp; (void)cmd; (void)show;
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) return 2;
    lstrcpynW(gFile, argv[1], MAX_PATH);
    HDC sdc = GetDC(NULL); gScale = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc); if (gScale < 96) gScale = 96;
    gStartTick = GetTickCount(); GetSystemTimeAsFileTime(&gStartFt); load();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);   /* no "insert a disk" box for empty card readers while polling */
    if (argc >= 4 && !lstrcmpW(argv[2], L"--bmp")) { if (argc >= 5 && !lstrcmpW(argv[4], L"installing")) gInstalling = 1; return save_bmp(argv[3]); }
    if (argc >= 3) lstrcpynW(gTitle, argv[2], 128);
    WNDCLASSW wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"PswUi"; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    if (!RegisterClassW(&wc)) return 3;
    gH = 0; HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"PswUi", L"PSWloader", WS_POPUP, 0, 0, S(W0), 300, NULL, NULL, hi, NULL);
    if (!hw) return 3;
    fit(hw); ShowWindow(hw, SW_SHOWNOACTIVATE); UpdateWindow(hw); SetTimer(hw, 1, 250, NULL);
    MSG m; while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
