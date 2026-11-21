/** @file
  PS5 loader UART TX and retained-arena diagnostic ring.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Register contract: pinned ps5-linux-loader shellcode_hv/utils.c.
  UART configuration is inherited; RX/baud/flow control are not implemented.
  The ring is BSP-only, non-persistent, and valid only while the loader arena
  stays reserved. This instance is not a runtime or multiprocessor console.
**/
#include <Base.h>
#include <Library/SerialPortLib.h>
#include <Library/IoLib.h>

#define PS5_LOG_BASE  0x111001000ULL
#define PS5_LOG_SIZE  65536U
#define PS5_LOG_MAGIC 0x31474F4C355350ULL

RETURN_STATUS EFIAPI SerialPortInitialize (VOID) {
  return RETURN_SUCCESS;
}

UINTN EFIAPI SerialPortWrite (IN UINT8 *Buffer, IN UINTN NumberOfBytes) {
  volatile UINT64 *Header = (volatile UINT64 *)(UINTN)PS5_LOG_BASE;
  volatile UINT8 *Ring = (volatile UINT8 *)(UINTN)(PS5_LOG_BASE + 16);
  UINTN Written = 0;
  BOOLEAN UartReady = TRUE;
  if (Buffer == NULL) {
    return 0;
  }
  if (Header[0] != PS5_LOG_MAGIC) {
    Header[1] = 0;
    Header[0] = PS5_LOG_MAGIC;
  }
  for (UINTN Index = 0; Index < NumberOfBytes; Index++) {
    UINT64 Position = Header[1];
    Ring[Position % PS5_LOG_SIZE] = Buffer[Index];
    Header[1] = Position + 1;
    // A missing/busy UART must not hang the firmware's diagnostic path.
    if (UartReady) {
      UINTN Budget = 10000;
      while (Budget != 0 && (MmioRead32 (0xC101010C) & BIT5) == 0) {
        Budget--;
      }
      if (Budget == 0) {
        UartReady = FALSE;
      } else {
        MmioWrite32 (0xC1010104, Buffer[Index]);
        Written++;
      }
    }
  }
#ifdef PS5_WARM_RESET_TEST
  { /* lab: during a warm re-entry (marker set by the trampoline) one 10x10 square per message on the screen, colour =
     * hash of its first 16 bytes (FNV-1a | 0x404040), so the last message before a hang can be identified offline */
    volatile UINT64 *W = (volatile UINT64 *)(UINTN)0x111806000ULL;
    if (W[0] == 0x59525445524D5257ULL && W[2] != 0 && NumberOfBytes > 0) {
      UINT32 H = 2166136261u;
      for (UINTN I = 0; I < NumberOfBytes && I < 16; I++) { H ^= Buffer[I]; H *= 16777619u; }
      UINT32 Color = (H & 0x00FFFFFF) | 0x00404040;
      UINT64 N = W[1]++;
      volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)W[2];
      UINT64 Stride = W[3], X = (N % 40) * 30, Y = 330 + (N / 40) * 30;
      for (UINT64 Dy = 0; Dy < 20; Dy++) for (UINT64 Dx = 0; Dx < 20; Dx++) Fb[(Y + Dy) * Stride + X + Dx] = Color;
    }
  }
#endif
  return Written;
}

UINTN EFIAPI SerialPortRead (OUT UINT8 *Buffer, IN UINTN NumberOfBytes) { return 0; }
BOOLEAN EFIAPI SerialPortPoll (VOID) { return FALSE; }
RETURN_STATUS EFIAPI SerialPortSetControl (IN UINT32 Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortGetControl (OUT UINT32 *Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortSetAttributes (
  IN OUT UINT64 *BaudRate, IN OUT UINT32 *ReceiveFifoDepth, IN OUT UINT32 *Timeout,
  IN OUT EFI_PARITY_TYPE *Parity, IN OUT UINT8 *DataBits, IN OUT EFI_STOP_BITS_TYPE *StopBits
  ) { return RETURN_UNSUPPORTED; }
