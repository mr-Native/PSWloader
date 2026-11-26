/** SPDX-License-Identifier: BSD-2-Clause-Patent
 * PS5 segment-zero PCI configuration access, before ExitBootServices only.
 * Root-port identities/topology come from the captured production boot.
 * Shared lock is initialized by bootstrap.c before entering UEFI.
 */
#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PciExpressLib.h>
#include <Library/SynchronizationLib.h>
#include "pci-transport.h"

#define ECAM_BASE 0xF0000000U
#define ECAM_SIZE 0x08000000U
#ifndef SHARED_LOCK
#define SHARED_LOCK ((volatile UINT32 *)(UINTN)0x111000100ULL)
#endif

STATIC int RawRead (void *Context, uint32_t Address, unsigned Width, uint32_t *Value) {
  if (Width == 1) *Value = MmioRead8 (ECAM_BASE + Address);
  else if (Width == 2) *Value = MmioRead16 (ECAM_BASE + Address);
  else *Value = MmioRead32 (ECAM_BASE + Address);
  return 0;
}
STATIC int RawWrite (void *Context, uint32_t Address, unsigned Width, uint32_t Value) {
  if (Width == 1) MmioWrite8 (ECAM_BASE + Address, (UINT8)Value);
  else if (Width == 2) MmioWrite16 (ECAM_BASE + Address, (UINT16)Value);
  else MmioWrite32 (ECAM_BASE + Address, Value);
  return 0;
}
// The adapter holds the shared lock around ARI discovery and the complete RMW.
STATIC int AlreadyLocked (void *Context) { return 0; }
STATIC void StayLocked (void *Context) { }
STATIC CONST struct ps5_pci_io Io = {NULL, AlreadyLocked, StayLocked, RawRead, RawWrite};

STATIC BOOLEAN AriForwarding (UINTN Address) {
  UINTN Bus = Address >> 20;
  UINTN Bridge;
  UINT32 Identity;
  if (Bus == 0x40) { Bridge = 0x9000; Identity = 0x13DF1022; }
  else if (Bus == 0x20) { Bridge = 0x41000; Identity = 0x13E51022; }
  else return FALSE;
  if (MmioRead32 (ECAM_BASE + Bridge) != Identity ||
      MmioRead8 (ECAM_BASE + Bridge + 0x19) != Bus ||
      (MmioRead16 (ECAM_BASE + Bridge + 6) & BIT4) == 0) return FALSE;
  UINTN Cap = MmioRead8 (ECAM_BASE + Bridge + 0x34) & 0xFC;
  // At most 48 conventional capabilities; malformed/cyclic lists fail closed.
  for (UINTN Count = 0; Count < 48 && Cap >= 0x40 && Cap <= 0xFC; Count++) {
    if (MmioRead8 (ECAM_BASE + Bridge + Cap) == 0x10) {
      UINT16 Flags = MmioRead16 (ECAM_BASE + Bridge + Cap + 2);
      if ((Flags & 15) < 2 || ((Flags >> 4) & 15) != 4 || Cap > 0xD4) return FALSE;
      return (MmioRead16 (ECAM_BASE + Bridge + Cap + 0x28) & BIT5) != 0;
    }
    Cap = MmioRead8 (ECAM_BASE + Bridge + Cap + 1) & 0xFC;
  }
  return FALSE;
}

STATIC UINT32 Access (UINTN Address, UINTN Width, UINTN Mode, UINT32 And, UINT32 Or) {
  if (Address >= ECAM_SIZE || (Address & (Width - 1)) != 0) return MAX_UINT32;
  BOOLEAN Interrupts = SaveAndDisableInterrupts ();
  UINTN Budget = 1000000;
  while (InterlockedCompareExchange32 (SHARED_LOCK, 0, 1) != 0) {
    if (--Budget == 0) {
      SetInterruptState (Interrupts);
      return MAX_UINT32;
    }
    CpuPause ();
  }
  int Ari = AriForwarding (Address) ? 1 : 0;
  uint32_t Value = Or;
  int Status = 0;
  if (Mode != 1) Status = ps5_pci_access (&Io, (uint32_t)Address, (unsigned)Width, Ari, 0, &Value);
  if (Status == 0 && Mode != 0) {
    if (Mode == 2) Value = (Value & And) | Or;
    Status = ps5_pci_access (&Io, (uint32_t)Address, (unsigned)Width, Ari, 1, &Value);
  }
  MemoryFence ();
  InterlockedCompareExchange32 (SHARED_LOCK, 1, 0);
  SetInterruptState (Interrupts);
  return Status == 0 ? Value : MAX_UINT32;
}

