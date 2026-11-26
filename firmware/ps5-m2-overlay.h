/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PS5 internal M.2 NVMe GPT Overlay (N4 - UEFI BlockIo filter).
 *
 * Implements the exact C/UEFI counterpart of overlay_sectors() in ps5_m2_layout.py:
 *   1. Validates the nested PS5-Linux/Windows GPT at LBA 65536..65569 and backup header at N-1-34224.
 *   2. Synthesises a standard GPT view (PMBR@0, Header@1, Entries@2..33, BackupEntries@N-33..N-2, BackupHeader@N-1)
 *      pointing at the same absolute partition LBAs, with FirstUsable..LastUsable clamped to the partition extent
 *      so GameOS areas ([0, 65536) and [N-34224, N)) are completely hidden from UEFI/Windows.
 *   3. Enforces a hard write guard: any write touching LBA < 65536 or LBA > N-1-34224 is refused with EFI_WRITE_PROTECTED.
 */
#ifndef PS5_M2_OVERLAY_H
#define PS5_M2_OVERLAY_H

#ifndef PS5_M2_UEFI_BINDING
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t   UINTN;
typedef int      BOOLEAN;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#endif

#define PS5_M2_SECTOR_SIZE     512U
#define PS5_M2_SONY_HEAD       65536ULL
#define PS5_M2_SONY_TAIL       34224ULL
#define PS5_M2_GPT_ENTRIES     128U
#define PS5_M2_GPT_ENTRY_SIZE  128U
#define PS5_M2_ENTRY_SECTORS   32U
#define PS5_M2_HEAD_SECTORS    (2U + PS5_M2_ENTRY_SECTORS) /* 34: PMBR + Primary Hdr + 32 Entry sectors */
#define PS5_M2_TAIL_SECTORS    (PS5_M2_ENTRY_SECTORS + 1U) /* 33: 32 Backup Entry sectors + Backup Hdr */
#define PS5_M2_MIN_ALIGN_SECS  (4ULL * 2048ULL)

typedef struct {
  UINT64  TotalSectors;
  UINT64  NestedLastLba;
  UINT64  FirstUsable;
  UINT64  LastUsable;
  UINT32  PartitionCount;
  BOOLEAN Valid;
  UINT8   Head[PS5_M2_HEAD_SECTORS][PS5_M2_SECTOR_SIZE]; /* LBA 0..33 */
  UINT8   Tail[PS5_M2_TAIL_SECTORS][PS5_M2_SECTOR_SIZE]; /* LBA TotalSectors-33..TotalSectors-1 */
} PS5_M2_OVERLAY;

static inline UINT16 Ps5M2R16(const UINT8 *P) {
  return (UINT16)P[0] | ((UINT16)P[1] << 8);
}
static inline UINT32 Ps5M2R32(const UINT8 *P) {
  return (UINT32)Ps5M2R16(P) | ((UINT32)Ps5M2R16(P + 2) << 16);
}
static inline UINT64 Ps5M2R64(const UINT8 *P) {
  return (UINT64)Ps5M2R32(P) | ((UINT64)Ps5M2R32(P + 4) << 32);
}
static inline void Ps5M2W16(UINT8 *P, UINT16 V) {
  P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8);
}
static inline void Ps5M2W32(UINT8 *P, UINT32 V) {
  Ps5M2W16(P, (UINT16)V); Ps5M2W16(P + 2, (UINT16)(V >> 16));
}
static inline void Ps5M2W64(UINT8 *P, UINT64 V) {
  Ps5M2W32(P, (UINT32)V); Ps5M2W32(P + 4, (UINT32)(V >> 32));
}
static inline void Ps5M2Zero(UINT8 *D, UINTN N) {
  while (N--) *D++ = 0;
}
static inline void Ps5M2Copy(UINT8 *D, const UINT8 *S, UINTN N) {
  while (N--) *D++ = *S++;
}
static inline UINT32 Ps5M2Crc32(const UINT8 *Data, UINTN Size) {
  UINT32 Crc = 0xFFFFFFFFU;
  for (UINTN I = 0; I < Size; I++) {
    Crc ^= Data[I];
    for (UINTN B = 0; B < 8; B++) {
      Crc = (Crc >> 1) ^ (0xEDB88320U & (0U - (Crc & 1U)));
    }
  }
  return ~Crc;
}

