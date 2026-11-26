/** @file
  Ps5InfoDxe v0.2 (15.11.2026): HII forms "PS5 System" (read only) and "PS5 Boot" (Windows / restart /
  power off) on the UiApp front page, for the LVGL setup (--lvgl-ui).
  Values: firmware label + vendor/revision, CPUID brand, usable RAM from the memory map, active GOP mode,
  monitor/EDID from the volatile "Ps5HdmiEdid"/"Ps5HdmiEdidInfo" and the HDMI mode from "Ps5HdmiMode"
  (published by HdmiProbeDxe / the BDS video path). No MMIO, no ICC, no variable writes.
  The strings are filled at entry and again whenever the form opens or "Refresh" is chosen, because the EDID
  and the mode are only published by the BDS display start, after this driver was dispatched.
  v1.2 (18.11.2026): Diagnostics tab - traffic light (GREEN/ORANGE/RED of the last check, always English) and a read-only
  log viewer (form 2, "Show log") built from the volatile Ps5DiagLog the BDS writes with \PS5DIAG.LOG.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/GraphicsOutput.h>
#include <Guid/MdeModuleHii.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/HiiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Guid/GlobalVariable.h>
#include "../Include/Ps5BootNow.h"   /* r5.4 */
#include "../Include/Ps5Diag.h"     /* r5.20: BIOS system check */
#include "../Include/Ps5PowerPreset.h"   /* r5.6 */

#ifndef PS5_FIRMWARE_LABEL
#define PS5_FIRMWARE_LABEL L"PSWloader v1.2"   /* r5.22: user-visible name PSWloader; r5.24: revision only under Advanced */
#endif
#ifndef PS5_FIRMWARE_REV
#define PS5_FIRMWARE_REV L"r5.25.2"
#endif
#define PS5_INFO_REFRESH_KEY 0x1360
#define PS5_BOOT_WINDOWS_KEY 0x1361
#define PS5_BOOT_RESTART_KEY 0x1362
#define PS5_BOOT_POWEROFF_KEY 0x1363
#define PS5_DIAG_RUN_KEY 0x136B     /* r5.20 */

extern UINT8 Ps5InfoVfrBin[];
extern UINT8 Ps5BootVfrBin[];   /* v0.2: 'PS5 Boot' */
extern UINT8 Ps5PerfVfrBin[], Ps5ExitVfrBin[];   /* v0.5 (r5.7): tabs Performance / Exit (r5.22: Advanced/Language tab removed) */
extern UINT8 Ps5DispVfrBin[], Ps5DiagVfrBin[];                    /* v0.8 (r5.18): tabs Display / Diagnostics */
extern UINT8 Ps5InfoDxeStrings[];

STATIC EFI_GUID mFormsetGuid = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x01}};
STATIC EFI_GUID mPs5HdmiGuid = {0x5efc5ae0,0xb753,0x4f7a,{0xa6,0x62,0xe3,0x4d,0x15,0xa6,0xd8,0xe5}};
STATIC EFI_HII_HANDLE mHii, mBootHii;
STATIC EFI_HANDLE mHandle, mBootHandle;   /* v0.2: UiApp lists one formset per HII handle -> own handle for 'PS5 Boot' */
STATIC EFI_GUID mBootFormsetGuid = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x03}};

#pragma pack(1)
typedef struct { VENDOR_DEVICE_PATH Vendor; EFI_DEVICE_PATH_PROTOCOL End; } PS5_INFO_PATH;
#pragma pack()
STATIC PS5_INFO_PATH mPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{(UINT8)sizeof(VENDOR_DEVICE_PATH),(UINT8)(sizeof(VENDOR_DEVICE_PATH)>>8)}},
   {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x02}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{END_DEVICE_PATH_LENGTH,0}}
};
STATIC PS5_INFO_PATH mBootPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{(UINT8)sizeof(VENDOR_DEVICE_PATH),(UINT8)(sizeof(VENDOR_DEVICE_PATH)>>8)}},
   {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x04}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{END_DEVICE_PATH_LENGTH,0}}
};
STATIC PS5_INFO_PATH mPerfPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{(UINT8)sizeof(VENDOR_DEVICE_PATH),(UINT8)(sizeof(VENDOR_DEVICE_PATH)>>8)}},
   {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x0d}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{END_DEVICE_PATH_LENGTH,0}}
};
STATIC PS5_INFO_PATH mExitPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{(UINT8)sizeof(VENDOR_DEVICE_PATH),(UINT8)(sizeof(VENDOR_DEVICE_PATH)>>8)}},
   {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x0f}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{END_DEVICE_PATH_LENGTH,0}}
};
STATIC PS5_INFO_PATH mDispPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{(UINT8)sizeof(VENDOR_DEVICE_PATH),(UINT8)(sizeof(VENDOR_DEVICE_PATH)>>8)}},
   {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x11}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{END_DEVICE_PATH_LENGTH,0}}
};
STATIC PS5_INFO_PATH mDiagPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{(UINT8)sizeof(VENDOR_DEVICE_PATH),(UINT8)(sizeof(VENDOR_DEVICE_PATH)>>8)}},
   {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x13}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{END_DEVICE_PATH_LENGTH,0}}
};
STATIC EFI_HANDLE mPerfHandle, mExitHandle, mDispHandle, mDiagHandle;
STATIC EFI_HII_HANDLE mDispHii, mDiagHii;   /* r5.22: each formset has its own string package */
STATIC EFI_GUID mDispGuid = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x10}};
STATIC EFI_GUID mDiagGuid = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x12}};
STATIC EFI_GUID mPerfGuid = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x0a}};
STATIC EFI_GUID mExitGuid = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x0c}};