STATIC UINT32 FieldMask (UINTN Start, UINTN End, UINTN Bits) {
  ASSERT (Start <= End && End < Bits);
  if (Start > End || End >= Bits) return 0;
  return (UINT32)((LShiftU64 (1, End - Start + 1) - 1) << Start);
}

#define PCI_WIDTH(Bits, Type) \
Type EFIAPI PciExpressRead##Bits (UINTN A) { return (Type)Access (A, Bits/8, 0, 0, 0); } \
Type EFIAPI PciExpressWrite##Bits (UINTN A, Type V) { return (Type)Access (A, Bits/8, 1, 0, V); } \
Type EFIAPI PciExpressOr##Bits (UINTN A, Type V) { return (Type)Access (A, Bits/8, 2, MAX_UINT32, V); } \
Type EFIAPI PciExpressAnd##Bits (UINTN A, Type V) { return (Type)Access (A, Bits/8, 2, V, 0); } \
Type EFIAPI PciExpressAndThenOr##Bits (UINTN A, Type D, Type V) { return (Type)Access (A, Bits/8, 2, D, V); } \
Type EFIAPI PciExpressBitFieldRead##Bits (UINTN A, UINTN S, UINTN E) { \
  UINT32 M = FieldMask (S,E,Bits); if (!M) return 0; \
  return (Type)((PciExpressRead##Bits (A) & M) >> S); } \
Type EFIAPI PciExpressBitFieldAndThenOr##Bits (UINTN A, UINTN S, UINTN E, Type D, Type V) { \
  UINT32 M = FieldMask (S,E,Bits); if (!M) return (Type)-1; \
  ASSERT (((UINT32)V & ~(M >> S)) == 0 && ((UINT32)D & ~(M >> S)) == 0); \
  return (Type)Access (A, Bits/8, 2, ~M | (((UINT32)D << S) & M), ((UINT32)V << S) & M); } \
Type EFIAPI PciExpressBitFieldWrite##Bits (UINTN A, UINTN S, UINTN E, Type V) { \
  return PciExpressBitFieldAndThenOr##Bits (A,S,E,0,V); } \
Type EFIAPI PciExpressBitFieldOr##Bits (UINTN A, UINTN S, UINTN E, Type V) { \
  UINT32 M = FieldMask (S,E,Bits); if (!M) return (Type)-1; \
  return PciExpressBitFieldAndThenOr##Bits (A,S,E,(Type)(M >> S),V); } \
Type EFIAPI PciExpressBitFieldAnd##Bits (UINTN A, UINTN S, UINTN E, Type D) { \
  return PciExpressBitFieldAndThenOr##Bits (A,S,E,D,0); }

PCI_WIDTH (8, UINT8)
PCI_WIDTH (16, UINT16)
PCI_WIDTH (32, UINT32)

STATIC UINTN BufferAccess (UINTN Address, UINTN Size, VOID *Buffer, BOOLEAN Write) {
  if (Address >= ECAM_SIZE || Size > 0x1000 - (Address & 0xFFF) || (Size && !Buffer)) return 0;
  UINT8 *Bytes = Buffer;
  UINTN Done = 0;
  while (Done < Size) {
    UINTN Width = 1;
    if (((Address + Done) & 3) == 0 && Size - Done >= 4) Width = 4;
    else if (((Address + Done) & 1) == 0 && Size - Done >= 2) Width = 2;
    UINT32 Value = 0;
    if (Write) for (UINTN I = 0; I < Width; I++) Value |= (UINT32)Bytes[Done + I] << (8 * I);
    Value = Access (Address + Done, Width, Write ? 1 : 0, 0, Value);
    if (!Write) for (UINTN I = 0; I < Width; I++) Bytes[Done + I] = (UINT8)(Value >> (8 * I));
    Done += Width;
  }
  return Done;
}
UINTN EFIAPI PciExpressReadBuffer (UINTN A, UINTN S, VOID *B) { return BufferAccess (A,S,B,FALSE); }
UINTN EFIAPI PciExpressWriteBuffer (UINTN A, UINTN S, VOID *B) { return BufferAccess (A,S,B,TRUE); }
RETURN_STATUS EFIAPI PciExpressRegisterForRuntimeAccess (UINTN Address) { return RETURN_UNSUPPORTED; }
