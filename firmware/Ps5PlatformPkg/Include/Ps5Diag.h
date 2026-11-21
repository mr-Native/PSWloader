/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * r5.20 (17.11.2026): BIOS "System check" from inside the setup browser. Installed by the BDS (Ps5BootWindows.c, needs
 * PS5_NATIVE_SETUP + PS5_NATIVE_DP_VIDEO) before UiApp starts. Run() executes the read-only check (ps5-diag-uefi.h),
 * writes \PS5DIAG.LOG on the config ESP and returns ready-made lines in the configured language, so Ps5InfoDxe only
 * sets strings. Call at TPL_APPLICATION (browser callback).
 * 18.11.2026 (traffic light): every run also stores the complete \PS5DIAG.LOG text (ASCII, CRLF lines, English,
 * first line "Verdict: GREEN|ORANGE|RED - ...") in the volatile boot-services variable Ps5DiagLog under the protocol
 * GUID; Ps5InfoDxe reads it for the traffic light and the log viewer. Line[]/Summary stay for older readers. */
#ifndef PS5_DIAG_PROTOCOL_H_
#define PS5_DIAG_PROTOCOL_H_
#define PS5_DIAG_PROTOCOL_GUID {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x07}}
#define PS5_DIAG_LINES 24
#define PS5_DIAG_LINE_CHARS 100
#define PS5_DIAG_LOG_VARIABLE L"Ps5DiagLog"   /* 18.11.2026 */
#define PS5_DIAG_LOG_MAX 6144                 /* bytes, worst case (21 items + 21 causes) fits */
typedef struct {
  UINT32 Lines, Pass, Warn, Fail, Skip;
  INT32 LogStatus;                                   /* EFI_STATUS of the \PS5DIAG.LOG write, as INT32 (0 = written) */
  CHAR16 Summary[PS5_DIAG_LINE_CHARS];
  CHAR16 Line[PS5_DIAG_LINES][PS5_DIAG_LINE_CHARS];  /* "PASS  Processor (APU): detail" */
} PS5_DIAG_RESULT;
typedef struct {
  UINT32 Revision;                                   /* 1 */
  EFI_STATUS (EFIAPI *Run)(PS5_DIAG_RESULT *Out);
} PS5_DIAG_PROTOCOL;
#endif