STATIC VOID EFIAPI Set (EFI_STRING_ID Id, CONST CHAR16 *Fmt, ...)   /* EFIAPI: VA_LIST must match UnicodeVSPrint (ms_abi) */
{
  CHAR16 Buf[160]; VA_LIST Args;
  VA_START (Args, Fmt); UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Args); VA_END (Args);
  HiiSetString (mHii, Id, Buf, NULL);
  DEBUG ((DEBUG_INFO, "PS5 info %u: %s\n", (UINT32)Id, Buf));
}
/* r5.22: the same for another formset's string package (Display, Diagnostics); before, the diagnostics result went
 * into the Overview package and never showed on its own tab */
STATIC VOID EFIAPI SetH (EFI_HII_HANDLE H, EFI_STRING_ID Id, CONST CHAR16 *Fmt, ...)
{
  CHAR16 Buf[160]; VA_LIST Args;
  if (H == NULL) return;
  VA_START (Args, Fmt); UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Args); VA_END (Args);
  HiiSetString (H, Id, Buf, NULL);
}

/* EDID block 0: monitor name descriptor (tag 0xFC) or "vendor product" */
STATIC VOID EdidName (CONST UINT8 *E, UINTN N, CHAR16 *Out, UINTN Chars)
{
  Out[0] = 0;
  if (N < 128) return;
  for (UINTN D = 0x36; D + 18 <= 0x7E; D += 18) {
    if (E[D] == 0 && E[D + 1] == 0 && E[D + 3] == 0xFC) {
      UINTN K = 0;
      for (UINTN I = 5; I < 18 && K + 1 < Chars && E[D + I] != 0x0A; I++) Out[K++] = (CHAR16)E[D + I];
      while (K > 0 && Out[K - 1] == L' ') K--;
      Out[K] = 0; return;
    }
  }
  UINT16 V = (UINT16)((E[8] << 8) | E[9]);
  UnicodeSPrint (Out, Chars * sizeof (CHAR16), L"%c%c%c %04x", L'@' + ((V >> 10) & 31), L'@' + ((V >> 5) & 31), L'@' + (V & 31),
                 (UINT32)(E[10] | (E[11] << 8)));
}

