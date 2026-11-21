/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * pswsvc v1.9 (PSWloader) - small helper for Windows Setup's Windows PE, which has no sc.exe and no pnputil restart:
 *   pswsvc.exe start <name> <full path to .sys>   create (or update) and start a kernel-driver service (pswdisk)
 *   pswsvc.exe usbreset                          restart every USB disk that reports 0 bytes
 *   pswsvc.exe diskcheck <n> <text>              v1.4: exit 0 only if \\.\PhysicalDrive<n> has a size, its vendor/product
 *                                                contains <text> and it is not the internal SanDisk/Optimus (no disk access
 *                                                besides two property queries)
 *   pswsvc.exe usbcycle <stick>                  v1.5: cycle the USB port of every USB disk with a size except disk <stick>
 *                                                (the setup stick); nothing is read from or written to a disk. v1.6: before
 *                                                the cycle, idle power-down is turned off in the USB device's Device
 *                                                Parameters (WDF\IdleInWorkingState = 0 - the "allow the computer to turn off
 *                                                this device" switch of KMDF drivers like UASPStor/USBSTOR - plus
 *                                                SelectiveSuspendEnabled, EnhancedPowerManagementEnabled, AllowIdleIrpInD3 = 0),
 *                                                the re-enumeration makes the driver read them. HW 18.11.: with USB selective
 *                                                suspend a USB SSD on the console's AMD xHCI does not come back from idle
 *                                                (events 129/153, Windows Setup stalls); powercfg does not reach it in
 *                                                Windows Setup's Windows PE
 *   pswsvc.exe keepawake <disk>                  v1.7: read one 4 KiB sector (unbuffered, read only) from \\.\PhysicalDrive<disk> every
 *                                                second, forever - keeps the setup stick from USB selective suspend while
 *                                                Windows Setup waits in its dialogs (HW 18.11.: Setup stalled at its first read
 *                                                of install.swm after the stick had been idle; in Windows PE powercfg does
 *                                                not reach the device and the stick cannot be port-cycled while it runs Setup)
 *   pswsvc.exe hidetestmode                      v1.8: Windows Setup shows the "Test Mode" text of test signing in the corner of its
 *                                                screen; in Windows PE the text exists only in shell32.dll.mui (string table
 *                                                entry of 9 characters, next to the branding strings). For every
 *                                                %SystemRoot%\System32\<lang>\shell32.dll.mui of the RAM disk X: a copy with
 *                                                the stand-alone entry replaced by spaces of the same length is written and
 *                                                swapped in (rename works while the file is in use), then the desktop is
 *                                                repainted. Only the RAM copy of Windows PE is changed, nothing on a disk.
 *   pswsvc.exe patchmui <in> <out>               v1.9: writes a copy of a shell32.dll.mui with the whole corner text (test mode,
 *                                                build line, evaluation/secure boot notes) blanked - psw-drivers.cmd puts it into the
 *                                                stick's boot.wim, because Windows PE reads the text before any script runs
 *   pswsvc.exe stop <name>                       stop a service (v1.3: the Virtual Disk Service caches the bus type of
 *                                                every disk it saw before pswdisk started; stopping it makes the next
 *                                                user - diskpart, Windows Setup - read the disks again)
 * v1.1 usbreset: some USB SSD enclosures (seen: Realtek RTL9210) answer the first capacity query with 0 bytes after power-on
 * until the device is restarted once (it then even reports itself as an empty removable reader); Windows Setup shows the disk
 * as 0 B. v1.2: if the disk still reports 0 bytes after the device restart, its USB port is cycled
 * (IOCTL_USB_HUB_CYCLE_PORT - the same as unplugging and plugging the cable in again). Disks with a size (the setup stick, internal disks) are never touched; an empty card reader is just re-enumerated. Nothing is read from or written to any disk.
 * Prints one line per action; exit code 0 = ok, otherwise a Win32 error code. */
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <wincrypt.h>
#include <stdio.h>

/* GUID_DEVINTERFACE_USB_HUB {F18A0E88-C30C-11D0-8815-00A0C906BED8} */
static const GUID PswHubIf = { 0xf18a0e88, 0xc30c, 0x11d0, { 0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8 } };
#define PSW_IOCTL_USB_HUB_CYCLE_PORT CTL_CODE(0x22, 273, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct { ULONG ConnectionIndex; ULONG StatusReturned; } PSW_USB_CYCLE_PORT_PARAMS;

/* v1.2: walk up from the disk devnode to the USB device, then cycle that device's port on its parent hub */
static int cycle_usb_port(DEVINST disk)
{
    DEVINST cur = disk, parent; wchar_t id[512];
    for (int depth = 0; depth < 4; depth++) {
        if (CM_Get_Device_IDW(cur, id, 512, 0) == CR_SUCCESS && !wcsncmp(id, L"USB\\VID_", 8)) break;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) return -1;
        cur = parent;
    }
    if (CM_Get_Device_IDW(cur, id, 512, 0) != CR_SUCCESS || wcsncmp(id, L"USB\\VID_", 8)) return -2;
    ULONG port = 0, len = sizeof port;
    if (CM_Get_DevNode_Registry_PropertyW(cur, CM_DRP_ADDRESS, NULL, &port, &len, 0) != CR_SUCCESS || !port) return -3;
    DEVINST hub; wchar_t hubid[512];
    if (CM_Get_Parent(&hub, cur, 0) != CR_SUCCESS || CM_Get_Device_IDW(hub, hubid, 512, 0) != CR_SUCCESS) return -4;
    wchar_t list[2048] = L"";
    if (CM_Get_Device_Interface_ListW((LPGUID)&PswHubIf, hubid, list, 2048, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || !list[0]) return -5;
    HANDLE h = CreateFileW(list, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -6;
    PSW_USB_CYCLE_PORT_PARAMS cp = { port, 0 }; DWORD n = 0;
    BOOL ok = DeviceIoControl(h, PSW_IOCTL_USB_HUB_CYCLE_PORT, &cp, sizeof cp, &cp, sizeof cp, &n, NULL);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    wprintf(L"pswsvc usbreset:   cycle port %lu on %ls: %s (error %lu)\n", port, hubid, ok ? "ok" : "failed", err);
    return ok ? 0 : -7;
}

/* GUID_DEVINTERFACE_DISK {53F56307-B6BF-11D0-94F2-00A0C91EFB8B} */
static const GUID PswDiskIf = { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

static ULONGLONG disk_len(const wchar_t *path, STORAGE_BUS_TYPE *bus, BOOLEAN *removable)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    STORAGE_PROPERTY_QUERY q; ZeroMemory(&q, sizeof q); q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
    BYTE buf[1024]; DWORD n = 0;
    *bus = BusTypeUnknown; *removable = FALSE;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &n, NULL) && n >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf; *bus = d->BusType; *removable = d->RemovableMedia;
    }
    GET_LENGTH_INFORMATION li = {0};
    ULONGLONG len = DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &n, NULL) ? (ULONGLONG)li.Length.QuadPart : 0;
    CloseHandle(h);
    return len;
}