static inline BOOLEAN Ps5M2IsEfiPartSig(const UINT8 *H) {
  static const UINT8 Sig[8] = {'E','F','I',' ','P','A','R','T'};
  for (UINTN I = 0; I < 8; I++) if (H[I] != Sig[I]) return FALSE;
  return TRUE;
}

static inline void Ps5M2FormatPmbr(UINT8 *Sec, UINT32 StartLba, UINT64 SizeSectors) {
  Ps5M2Zero(Sec, PS5_M2_SECTOR_SIZE);
  UINT32 ClampedSize = (SizeSectors > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (UINT32)SizeSectors;
  Sec[446 + 0] = 0x00;
  Sec[446 + 1] = 0x00; Sec[446 + 2] = 0x02; Sec[446 + 3] = 0x00;
  Sec[446 + 4] = 0xEE;
  Sec[446 + 5] = 0xFF; Sec[446 + 6] = 0xFF; Sec[446 + 7] = 0xFF;
  Ps5M2W32(Sec + 446 + 8, StartLba);
  Ps5M2W32(Sec + 446 + 12, ClampedSize);
  Sec[510] = 0x55; Sec[511] = 0xAA;
}

static inline void Ps5M2FormatGptHeader(UINT8 *Sec, UINT64 MyLba, UINT64 AltLba,
                                        UINT64 FirstUsable, UINT64 LastUsable,
                                        const UINT8 *DiskGuid16, UINT64 EntriesLba,
                                        UINT32 EntriesCrc) {
  Ps5M2Zero(Sec, PS5_M2_SECTOR_SIZE);
  Sec[0]='E'; Sec[1]='F'; Sec[2]='I'; Sec[3]=' ';
  Sec[4]='P'; Sec[5]='A'; Sec[6]='R'; Sec[7]='T';
  Ps5M2W32(Sec + 8, 0x00010000U);
  Ps5M2W32(Sec + 12, 92U);
  Ps5M2W32(Sec + 16, 0U);
  Ps5M2W32(Sec + 20, 0U);
  Ps5M2W64(Sec + 24, MyLba);
  Ps5M2W64(Sec + 32, AltLba);
  Ps5M2W64(Sec + 40, FirstUsable);
  Ps5M2W64(Sec + 48, LastUsable);
  Ps5M2Copy(Sec + 56, DiskGuid16, 16U);
  Ps5M2W64(Sec + 72, EntriesLba);
  Ps5M2W32(Sec + 80, PS5_M2_GPT_ENTRIES);
  Ps5M2W32(Sec + 84, PS5_M2_GPT_ENTRY_SIZE);
  Ps5M2W32(Sec + 88, EntriesCrc);
  Ps5M2W32(Sec + 16, Ps5M2Crc32(Sec, 92U));
}

static inline BOOLEAN Ps5M2ValidateGptHeader(const UINT8 *Sec, UINT64 ExpectedMyLba, UINT64 ExpectedAltLba) {
  if (!Ps5M2IsEfiPartSig(Sec)) return FALSE;
  UINT32 HSize = Ps5M2R32(Sec + 12);
  if (HSize != 92U) return FALSE;
  UINT8 Tmp[92];
  Ps5M2Copy(Tmp, Sec, 92U);
  Ps5M2W32(Tmp + 16, 0U);
  if (Ps5M2Crc32(Tmp, 92U) != Ps5M2R32(Sec + 16)) return FALSE;
  if (Ps5M2R64(Sec + 24) != ExpectedMyLba || Ps5M2R64(Sec + 32) != ExpectedAltLba) return FALSE;
  UINT32 NumEntries = Ps5M2R32(Sec + 80);
  if ((NumEntries != PS5_M2_GPT_ENTRIES && NumEntries != 32U) || Ps5M2R32(Sec + 84) != PS5_M2_GPT_ENTRY_SIZE) return FALSE;
  return TRUE;
}

/* Build the synthetic GPT overlay from:
 *   - TotalSectors: disk sector count (N)
 *   - NestedHead34: 34 sectors read from LBA 65536..65569 (17,408 bytes)
 *   - NestedBackupHdr: 1 sector read from LBA (N - 1 - 34224) (512 bytes)
 */
static inline BOOLEAN Ps5M2BuildOverlay(UINT64 TotalSectors, const UINT8 *NestedHead34,
                                        const UINT8 *NestedBackupHdr, PS5_M2_OVERLAY *Out) {
  if (!NestedHead34 || !NestedBackupHdr || !Out) return FALSE;
  Ps5M2Zero((UINT8 *)Out, sizeof(*Out));
  if (TotalSectors <= PS5_M2_SONY_HEAD + PS5_M2_SONY_TAIL + PS5_M2_MIN_ALIGN_SECS) return FALSE;
  UINT64 LastLba = TotalSectors - 1ULL - PS5_M2_SONY_TAIL;

  /* 1. Validate nested PMBR at LBA 65536 */
  const UINT8 *Pmbr = NestedHead34;
  if (Pmbr[510] != 0x55 || Pmbr[511] != 0xAA) return FALSE;
  if (Pmbr[446 + 4] != 0xEE || Ps5M2R32(Pmbr + 446 + 8) != (UINT32)(PS5_M2_SONY_HEAD + 1ULL)) return FALSE;

  /* 2. Validate nested Primary GPT Header at LBA 65537 */
  const UINT8 *PriHdr = NestedHead34 + PS5_M2_SECTOR_SIZE;
  if (!Ps5M2ValidateGptHeader(PriHdr, PS5_M2_SONY_HEAD + 1ULL, LastLba)) return FALSE;
  if (Ps5M2R64(PriHdr + 72) != PS5_M2_SONY_HEAD + 2ULL) return FALSE;
  UINT32 NumEntries = Ps5M2R32(PriHdr + 80);
  UINT32 EntrySectors = (NumEntries * PS5_M2_GPT_ENTRY_SIZE) / PS5_M2_SECTOR_SIZE;

  /* 3. Validate Entry Array CRC32 (32 sectors for 128 entries, or 8 sectors for 32 entries) */
  const UINT8 *Entries = NestedHead34 + 2U * PS5_M2_SECTOR_SIZE;
  UINT32 EntriesCrc = Ps5M2Crc32(Entries, EntrySectors * PS5_M2_SECTOR_SIZE);
  if (EntriesCrc != Ps5M2R32(PriHdr + 88)) return FALSE;

  /* 4. Validate Backup GPT Header at LBA LastLba */
  if (!Ps5M2ValidateGptHeader(NestedBackupHdr, LastLba, PS5_M2_SONY_HEAD + 1ULL)) return FALSE;
  if (Ps5M2R32(NestedBackupHdr + 80) != NumEntries || Ps5M2R32(NestedBackupHdr + 88) != EntriesCrc) return FALSE;

  /* 5. Scan partitions and verify bounds within [NestedFirstUsable, NestedLastUsable] */
  UINT64 NestedFu = Ps5M2R64(PriHdr + 40);
  UINT64 NestedLu = Ps5M2R64(PriHdr + 48);
  if (NestedFu < PS5_M2_SONY_HEAD + 2ULL + EntrySectors || NestedLu > LastLba - EntrySectors - 1ULL || NestedFu > NestedLu) return FALSE;

  UINT64 MinFirst = ~0ULL;
  UINT64 MaxLast = 0ULL;
  UINT32 PartCount = 0U;
  for (UINTN I = 0; I < NumEntries; I++) {
    const UINT8 *E = Entries + I * PS5_M2_GPT_ENTRY_SIZE;
    BOOLEAN NonZeroType = FALSE;
    for (UINTN K = 0; K < 16U; K++) if (E[K] != 0) { NonZeroType = TRUE; break; }
    if (!NonZeroType) continue;
    UINT64 First = Ps5M2R64(E + 32);
    UINT64 Last  = Ps5M2R64(E + 40);
    if (First < NestedFu || Last < First || Last > NestedLu) return FALSE;
    if (First < MinFirst) MinFirst = First;
    if (Last > MaxLast) MaxLast = Last;
    PartCount++;
  }
  if (PartCount == 0U) return FALSE;

  /* 6. Synthesise standard GPT sectors matching overlay_sectors() in ps5_m2_layout.py */
  const UINT8 *DiskGuid = PriHdr + 56;
  for (UINTN S = 0; S < EntrySectors; S++) {
    Ps5M2Copy(Out->Head[2U + S], Entries + S * PS5_M2_SECTOR_SIZE, PS5_M2_SECTOR_SIZE);
    Ps5M2Copy(Out->Tail[S], Entries + S * PS5_M2_SECTOR_SIZE, PS5_M2_SECTOR_SIZE);
  }
  UINT32 SynthEntriesCrc = (NumEntries == PS5_M2_GPT_ENTRIES)
      ? EntriesCrc
      : Ps5M2Crc32(&Out->Head[2][0], PS5_M2_ENTRY_SECTORS * PS5_M2_SECTOR_SIZE);

  Ps5M2FormatPmbr(Out->Head[0], 1U, TotalSectors - 1ULL);
  Ps5M2FormatGptHeader(Out->Head[1], 1ULL, TotalSectors - 1ULL, MinFirst, MaxLast,
                       DiskGuid, 2ULL, SynthEntriesCrc);
  Ps5M2FormatGptHeader(Out->Tail[PS5_M2_ENTRY_SECTORS], TotalSectors - 1ULL, 1ULL, MinFirst, MaxLast,
                       DiskGuid, TotalSectors - 1ULL - PS5_M2_ENTRY_SECTORS, SynthEntriesCrc);

  Out->TotalSectors   = TotalSectors;
  Out->NestedLastLba  = LastLba;
  Out->FirstUsable    = MinFirst;
  Out->LastUsable     = MaxLast;
  Out->PartitionCount = PartCount;
  Out->Valid          = TRUE;
  return TRUE;
}

static inline BOOLEAN Ps5M2OverlayTouchesSynthetic(const PS5_M2_OVERLAY *Ov, UINT64 Lba, UINTN SectorCount) {
  if (!Ov || !Ov->Valid || SectorCount == 0) return FALSE;
  if (Lba + SectorCount < Lba) return TRUE;
  UINT64 Last = Lba + SectorCount - 1ULL;
  if (Lba < PS5_M2_SONY_HEAD || Last > Ov->NestedLastLba) return TRUE;
  return FALSE;
}

static inline BOOLEAN Ps5M2OverlayIsPureSynthetic(const PS5_M2_OVERLAY *Ov, UINT64 Lba, UINTN SectorCount) {
  if (!Ov || !Ov->Valid || SectorCount == 0 || Lba + SectorCount < Lba || Lba + SectorCount > Ov->TotalSectors) return FALSE;
  UINT64 End = Lba + SectorCount;
  if (End <= PS5_M2_SONY_HEAD) return TRUE;
  if (Lba > Ov->NestedLastLba) return TRUE;
  return FALSE;
}

static inline void Ps5M2ApplyReadOverlay(const PS5_M2_OVERLAY *Ov, UINT64 Lba, UINTN SectorCount, UINT8 *Buffer) {
  if (!Ov || !Ov->Valid || !Buffer || SectorCount == 0) return;
  UINT64 TailBase = Ov->TotalSectors - PS5_M2_TAIL_SECTORS;
  for (UINTN I = 0; I < SectorCount; I++) {
    UINT64 Cur = Lba + I;
    UINT8 *Dst = Buffer + I * PS5_M2_SECTOR_SIZE;
    if (Cur < PS5_M2_HEAD_SECTORS) {
      Ps5M2Copy(Dst, Ov->Head[Cur], PS5_M2_SECTOR_SIZE);
    } else if (Cur < PS5_M2_SONY_HEAD) {
      Ps5M2Zero(Dst, PS5_M2_SECTOR_SIZE);
    } else if (Cur >= TailBase && Cur < Ov->TotalSectors) {
      Ps5M2Copy(Dst, Ov->Tail[Cur - TailBase], PS5_M2_SECTOR_SIZE);
    } else if (Cur > Ov->NestedLastLba && Cur < TailBase) {
      Ps5M2Zero(Dst, PS5_M2_SECTOR_SIZE);
    }
  }
}

/* Hard write guard: refuses any write that touches GameOS head [0, 65536) or GameOS tail (NestedLastLba, TotalSectors). */
static inline BOOLEAN Ps5M2CheckWriteAllowed(const PS5_M2_OVERLAY *Ov, UINT64 Lba, UINTN SectorCount) {
  if (!Ov || !Ov->Valid || SectorCount == 0) return FALSE;
  if (Lba + SectorCount < Lba) return FALSE;
  UINT64 Last = Lba + SectorCount - 1ULL;
  if (Lba < PS5_M2_SONY_HEAD || Last > Ov->NestedLastLba) return FALSE;
  return TRUE;
}

/* Fail-safe detection for a GameOS-formatted M.2 SSD whose nested GPT at LBA 65536 is missing or corrupted:
 *   - Case 1: LBA 65536/65537 carries a nested PMBR (0x55AA) or "EFI PART" signature, but Ps5M2BuildOverlay failed.
 *   - Case 2: LBA 0 lacks standard 0x55AA MBR/PMBR signature and contains non-zero GameOS header bytes (>= 64 non-zero bytes).
 */
static inline BOOLEAN Ps5M2ShouldFailSafeProtect(const UINT8 *Lba0, const UINT8 *NestedHead34) {
  if (NestedHead34 != NULL) {
    if ((NestedHead34[510] == 0x55 && NestedHead34[511] == 0xAA && NestedHead34[446 + 4] == 0xEE) ||
        Ps5M2IsEfiPartSig(NestedHead34 + PS5_M2_SECTOR_SIZE)) {
      return TRUE;
    }
  }
  if (Lba0 != NULL && (Lba0[510] != 0x55 || Lba0[511] != 0xAA)) {
    UINTN NonZero = 0;
    for (UINTN I = 0; I < PS5_M2_SECTOR_SIZE; I++) {
      if (Lba0[I] != 0) NonZero++;
    }
    if (NonZero >= 64U) return TRUE;
  }
  return FALSE;
}

#ifdef PS5_M2_UEFI_BINDING
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>

typedef struct {
  EFI_HANDLE             Handle;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_BLOCK_READ         OrigReadBlocks;
  EFI_BLOCK_WRITE        OrigWriteBlocks;
  EFI_BLOCK_IO2_PROTOCOL *BlockIo2;
  EFI_BLOCK_READ_EX      OrigReadBlocksEx;
  EFI_BLOCK_WRITE_EX     OrigWriteBlocksEx;
  PS5_M2_OVERLAY         Overlay;
} PS5_M2_HOOK_CTX;

STATIC PS5_M2_HOOK_CTX mPs5M2Ctx;

STATIC EFI_STATUS EFIAPI Ps5M2ReadBlocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                                         EFI_LBA Lba, UINTN BufferSize, VOID *Buffer) {
  if (This == NULL || Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (BufferSize == 0) return EFI_SUCCESS;
  if ((BufferSize % PS5_M2_SECTOR_SIZE) != 0) return EFI_BAD_BUFFER_SIZE;
  UINTN Sectors = BufferSize / PS5_M2_SECTOR_SIZE;
  if (This == mPs5M2Ctx.BlockIo && mPs5M2Ctx.Overlay.Valid) {
    if (Ps5M2OverlayIsPureSynthetic(&mPs5M2Ctx.Overlay, Lba, Sectors)) {
      Ps5M2ApplyReadOverlay(&mPs5M2Ctx.Overlay, Lba, Sectors, (UINT8 *)Buffer);
      return EFI_SUCCESS;
    }
    EFI_STATUS Status = mPs5M2Ctx.OrigReadBlocks(This, MediaId, Lba, BufferSize, Buffer);
    if (!EFI_ERROR(Status)) {
      Ps5M2ApplyReadOverlay(&mPs5M2Ctx.Overlay, Lba, Sectors, (UINT8 *)Buffer);
    }
    return Status;
  }
  return mPs5M2Ctx.OrigReadBlocks(This, MediaId, Lba, BufferSize, Buffer);
}

STATIC EFI_STATUS EFIAPI Ps5M2WriteBlocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                                          EFI_LBA Lba, UINTN BufferSize, VOID *Buffer) {
  if (This == NULL || Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (BufferSize == 0) return EFI_SUCCESS;
  if ((BufferSize % PS5_M2_SECTOR_SIZE) != 0) return EFI_BAD_BUFFER_SIZE;
  UINTN Sectors = BufferSize / PS5_M2_SECTOR_SIZE;
  if (This == mPs5M2Ctx.BlockIo && mPs5M2Ctx.Overlay.Valid) {
    if (!Ps5M2CheckWriteAllowed(&mPs5M2Ctx.Overlay, Lba, Sectors)) {
      DEBUG((DEBUG_ERROR, "PS5 M2 overlay: BLOCKED write to GameOS area LBA=%Lu count=%Lu\n",
             (UINT64)Lba, (UINT64)Sectors));
      return EFI_WRITE_PROTECTED;
    }
  }
  return mPs5M2Ctx.OrigWriteBlocks(This, MediaId, Lba, BufferSize, Buffer);
}

STATIC EFI_STATUS EFIAPI Ps5M2ReadBlocksEx(EFI_BLOCK_IO2_PROTOCOL *This, UINT32 MediaId,
                                           EFI_LBA Lba, EFI_BLOCK_IO2_TOKEN *Token,
                                           UINTN BufferSize, VOID *Buffer) {
  if (This == NULL) return EFI_INVALID_PARAMETER;
  if (This == mPs5M2Ctx.BlockIo2 && mPs5M2Ctx.Overlay.Valid) {
    EFI_STATUS Status = Ps5M2ReadBlocks(mPs5M2Ctx.BlockIo, MediaId, Lba, BufferSize, Buffer);
    if (Token != NULL && Token->Event != NULL && !EFI_ERROR(Status)) {
      Token->TransactionStatus = EFI_SUCCESS;
      gBS->SignalEvent(Token->Event);
    }
    return Status;
  }
  return mPs5M2Ctx.OrigReadBlocksEx(This, MediaId, Lba, Token, BufferSize, Buffer);
}

STATIC EFI_STATUS EFIAPI Ps5M2WriteBlocksEx(EFI_BLOCK_IO2_PROTOCOL *This, UINT32 MediaId,
                                            EFI_LBA Lba, EFI_BLOCK_IO2_TOKEN *Token,
                                            UINTN BufferSize, VOID *Buffer) {
  if (This == NULL) return EFI_INVALID_PARAMETER;
  if (This == mPs5M2Ctx.BlockIo2 && mPs5M2Ctx.Overlay.Valid) {
    EFI_STATUS Status = Ps5M2WriteBlocks(mPs5M2Ctx.BlockIo, MediaId, Lba, BufferSize, Buffer);
    if (Token != NULL && Token->Event != NULL && !EFI_ERROR(Status)) {
      Token->TransactionStatus = EFI_SUCCESS;
      gBS->SignalEvent(Token->Event);
    }
    return Status;
  }
  return mPs5M2Ctx.OrigWriteBlocksEx(This, MediaId, Lba, Token, BufferSize, Buffer);
}

STATIC EFI_STATUS Ps5InstallM2BlockIoOverlay(VOID) {
  if (mPs5M2Ctx.Overlay.Valid) return EFI_ALREADY_STARTED;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  EFI_STATUS FoundStatus = EFI_NOT_FOUND;
  for (UINTN I = 0; I < Count; I++) {
    EFI_BLOCK_IO_PROTOCOL *Bio = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Bio)) || Bio == NULL || Bio->Media == NULL) continue;
    if (!Bio->Media->MediaPresent || Bio->Media->LogicalPartition || Bio->Media->BlockSize != PS5_M2_SECTOR_SIZE) continue;
    UINT64 Total = (UINT64)Bio->Media->LastBlock + 1ULL;
    if (Total <= PS5_M2_SONY_HEAD + PS5_M2_SONY_TAIL + PS5_M2_MIN_ALIGN_SECS) continue;
    UINT8 Head34[PS5_M2_HEAD_SECTORS * PS5_M2_SECTOR_SIZE];
    UINT8 BackupHdr[PS5_M2_SECTOR_SIZE];
    UINT64 LastLba = Total - 1ULL - PS5_M2_SONY_TAIL;
    if (EFI_ERROR(Bio->ReadBlocks(Bio, Bio->Media->MediaId, PS5_M2_SONY_HEAD, sizeof(Head34), Head34))) continue;
    if (EFI_ERROR(Bio->ReadBlocks(Bio, Bio->Media->MediaId, LastLba, sizeof(BackupHdr), BackupHdr))) continue;
    if (!Ps5M2BuildOverlay(Total, Head34, BackupHdr, &mPs5M2Ctx.Overlay)) continue;
    mPs5M2Ctx.Handle          = Handles[I];
    mPs5M2Ctx.BlockIo         = Bio;
    mPs5M2Ctx.OrigReadBlocks  = Bio->ReadBlocks;
    mPs5M2Ctx.OrigWriteBlocks = Bio->WriteBlocks;
    Bio->ReadBlocks           = Ps5M2ReadBlocks;
    Bio->WriteBlocks          = Ps5M2WriteBlocks;
    EFI_BLOCK_IO2_PROTOCOL *Bio2 = NULL;
    if (!EFI_ERROR(gBS->HandleProtocol(Handles[I], &gEfiBlockIo2ProtocolGuid, (VOID **)&Bio2)) && Bio2 != NULL) {
      mPs5M2Ctx.BlockIo2          = Bio2;
      mPs5M2Ctx.OrigReadBlocksEx  = Bio2->ReadBlocksEx;
      mPs5M2Ctx.OrigWriteBlocksEx = Bio2->WriteBlocksEx;
      Bio2->ReadBlocksEx          = Ps5M2ReadBlocksEx;
      Bio2->WriteBlocksEx         = Ps5M2WriteBlocksEx;
    }
    DEBUG((DEBUG_ERROR, "PS5 M2 overlay: installed on handle=%p bio2=%u total_sectors=%Lu parts=%u usable=[%Lu..%Lu]\n",
           Handles[I], (UINT32)(Bio2 != NULL), Total, mPs5M2Ctx.Overlay.PartitionCount,
           mPs5M2Ctx.Overlay.FirstUsable, mPs5M2Ctx.Overlay.LastUsable));
    gBS->DisconnectController(Handles[I], NULL, NULL);
    gBS->ConnectController(Handles[I], NULL, NULL, TRUE);
    FoundStatus = EFI_SUCCESS;
    break;
  }
  FreePool(Handles);
  return FoundStatus;
}
#endif /* PS5_M2_UEFI_BINDING */

#endif /* PS5_M2_OVERLAY_H */