/* r5.21: names of the bootable disks the BDS published (Ps5BootDevs) into the Boot tab's ordered list */
STATIC VOID RefreshBootList (VOID)
{
  STATIC CONST EFI_STRING_ID Tok[16] = { STRING_TOKEN (STR_BDEV1),STRING_TOKEN (STR_BDEV2),STRING_TOKEN (STR_BDEV3),STRING_TOKEN (STR_BDEV4),STRING_TOKEN (STR_BDEV5),STRING_TOKEN (STR_BDEV6),STRING_TOKEN (STR_BDEV7),STRING_TOKEN (STR_BDEV8),STRING_TOKEN (STR_BDEV9),STRING_TOKEN (STR_BDEV10),STRING_TOKEN (STR_BDEV11),STRING_TOKEN (STR_BDEV12),STRING_TOKEN (STR_BDEV13),STRING_TOKEN (STR_BDEV14),STRING_TOKEN (STR_BDEV15),STRING_TOKEN (STR_BDEV16) };
  STATIC PS5_BOOT_DEVS Devs; UINTN N = sizeof (Devs);
  if (mBootHii == NULL) return;
  if (EFI_ERROR (gRT->GetVariable (L"Ps5BootDevs", &mPs5HdmiGuid, NULL, &N, &Devs)) || N != sizeof (Devs)) Devs.Count = 0;
  for (UINTN I = 0; I < 16; I++)
    if (I < Devs.Count) { Devs.Name[I][PS5_BOOTDEV_NAME - 1] = 0; HiiSetString (mBootHii, Tok[I], Devs.Name[I], NULL); }
  DEBUG ((DEBUG_ERROR, "PS5 boot list: %u disks shown in the Boot tab\n", Devs.Count));
}
STATIC VOID Refresh (VOID)
{
  RefreshBootList ();
  Set (STRING_TOKEN (STR_FW_V), L"%s", PS5_FIRMWARE_LABEL);
  Set (STRING_TOKEN (STR_REV_V), L"%s", PS5_FIRMWARE_REV);   /* r5.24 */
  Set (STRING_TOKEN (STR_UEFI_V), L"%s rev 0x%x, UEFI %u.%u", gST->FirmwareVendor, gST->FirmwareRevision,
       gST->Hdr.Revision >> 16, (gST->Hdr.Revision & 0xFFFF) / 10);

  UINT32 Max = 0, Brand[13] = {0};
  AsmCpuid (0x80000000, &Max, NULL, NULL, NULL);
  if (Max >= 0x80000004) {
    for (UINT32 I = 0; I < 3; I++) AsmCpuid (0x80000002 + I, &Brand[I * 4], &Brand[I * 4 + 1], &Brand[I * 4 + 2], &Brand[I * 4 + 3]);
    CHAR8 *B = (CHAR8 *)Brand; while (*B == ' ') B++;
    Set (STRING_TOKEN (STR_CPU_V), L"%a", B);
  }

  UINTN Size = 0, Key, DescSize; UINT32 DescVer; UINT64 Pages = 0;
  EFI_MEMORY_DESCRIPTOR *Map = NULL;
  if (gBS->GetMemoryMap (&Size, NULL, &Key, &DescSize, &DescVer) == EFI_BUFFER_TOO_SMALL) {
    Size += 8 * DescSize; Map = AllocatePool (Size);
    if (Map != NULL && !EFI_ERROR (gBS->GetMemoryMap (&Size, Map, &Key, &DescSize, &DescVer))) {
      for (UINT8 *P = (UINT8 *)Map; P < (UINT8 *)Map + Size; P += DescSize) {
        EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)P;
        if (D->Type == EfiConventionalMemory || D->Type == EfiBootServicesCode || D->Type == EfiBootServicesData ||
            D->Type == EfiLoaderCode || D->Type == EfiLoaderData || D->Type == EfiRuntimeServicesCode ||
            D->Type == EfiRuntimeServicesData || D->Type == EfiACPIReclaimMemory) Pages += D->NumberOfPages;
      }
    }
    if (Map != NULL) FreePool (Map);
  }
  Set (STRING_TOKEN (STR_RAM_V), L"%Lu MiB usable by the OS", Pages / 256);

  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) && Gop->Mode && Gop->Mode->Info)
    Set (STRING_TOKEN (STR_GOP_V), L"%u x %u (pitch %u)", Gop->Mode->Info->HorizontalResolution,
         Gop->Mode->Info->VerticalResolution, Gop->Mode->Info->PixelsPerScanLine);

  UINT8 Edid[512]; UINTN N = sizeof (Edid); UINT32 Attr = 0;
  if (!EFI_ERROR (gRT->GetVariable (L"Ps5HdmiEdid", &mPs5HdmiGuid, &Attr, &N, Edid)) && N >= 128) {
    CHAR16 Name[20]; EdidName (Edid, N, Name, ARRAY_SIZE (Name));
    UINT32 Pclk = (UINT32)(Edid[0x36] | (Edid[0x37] << 8)) * 10;
    UINT32 H = Edid[0x38] | ((Edid[0x3A] & 0xF0) << 4), V = Edid[0x3B] | ((Edid[0x3D] & 0xF0) << 4);
    Set (STRING_TOKEN (STR_MON_V), L"%s, preferred %u x %u (%u kHz)", Name, H, V, Pclk);
    Set (STRING_TOKEN (STR_EDID_V), L"%u bytes, extensions %u", (UINT32)N, Edid[126]);
  } else {
    Set (STRING_TOKEN (STR_MON_V), L"no EDID (monitor off at boot, or 1080p fallback)");
    Set (STRING_TOKEN (STR_EDID_V), L"not published");
  }

  UINT8 Mode[64]; N = sizeof (Mode);
  /* struct ps5_hdmi_mode_var (ps5-hdmi-mode-var.h): width @16, height @18, refresh_hz @20, clock_khz @24 */
  if (!EFI_ERROR (gRT->GetVariable (L"Ps5HdmiMode", &mPs5HdmiGuid, &Attr, &N, Mode)) && N >= 28) {
    UINT16 W = (UINT16)(Mode[16] | (Mode[17] << 8)), Hh = (UINT16)(Mode[18] | (Mode[19] << 8)), R = (UINT16)(Mode[20] | (Mode[21] << 8));
    UINT32 Clk = Mode[24] | (Mode[25] << 8) | (Mode[26] << 16) | ((UINT32)Mode[27] << 24);
    Set (STRING_TOKEN (STR_MODE_V), L"%u x %u @ %u Hz", W, Hh, R);
    SetH (mDispHii, STRING_TOKEN (STR_DMODE_V), L"%u x %u @ %u Hz", W, Hh, R);
    Set (STRING_TOKEN (STR_PCLK_V), L"%u.%03u MHz", Clk / 1000, Clk % 1000);   /* r5.22: Overview > Advanced */
  } else {
    Set (STRING_TOKEN (STR_MODE_V), L"not published");
    SetH (mDispHii, STRING_TOKEN (STR_DMODE_V), L"not published");
    Set (STRING_TOKEN (STR_PCLK_V), L"-");
  }
}

/* v1.2 (18.11.2026, user: traffic light + "log readable directly in the UEFI, clean and clearly arranged"):
 * Diagnostics form 2 = read-only log viewer. The BDS stores the last \PS5DIAG.LOG text in the volatile variable Ps5DiagLog
 * (Ps5Diag.h); each line becomes one opcode between labels 0x2000/0x2FFF of form 2 (HiiUpdateForm):
 *   "== Area =="              -> subtitle "Area"
 *   "  WARN  Title    detail" -> row "WARN   Title:  detail"
 *   other text lines          -> row with the line as it is (Verdict, Firmware, Result, ...)
 *   lines ending in ':'       -> subtitle; empty lines -> one spacer
 * Rows are action opcodes WITHOUT the callback flag (question ids PS5_DIAG_VIEW_QID + n, a click does nothing): the LVGL
 * renderer only moves the focus - and with it the scroll position - over focusable rows, a page of plain text rows
 * could not be scrolled with the keyboard or the power button. The row's help (help pane) is the full line, so a row
 * cut to PS5_DIAG_VIEW_CHARS can still be read in full.
 * At most PS5_DIAG_VIEW_MAX opcodes. String ids are created once (HiiSetString with id 0) and reused on later runs, so
 * repeated runs do not grow the string package. The check output is always English; HiiSetString without a language
 * sets it for en-US and de-DE alike. */