/* v1.5 */
static long disk_number(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    STORAGE_DEVICE_NUMBER dn; DWORD n = 0; long r = -1;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &dn, sizeof dn, &n, NULL)) r = (long)dn.DeviceNumber;
    CloseHandle(h);
    return r;
}

/* v1.6: idle power-down off in the USB device node's Device Parameters (hardware key) */
static int set_noidle(DEVINST disk)
{
    DEVINST cur = disk, parent; wchar_t id[512]; HKEY hk, wdf; DWORD zero = 0; int bad = 0;
    for (int depth = 0; depth < 4; depth++) {
        if (CM_Get_Device_IDW(cur, id, 512, 0) == CR_SUCCESS && !wcsncmp(id, L"USB\\VID_", 8)) break;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) return -1;
        cur = parent;
    }
    if (CM_Get_Device_IDW(cur, id, 512, 0) != CR_SUCCESS || wcsncmp(id, L"USB\\VID_", 8)) return -2;
    if (CM_Open_DevNode_Key(cur, KEY_ALL_ACCESS, 0, RegDisposition_OpenAlways, &hk, CM_REGISTRY_HARDWARE) != CR_SUCCESS) return -3;
    static const wchar_t *v[] = { L"SelectiveSuspendEnabled", L"EnhancedPowerManagementEnabled", L"AllowIdleIrpInD3", L"DeviceSelectiveSuspended" };
    for (int i = 0; i < 4; i++) bad |= RegSetValueExW(hk, v[i], 0, REG_DWORD, (const BYTE *)&zero, sizeof zero) != ERROR_SUCCESS;
    if (RegCreateKeyExW(hk, L"WDF", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &wdf, NULL) == ERROR_SUCCESS) {
        bad |= RegSetValueExW(wdf, L"IdleInWorkingState", 0, REG_DWORD, (const BYTE *)&zero, sizeof zero) != ERROR_SUCCESS;
        RegCloseKey(wdf);
    } else bad = 1;
    RegCloseKey(hk);
    wprintf(L"pswsvc usbcycle:   idle power-down off on %ls: %s\n", id, bad ? "partly failed" : "ok");
    return bad ? -4 : 0;
}

static int usbcycle(long stick)
{
    HDEVINFO set = SetupDiGetClassDevsW(&PswDiskIf, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { wprintf(L"pswsvc usbcycle: SetupDiGetClassDevs error %lu\n", GetLastError()); return (int)GetLastError(); }
    SP_DEVICE_INTERFACE_DATA ifd; ZeroMemory(&ifd, sizeof ifd); ifd.cbSize = sizeof ifd;
    int cycled = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &PswDiskIf, i, &ifd); i++) {
        BYTE dbuf[2048]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)dbuf; det->cbSize = sizeof *det;
        SP_DEVINFO_DATA dev; ZeroMemory(&dev, sizeof dev); dev.cbSize = sizeof dev;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof dbuf, NULL, &dev)) continue;
        STORAGE_BUS_TYPE bus; BOOLEAN rem; ULONGLONG len = disk_len(det->DevicePath, &bus, &rem);
        long num = disk_number(det->DevicePath);
        wchar_t id[512] = L""; SetupDiGetDeviceInstanceIdW(set, &dev, id, 512, NULL);
        if (bus != BusTypeUsb || len == 0 || num < 0 || num == stick) continue;
        wprintf(L"pswsvc usbcycle: disk %ld %ls size %llu\n", num, id, len);
        (void)set_noidle(dev.DevInst);   /* v1.6 */
        int r = cycle_usb_port(dev.DevInst);
        if (r) wprintf(L"pswsvc usbcycle:   port cycle not possible (%d)\n", r); else cycled++;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (cycled) Sleep(15000);   /* re-enumeration */
    wprintf(L"pswsvc usbcycle: %d port(s) cycled\n", cycled);
    return 0;
}

static int usbreset(void)
{
    HDEVINFO set = SetupDiGetClassDevsW(&PswDiskIf, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { wprintf(L"pswsvc usbreset: SetupDiGetClassDevs error %lu\n", GetLastError()); return (int)GetLastError(); }
    SP_DEVICE_INTERFACE_DATA ifd; ZeroMemory(&ifd, sizeof ifd); ifd.cbSize = sizeof ifd;
    int restarted = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &PswDiskIf, i, &ifd); i++) {
        BYTE dbuf[2048]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)dbuf; det->cbSize = sizeof *det;
        SP_DEVINFO_DATA dev; ZeroMemory(&dev, sizeof dev); dev.cbSize = sizeof dev;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof dbuf, NULL, &dev)) continue;
        STORAGE_BUS_TYPE bus; BOOLEAN rem; ULONGLONG len = disk_len(det->DevicePath, &bus, &rem);
        wchar_t id[512] = L""; SetupDiGetDeviceInstanceIdW(set, &dev, id, 512, NULL);
        wprintf(L"pswsvc usbreset: %ls bus %d removable %d size %llu\n", id, (int)bus, (int)rem, len);
        if (bus != BusTypeUsb || len != 0) continue;   /* v1.1: also "removable": a stuck bridge presents itself as an empty removable reader */
        SP_PROPCHANGE_PARAMS pc; ZeroMemory(&pc, sizeof pc); pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE; pc.StateChange = DICS_PROPCHANGE; pc.Scope = DICS_FLAG_GLOBAL;
        if (!SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev)) {
            wprintf(L"pswsvc usbreset:   restart failed, error %lu\n", GetLastError()); continue;
        }
        restarted++;
        Sleep(5000);
        len = disk_len(det->DevicePath, &bus, &rem);
        wprintf(L"pswsvc usbreset:   restarted, size now %llu\n", len);
        if (len == 0) {   /* v1.2: still 0 bytes - cycle the USB port (like unplug/replug) */
            int r = cycle_usb_port(dev.DevInst);
            if (r) wprintf(L"pswsvc usbreset:   port cycle not possible (%d)\n", r);
            Sleep(10000);
            wprintf(L"pswsvc usbreset:   after port cycle: see the disk list (the device re-enumerates with a new handle)\n");
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    wprintf(L"pswsvc usbreset: %d disk(s) restarted\n", restarted);
    return 0;
}

