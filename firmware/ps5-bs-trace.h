/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * lab --warm-bs-trace (19.11.2026): boot-services trace for a warm run, painted on the screen by an AP.
 *
 * Right before bootmgfw starts (warm run only) every EFI_BOOT_SERVICES entry is replaced by a thunk. A thunk only stores
 * the service index and the caller's return address in the reserved arena (0x1_1186_0000) and jumps to the original with
 * all registers and the stack untouched - valid for every signature, also the variadic ones. One AP paints the cells four
 * times a second onto the boot logo (top left, 5x scaled 3x5 hex font, 1920x1080 capture readable). The BSP needs to do
 * nothing: if it hangs, the last picture stays.
 *
 * Screen lines (hex only, top to bottom):
 *   0 yellow  AP frame counter (8 digits) - changes = the AP paints
 *   1 white   boot-service calls (16 digits) - changes = the BSP still calls boot services
 *   2 blue    software-timer ticks (16 digits, counted by LocalApicTimerDxe)
 *   3 cyan    last service index (2 digits) + its caller's return address (16 digits)
 *   4 green   loaded-image count (2 digits)
 *   5-8 green the last 4 loaded images: base (16 digits) + size (8 digits), slot = count mod 4
 *   9-24 grey the last 16 calls, newest first: index (2) + caller (16)
 * Service index = position in EFI_BOOT_SERVICES after the header: 00 RaiseTPL 01 RestoreTPL 02 AllocatePages
 * 03 FreePages 04 GetMemoryMap 05 AllocatePool 06 FreePool 07 CreateEvent 08 SetTimer 09 WaitForEvent 0A SignalEvent
 * 0B CloseEvent 0C CheckEvent 0D InstallProtocolInterface 0E ReinstallProtocolInterface 0F UninstallProtocolInterface
 * 10 HandleProtocol 11 (reserved) 12 RegisterProtocolNotify 13 LocateHandle 14 LocateDevicePath
 * 15 InstallConfigurationTable 16 LoadImage 17 StartImage 18 Exit 19 UnloadImage 1A ExitBootServices
 * 1B GetNextMonotonicCount 1C Stall 1D SetWatchdogTimer 1E ConnectController 1F DisconnectController 20 OpenProtocol
 * 21 CloseProtocol 22 OpenProtocolInformation 23 ProtocolsPerHandle 24 LocateHandleBuffer 25 LocateProtocol
 * 26 InstallMultipleProtocolInterfaces 27 UninstallMultipleProtocolInterfaces 28 CalculateCrc32 29 CopyMem 2A SetMem
 * 2B CreateEventEx
 *
 * Arena 0x1_1186_0000 (free: rescue copy ends at 0x1_1185_0020): +0 calls, +8 last index, +0x10 last caller, +0x18 magic
 * "PSWBSTRC", +0x20 AP frames, +0x38 soft ticks, +0x40 index ring[16], +0xC0 caller ring[16], +0x140 FB base,
 * +0x148 FB stride, +0x160 image count, +0x168 images[4] (base, size).
 * ExitBootServices: MpInitLib parks the APs (INIT-SIPI) - the painting stops there, which is fine: the hang is before.
 */
#ifndef PS5_BS_TRACE_H
#define PS5_BS_TRACE_H
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Pi/PiMultiPhase.h>
#include <Protocol/MpService.h>

#define PS5_BST        0x111860000ULL
#define PS5_BST_MAGIC  0x4352545342575350ULL   /* "PSWBSTRC" */
#define PS5_BST_N      44
STATIC_ASSERT ((sizeof (EFI_BOOT_SERVICES) - sizeof (EFI_TABLE_HEADER)) / sizeof (VOID *) == PS5_BST_N,
               "EFI_BOOT_SERVICES entry count");

/* originals, indexed like the thunks (global + used: the assembly below refers to it, also under LTO) */
__attribute__((used)) UINT64  Ps5BsTraceOrig[PS5_BST_N];
extern UINT8                  Ps5BsTraceThunks[];   /* PS5_BST_N thunks of 10 bytes: mov $n,%eax; jmp common */