#define PS5_DIAG_VIEW_MAX   60
#define PS5_DIAG_VIEW_CHARS 90
#define PS5_DIAG_VIEW_HELP_CHARS 158
#define PS5_DIAG_VIEW_QID   0x1400   /* 0x1400..0x143B; the other Ps5InfoDxe keys are 0x1360..0x136D */
#define PS5_DIAG_VIEW_LABEL     0x2000
#define PS5_DIAG_VIEW_LABEL_END 0x2FFF
STATIC EFI_STRING_ID mViewId[PS5_DIAG_VIEW_MAX][2];   /* [0] row text, [1] help */
STATIC EFI_STRING_ID ViewString (UINTN Slot, UINTN Col, CONST CHAR8 *A, UINTN Len, UINTN Max)
{
  CHAR16 W[PS5_DIAG_VIEW_HELP_CHARS + 2]; UINTN K = 0;
  if (Max > PS5_DIAG_VIEW_HELP_CHARS) Max = PS5_DIAG_VIEW_HELP_CHARS;
  while (Len > 0 && A[Len - 1] == ' ') Len--;                       /* trim right */
  while (Len > 0 && *A == ' ') { A++; Len--; }                       /* trim left */
  for (; K < Len && K < Max; K++) W[K] = (CHAR16)(UINT8)A[K];
  if (Len > Max && K >= 3) { W[K - 3] = L'.'; W[K - 2] = L'.'; W[K - 1] = L'.'; }
  if (K == 0) W[K++] = L' ';                                         /* never an empty string */
  W[K] = 0;
  EFI_STRING_ID Id = HiiSetString (mDiagHii, mViewId[Slot][Col], W, NULL);
  if (Id != 0) mViewId[Slot][Col] = Id;
  return Id;
}
/* (Re)build form 2 from ASCII text; returns the verdict word index 0 GREEN, 1 ORANGE, 2 RED, -1 none */
STATIC INTN DiagBuildView (CONST CHAR8 *T, UINTN N)
{
  STATIC CONST CHAR8 *Words[4] = { "PASS", "WARN", "FAIL", "SKIP" };
  STATIC CONST CHAR8 More[] = "... (more in \\PS5DIAG.LOG on the ESP)";
  INTN Verdict = -1; UINTN Slot = 0, Pos = 0; BOOLEAN LastSpacer = TRUE;
  VOID *Start, *End;
  if (mDiagHii == NULL) return -1;
  Start = HiiAllocateOpCodeHandle (); End = HiiAllocateOpCodeHandle ();
  if (Start != NULL && End != NULL) {
    EFI_IFR_GUID_LABEL *L1 = (EFI_IFR_GUID_LABEL *)HiiCreateGuidOpCode (Start, &gEfiIfrTianoGuid, NULL, sizeof (EFI_IFR_GUID_LABEL));
    EFI_IFR_GUID_LABEL *L2 = (EFI_IFR_GUID_LABEL *)HiiCreateGuidOpCode (End, &gEfiIfrTianoGuid, NULL, sizeof (EFI_IFR_GUID_LABEL));
    L1->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL; L1->Number = PS5_DIAG_VIEW_LABEL;
    L2->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL; L2->Number = PS5_DIAG_VIEW_LABEL_END;
    while (Pos < N && Slot < PS5_DIAG_VIEW_MAX) {
      CONST CHAR8 *L = T + Pos; UINTN Len = 0, I;
      CHAR8 Row[PS5_DIAG_VIEW_HELP_CHARS + 2]; UINTN R = 0;
      while (Pos + Len < N && L[Len] != '\n' && L[Len] != 0) Len++;
      Pos += Len + 1;
      if (Len > 0 && L[Len - 1] == '\r') Len--;
      if (Slot == PS5_DIAG_VIEW_MAX - 1 && Pos < N) {                /* last slot and more text left */
        HiiCreateSubTitleOpCode (Start, ViewString (Slot++, 0, More, sizeof (More) - 1, PS5_DIAG_VIEW_CHARS), 0, 0, 0);
        break;
      }
      if (Len == 0) {                                                 /* blank line: one spacer */
        if (!LastSpacer) { HiiCreateSubTitleOpCode (Start, STRING_TOKEN (STR_NULL), 0, 0, 0); LastSpacer = TRUE; }
        continue;
      }
      LastSpacer = FALSE;
      if (Len > 6 && AsciiStrnCmp (L, "== ", 3) == 0 && L[Len - 1] == '=' && L[Len - 2] == '=') {   /* area */
        HiiCreateSubTitleOpCode (Start, ViewString (Slot++, 0, L + 3, Len - 5, PS5_DIAG_VIEW_CHARS), 0, 0, 0);
        continue;
      }
      if (L[Len - 1] == ':') {                                        /* "Caused by:" */
        HiiCreateSubTitleOpCode (Start, ViewString (Slot++, 0, L, Len - 1, PS5_DIAG_VIEW_CHARS), 0, 0, 0);
        continue;
      }
      BOOLEAN Item = FALSE;
      if (Len > 8 && L[0] == ' ' && L[1] == ' ' && L[6] == ' ' && L[7] == ' ')
        for (I = 0; I < 4; I++) if (AsciiStrnCmp (L + 2, Words[I], 4) == 0) Item = TRUE;
      if (Item) {                                                     /* "WARN   Title:  detail" (title ends at 2+ spaces) */
        UINTN Cut = 8, Val;
        while (Cut + 1 < Len && !(L[Cut] == ' ' && L[Cut + 1] == ' ')) Cut++;
        for (Val = Cut; Val < Len && L[Val] == ' '; Val++) ;
        for (I = 2; I < 6; I++) Row[R++] = L[I];
        Row[R++] = ' '; Row[R++] = ' '; Row[R++] = ' ';
        for (I = 8; I < Cut && R < PS5_DIAG_VIEW_HELP_CHARS; I++) Row[R++] = L[I];
        if (Val < Len && R + 3 < PS5_DIAG_VIEW_HELP_CHARS) { Row[R++] = ':'; Row[R++] = ' '; Row[R++] = ' '; }
        for (I = Val; I < Len && R < PS5_DIAG_VIEW_HELP_CHARS; I++) Row[R++] = L[I];
      } else {
        if (Len > 9 && AsciiStrnCmp (L, "Verdict: ", 9) == 0) {
          if (AsciiStrnCmp (L + 9, "GREEN", 5) == 0) Verdict = 0;
          else if (AsciiStrnCmp (L + 9, "ORANGE", 6) == 0) Verdict = 1;
          else if (AsciiStrnCmp (L + 9, "RED", 3) == 0) Verdict = 2;
        }
        for (I = 0; I < Len && R < PS5_DIAG_VIEW_HELP_CHARS; I++) Row[R++] = L[I];
      }
      EFI_STRING_ID P = ViewString (Slot, 0, Row, R, PS5_DIAG_VIEW_CHARS), H = ViewString (Slot, 1, Row, R, PS5_DIAG_VIEW_HELP_CHARS);
      HiiCreateActionOpCode (Start, (EFI_QUESTION_ID)(PS5_DIAG_VIEW_QID + Slot), P, H, 0, 0);
      Slot++;
    }
    HiiUpdateForm (mDiagHii, &mDiagGuid, 2, Start, End);
  }
  if (Start != NULL) HiiFreeOpCodeHandle (Start);
  if (End != NULL) HiiFreeOpCodeHandle (End);
  return Verdict;
}
/* traffic light + viewer from Ps5DiagLog; FALSE when no check ran in this boot (the variable is volatile) */
STATIC BOOLEAN DiagRefreshView (VOID)
{
  STATIC EFI_GUID LogGuid = PS5_DIAG_PROTOCOL_GUID;
  STATIC CHAR8 Text[PS5_DIAG_LOG_MAX];
  UINTN N = sizeof (Text);
  if (mDiagHii == NULL) return FALSE;
  if (EFI_ERROR (gRT->GetVariable (PS5_DIAG_LOG_VARIABLE, &LogGuid, NULL, &N, Text)) || N == 0) {
    SetH (mDiagHii, STRING_TOKEN (STR_DIAG_VERDICT), L"%s", L"No system check run yet.");
    { STATIC CONST CHAR8 None[] = "No system check run yet. Choose \"Run system check\" first."; DiagBuildView (None, sizeof (None) - 1); }
    return FALSE;
  }
  switch (DiagBuildView (Text, N)) {   /* the renderer cannot colour a text: bullet + colour word + the sentence */
    case 0:  SetH (mDiagHii, STRING_TOKEN (STR_DIAG_VERDICT), L"%c GREEN - System compatible.", (CHAR16)0x2022); break;
    case 1:  SetH (mDiagHii, STRING_TOKEN (STR_DIAG_VERDICT), L"%c ORANGE - System could currently be incompatible or cause unexpected problems (details in the log).", (CHAR16)0x2022); break;
    case 2:  SetH (mDiagHii, STRING_TOKEN (STR_DIAG_VERDICT), L"%c RED - System incompatible (details in the log).", (CHAR16)0x2022); break;
    default: SetH (mDiagHii, STRING_TOKEN (STR_DIAG_VERDICT), L"%s", L"Verdict missing in the log - see Show log."); break;
  }
  return TRUE;
}
/* fallback for a BDS without Ps5DiagLog: its ready-made lines, one text each */
STATIC VOID DiagViewLines (CONST PS5_DIAG_RESULT *Res)
{
  STATIC CHAR8 Text[PS5_DIAG_LINES * (PS5_DIAG_LINE_CHARS + 1)];
  UINTN N = 0;
  for (UINTN I = 0; I < Res->Lines && I < PS5_DIAG_LINES; I++) {
    for (UINTN K = 0; K < PS5_DIAG_LINE_CHARS && Res->Line[I][K] != 0; K++) Text[N++] = Res->Line[I][K] < 0x80 ? (CHAR8)Res->Line[I][K] : '?';
    Text[N++] = '\n';
  }
  DiagBuildView (Text, N);
}