/* v1.7 */
static int keepawake(long disk)
{
    wchar_t path[64]; swprintf(path, 64, L"\\\\.\\PhysicalDrive%ld", disk);
    BYTE *buf = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   /* page aligned for NO_BUFFERING */
    if (!buf) return 8;
    wprintf(L"pswsvc keepawake: %ls\n", path); fflush(stdout);
    for (ULONGLONG i = 0;; i++) {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER off; off.QuadPart = (LONGLONG)((i % 64) * 1048576ULL);   /* first 64 MiB, a different sector each time */
            DWORD n = 0;
            if (SetFilePointerEx(h, off, NULL, FILE_BEGIN)) (void)ReadFile(h, buf, 4096, &n, NULL);
            CloseHandle(h);
        }
        Sleep(1000);
    }
}

/* v1.9: the corner text of Windows (watermark) is a group of string-table entries in shell32.dll.mui; see
 * tools/psw-hide-watermark.py (same rules). Entries are blanked with spaces of the same length:
 *   - the test-mode entry (Testmodus / Test Mode / ...);
 *   - every entry holding "(Build %ws" in the 16 entries from the test-mode entry, and the entry after the last of them;
 *   - the entry "%ws Build %ws" and the following entries up to the end of its block (empty entry).
 * Language independent apart from the test-mode words. Returns the number of blanked entries. */
static DWORD wm_entry(const BYTE *b, DWORD len, DWORD k) { return (k + 2 <= len) ? (DWORD)(b[k] | (b[k + 1] << 8)) : 0; }
static int wm_has(const BYTE *b, DWORD k, DWORD n, const wchar_t *needle)
{
    DWORD m = (DWORD)wcslen(needle);
    for (DWORD i = 0; i + m <= n; i++) if (!memcmp(b + k + 2 + 2 * i, needle, 2 * m)) return 1;
    return 0;
}
static void wm_blank(BYTE *b, DWORD k, DWORD n) { for (DWORD c = 0; c < n; c++) { b[k + 2 + 2 * c] = ' '; b[k + 3 + 2 * c] = 0; } }
static int wm_patch(BYTE *b, DWORD len)
{
    static const wchar_t *needles[] = { L"Testmodus", L"Test Mode", L"Mode test", L"Modo de prueba", L"Modalit\x00e0 test" };
    BYTE *orig = (BYTE *)HeapAlloc(GetProcessHeap(), 0, len ? len : 1); int hits = 0;
    if (!orig) return 0;
    memcpy(orig, b, len);
    for (size_t q = 0; q < sizeof needles / sizeof needles[0]; q++) {
        DWORD nl = (DWORD)wcslen(needles[q]);
        for (DWORD i = 2; i + 2 * nl <= len; i += 2) {
            if (wm_entry(orig, len, i - 2) != nl || memcmp(orig + i, needles[q], 2 * nl)) continue;
            wm_blank(b, i - 2, nl); hits++;
            DWORD p = i - 2, last = 0; int have = 0;
            for (int e = 0; e < 16 && p + 2 <= len; e++) {
                DWORD n = wm_entry(orig, len, p);
                if (p + 2 + 2 * n > len) break;
                if (n && wm_has(orig, p, n, L"(Build %ws")) { wm_blank(b, p, n); hits++; last = p; have = 1; }
                p += 2 + 2 * n;
            }
            if (have) { DWORD q2 = last + 2 + 2 * wm_entry(orig, len, last), n2 = wm_entry(orig, len, q2); if (n2 && q2 + 2 + 2 * n2 <= len) { wm_blank(b, q2, n2); hits++; } }
        }
    }
    static const wchar_t bw[] = L"%ws Build %ws"; DWORD bl = 13;
    for (DWORD i = 2; i + 2 * bl <= len; i += 2) {
        if (wm_entry(orig, len, i - 2) != bl || memcmp(orig + i, bw, 2 * bl)) continue;
        DWORD p = i - 2;
        for (int e = 0; e < 16 && p + 2 <= len; e++) {
            DWORD n = wm_entry(orig, len, p);
            if (!n || p + 2 + 2 * n > len) break;
            wm_blank(b, p, n); hits++; p += 2 + 2 * n;
        }
    }
    HeapFree(GetProcessHeap(), 0, orig);
    return hits;
}
static BYTE *read_all(const wchar_t *f, DWORD *len)
{
    HANDLE h = CreateFileW(f, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = 0; *len = GetFileSize(h, NULL); BYTE *b = (BYTE *)HeapAlloc(GetProcessHeap(), 0, *len ? *len : 1);
    BOOL ok = b && ReadFile(h, b, *len, &n, NULL) && n == *len; CloseHandle(h);
    if (!ok && b) { HeapFree(GetProcessHeap(), 0, b); b = NULL; }
    return b;
}
static BOOL write_all(const wchar_t *f, const BYTE *b, DWORD len)
{
    HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); DWORD n = 0;
    BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, b, len, &n, NULL) && n == len;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}