__asm__ (
  ".pushsection .text\n"
  ".p2align 4\n"
  ".globl Ps5BsTraceThunks\n"
  "Ps5BsTraceThunks:\n"
  ".set ps5bstn, 0\n"
  ".rept 44\n"
  "  .byte 0xb8\n  .long ps5bstn\n"
  "  .byte 0xe9\n  .long Ps5BsTraceCommon - . - 4\n"
  "  .set ps5bstn, ps5bstn + 1\n"
  ".endr\n"
  "Ps5BsTraceCommon:\n"                     /* rax = index; rcx/rdx/r8/r9 and the stack belong to the callee */
  "  movabs $0x111860000, %r10\n"
  "  mov %rax, 8(%r10)\n"
  "  mov (%rsp), %r11\n"
  "  mov %r11, 0x10(%r10)\n"
  "  mov (%r10), %r11\n"
  "  inc %r11\n"
  "  mov %r11, (%r10)\n"
  "  and $15, %r11\n"
  "  mov %rax, 0x40(%r10,%r11,8)\n"
  "  mov (%rsp), %rax\n"
  "  mov %rax, 0xc0(%r10,%r11,8)\n"
  "  mov 8(%r10), %rax\n"
  "  lea Ps5BsTraceOrig(%rip), %r11\n"
  "  jmp *(%r11,%rax,8)\n"
  ".popsection\n"
  );

STATIC CONST UINT16  mPs5BstFont[16] = {   /* 3x5, row 0 in bits 14..12 */
  0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,
  0x7BEF, 0x7BCF, 0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79E7, 0x79E4
};
#define PS5_BST_SCALE  5
#define PS5_BST_ADV    20   /* 3 * 5 + 5 */
#define PS5_BST_LINE   32
#define PS5_BST_X0     16
#define PS5_BST_Y0     24

STATIC VOID Ps5BstHex (volatile UINT32 *Fb, UINT64 Stride, UINTN Col, UINTN Line, UINT64 V, UINTN Digits, UINT32 Color)
{
  for (UINTN D = 0; D < Digits; D++) {
    UINT16 G = mPs5BstFont[(V >> (4 * (Digits - 1 - D))) & 15];
    UINTN  X0 = PS5_BST_X0 + (Col + D) * PS5_BST_ADV, Y0 = PS5_BST_Y0 + Line * PS5_BST_LINE;
    for (UINTN Y = 0; Y < 5 * PS5_BST_SCALE; Y++) {
      for (UINTN X = 0; X < 3 * PS5_BST_SCALE; X++) {
        BOOLEAN On = ((G >> ((4 - Y / PS5_BST_SCALE) * 3 + (2 - X / PS5_BST_SCALE))) & 1) != 0;
        Fb[(Y0 + Y) * Stride + X0 + X] = On ? Color : 0;
      }
    }
  }
}

/* AP: never returns (ExitBootServices parks it) */
STATIC VOID EFIAPI Ps5BsTracePaint (IN VOID *Context)
{
  volatile UINT64  *C  = (volatile UINT64 *)(UINTN)PS5_BST;
  volatile UINT32  *Fb = (volatile UINT32 *)(UINTN)C[0x140 / 8];
  UINT64           S   = C[0x148 / 8];
  for (UINT64 F = 0; ; F++) {
    UINT64 N = C[0];
    C[0x20 / 8] = F;
    for (UINTN Y = PS5_BST_Y0 - 8; Y < PS5_BST_Y0 + 25 * PS5_BST_LINE; Y++) {   /* black panel */
      for (UINTN X = PS5_BST_X0 - 8; X < PS5_BST_X0 + 26 * PS5_BST_ADV; X++) Fb[Y * S + X] = 0;
    }
    Ps5BstHex (Fb, S, 0, 0, F, 8, 0x00FFFF00);
    Ps5BstHex (Fb, S, 0, 1, N, 16, 0x00FFFFFF);
    Ps5BstHex (Fb, S, 0, 2, C[0x38 / 8], 16, 0x0080C0FF);
    Ps5BstHex (Fb, S, 0, 3, C[1], 2, 0x0000FFFF);
    Ps5BstHex (Fb, S, 3, 3, C[2], 16, 0x0000FFFF);
    Ps5BstHex (Fb, S, 0, 4, C[0x160 / 8], 2, 0x0000FF00);
    for (UINTN I = 0; I < 4; I++) {
      Ps5BstHex (Fb, S, 0, 5 + I, C[0x168 / 8 + 2 * I], 16, 0x0000FF00);
      Ps5BstHex (Fb, S, 17, 5 + I, C[0x170 / 8 + 2 * I], 8, 0x0000FF00);
    }
    for (UINTN K = 0; K < 16; K++) {
      UINTN P = (UINTN)((N - K) & 15);
      Ps5BstHex (Fb, S, 0, 9 + K, C[0x40 / 8 + P], 2, 0x00C0C0C0);
      Ps5BstHex (Fb, S, 3, 9 + K, C[0xC0 / 8 + P], 16, 0x00C0C0C0);
    }
    UINT64 T0 = AsmReadTsc ();
    while (AsmReadTsc () - T0 < 1596300000ULL / 4) CpuPause ();
  }
}