STATIC EFI_STATUS EFIAPI Extract (CONST EFI_HII_CONFIG_ACCESS_PROTOCOL *This, CONST EFI_STRING Request, EFI_STRING *Progress, EFI_STRING *Results)
{
  if (Progress == NULL || Results == NULL) return EFI_INVALID_PARAMETER;
  *Progress = Request; return EFI_NOT_FOUND;                       /* no varstore: nothing to extract */
}
STATIC EFI_STATUS EFIAPI Route (CONST EFI_HII_CONFIG_ACCESS_PROTOCOL *This, CONST EFI_STRING Configuration, EFI_STRING *Progress)
{
  if (Configuration == NULL || Progress == NULL) return EFI_INVALID_PARAMETER;
  *Progress = Configuration; return EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI Callback (CONST EFI_HII_CONFIG_ACCESS_PROTOCOL *This, EFI_BROWSER_ACTION Action, EFI_QUESTION_ID Id,
                                   UINT8 Type, EFI_IFR_TYPE_VALUE *Value, EFI_BROWSER_ACTION_REQUEST *Request)
{
  if (Action == EFI_BROWSER_ACTION_FORM_OPEN) { Refresh (); return EFI_SUCCESS; }
  /* v0.3 (r5.4, menu test 16.11.): REQUEST_EXIT from this sub-formset only went back to the front page
   * (FindNextMenu, FormSetLevel), UiApp never returned. Now: the BDS boot-now protocol runs the same post-menu
   * path directly (like BootManagerUiLib boots from its callback); EXIT only as fallback without the protocol. */
  if (Action == EFI_BROWSER_ACTION_CHANGED && Id == PS5_BOOT_WINDOWS_KEY) {
    STATIC EFI_GUID BootNowGuid = PS5_BOOT_NOW_PROTOCOL_GUID;
    PS5_BOOT_NOW_PROTOCOL *BootNow = NULL;
    if (!EFI_ERROR (gBS->LocateProtocol (&BootNowGuid, NULL, (VOID **)&BootNow)) && BootNow != NULL && BootNow->BootWindows != NULL) {
      DEBUG ((DEBUG_ERROR, "PS5 boot menu: Boot Windows chosen - BDS boot-now\n"));
      EFI_STATUS BootStatus = BootNow->BootWindows ();           /* returns only if the boot failed */
      (VOID)BootStatus;
      DEBUG ((DEBUG_ERROR, "PS5 boot menu: boot-now returned %r\n", BootStatus));
      return EFI_SUCCESS;
    }
    DEBUG ((DEBUG_ERROR, "PS5 boot menu: Boot Windows chosen - no boot-now protocol, leaving the setup browser\n"));
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_EXIT;
    return EFI_SUCCESS;
  }
  if (Action == EFI_BROWSER_ACTION_CHANGED && (Id == PS5_BOOT_RESTART_KEY || Id == PS5_BOOT_POWEROFF_KEY)) {
    DEBUG ((DEBUG_ERROR, "PS5 boot menu: %a chosen\n", Id == PS5_BOOT_RESTART_KEY ? "restart" : "power off"));
    { /* r5.21: settings (boot order list etc.) to \PS5UEFI.CFG before the reset - before, only a Windows start saved them */
      STATIC EFI_GUID BootNowGuid = PS5_BOOT_NOW_PROTOCOL_GUID; PS5_BOOT_NOW_PROTOCOL *Bn = NULL;
      if (!EFI_ERROR (gBS->LocateProtocol (&BootNowGuid, NULL, (VOID **)&Bn)) && Bn != NULL && Bn->Revision >= 2 && Bn->SaveSettings != NULL)
      { EFI_STATUS Sv = Bn->SaveSettings (); DEBUG ((DEBUG_ERROR, "PS5 boot menu: settings saved before reset (%r)\n", Sv)); (VOID)Sv; }
    }
    gRT->ResetSystem (Id == PS5_BOOT_RESTART_KEY ? EfiResetCold : EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_DEVICE_ERROR;                                        /* only if the reset returned */
  }
  /* r5.6: power preset - store at once (volatile variable, the BDS writes \PS5POWER.CFG before bootmgr) and
   * submit the form so the browser keeps no unsaved state (no "save changes?" popup). */
  /* v0.5 (r5.7): language - PlatformLang for the browser now, Ps5Lang for the form, \PS5UEFI.CFG written by the BDS */
  /* r5.23 (user: "dropping the disk saves the order at once"): the renderer submits the boot list once per drop;
   * the browser would write Ps5BootListSel only after this callback (FORM_APPLY), so write it here from the new value
   * and let the BDS put it into \PS5UEFI.CFG now - not only at "Save & Exit" / Windows start. */
  if (Action == EFI_BROWSER_ACTION_CHANGED && Id == PS5_BOOTLIST_KEY && Type == EFI_IFR_TYPE_BUFFER && Value != NULL) {
    STATIC EFI_GUID ListGuid = PS5_POWER_PRESET_GUID, BootNowGuid = PS5_BOOT_NOW_PROTOCOL_GUID;
    PS5_BOOT_LIST L; PS5_BOOT_NOW_PROTOCOL *Bn = NULL; EFI_STATUS Sv = EFI_NOT_STARTED;
    CopyMem (L.Order, Value, sizeof (L.Order));
    EFI_STATUS Sl = gRT->SetVariable (L"Ps5BootListSel", &ListGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (L), &L);
    if (!EFI_ERROR (Sl) && !EFI_ERROR (gBS->LocateProtocol (&BootNowGuid, NULL, (VOID **)&Bn)) && Bn != NULL && Bn->Revision >= 2 && Bn->SaveSettings != NULL)
      Sv = Bn->SaveSettings ();
    DEBUG ((DEBUG_ERROR, "PS5 boot list: %02x %02x %02x %02x stored (%r), saved to PS5UEFI.CFG (%r)\n", L.Order[0], L.Order[1], L.Order[2], L.Order[3], Sl, Sv));
    (VOID)Sv;
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_FORM_APPLY;
    return EFI_SUCCESS;
  }
  if (Action == EFI_BROWSER_ACTION_CHANGED && Value != NULL &&
      (Id == PS5_FAST_KEY || Id == PS5_PSTATE_KEY || Id == PS5_BOOTORDER_KEY || Id == PS5_REFRESH_KEY || Id == PS5_USBCYCLE_KEY || Id == PS5_FASTRESTART_KEY)) {
    STATIC EFI_GUID SetGuid = PS5_POWER_PRESET_GUID, BootNowGuid = PS5_BOOT_NOW_PROTOCOL_GUID;
    UINT32 V = (Type == EFI_IFR_TYPE_NUM_SIZE_8 ? Value->u8 : Type == EFI_IFR_TYPE_NUM_SIZE_16 ? Value->u16 : Value->u32) ? 1 : 0;
    CHAR16 *Var = Id == PS5_FAST_KEY ? L"Ps5FastBoot" :
                  Id == PS5_PSTATE_KEY ? L"Ps5PStates" :
                  Id == PS5_BOOTORDER_KEY ? L"Ps5BootOrder" :
                  Id == PS5_REFRESH_KEY ? L"Ps5Refresh" :
                  Id == PS5_FASTRESTART_KEY ? L"Ps5FastRestart" : L"Ps5UsbCycle";
    EFI_STATUS St = gRT->SetVariable (Var, &SetGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (V), &V);
    PS5_BOOT_NOW_PROTOCOL *Bn = NULL; EFI_STATUS Sv = EFI_NOT_STARTED;
    if (!EFI_ERROR (St) && !EFI_ERROR (gBS->LocateProtocol (&BootNowGuid, NULL, (VOID **)&Bn)) && Bn != NULL && Bn->Revision >= 2 && Bn->SaveSettings != NULL)
      Sv = Bn->SaveSettings ();
    DEBUG ((DEBUG_ERROR, "PS5 setting %s=%u stored (%r), saved to PS5UEFI.CFG (%r)\n", Var, V, St, Sv));
    (VOID)Sv;
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_FORM_APPLY;
    return EFI_SUCCESS;
  }
  if (Action == EFI_BROWSER_ACTION_CHANGED && Id == PS5_RESOLUTION_KEY && Value != NULL) {   /* r5.27: full value, not 0/1 */
    STATIC EFI_GUID SetGuid = PS5_POWER_PRESET_GUID, BootNowGuid = PS5_BOOT_NOW_PROTOCOL_GUID;
    UINT32 V = Type == EFI_IFR_TYPE_NUM_SIZE_8 ? Value->u8 : Type == EFI_IFR_TYPE_NUM_SIZE_16 ? Value->u16 : Value->u32;
    if (V > 8) V = 0;
    EFI_STATUS St = gRT->SetVariable (L"Ps5Resolution", &SetGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (V), &V);
    PS5_BOOT_NOW_PROTOCOL *Bn = NULL; EFI_STATUS Sv = EFI_NOT_STARTED;
    if (!EFI_ERROR (St) && !EFI_ERROR (gBS->LocateProtocol (&BootNowGuid, NULL, (VOID **)&Bn)) && Bn != NULL && Bn->Revision >= 2 && Bn->SaveSettings != NULL)
      Sv = Bn->SaveSettings ();
    DEBUG ((DEBUG_ERROR, "PS5 setting Ps5Resolution=%u stored (%r), saved to PS5UEFI.CFG (%r)\n", V, St, Sv));
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_FORM_APPLY;
    return EFI_SUCCESS;
  }
  if (Action == EFI_BROWSER_ACTION_CHANGED && Id == PS5_LANG_KEY && Value != NULL) {
    STATIC EFI_GUID LangGuid = PS5_POWER_PRESET_GUID, BootNowGuid = PS5_BOOT_NOW_PROTOCOL_GUID;
    PS5_LANG L; L.Lang = (Type == EFI_IFR_TYPE_NUM_SIZE_8 ? Value->u8 : Type == EFI_IFR_TYPE_NUM_SIZE_16 ? Value->u16 : Value->u32) == 1 ? 1 : 0;
    CHAR8 *Pl = L.Lang ? "de-DE" : "en-US";
    gRT->SetVariable (L"Ps5Lang", &LangGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (L), &L);
    EFI_STATUS S = gRT->SetVariable (L"PlatformLang", &gEfiGlobalVariableGuid,
                     EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE, AsciiStrSize (Pl), Pl);
    PS5_BOOT_NOW_PROTOCOL *Bn = NULL; EFI_STATUS Sv = EFI_NOT_STARTED;
    if (!EFI_ERROR (S) && !EFI_ERROR (gBS->LocateProtocol (&BootNowGuid, NULL, (VOID **)&Bn)) && Bn != NULL && Bn->Revision >= 2 && Bn->SaveSettings != NULL)
      Sv = Bn->SaveSettings ();
    DEBUG ((DEBUG_ERROR, "PS5 language: %a (%r), saved to PS5UEFI.CFG (%r)\n", Pl, S, Sv));
    (VOID)Sv;
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_FORM_APPLY;
    return EFI_SUCCESS;
  }
  if (Action == EFI_BROWSER_ACTION_CHANGED && Id == PS5_POWER_KEY && Value != NULL) {
    STATIC EFI_GUID PowerGuid = PS5_POWER_PRESET_GUID, BootNowGuid = PS5_BOOT_NOW_PROTOCOL_GUID;
    PS5_POWER_PRESET P;
    P.Preset = (UINT32)Value->u32;
    if (Type == EFI_IFR_TYPE_NUM_SIZE_8) P.Preset = Value->u8;
    else if (Type == EFI_IFR_TYPE_NUM_SIZE_16) P.Preset = Value->u16;
    if (P.Preset > PS5_POWER_OC) P.Preset = PS5_POWER_STANDARD;
    EFI_STATUS S = gRT->SetVariable (L"Ps5PowerPreset", &PowerGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (P), &P);
    PS5_BOOT_NOW_PROTOCOL *Bn = NULL; EFI_STATUS Sv = EFI_NOT_STARTED;
    if (!EFI_ERROR (S) && !EFI_ERROR (gBS->LocateProtocol (&BootNowGuid, NULL, (VOID **)&Bn)) && Bn != NULL && Bn->Revision >= 2 && Bn->SaveSettings != NULL)
      Sv = Bn->SaveSettings ();
    DEBUG ((DEBUG_ERROR, "PS5 power preset: %u chosen (%r), saved to PS5UEFI.CFG (%r)\n", P.Preset, S, Sv));
    (VOID)Sv;
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_FORM_APPLY;
    return EFI_SUCCESS;
  }
  if (Action == EFI_BROWSER_ACTION_CHANGED && Id == PS5_DIAG_RUN_KEY) {   /* r5.20: run the read-only system check in the BDS */
    STATIC EFI_GUID DiagGuid = PS5_DIAG_PROTOCOL_GUID;
    STATIC PS5_DIAG_RESULT Res;
    PS5_DIAG_PROTOCOL *Diag = NULL;
    if (EFI_ERROR (gBS->LocateProtocol (&DiagGuid, NULL, (VOID **)&Diag)) || Diag == NULL || Diag->Run == NULL) {
      SetH (mDiagHii, STRING_TOKEN (STR_DIAG_SUMMARY), L"%s", L"System check not available (needs the PSWloader boot path).");
    } else {
      SetH (mDiagHii, STRING_TOKEN (STR_DIAG_SUMMARY), L"%s", L"...");
      EFI_STATUS S = Diag->Run (&Res);
      if (EFI_ERROR (S)) {
        SetH (mDiagHii, STRING_TOKEN (STR_DIAG_SUMMARY), L"System check failed (%r).", S);
      } else {
        if (Res.Summary[0] != L'\0') {
          SetH (mDiagHii, STRING_TOKEN (STR_DIAG_SUMMARY), L"%s", Res.Summary);
        } else {
          SetH (mDiagHii, STRING_TOKEN (STR_DIAG_SUMMARY), L"%u PASS, %u WARN, %u FAIL, %u SKIP - %s", Res.Pass, Res.Warn,
                Res.Fail, Res.Skip, Res.LogStatus == 0 ? L"saved" : L"please plug in a FAT32 USB drive to save the log");
        }
        if (!DiagRefreshView ()) {   /* BDS without Ps5DiagLog (older build): show its lines, no verdict */
          SetH (mDiagHii, STRING_TOKEN (STR_DIAG_VERDICT), L"%s", L"Verdict not available (BDS without Ps5DiagLog) - see Show log.");
          DiagViewLines (&Res);
        }
      }
      DEBUG ((DEBUG_ERROR, "PS5 system check from the menu: %r, %u PASS %u WARN %u FAIL %u SKIP\n", S, Res.Pass, Res.Warn, Res.Fail, Res.Skip));
    }
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_FORM_APPLY;   /* redraw with the new strings */
    return EFI_SUCCESS;
  }
  if (Id >= PS5_DIAG_VIEW_QID && Id < PS5_DIAG_VIEW_QID + PS5_DIAG_VIEW_MAX) return EFI_SUCCESS;   /* v1.2: log viewer rows, read only */
  if (Action == EFI_BROWSER_ACTION_CHANGED && Id == PS5_INFO_REFRESH_KEY) {
    Refresh ();
    if (Request != NULL) *Request = EFI_BROWSER_ACTION_REQUEST_FORM_APPLY;   /* redraw with the new strings */
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL mAccess = { Extract, Route, Callback };

EFI_STATUS EFIAPI Ps5InfoEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = gBS->InstallMultipleProtocolInterfaces (&mHandle, &gEfiDevicePathProtocolGuid, &mPath,
                                                              &gEfiHiiConfigAccessProtocolGuid, &mAccess, NULL);
  if (EFI_ERROR (Status)) return Status;
  mHii = HiiAddPackages (&mFormsetGuid, mHandle, Ps5InfoVfrBin, Ps5InfoDxeStrings, NULL);
  if (mHii == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (mHandle, &gEfiDevicePathProtocolGuid, &mPath, &gEfiHiiConfigAccessProtocolGuid, &mAccess, NULL);
    return EFI_OUT_OF_RESOURCES;
  }
  /* r5.22 (PROPOSAL uefi-gui): six tabs in front-page order Overview, Performance & Fan, Display, Boot, Diagnostics,
   * Save & Exit (one HII handle each; registration order = tab order; language moved to Overview). */
  if (!EFI_ERROR (gBS->InstallMultipleProtocolInterfaces (&mPerfHandle, &gEfiDevicePathProtocolGuid, &mPerfPath, &gEfiHiiConfigAccessProtocolGuid, &mAccess, NULL)))
    HiiAddPackages (&mPerfGuid, mPerfHandle, Ps5PerfVfrBin, Ps5InfoDxeStrings, NULL);
  if (!EFI_ERROR (gBS->InstallMultipleProtocolInterfaces (&mDispHandle, &gEfiDevicePathProtocolGuid, &mDispPath, &gEfiHiiConfigAccessProtocolGuid, &mAccess, NULL)))
    mDispHii = HiiAddPackages (&mDispGuid, mDispHandle, Ps5DispVfrBin, Ps5InfoDxeStrings, NULL);
  if (!EFI_ERROR (gBS->InstallMultipleProtocolInterfaces (&mBootHandle, &gEfiDevicePathProtocolGuid, &mBootPath,
                                                          &gEfiHiiConfigAccessProtocolGuid, &mAccess, NULL)))
    mBootHii = HiiAddPackages (&mBootFormsetGuid, mBootHandle, Ps5BootVfrBin, Ps5InfoDxeStrings, NULL);
  if (!EFI_ERROR (gBS->InstallMultipleProtocolInterfaces (&mDiagHandle, &gEfiDevicePathProtocolGuid, &mDiagPath, &gEfiHiiConfigAccessProtocolGuid, &mAccess, NULL)))
    mDiagHii = HiiAddPackages (&mDiagGuid, mDiagHandle, Ps5DiagVfrBin, Ps5InfoDxeStrings, NULL);
  DiagRefreshView ();   /* v1.2: "No system check run yet." in the traffic light and the log viewer */
  if (!EFI_ERROR (gBS->InstallMultipleProtocolInterfaces (&mExitHandle, &gEfiDevicePathProtocolGuid, &mExitPath, &gEfiHiiConfigAccessProtocolGuid, &mAccess, NULL)))
    HiiAddPackages (&mExitGuid, mExitHandle, Ps5ExitVfrBin, Ps5InfoDxeStrings, NULL);
  Refresh ();
  DEBUG ((DEBUG_INFO, "PS5 info: HII forms 'PS5 System' and 'PS5 Boot' (%a) published\n", mBootHii != NULL ? "ok" : "FAILED"));
  return EFI_SUCCESS;
}