/* v1.9: patchmui <in> <out> - write the blanked copy (also when nothing was left to blank) */
static int patchmui(const wchar_t *in, const wchar_t *out)
{
    DWORD len = 0; BYTE *b = read_all(in, &len);
    if (!b) { wprintf(L"pswsvc patchmui: %ls: read error\n", in); return 2; }
    int hits = wm_patch(b, len); BOOL ok = write_all(out, b, len);
    wprintf(L"pswsvc patchmui: %ls: %d entries blanked -> %ls %ls\n", in, hits, out, ok ? L"ok" : L"WRITE FAILED");
    HeapFree(GetProcessHeap(), 0, b);
    return ok ? 0 : 3;
}
/* v1.8 (v1.9: full corner text): the RAM copy of Windows PE's shell32.dll.mui, swapped in by rename */
static int hidetestmode(void)
{
    wchar_t sys[MAX_PATH], pat[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(pat, MAX_PATH, L"%ls\\*", sys);
    WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return 2;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) != 5 || fd.cFileName[2] != L'-') continue;
        wchar_t f[MAX_PATH], fnew[MAX_PATH], fold[MAX_PATH]; DWORD len = 0;
        swprintf(f, MAX_PATH, L"%ls\\%ls\\shell32.dll.mui", sys, fd.cFileName);
        BYTE *b = read_all(f, &len);
        if (!b) continue;
        int hit = wm_patch(b, len); BOOL ok = TRUE;
        if (hit) {
            swprintf(fnew, MAX_PATH, L"%ls.psw-new", f); swprintf(fold, MAX_PATH, L"%ls.psw-orig", f);
            ok = write_all(fnew, b, len) && MoveFileExW(f, fold, MOVEFILE_REPLACE_EXISTING) && MoveFileExW(fnew, f, 0);
        }
        wprintf(L"pswsvc hidetestmode: %ls: %d entries %ls\n", f, hit, hit ? (ok ? L"hidden" : L"FAILED") : L"(nothing to hide)");
        HeapFree(GetProcessHeap(), 0, b);
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 2000, NULL);
    return 0;
}

/* v2.0: import a DER certificate into LocalMachine\Root and TrustedPublisher (WinPE has no certutil.exe) */
static int addcert(const wchar_t *path)
{
    DWORD len = 0;
    BYTE *der = read_all(path, &len);
    if (!der || !len) { wprintf(L"pswsvc addcert: %ls: read error\n", path); if (der) HeapFree(GetProcessHeap(), 0, der); return 2; }
    static const wchar_t *stores[] = { L"Root", L"TrustedPublisher" };
    int rc = 0;
    for (int i = 0; i < 2; i++) {
        HCERTSTORE st = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                      CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, stores[i]);
        if (!st) st = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, stores[i]);
        if (!st) { wprintf(L"pswsvc addcert: open %ls failed (%lu)\n", stores[i], GetLastError()); rc = 1; continue; }
        BOOL ok = CertAddEncodedCertificateToStore(st, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der, len,
                                                   CERT_STORE_ADD_REPLACE_EXISTING, NULL);
        wprintf(L"pswsvc addcert: %ls -> %ls (%lu)\n", stores[i], ok ? L"ok" : L"FAILED", ok ? 0 : GetLastError());
        if (!ok) rc = 1;
        CertCloseStore(st, 0);
    }
    HeapFree(GetProcessHeap(), 0, der);
    return rc;
}

static int multi_sz_has_i(const wchar_t *buf, DWORD chars, const wchar_t *needle)
{
    DWORD pos = 0;
    while (pos < chars && buf[pos]) {
        const wchar_t *s = buf + pos;
        size_t sl = wcslen(s);
        if (!_wcsicmp(s, needle)) return 1;
        pos += (DWORD)sl + 1;
    }
    return 0;
}

static int multi_sz_contains_substr_i(const wchar_t *buf, DWORD chars, const wchar_t *sub)
{
    size_t nl = wcslen(sub);
    DWORD pos = 0;
    while (pos < chars && buf[pos]) {
        const wchar_t *s = buf + pos;
        size_t sl = wcslen(s);
        for (size_t i = 0; i + nl <= sl; i++) {
            if (!_wcsnicmp(s + i, sub, nl)) return 1;
        }
        pos += (DWORD)sl + 1;
    }
    return 0;
}

static void reg_set_dw(HKEY k, const wchar_t *name, DWORD val)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof val);
}

static void reg_set_str(HKEY k, const wchar_t *name, DWORD type, const wchar_t *val)
{
    RegSetValueExW(k, name, 0, type, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
}

static void reg_prepend_multisz(HKEY k, const wchar_t *name, const wchar_t *item)
{
    wchar_t cur[2048]; DWORD cb = sizeof cur, type = 0;
    ZeroMemory(cur, sizeof cur);
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)cur, &cb) == ERROR_SUCCESS && type == REG_MULTI_SZ) {
        if (multi_sz_has_i(cur, cb / sizeof(wchar_t), item)) return;
    } else {
        cb = 0;
    }
    wchar_t out[2048]; DWORD opos = 0;
    size_t il = wcslen(item);
    if (il + 2 >= 2048) return;
    memcpy(out, item, (il + 1) * sizeof(wchar_t));
    opos = (DWORD)(il + 1);
    DWORD cpos = 0, cchars = cb / sizeof(wchar_t);
    while (cpos < cchars && cur[cpos]) {
        size_t sl = wcslen(cur + cpos);
        if (opos + sl + 2 < 2048) {
            memcpy(out + opos, cur + cpos, (sl + 1) * sizeof(wchar_t));
            opos += (DWORD)(sl + 1);
        }
        cpos += (DWORD)(sl + 1);
    }
    out[opos++] = 0;
    RegSetValueExW(k, name, 0, REG_MULTI_SZ, (const BYTE *)out, opos * sizeof(wchar_t));
}