STATIC VOID   *mPs5BstImageReg;
STATIC EFI_GUID mPs5BstLoadedImageGuid = { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
STATIC VOID EFIAPI Ps5BsTraceImage (IN EFI_EVENT Event, IN VOID *Context)
{
  volatile UINT64 *C = (volatile UINT64 *)(UINTN)PS5_BST;
  for ( ; ; ) {
    EFI_HANDLE                 H  = NULL;
    UINTN                      Sz = sizeof (H);
    EFI_LOADED_IMAGE_PROTOCOL  *Li = NULL;
    if (EFI_ERROR (gBS->LocateHandle (ByRegisterNotify, NULL, mPs5BstImageReg, &Sz, &H))) break;
    if (!EFI_ERROR (gBS->HandleProtocol (H, &mPs5BstLoadedImageGuid, (VOID **)&Li)) && (Li != NULL)) {
      UINT64 N = C[0x160 / 8];
      C[0x168 / 8 + 2 * (N % 4)] = (UINT64)(UINTN)Li->ImageBase;
      C[0x170 / 8 + 2 * (N % 4)] = Li->ImageSize;
      C[0x160 / 8] = N + 1;
    }
  }
}

/* BSP, right before bootmgfw starts, warm run only */
STATIC VOID Ps5BsTraceStart (VOID)
{
  volatile UINT64                *C  = (volatile UINT64 *)(UINTN)PS5_BST;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *G  = NULL;
  EFI_MP_SERVICES_PROTOCOL       *Mp = NULL;
  EFI_EVENT                      Ev  = NULL, Done = NULL;
  EFI_STATUS                     St;
  UINTN                          *T  = (UINTN *)((UINT8 *)gBS + sizeof (EFI_TABLE_HEADER));
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&G)) || (G == NULL) ||
      (G->Mode == NULL) || (G->Mode->Info == NULL) || (G->Mode->FrameBufferBase == 0)) {
    DEBUG ((DEBUG_ERROR, "PS5 bs-trace: no GOP framebuffer - off\n"));
    return;
  }
  for (UINTN I = 0; I < 0x200 / 8; I++) C[I] = 0;
  C[0x140 / 8] = G->Mode->FrameBufferBase;
  C[0x148 / 8] = G->Mode->Info->PixelsPerScanLine;
  if (!EFI_ERROR (gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Ps5BsTraceImage, NULL, &Ev))) {
    gBS->RegisterProtocolNotify (&mPs5BstLoadedImageGuid, Ev, &mPs5BstImageReg);
  }
  St = gBS->LocateProtocol (&gEfiMpServiceProtocolGuid, NULL, (VOID **)&Mp);
  if (!EFI_ERROR (St)) St = gBS->CreateEvent (0, 0, NULL, NULL, &Done);
  if (!EFI_ERROR (St)) St = Mp->StartupThisAP (Mp, Ps5BsTracePaint, 1, Done, 0, NULL, NULL);
  DEBUG ((DEBUG_ERROR, "PS5 bs-trace: painter AP 1 %r, FB %lx stride %u, thunks at %p\n",
          St, G->Mode->FrameBufferBase, G->Mode->Info->PixelsPerScanLine, Ps5BsTraceThunks));
  for (UINTN I = 0; I < PS5_BST_N; I++) {
    Ps5BsTraceOrig[I] = T[I];
    if (T[I] != 0) T[I] = (UINTN)(Ps5BsTraceThunks + 10 * I);
  }
  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  C[0x18 / 8] = PS5_BST_MAGIC;   /* LocalApicTimerDxe counts its soft ticks into +0x38 from now on */
}
#endif