/* v2.0: check if Windows Test Signing is active (CODEINTEGRITY_OPTION_TESTSIGN = 0x02) */
typedef struct { ULONG Length; ULONG CodeIntegrityOptions; } PSW_CI_INFO;
typedef LONG (WINAPI *PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
static int is_testsigning_on(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return 1;
    PFN_NtQuerySystemInformation fn = (PFN_NtQuerySystemInformation)GetProcAddress(nt, "NtQuerySystemInformation");
    if (!fn) return 1;
    PSW_CI_INFO ci = { sizeof ci, 0 };
    ULONG ret = 0;
    if (fn(103 /* SystemCodeIntegrityInformation */, &ci, sizeof ci, &ret) != 0) return 1;
    wprintf(L"pswsvc: CodeIntegrityOptions=0x%08lx (testsigning=%d)\n", ci.CodeIntegrityOptions, (ci.CodeIntegrityOptions & 0x02) ? 1 : 0);
    return (ci.CodeIntegrityOptions & 0x02) ? 1 : 0;
}

/* v2.0: install ps5nvmsi + ps5m2flt in WinPE for ANY PCI NVMe (PCI\CC_010802) and restart the NVMe controller */
static int nvmeinit(const wchar_t *nvmsi_src, const wchar_t *m2flt_src)
{
    if (!is_testsigning_on()) {
        wprintf(L"pswsvc nvmeinit: TESTSIGNING is OFF - skipping filter attach to avoid breaking disks (error 577)\n");
        return 577;
    }
    wchar_t sys[MAX_PATH], dst_nv[MAX_PATH], dst_m2[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(dst_nv, MAX_PATH, L"%ls\\drivers\\ps5nvmsi.sys", sys);
    swprintf(dst_m2, MAX_PATH, L"%ls\\drivers\\ps5m2flt.sys", sys);
    BOOL c1 = CopyFileW(nvmsi_src, dst_nv, FALSE);
    BOOL c2 = CopyFileW(m2flt_src, dst_m2, FALSE);
    wprintf(L"pswsvc nvmeinit: copy ps5nvmsi=%ls ps5m2flt=%ls\n", c1 ? L"ok" : L"FAIL", c2 ? L"ok" : L"FAIL");

    HKEY hk = NULL, pk = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ps5nvmsi", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
        reg_set_dw(hk, L"Type", 1); reg_set_dw(hk, L"Start", 0); reg_set_dw(hk, L"ErrorControl", 1);
        reg_set_str(hk, L"Group", REG_SZ, L"SCSI miniport");
        reg_set_str(hk, L"ImagePath", REG_EXPAND_SZ, L"System32\\drivers\\ps5nvmsi.sys");
        reg_set_str(hk, L"DisplayName", REG_SZ, L"PS5 NVMe MSI proxy filter (ps5nvmsi)");
        if (RegCreateKeyExW(hk, L"Parameters", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &pk, NULL) == ERROR_SUCCESS) {
            reg_set_str(pk, L"InstalledBy", REG_SZ, L"pswsvc");
            reg_set_dw(pk, L"MsiAddrTemplate", 0xFEE0000CU);
            reg_set_dw(pk, L"MsiDataTemplate", 0x4900U);
            reg_set_dw(pk, L"MsiDestKind", 2U);
            reg_set_dw(pk, L"OrigLowerFiltersPresent", 0U);
            RegCloseKey(pk);
        }
        RegDeleteKeyW(hk, L"StartOverride");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ps5m2flt", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
        reg_set_dw(hk, L"Type", 1); reg_set_dw(hk, L"Start", 0); reg_set_dw(hk, L"ErrorControl", 1);
        reg_set_str(hk, L"Group", REG_SZ, L"System Bus Extender");
        reg_set_str(hk, L"ImagePath", REG_EXPAND_SZ, L"System32\\drivers\\ps5m2flt.sys");
        reg_set_str(hk, L"DisplayName", REG_SZ, L"PS5 M.2 Nested GPT Overlay Filter");
        RegDeleteKeyW(hk, L"StartOverride");
        RegCloseKey(hk);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\stornvme", 0, KEY_ALL_ACCESS, &hk) == ERROR_SUCCESS) {
        RegDeleteKeyW(hk, L"StartOverride");
        RegCloseKey(hk);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e967-e325-11ce-bfc1-08002be10318}", 0, KEY_ALL_ACCESS, &hk) == ERROR_SUCCESS) {
        reg_prepend_multisz(hk, L"UpperFilters", L"ps5m2flt");
        RegCloseKey(hk);
    }

    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) { wprintf(L"pswsvc nvmeinit: SetupDiGetClassDevs error %lu\n", GetLastError()); return 1; }
    SP_DEVINFO_DATA dev; ZeroMemory(&dev, sizeof dev); dev.cbSize = sizeof dev;
    int found = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t ids[2048]; DWORD cb = 0; int is_nvme = 0;
        ZeroMemory(ids, sizeof ids);
        if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_COMPATIBLEIDS, NULL, (BYTE *)ids, sizeof ids - 2, &cb))
            is_nvme |= multi_sz_contains_substr_i(ids, cb / sizeof(wchar_t), L"PCI\\CC_010802");
        ZeroMemory(ids, sizeof ids);
        if (!is_nvme && SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof ids - 2, &cb))
            is_nvme |= multi_sz_contains_substr_i(ids, cb / sizeof(wchar_t), L"PCI\\CC_010802");
        if (!is_nvme) continue;
        found++;
        wchar_t inst[512] = L"";
        SetupDiGetDeviceInstanceIdW(set, &dev, inst, 512, NULL);
        wprintf(L"pswsvc nvmeinit: found NVMe %ls\n", inst);

        /* Set LowerFilters = ps5nvmsi on this PCI NVMe devnode */
        wchar_t lf[1024]; DWORD lf_cb = 0; ZeroMemory(lf, sizeof lf);
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_LOWERFILTERS, NULL, (BYTE *)lf, sizeof lf - 4, &lf_cb) ||
            !multi_sz_has_i(lf, lf_cb / sizeof(wchar_t), L"ps5nvmsi")) {
            wchar_t nlf[1024]; DWORD pos = 0;
            wcscpy(nlf, L"ps5nvmsi"); pos = 9;
            DWORD cpos = 0, cchars = lf_cb / sizeof(wchar_t);
            while (cpos < cchars && lf[cpos]) {
                size_t sl = wcslen(lf + cpos);
                if (pos + sl + 2 < 1024) { memcpy(nlf + pos, lf + cpos, (sl + 1) * sizeof(wchar_t)); pos += (DWORD)(sl + 1); }
                cpos += (DWORD)(sl + 1);
            }
            nlf[pos++] = 0;
            BOOL ok = SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_LOWERFILTERS, (const BYTE *)nlf, pos * sizeof(wchar_t));
            wprintf(L"pswsvc nvmeinit:   LowerFilters=ps5nvmsi: %ls (%lu)\n", ok ? L"ok" : L"FAIL", ok ? 0 : GetLastError());
        }

        /* Set MSISupported=1 and MessageNumberLimit=1 in hardware Device Parameters */
        HKEY hwk = NULL;
        if (CM_Open_DevNode_Key(dev.DevInst, KEY_ALL_ACCESS, 0, RegDisposition_OpenAlways, &hwk, CM_REGISTRY_HARDWARE) == CR_SUCCESS) {
            HKEY msik = NULL;
            if (RegCreateKeyExW(hwk, L"Interrupt Management\\MessageSignaledInterruptProperties", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &msik, NULL) == ERROR_SUCCESS) {
                reg_set_dw(msik, L"MSISupported", 1);
                reg_set_dw(msik, L"MessageNumberLimit", 1);
                RegCloseKey(msik);
                wprintf(L"pswsvc nvmeinit:   MSISupported=1 MessageNumberLimit=1 ok\n");
            }
            RegCloseKey(hwk);
        }

        /* Restart the NVMe PCI device so ps5nvmsi attaches and stornvme starts cleanly */
        SP_PROPCHANGE_PARAMS pc; ZeroMemory(&pc, sizeof pc);
        pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pc.Scope = DICS_FLAG_GLOBAL;
        pc.StateChange = DICS_DISABLE;
        BOOL d_ok = SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc) &&
                    SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev);
        Sleep(500);
        pc.StateChange = DICS_ENABLE;
        BOOL e_ok = SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc) &&
                    SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev);
        if (!e_ok) {
            pc.StateChange = DICS_PROPCHANGE;
            e_ok = SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc) &&
                   SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev);
        }
        wprintf(L"pswsvc nvmeinit:   restart disable=%d enable=%d (err=%lu)\n", (int)d_ok, (int)e_ok, e_ok ? 0 : GetLastError());
    }
    SetupDiDestroyDeviceInfoList(set);
    if (found) {
        DEVINST root = 0;
        if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
            CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
        Sleep(3000);
    }
    wprintf(L"pswsvc nvmeinit: %d NVMe controller(s) processed\n", found);
    return 0;
}

/* v2.1: enable SeBackupPrivilege + SeRestorePrivilege so RegLoadKeyW / RegUnLoadKeyW work in WinPE */
static void enable_reg_privs(void)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    static const wchar_t *privs[] = { SE_RESTORE_NAME, SE_BACKUP_NAME };
    for (int i = 0; i < 2; i++) {
        TOKEN_PRIVILEGES tp; ZeroMemory(&tp, sizeof tp);
        if (LookupPrivilegeValueW(NULL, privs[i], &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, NULL, NULL);
        }
    }
    CloseHandle(tok);
}

/* v2.1: inject ps5nvmsi + ps5m2flt as boot-start drivers into an offline Windows installation on <drive>:\ */
static int inject_offline_nvme(wchar_t drive, const wchar_t *nvmsi_src, const wchar_t *m2flt_src, FILE *logf)
{
    wchar_t hive[MAX_PATH], dst_nv[MAX_PATH], dst_m2[MAX_PATH];
    swprintf(hive, MAX_PATH, L"%lc:\\Windows\\System32\\config\\SYSTEM", drive);
    swprintf(dst_nv, MAX_PATH, L"%lc:\\Windows\\System32\\drivers\\ps5nvmsi.sys", drive);
    swprintf(dst_m2, MAX_PATH, L"%lc:\\Windows\\System32\\drivers\\ps5m2flt.sys", drive);

    enable_reg_privs();
    LONG rc = RegLoadKeyW(HKEY_LOCAL_MACHINE, L"PSW_OFFLINE_SYS", hive);
    if (rc != ERROR_SUCCESS) {
        if (logf) { fwprintf(logf, L"nvmewatch: RegLoadKey %ls failed (%ld) - will retry\n", hive, rc); fflush(logf); }
        return 0; /* retry later (hive may be locked by Setup) */
    }

    BOOL c1 = CopyFileW(nvmsi_src, dst_nv, FALSE);
    BOOL c2 = CopyFileW(m2flt_src, dst_m2, FALSE);
    if (logf) { fwprintf(logf, L"nvmewatch: %lc:\\ copy ps5nvmsi=%d ps5m2flt=%d\n", drive, (int)c1, (int)c2); fflush(logf); }

    HKEY root = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"PSW_OFFLINE_SYS", 0, KEY_ALL_ACCESS, &root) == ERROR_SUCCESS) {
        DWORD csnum = 1, cb = sizeof csnum;
        HKEY sel = NULL;
        if (RegOpenKeyExW(root, L"Select", 0, KEY_READ, &sel) == ERROR_SUCCESS) {
            RegQueryValueExW(sel, L"Default", NULL, NULL, (BYTE *)&csnum, &cb);
            RegCloseKey(sel);
        }
        wchar_t cs[64]; swprintf(cs, 64, L"ControlSet%03lu", (unsigned long)(csnum ? csnum : 1));
        wchar_t sub[512]; HKEY hk = NULL, pk = NULL;

        /* 1. Services\ps5nvmsi (boot-start) */
        swprintf(sub, 512, L"%ls\\Services\\ps5nvmsi", cs);
        if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
            reg_set_dw(hk, L"Type", 1); reg_set_dw(hk, L"Start", 0); reg_set_dw(hk, L"ErrorControl", 1);
            reg_set_str(hk, L"Group", REG_SZ, L"SCSI miniport");
            reg_set_str(hk, L"ImagePath", REG_EXPAND_SZ, L"System32\\drivers\\ps5nvmsi.sys");
            reg_set_str(hk, L"DisplayName", REG_SZ, L"PS5 NVMe MSI proxy filter (ps5nvmsi)");
            if (RegCreateKeyExW(hk, L"Parameters", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &pk, NULL) == ERROR_SUCCESS) {
                reg_set_str(pk, L"InstalledBy", REG_SZ, L"pswsvc-offline");
                reg_set_dw(pk, L"MsiAddrTemplate", 0xFEE0000CU);
                reg_set_dw(pk, L"MsiDataTemplate", 0x4900U);
                reg_set_dw(pk, L"MsiDestKind", 2U);
                reg_set_dw(pk, L"OrigLowerFiltersPresent", 0U);
                RegCloseKey(pk);
            }
            RegDeleteKeyW(hk, L"StartOverride");
            RegCloseKey(hk);
        }

        /* 2. Services\ps5m2flt (boot-start) */
        swprintf(sub, 512, L"%ls\\Services\\ps5m2flt", cs);
        if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
            reg_set_dw(hk, L"Type", 1); reg_set_dw(hk, L"Start", 0); reg_set_dw(hk, L"ErrorControl", 1);
            reg_set_str(hk, L"Group", REG_SZ, L"System Bus Extender");
            reg_set_str(hk, L"ImagePath", REG_EXPAND_SZ, L"System32\\drivers\\ps5m2flt.sys");
            reg_set_str(hk, L"DisplayName", REG_SZ, L"PS5 M.2 Nested GPT Overlay Filter");
            RegDeleteKeyW(hk, L"StartOverride");
            RegCloseKey(hk);
        }

        /* 3. Services\stornvme (ensure boot-start and remove StartOverride) */
        swprintf(sub, 512, L"%ls\\Services\\stornvme", cs);
        if (RegOpenKeyExW(root, sub, 0, KEY_ALL_ACCESS, &hk) == ERROR_SUCCESS) {
            reg_set_dw(hk, L"Start", 0);
            RegDeleteKeyW(hk, L"StartOverride");
            RegCloseKey(hk);
        }

        /* 4. DiskDrive class UpperFilters = ps5m2flt before PartMgr */
        swprintf(sub, 512, L"%ls\\Control\\Class\\{4d36e967-e325-11ce-bfc1-08002be10318}", cs);
        if (RegOpenKeyExW(root, sub, 0, KEY_ALL_ACCESS, &hk) == ERROR_SUCCESS) {
            reg_prepend_multisz(hk, L"UpperFilters", L"ps5m2flt");
            RegCloseKey(hk);
        }

        /* 4b. SCSIAdapter class LowerFilters = ps5nvmsi (works on first boot of a fresh install regardless of ParentIdPrefix!) */
        swprintf(sub, 512, L"%ls\\Control\\Class\\{4d36e97b-e325-11ce-bfc1-08002be10318}", cs);
        if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
            reg_prepend_multisz(hk, L"LowerFilters", L"ps5nvmsi");
            RegCloseKey(hk);
        }

        /* v2.3: no Enum\PCI\<NVMe instance> key any more. In a fresh install Enum is still empty, so a key created here
         * (only LowerFilters + MSI values, no Service/ClassGUID/ConfigFlags) carries the real instance id of the boot
         * NVMe; PnP then takes it as an already configured device without a driver and the first boot waits forever on
         * the spinner. The SCSIAdapter class filter (4b) attaches ps5nvmsi and ps5nvmsi >= 0.8.6 sets the MSI limit itself. */
        if (logf) { fwprintf(logf, L"nvmewatch: %lc:\\ offline SYSTEM (%ls) patched ok\n", drive, cs); fflush(logf); }
        RegFlushKey(root);
        RegCloseKey(root);
    }

    LONG urc = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"PSW_OFFLINE_SYS");
    if (logf) { fwprintf(logf, L"nvmewatch: RegUnLoadKey=%ld\n", urc); fflush(logf); }
    return (c1 && c2 && urc == ERROR_SUCCESS) ? 1 : 0;
}

/* v2.1: background watcher in WinPE - waits for Windows Setup to finish copying files to any drive and injects
 * ps5nvmsi + ps5m2flt into the offline SYSTEM hive before Setup reboots (prevents 0x7B INACCESSIBLE_BOOT_DEVICE). */
static int nvmewatch(const wchar_t *stick_drive, const wchar_t *nvmsi_src, const wchar_t *m2flt_src)
{
    wchar_t skip = (stick_drive && stick_drive[0]) ? towupper(stick_drive[0]) : 0;
    wchar_t stick_log[MAX_PATH] = L"";
    if (skip) swprintf(stick_log, MAX_PATH, L"%lc:\\PSW\\psw-nvmewatch.log", skip);
    FILE *logf = _wfopen(L"X:\\psw-nvmewatch.log", L"w");
    if (logf) { fwprintf(logf, L"nvmewatch v2.3 started (skip=%lc)\n", skip ? skip : L'-'); fflush(logf); }
    for (;;) {
        for (wchar_t d = L'C'; d <= L'Z'; d++) {
            if (d == L'X' || d == skip) continue;
            wchar_t sys[MAX_PATH], spec[MAX_PATH], drv[MAX_PATH];
            swprintf(sys, MAX_PATH, L"%lc:\\Windows\\System32\\config\\SYSTEM", d);
            swprintf(spec, MAX_PATH, L"%lc:\\Windows\\Setup\\Scripts\\psw-specialize.cmd", d);
            swprintf(drv, MAX_PATH, L"%lc:\\Windows\\System32\\drivers\\ps5nvmsi.sys", d);
            if (GetFileAttributesW(sys) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(spec) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(drv) == INVALID_FILE_ATTRIBUTES) {
                if (inject_offline_nvme(d, nvmsi_src, m2flt_src, logf) && stick_log[0]) {
                    CopyFileW(L"X:\\psw-nvmewatch.log", stick_log, FALSE);
                }
            }
        }
        Sleep(1000);
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 5 && !_wcsicmp(argv[1], L"nvmewatch")) return nvmewatch(argv[2], argv[3], argv[4]);   /* v2.1 */
    if (argc == 3 && !_wcsicmp(argv[1], L"addcert")) return addcert(argv[2]);   /* v2.0 */
    if (argc == 4 && !_wcsicmp(argv[1], L"nvmeinit")) return nvmeinit(argv[2], argv[3]);   /* v2.0 */
    if (argc == 2 && !_wcsicmp(argv[1], L"hidetestmode")) return hidetestmode();   /* v1.8 */
    if (argc == 4 && !_wcsicmp(argv[1], L"patchmui")) return patchmui(argv[2], argv[3]);   /* v1.9 */
    if (argc == 2 && !_wcsicmp(argv[1], L"usbreset")) return usbreset();
    if (argc == 3 && !_wcsicmp(argv[1], L"keepawake")) {   /* v1.7 */
        wchar_t *e = NULL; long n = wcstol(argv[2], &e, 10);
        if (!e || *e || n < 0) { fwprintf(stderr, L"pswsvc keepawake: <disk> must be a disk number\n"); return 87; }
        return keepawake(n);
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"usbcycle")) {   /* v1.5 */
        wchar_t *e = NULL; long n = wcstol(argv[2], &e, 10);
        if (!e || *e || n < 0) { fwprintf(stderr, L"pswsvc usbcycle: <stick> must be a disk number\n"); return 87; }
        return usbcycle(n);
    }
    if (argc == 4 && !_wcsicmp(argv[1], L"diskcheck")) {   /* v1.4 */
        wchar_t path[64]; swprintf(path, 64, L"\\\\.\\PhysicalDrive%ls", argv[2]);
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);   /* read access for GET_LENGTH_INFO; no data is read */
        if (h == INVALID_HANDLE_VALUE) h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { wprintf(L"pswsvc diskcheck: %ls open error %lu\n", path, GetLastError()); return 2; }
        STORAGE_PROPERTY_QUERY q; ZeroMemory(&q, sizeof q); q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
        BYTE buf[1024]; DWORD n = 0; char model[300] = "";
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &n, NULL) && n >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
            if (d->VendorIdOffset && d->VendorIdOffset < n) { strncat(model, (char *)buf + d->VendorIdOffset, 120); strcat(model, " "); }
            if (d->ProductIdOffset && d->ProductIdOffset < n) strncat(model, (char *)buf + d->ProductIdOffset, 120);
        }
        GET_LENGTH_INFORMATION li; ZeroMemory(&li, sizeof li);
        BOOL lok = DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &n, NULL);
        CloseHandle(h);
        char want[200]; WideCharToMultiByte(CP_ACP, 0, argv[3], -1, want, sizeof want, NULL, NULL);
        char um[300], uw[200]; strcpy(um, model); strcpy(uw, want); CharUpperA(um); CharUpperA(uw);
        int has = strstr(um, uw) != NULL, sd = strstr(um, "SANDISK") || strstr(um, "OPTIMUS");
        unsigned long long sz = lok ? (unsigned long long)li.Length.QuadPart : 0;
        int ok = has && !sd && sz > 0;
        wprintf(L"pswsvc diskcheck: disk %ls model '%hs' size %llu -> %s\n", argv[2], model, sz, ok ? "OK" : "REFUSED");
        return ok ? 0 : 1;
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"stop")) {   /* v1.3 */
        SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        SC_HANDLE s = m ? OpenServiceW(m, argv[2], SERVICE_STOP | SERVICE_QUERY_STATUS) : NULL;
        if (!s) { DWORD e = GetLastError(); wprintf(L"pswsvc: stop %ls: open error %lu\n", argv[2], e); if (m) CloseServiceHandle(m); return (int)e; }
        SERVICE_STATUS st = {0}; DWORD rc = 0;
        if (!ControlService(s, SERVICE_CONTROL_STOP, &st)) { rc = GetLastError(); if (rc == ERROR_SERVICE_NOT_ACTIVE) rc = 0; }
        for (int i = 0; i < 40 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(250);
        wprintf(L"pswsvc: stop %ls: %s (error %lu, state %lu)\n", argv[2], rc ? "FAILED" : "ok", rc, st.dwCurrentState);
        CloseServiceHandle(s); CloseServiceHandle(m); return (int)rc;
    }
    if (argc != 4 || _wcsicmp(argv[1], L"start")) { fwprintf(stderr, L"usage: pswsvc start <name> <path.sys> | pswsvc usbreset | pswsvc usbcycle <stick> | pswsvc keepawake <disk> | pswsvc stop <name> | pswsvc diskcheck <n> <text> | pswsvc hidetestmode | pswsvc patchmui <in> <out>\n"); return 87; }
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) { DWORD e = GetLastError(); wprintf(L"pswsvc: OpenSCManager error %lu\n", e); return (int)e; }
    SC_HANDLE s = CreateServiceW(scm, argv[2], argv[2], SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                                 SERVICE_ERROR_NORMAL, argv[3], NULL, NULL, NULL, NULL, NULL);
    if (!s && GetLastError() == ERROR_SERVICE_EXISTS) {
        s = OpenServiceW(scm, argv[2], SERVICE_ALL_ACCESS);
        if (s && !ChangeServiceConfigW(s, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, argv[3],
                                       NULL, NULL, NULL, NULL, NULL, NULL))
            wprintf(L"pswsvc: ChangeServiceConfig error %lu (continuing)\n", GetLastError());
    }
    if (!s) { DWORD e = GetLastError(); wprintf(L"pswsvc: create/open %ls error %lu\n", argv[2], e); CloseServiceHandle(scm); return (int)e; }
    DWORD rc = 0;
    if (!StartServiceW(s, 0, NULL)) { rc = GetLastError(); if (rc == ERROR_SERVICE_ALREADY_RUNNING) rc = 0; }
    SERVICE_STATUS st = {0};
    QueryServiceStatus(s, &st);
    wprintf(L"pswsvc: %ls start %s (error %lu, state %lu)\n", argv[2], rc ? "FAILED" : "ok", rc, st.dwCurrentState);
    CloseServiceHandle(s); CloseServiceHandle(scm);
    return (int)rc;
}
