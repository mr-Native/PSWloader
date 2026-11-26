/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5m2flt.c - PS5 Internal M.2 NVMe GPT Overlay Filter Driver (N5, Hardened v1.1)
 *
 * WDM DiskDrive UpperFilter (installed immediately below PartMgr in
 * HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}\UpperFilters = ["ps5m2flt", "PartMgr"]).
 *
 * Behaviour (incorporating the N4/N5 review hardening 1–4):
 *   1. On IRP_MN_START_DEVICE (after disk.sys starts the FDO):
 *      - Queries disk byte length via IOCTL_DISK_GET_LENGTH_INFO.
 *      - Reads LBA 0 (1 sector), nested GPT head (LBA 65536..65569, 34 sectors), and backup GPT header (LBA N-1-34224, 1 sector).
 *      - Validates nested GPT (128 or 32 entries) and builds synthetic standard GPT overlay via Ps5M2BuildOverlay().
 *      - Fail-safe (#3): if nested GPT is invalid/missing on a GameOS-formatted M.2 disk (non-0x55AA LBA 0 with GameOS data,
 *        or corrupted nested GPT at LBA 65536), enables FailSafeWriteProtect so Windows cannot overwrite GameOS.
 *      - Non-matching standard disks (e.g., USB boot stick) remain 100% transparent passthrough.
 *   2. On IRP_MJ_READ (when Overlay.Valid == TRUE):
 *      - Fast path: if read lies strictly inside [65536, N-1-34224], passes straight down with zero overhead.
 *      - Pure synthetic path: if read lies entirely within [0, 65536) or (N-1-34224, N), returns synthetic GPT for
 *        0..33 / N-33..N-1 and zeros (#2) for 34..65535 / N-34224..N-34 directly from RAM without calling disk.sys.
 *      - Boundary-crossing path: forwards read to disk.sys and overlays/zeros the Sony sectors in the completion routine.
 *   3. On IRP_MJ_WRITE:
 *      - If FailSafeWriteProtect == TRUE: blocks all writes with STATUS_MEDIA_WRITE_PROTECTED.
 *      - If Overlay.Valid == TRUE: blocks any write touching [0, 65536) or (N-1-34224, N) with STATUS_MEDIA_WRITE_PROTECTED.
 *   4. On IRP_MJ_DEVICE_CONTROL (#1):
 *      - When Overlay.Valid or FailSafeWriteProtect: blocks raw pass-through / format / layout-overwrite IOCTLs
 *        (SCSI/ATA pass-through, NVMe protocol command, reinitialize media, format tracks, create disk, set drive layout),
 *        and inspects IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES (TRIM) ranges to reject any range overlapping Sony areas.
 */

#include <ntddk.h>
#include <ntdddisk.h>

#define BOOLEAN PS5_M2_HOST_BOOL
#include "ps5-m2-overlay.h"   /* found through /I in build-ps5m2flt.sh */
#undef BOOLEAN

#define PS5_M2_POOL_TAG '2MsP'

/* IOCTL codes guarded when Overlay.Valid or FailSafeWriteProtect is active */
#define PS5_IOCTL_SCSI_PASS_THROUGH             0x0004D004UL
#define PS5_IOCTL_SCSI_PASS_THROUGH_DIRECT      0x0004D014UL
#define PS5_IOCTL_SCSI_PASS_THROUGH_EX          0x0004D024UL
#define PS5_IOCTL_SCSI_PASS_THROUGH_DIRECT_EX   0x0004D028UL
#define PS5_IOCTL_ATA_PASS_THROUGH              0x0004D02CUL
#define PS5_IOCTL_ATA_PASS_THROUGH_DIRECT       0x0004D030UL
#define PS5_IOCTL_STORAGE_PROTOCOL_COMMAND      0x002DD3C0UL
#define PS5_IOCTL_STORAGE_REINITIALIZE_MEDIA    0x002D5190UL
#define PS5_IOCTL_STORAGE_MANAGE_DATA_SET_ATTR  0x002D9404UL
#define PS5_IOCTL_DISK_IS_WRITABLE              0x00070024UL
#define PS5_IOCTL_DISK_SET_DRIVE_LAYOUT         0x0007C010UL
#define PS5_IOCTL_DISK_FORMAT_TRACKS            0x0007C018UL
#define PS5_IOCTL_DISK_FORMAT_TRACKS_EX         0x0007C02CUL
#define PS5_IOCTL_DISK_SET_DRIVE_LAYOUT_EX      0x0007C054UL
#define PS5_IOCTL_DISK_CREATE_DISK              0x0007C058UL

#define PS5_DSM_FLAG_ENTIRE_DATA_SET_RANGE      0x00000001UL

typedef struct _PS5_DSM_INPUT {
  ULONG Size;
  ULONG Action;
  ULONG Flags;
  ULONG ParameterBlockOffset;
  ULONG ParameterBlockLength;
  ULONG DataSetRangesOffset;
  ULONG DataSetRangesLength;
} PS5_DSM_INPUT;

typedef struct _PS5_DSM_RANGE {
  LONGLONG StartingOffset;
  UINT64   LengthInBytes;
} PS5_DSM_RANGE;

typedef struct _PS5_M2_DEVEXT {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT LowerDevice;
  PDEVICE_OBJECT PhysicalDevice;
  IO_REMOVE_LOCK RemoveLock;
  KMUTEX         OverlayLock;
  BOOLEAN        FailSafeWriteProtect;
  UINT64         NestedFirstUsable;
  UINT64         NestedLastUsable;
  PS5_M2_OVERLAY Overlay;
} PS5_M2_DEVEXT, *PPS5_M2_DEVEXT;

typedef struct _PS5_M2_READ_CTX {
  PPS5_M2_DEVEXT DevExt;
  UINT64         StartLba;
  UINTN          SectorCount;
} PS5_M2_READ_CTX, *PPS5_M2_READ_CTX;

static PUCHAR Ps5M2MapIrpBuffer(PIRP Irp) {
  if (Irp->MdlAddress != NULL) {
    return (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
  }
  return (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
}

static NTSTATUS Ps5M2SyncIrpCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
  UNREFERENCED_PARAMETER(DeviceObject);
  UNREFERENCED_PARAMETER(Irp);
  KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS Ps5M2ForwardIrpSynchronously(PDEVICE_OBJECT LowerDevice, PIRP Irp) {
  KEVENT Event;
  KeInitializeEvent(&Event, NotificationEvent, FALSE);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Ps5M2SyncIrpCompletion, &Event, TRUE, TRUE, TRUE);
  NTSTATUS Status = IoCallDriver(LowerDevice, Irp);
  if (Status == STATUS_PENDING) {
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;
  }
  return Status;
}

static NTSTATUS Ps5M2QueryDiskLength(PDEVICE_OBJECT LowerDevice, UINT64 *OutTotalBytes) {
  KEVENT Event;
  IO_STATUS_BLOCK IoStatus;
  GET_LENGTH_INFORMATION LengthInfo;

  RtlZeroMemory(&LengthInfo, sizeof(LengthInfo));
  KeInitializeEvent(&Event, NotificationEvent, FALSE);

  PIRP Irp = IoBuildDeviceIoControlRequest(
      IOCTL_DISK_GET_LENGTH_INFO,
      LowerDevice,
      NULL,
      0,
      &LengthInfo,
      sizeof(LengthInfo),
      FALSE,
      &Event,
      &IoStatus);
  if (Irp == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  NTSTATUS Status = IoCallDriver(LowerDevice, Irp);
  if (Status == STATUS_PENDING) {
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = IoStatus.Status;
  }
  if (NT_SUCCESS(Status)) {
    *OutTotalBytes = (UINT64)LengthInfo.Length.QuadPart;
  }
  return Status;
}

static NTSTATUS Ps5M2ReadSectorsSync(PDEVICE_OBJECT LowerDevice, UINT64 StartLba, ULONG ByteCount, PVOID Buffer) {
  KEVENT Event;
  IO_STATUS_BLOCK IoStatus;
  LARGE_INTEGER Offset;

  Offset.QuadPart = (LONGLONG)(StartLba * (UINT64)PS5_M2_SECTOR_SIZE);
  KeInitializeEvent(&Event, NotificationEvent, FALSE);

  PIRP Irp = IoBuildSynchronousFsdRequest(
      IRP_MJ_READ,
      LowerDevice,
      Buffer,
      ByteCount,
      &Offset,
      &Event,
      &IoStatus);
  if (Irp == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  NTSTATUS Status = IoCallDriver(LowerDevice, Irp);
  if (Status == STATUS_PENDING) {
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = IoStatus.Status;
  }
  return Status;
}

static NTSTATUS Ps5M2WriteSectorsSync(PDEVICE_OBJECT LowerDevice, UINT64 StartLba, ULONG ByteCount, PVOID Buffer) {
  KEVENT Event;
  IO_STATUS_BLOCK IoStatus;
  LARGE_INTEGER Offset;

  Offset.QuadPart = (LONGLONG)(StartLba * (UINT64)PS5_M2_SECTOR_SIZE);
  KeInitializeEvent(&Event, NotificationEvent, FALSE);

  PIRP Irp = IoBuildSynchronousFsdRequest(
      IRP_MJ_WRITE,
      LowerDevice,
      Buffer,
      ByteCount,
      &Offset,
      &Event,
      &IoStatus);
  if (Irp == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  NTSTATUS Status = IoCallDriver(LowerDevice, Irp);
  if (Status == STATUS_PENDING) {
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = IoStatus.Status;
  }
  return Status;
}

static BOOLEAN Ps5M2HasNestedSig(const UINT8 *NestedHead34) {
  if (NestedHead34 == NULL) return FALSE;
  return (NestedHead34[510] == 0x55 && NestedHead34[511] == 0xAA && NestedHead34[446 + 4] == 0xEE) ||
         Ps5M2IsEfiPartSig(NestedHead34 + PS5_M2_SECTOR_SIZE);
}

/* Microsoft Basic Data GUID {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7} so fastfat.sys reads our synthetic FAT16 label */
/* v1.2.4: Non-mounted reserved GPT Partition Type GUID ({50533500-4741-4D45-4F53-524553564400}) for GameOS
 * guard partitions so volmgr/fastfat/autochk/setupcl never mount or check them (while Windows Setup still
 * respects them as protected reserved space and never places ESP inside GameOS head/tail). */
static const UINT8 gPs5GameOsReservedTypeGuid[16] = {
  0x00, 0x35, 0x53, 0x50, 0x41, 0x47, 0x45, 0x4D,
  0x4F, 0x53, 0x52, 0x45, 0x53, 0x56, 0x44, 0x00
};
static const UINT8 gPs5GameOsHeadPartGuid[16] = {
  0x47, 0x35, 0x53, 0x50, 0x4D, 0x41, 0x4F, 0x45,
  0x53, 0x48, 0x45, 0x41, 0x44, 0x45, 0x52, 0x11
};
static const UINT8 gPs5GameOsTailPartGuid[16] = {
  0x47, 0x35, 0x53, 0x50, 0x4D, 0x41, 0x4F, 0x45,
  0x53, 0x54, 0x41, 0x49, 0x4C, 0x45, 0x52, 0x22
};

static BOOLEAN Ps5M2MemEq(const UINT8 *A, const UINT8 *B, UINTN N) {
  for (UINTN I = 0; I < N; I++) if (A[I] != B[I]) return FALSE;
  return TRUE;
}

static BOOLEAN Ps5M2IsHeadGuardEntry(const UINT8 *Ent) {
  return Ps5M2MemEq(Ent + 16, gPs5GameOsHeadPartGuid, 16U);
}

static BOOLEAN Ps5M2IsTailGuardEntry(const UINT8 *Ent) {
  return Ps5M2MemEq(Ent + 16, gPs5GameOsTailPartGuid, 16U);
}

static BOOLEAN Ps5M2IsGuardEntry(const UINT8 *Ent) {
  return Ps5M2IsHeadGuardEntry(Ent) || Ps5M2IsTailGuardEntry(Ent);
}

static VOID Ps5M2WriteGuardEntry(UINT8 *Ent, const UINT8 *PartGuid,
                                 UINT64 FirstLba, UINT64 LastLba, const char *AsciiName) {
  Ps5M2Zero(Ent, PS5_M2_GPT_ENTRY_SIZE);
  Ps5M2Copy(Ent + 0, gPs5GameOsReservedTypeGuid, 16U);
  Ps5M2Copy(Ent + 16, PartGuid, 16U);
  Ps5M2W64(Ent + 32, FirstLba);
  Ps5M2W64(Ent + 40, LastLba);
  /* RequiredPartition (bit 0) | ReadOnly (bit 60) | NoDriveLetter (bit 63) */
  Ps5M2W64(Ent + 48, 0x9000000000000001ULL);
  for (UINTN I = 0; I < 36U && AsciiName[I]; I++) {
    Ent[56 + I * 2] = (UINT8)AsciiName[I];
    Ent[56 + I * 2 + 1] = 0;
  }
}

/* Populate Overlay.Head[0..33] and Overlay.Tail[0..32] from raw nested entries (up to 126 real partitions
 * at entries 0..125 so Windows partition numbers match UEFI 1:1) plus the 2 protected synthetic GameOS
 * guard partitions at entries 126 and 127. */
static VOID Ps5M2BuildSyntheticWithGuards(PPS5_M2_DEVEXT DevExt, const UINT8 *NestedEntries,
                                          UINT32 NumNestedEntries, const UINT8 *DiskGuid,
                                          UINT64 TotalSectors, UINT64 LastLba) {
  UINT64 GuardHeadFirst = PS5_M2_HEAD_SECTORS; /* 34 */
  UINT64 GuardHeadLast  = PS5_M2_SONY_HEAD + 2048ULL - 1ULL; /* 67583 -> usable starts at exact 33 MiB (67584) */
  UINT64 GuardTailFirst = ((LastLba - PS5_M2_ENTRY_SECTORS) / 2048ULL) * 2048ULL; /* exact 1 MiB boundary */
  UINT64 GuardTailLast  = TotalSectors - PS5_M2_HEAD_SECTORS; /* N - 34 */

  UINT64 Fu = GuardHeadLast + 1ULL; /* 67584 (33 MiB, 1 MiB aligned) */
  UINT64 Lu = GuardTailFirst - 1ULL;
  DevExt->NestedFirstUsable = Fu;
  DevExt->NestedLastUsable  = Lu;

  UINT8 SynthEntries[PS5_M2_ENTRY_SECTORS * PS5_M2_SECTOR_SIZE];
  Ps5M2Zero(SynthEntries, sizeof(SynthEntries));

  UINT32 OutIdx = 0U;
  UINT32 RealCount = 0U;
  for (UINT32 I = 0; I < NumNestedEntries && OutIdx < (PS5_M2_GPT_ENTRIES - 2U); I++) {
    const UINT8 *E = NestedEntries + (UINTN)I * PS5_M2_GPT_ENTRY_SIZE;
    BOOLEAN NonZero = FALSE;
    for (UINTN K = 0; K < 16U; K++) if (E[K] != 0) { NonZero = TRUE; break; }
    if (!NonZero || Ps5M2IsGuardEntry(E)) continue;
    Ps5M2Copy(SynthEntries + (UINTN)OutIdx * PS5_M2_GPT_ENTRY_SIZE, E, PS5_M2_GPT_ENTRY_SIZE);
    OutIdx++;
    RealCount++;
  }

  Ps5M2WriteGuardEntry(SynthEntries + (PS5_M2_GPT_ENTRIES - 2U) * PS5_M2_GPT_ENTRY_SIZE,
                       gPs5GameOsHeadPartGuid,
                       GuardHeadFirst, GuardHeadLast, "PS5 GameOS Header (Do Not Delete)");
  Ps5M2WriteGuardEntry(SynthEntries + (PS5_M2_GPT_ENTRIES - 1U) * PS5_M2_GPT_ENTRY_SIZE,
                       gPs5GameOsTailPartGuid,
                       GuardTailFirst, GuardTailLast, "PS5 GameOS Tail (Do Not Delete)");

  for (UINTN S = 0; S < PS5_M2_ENTRY_SECTORS; S++) {
    Ps5M2Copy(DevExt->Overlay.Head[2U + S], SynthEntries + S * PS5_M2_SECTOR_SIZE, PS5_M2_SECTOR_SIZE);
    Ps5M2Copy(DevExt->Overlay.Tail[S], SynthEntries + S * PS5_M2_SECTOR_SIZE, PS5_M2_SECTOR_SIZE);
  }

  UINT32 SynthCrc = Ps5M2Crc32(&DevExt->Overlay.Head[2][0], PS5_M2_ENTRY_SECTORS * PS5_M2_SECTOR_SIZE);
  Ps5M2FormatPmbr(DevExt->Overlay.Head[0], 1U, TotalSectors - 1ULL);
  Ps5M2FormatGptHeader(DevExt->Overlay.Head[1], 1ULL, TotalSectors - 1ULL,
                       GuardHeadFirst, GuardTailLast, DiskGuid, 2ULL, SynthCrc);
  Ps5M2FormatGptHeader(DevExt->Overlay.Tail[PS5_M2_ENTRY_SECTORS], TotalSectors - 1ULL, 1ULL,
                       GuardHeadFirst, GuardTailLast, DiskGuid, TotalSectors - 1ULL - PS5_M2_ENTRY_SECTORS, SynthCrc);

  DevExt->Overlay.TotalSectors   = TotalSectors;
  DevExt->Overlay.NestedLastLba  = LastLba;
  DevExt->Overlay.FirstUsable    = GuardHeadFirst;
  DevExt->Overlay.LastUsable     = GuardTailLast;
  DevExt->Overlay.PartitionCount = RealCount + 2U;
  DevExt->Overlay.Valid          = TRUE;
}

static BOOLEAN Ps5M2IsValidEmptyNestedGpt(const UINT8 *Head34, const UINT8 *BackupHdr, UINT64 LastLba) {
  const UINT8 *PriHdr = Head34 + PS5_M2_SECTOR_SIZE;
  if (!Ps5M2ValidateGptHeader(PriHdr, PS5_M2_SONY_HEAD + 1ULL, LastLba)) return FALSE;
  if (!Ps5M2ValidateGptHeader(BackupHdr, LastLba, PS5_M2_SONY_HEAD + 1ULL)) return FALSE;
  UINT32 NumEntries = Ps5M2R32(PriHdr + 80);
  UINT32 EntrySectors = (NumEntries * PS5_M2_GPT_ENTRY_SIZE) / PS5_M2_SECTOR_SIZE;
  UINT32 Crc = Ps5M2Crc32(Head34 + 2U * PS5_M2_SECTOR_SIZE, EntrySectors * PS5_M2_SECTOR_SIZE);
  return (Crc == Ps5M2R32(PriHdr + 88) && Crc == Ps5M2R32(BackupHdr + 88));
}

static VOID Ps5M2InitEmptyOverlay(PPS5_M2_DEVEXT DevExt, UINT64 TotalSectors, UINT64 LastLba, const UINT8 *OptGuid) {
  static const UINT8 DefaultGuid[16] = {
    0x50, 0x53, 0x35, 0x4D, 0x32, 0x47, 0x50, 0x54,
    0x90, 0x11, 0x50, 0x53, 0x57, 0x4C, 0x44, 0x52
  };
  UINT8 Empty[PS5_M2_GPT_ENTRY_SIZE];
  Ps5M2Zero(Empty, sizeof(Empty));
  Ps5M2BuildSyntheticWithGuards(DevExt, Empty, 0U, OptGuid ? OptGuid : DefaultGuid, TotalSectors, LastLba);
}

static VOID Ps5M2TryProbeNestedGpt(PPS5_M2_DEVEXT DevExt) {
  if (DevExt->Overlay.Valid || DevExt->FailSafeWriteProtect) {
    return;
  }

  UINT64 TotalBytes = 0;
  if (!NT_SUCCESS(Ps5M2QueryDiskLength(DevExt->LowerDevice, &TotalBytes))) {
    return;
  }
  if ((TotalBytes % PS5_M2_SECTOR_SIZE) != 0) {
    return;
  }

  UINT64 TotalSectors = TotalBytes / PS5_M2_SECTOR_SIZE;
  if (TotalSectors <= PS5_M2_SONY_HEAD + PS5_M2_SONY_TAIL + PS5_M2_MIN_ALIGN_SECS) {
    return;
  }

  ULONG HeadBytes = PS5_M2_HEAD_SECTORS * PS5_M2_SECTOR_SIZE;
  PUCHAR Scratch = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, HeadBytes + 2U * PS5_M2_SECTOR_SIZE, PS5_M2_POOL_TAG);
  if (Scratch == NULL) {
    return;
  }

  PUCHAR Head34 = Scratch;
  PUCHAR BackupHdr = Scratch + HeadBytes;
  PUCHAR Lba0 = BackupHdr + PS5_M2_SECTOR_SIZE;
  UINT64 LastLba = TotalSectors - 1ULL - PS5_M2_SONY_TAIL;

  BOOLEAN HaveLba0 = NT_SUCCESS(Ps5M2ReadSectorsSync(DevExt->LowerDevice, 0ULL, PS5_M2_SECTOR_SIZE, Lba0));
  /* Double-attach guard: if a lower ps5m2flt instance already exposed the synthetic guard partition at slot 126 (LBA 33, offset 256), stay transparent */
  if (HaveLba0 && Lba0[510] == 0x55 && Lba0[511] == 0xAA) {
    UINT8 Lba33[PS5_M2_SECTOR_SIZE];
    if (NT_SUCCESS(Ps5M2ReadSectorsSync(DevExt->LowerDevice, 33ULL, PS5_M2_SECTOR_SIZE, Lba33)) &&
        Ps5M2IsGuardEntry(Lba33 + 2U * PS5_M2_GPT_ENTRY_SIZE)) {
      DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                 "ps5m2flt: lower instance already active on this disk - staying transparent\n");
      ExFreePoolWithTag(Scratch, PS5_M2_POOL_TAG);
      return;
    }
  }
  BOOLEAN HaveHead = NT_SUCCESS(Ps5M2ReadSectorsSync(DevExt->LowerDevice, PS5_M2_SONY_HEAD, HeadBytes, Head34));
  BOOLEAN HaveBack = NT_SUCCESS(Ps5M2ReadSectorsSync(DevExt->LowerDevice, LastLba, PS5_M2_SECTOR_SIZE, BackupHdr));

  if (HaveHead && HaveBack && Ps5M2BuildOverlay(TotalSectors, Head34, BackupHdr, &DevExt->Overlay)) {
    UINT32 NumEntries = Ps5M2R32(Head34 + PS5_M2_SECTOR_SIZE + 80);
    UINT8 GuidCopy[16];
    Ps5M2Copy(GuidCopy, Head34 + PS5_M2_SECTOR_SIZE + 56, 16U);
    Ps5M2BuildSyntheticWithGuards(DevExt, Head34 + 2U * PS5_M2_SECTOR_SIZE, NumEntries, GuidCopy, TotalSectors, LastLba);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "ps5m2flt: activated guarded writable nested GPT overlay (total=%llu parts=%u safe=[%llu..%llu])\n",
               TotalSectors, DevExt->Overlay.PartitionCount, DevExt->NestedFirstUsable, DevExt->NestedLastUsable);
  } else if (HaveHead && HaveBack && Ps5M2IsValidEmptyNestedGpt(Head34, BackupHdr, LastLba)) {
    Ps5M2InitEmptyOverlay(DevExt, TotalSectors, LastLba, Head34 + PS5_M2_SECTOR_SIZE + 56);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "ps5m2flt: valid empty nested GPT detected -> guarded writable overlay (safe=[%llu..%llu])\n",
               DevExt->NestedFirstUsable, DevExt->NestedLastUsable);
  } else if (Ps5M2ShouldFailSafeProtect(HaveLba0 ? Lba0 : NULL, HaveHead ? Head34 : NULL)) {
    if (HaveHead && !Ps5M2HasNestedSig(Head34)) {
      Ps5M2InitEmptyOverlay(DevExt, TotalSectors, LastLba, NULL);
      DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                 "ps5m2flt: fresh GameOS M.2 detected -> empty guarded writable overlay (safe=[%llu..%llu])\n",
                 DevExt->NestedFirstUsable, DevExt->NestedLastUsable);
    } else {
      DevExt->FailSafeWriteProtect = TRUE;
      DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                 "ps5m2flt: FAIL-SAFE WRITE PROTECT active on GameOS disk with corrupted nested GPT (total=%llu)\n",
                 TotalSectors);
    }
  }

  ExFreePoolWithTag(Scratch, PS5_M2_POOL_TAG);
}

static NTSTATUS Ps5M2CompleteBlocked(PIRP Irp, PPS5_M2_DEVEXT DevExt) {
  Irp->IoStatus.Status = STATUS_MEDIA_WRITE_PROTECTED;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
  return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS Ps5M2DispatchPassThrough(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PPS5_M2_DEVEXT DevExt = (PPS5_M2_DEVEXT)DeviceObject->DeviceExtension;
  NTSTATUS Status = IoAcquireRemoveLock(&DevExt->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }
  IoSkipCurrentIrpStackLocation(Irp);
  Status = IoCallDriver(DevExt->LowerDevice, Irp);
  IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
  return Status;
}

static NTSTATUS Ps5M2DispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PPS5_M2_DEVEXT DevExt = (PPS5_M2_DEVEXT)DeviceObject->DeviceExtension;
  NTSTATUS Status = IoAcquireRemoveLock(&DevExt->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    PoStartNextPowerIrp(Irp);
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }
  PoStartNextPowerIrp(Irp);
  IoSkipCurrentIrpStackLocation(Irp);
  Status = PoCallDriver(DevExt->LowerDevice, Irp);
  IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
  return Status;
}

static NTSTATUS Ps5M2ReadCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
  UNREFERENCED_PARAMETER(DeviceObject);
  PPS5_M2_READ_CTX Ctx = (PPS5_M2_READ_CTX)Context;
  if (Irp->PendingReturned) {
    IoMarkIrpPending(Irp);
  }
  if (NT_SUCCESS(Irp->IoStatus.Status)) {
    PUCHAR Buffer = Ps5M2MapIrpBuffer(Irp);
    if (Buffer != NULL) {
      Ps5M2ApplyReadOverlay(&Ctx->DevExt->Overlay, Ctx->StartLba, Ctx->SectorCount, Buffer);
      for (UINTN I = 0; I < Ctx->SectorCount; I++) {
        UINT64 Cur = Ctx->StartLba + I;
        if ((Cur >= PS5_M2_SONY_HEAD && Cur < Ctx->DevExt->NestedFirstUsable) ||
            (Cur > Ctx->DevExt->NestedLastUsable && Cur < Ctx->DevExt->Overlay.TotalSectors - PS5_M2_TAIL_SECTORS)) {
          Ps5M2Zero(Buffer + I * PS5_M2_SECTOR_SIZE, PS5_M2_SECTOR_SIZE);
        }
      }
    }
  }
  IoReleaseRemoveLock(&Ctx->DevExt->RemoveLock, Irp);
  ExFreePoolWithTag(Ctx, PS5_M2_POOL_TAG);
  return STATUS_SUCCESS;
}

static NTSTATUS Ps5M2DispatchRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PPS5_M2_DEVEXT DevExt = (PPS5_M2_DEVEXT)DeviceObject->DeviceExtension;
  NTSTATUS Status = IoAcquireRemoveLock(&DevExt->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }

  if (!DevExt->Overlay.Valid) {
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(DevExt->LowerDevice, Irp);
    IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
    return Status;
  }

  PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
  ULONG Length = Sp->Parameters.Read.Length;
  LONGLONG ByteOffset = Sp->Parameters.Read.ByteOffset.QuadPart;

  if (Length == 0 || ByteOffset < 0 || (ByteOffset % PS5_M2_SECTOR_SIZE) != 0 || (Length % PS5_M2_SECTOR_SIZE) != 0) {
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(DevExt->LowerDevice, Irp);
    IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
    return Status;
  }

  UINT64 StartLba = (UINT64)ByteOffset / PS5_M2_SECTOR_SIZE;
  UINTN SectorCount = (UINTN)(Length / PS5_M2_SECTOR_SIZE);

  /* Fast path: normal partition I/O strictly inside [NestedFirstUsable, NestedLastUsable] */
  BOOLEAN TouchesHeadGuard = (StartLba < DevExt->NestedFirstUsable);
  BOOLEAN TouchesTailGuard = (StartLba + SectorCount >= StartLba) &&
                             (StartLba + SectorCount - 1ULL > DevExt->NestedLastUsable);
  if (!TouchesHeadGuard && !TouchesTailGuard) {
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(DevExt->LowerDevice, Irp);
    IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
    return Status;
  }

  /* Pure synthetic read (entirely within [0, NestedFirstUsable) or (NestedLastUsable, N)): complete immediately from RAM */
  if ((StartLba + SectorCount <= DevExt->NestedFirstUsable) ||
      (StartLba > DevExt->NestedLastUsable && StartLba + SectorCount <= DevExt->Overlay.TotalSectors)) {
    PUCHAR Buffer = Ps5M2MapIrpBuffer(Irp);
    if (Buffer == NULL) {
      Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
      Irp->IoStatus.Information = 0;
      IoCompleteRequest(Irp, IO_NO_INCREMENT);
      IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    Ps5M2Zero(Buffer, Length);
    Ps5M2ApplyReadOverlay(&DevExt->Overlay, StartLba, SectorCount, Buffer);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = Length;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
    return STATUS_SUCCESS;
  }

  /* Mixed read crossing synthetic + real sectors: forward and splice on completion */
  PPS5_M2_READ_CTX Ctx = (PPS5_M2_READ_CTX)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Ctx), PS5_M2_POOL_TAG);
  if (Ctx == NULL) {
    Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  Ctx->DevExt = DevExt;
  Ctx->StartLba = StartLba;
  Ctx->SectorCount = SectorCount;

  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Ps5M2ReadCompletion, Ctx, TRUE, TRUE, TRUE);
  return IoCallDriver(DevExt->LowerDevice, Irp);
}

static BOOLEAN Ps5M2ValidateGptEntrySectors(const UINT8 *Buf, UINTN SectorCount, UINT64 Fu, UINT64 Lu) {
  UINTN EntriesPerSec = PS5_M2_SECTOR_SIZE / PS5_M2_GPT_ENTRY_SIZE; /* 4 */
  for (UINTN S = 0; S < SectorCount; S++) {
    for (UINTN E = 0; E < EntriesPerSec; E++) {
      const UINT8 *Ent = Buf + S * PS5_M2_SECTOR_SIZE + E * PS5_M2_GPT_ENTRY_SIZE;
      BOOLEAN NonZero = FALSE;
      for (UINTN K = 0; K < 16U; K++) if (Ent[K] != 0) { NonZero = TRUE; break; }
      if (!NonZero) continue;
      if (Ps5M2IsGuardEntry(Ent)) continue; /* guard entries are validated separately */
      UINT64 First = Ps5M2R64(Ent + 32);
      UINT64 Last  = Ps5M2R64(Ent + 40);
      if (First < Fu || Last < First || Last > Lu) {
        return FALSE;
      }
    }
  }
  return TRUE;
}

/* Safely redirect writes targeting the synthetic primary GPT (LBA 0..33) or backup GPT (LBA N-33..N-1)
 * into the nested GPT at LBA 65536..65569 and LBA NestedLastLba-32..NestedLastLba.
 * The 2 synthetic GameOS guard partitions cannot be deleted/altered; real LBA 0..65535 and NestedLastLba+1..N-1 are NEVER written. */
static NTSTATUS Ps5M2HandleSyntheticGptWrite(PPS5_M2_DEVEXT DevExt, PIRP Irp,
                                             UINT64 StartLba, UINTN SectorCount, ULONG Length) {
  if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
    return Ps5M2CompleteBlocked(Irp, DevExt);
  }
  PUCHAR InBuf = Ps5M2MapIrpBuffer(Irp);
  if (InBuf == NULL) {
    Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  UINT64 TotalSectors = DevExt->Overlay.TotalSectors;
  UINT64 TailBase = TotalSectors - PS5_M2_TAIL_SECTORS;
  BOOLEAN IsPrimary = (StartLba + SectorCount <= PS5_M2_HEAD_SECTORS);
  BOOLEAN IsBackup  = (StartLba >= TailBase && StartLba + SectorCount <= TotalSectors);
  if (!IsPrimary && !IsBackup) {
    return Ps5M2CompleteBlocked(Irp, DevExt);
  }

  KeWaitForSingleObject(&DevExt->OverlayLock, Executive, KernelMode, FALSE, NULL);

  /* Validate all partition entry sectors before modifying RAM or disk */
  BOOLEAN TouchesGuardSec = FALSE;
  for (UINTN I = 0; I < SectorCount; I++) {
    UINT64 Cur = StartLba + I;
    const UINT8 *Sec = InBuf + I * PS5_M2_SECTOR_SIZE;
    if (IsPrimary && Cur >= 2ULL && Cur < PS5_M2_HEAD_SECTORS) {
      if (Cur == PS5_M2_HEAD_SECTORS - 1ULL) TouchesGuardSec = TRUE;
      if (!Ps5M2ValidateGptEntrySectors(Sec, 1U, DevExt->NestedFirstUsable, DevExt->NestedLastUsable)) {
        KeReleaseMutex(&DevExt->OverlayLock, FALSE);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "ps5m2flt: BLOCKED primary GPT entry overlapping GameOS area\n");
        return Ps5M2CompleteBlocked(Irp, DevExt);
      }
    } else if (IsBackup && Cur >= TailBase && Cur < TotalSectors - 1ULL) {
      if (Cur == TotalSectors - 2ULL) TouchesGuardSec = TRUE;
      if (!Ps5M2ValidateGptEntrySectors(Sec, 1U, DevExt->NestedFirstUsable, DevExt->NestedLastUsable)) {
        KeReleaseMutex(&DevExt->OverlayLock, FALSE);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "ps5m2flt: BLOCKED backup GPT entry overlapping GameOS area\n");
        return Ps5M2CompleteBlocked(Irp, DevExt);
      }
    }
  }

  /* Verify both GameOS guard partitions are still intact if the guard entry sector (LBA 33 / N-2) is being written */
  BOOLEAN RestoreGuards = FALSE;
  if (TouchesGuardSec) {
    BOOLEAN HasHeadGuard = FALSE, HasTailGuard = FALSE, HasNewReal = FALSE;
    for (UINTN I = 0; I < SectorCount; I++) {
      UINT64 Cur = StartLba + I;
      BOOLEAN IsEntSec = (IsPrimary && Cur >= 2ULL && Cur < PS5_M2_HEAD_SECTORS) ||
                         (IsBackup && Cur >= TailBase && Cur < TotalSectors - 1ULL);
      if (!IsEntSec) continue;
      UINTN SecIdx = (UINTN)(IsPrimary ? (Cur - 2ULL) : (Cur - TailBase));
      const UINT8 *Sec = InBuf + I * PS5_M2_SECTOR_SIZE;
      for (UINTN E = 0; E < 4U; E++) {
        const UINT8 *Ent = Sec + E * PS5_M2_GPT_ENTRY_SIZE;
        if (Ps5M2IsHeadGuardEntry(Ent)) { HasHeadGuard = TRUE; continue; }
        if (Ps5M2IsTailGuardEntry(Ent)) { HasTailGuard = TRUE; continue; }
        BOOLEAN NonZero = FALSE;
        for (UINTN K = 0; K < 16U; K++) if (Ent[K] != 0) { NonZero = TRUE; break; }
        if (NonZero && !Ps5M2MemEq(Ent, DevExt->Overlay.Head[2U + SecIdx] + E * PS5_M2_GPT_ENTRY_SIZE, PS5_M2_GPT_ENTRY_SIZE)) {
          HasNewReal = TRUE;
        }
      }
    }
    if (!HasHeadGuard || !HasTailGuard) {
      /* Also check remaining untouched sectors in Overlay in case Windows moved guard entries */
      for (UINTN S = 0; S < PS5_M2_ENTRY_SECTORS; S++) {
        UINT64 Lba = IsPrimary ? (2ULL + S) : (TailBase + S);
        if (Lba >= StartLba && Lba < StartLba + SectorCount) continue;
        for (UINTN E = 0; E < 4U; E++) {
          const UINT8 *Ent = DevExt->Overlay.Head[2U + S] + E * PS5_M2_GPT_ENTRY_SIZE;
          if (Ps5M2IsHeadGuardEntry(Ent)) HasHeadGuard = TRUE;
          if (Ps5M2IsTailGuardEntry(Ent)) HasTailGuard = TRUE;
        }
      }
      if (!HasHeadGuard || !HasTailGuard) {
        if (!HasNewReal) {
          KeReleaseMutex(&DevExt->OverlayLock, FALSE);
          DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                     "ps5m2flt: BLOCKED attempt to delete PS5 GameOS guard partition\n");
          return Ps5M2CompleteBlocked(Irp, DevExt);
        }
        /* Valid real partitions are being created/updated (e.g. Setup Next after an earlier blocked guard-delete
         * left VDS cache stale): allow the valid real partitions and auto-restore the 2 GameOS guard slots 126/127. */
        RestoreGuards = TRUE;
      }
    }
  }

  /* Apply validated entry sectors and DiskGuid to in-RAM overlay */
  UINT8 DiskGuid[16];
  Ps5M2Copy(DiskGuid, &DevExt->Overlay.Head[1][56], 16U);
  for (UINTN I = 0; I < SectorCount; I++) {
    UINT64 Cur = StartLba + I;
    const UINT8 *Sec = InBuf + I * PS5_M2_SECTOR_SIZE;
    if (IsPrimary) {
      if (Cur == 1ULL && Ps5M2IsEfiPartSig(Sec)) {
        Ps5M2Copy(DiskGuid, Sec + 56, 16U);
      } else if (Cur >= 2ULL && Cur < PS5_M2_HEAD_SECTORS) {
        Ps5M2Copy(DevExt->Overlay.Head[Cur], Sec, PS5_M2_SECTOR_SIZE);
        Ps5M2Copy(DevExt->Overlay.Tail[Cur - 2ULL], Sec, PS5_M2_SECTOR_SIZE);
      }
    } else {
      if (Cur == TotalSectors - 1ULL && Ps5M2IsEfiPartSig(Sec)) {
        Ps5M2Copy(DiskGuid, Sec + 56, 16U);
      } else if (Cur >= TailBase && Cur < TotalSectors - 1ULL) {
        UINT64 Idx = Cur - TailBase;
        Ps5M2Copy(DevExt->Overlay.Tail[Idx], Sec, PS5_M2_SECTOR_SIZE);
        Ps5M2Copy(DevExt->Overlay.Head[2ULL + Idx], Sec, PS5_M2_SECTOR_SIZE);
      }
    }
  }
  if (RestoreGuards) {
    UINT64 GuardHeadFirst = PS5_M2_HEAD_SECTORS;
    UINT64 GuardHeadLast  = DevExt->NestedFirstUsable - 1ULL;
    UINT64 GuardTailFirst = DevExt->NestedLastUsable + 1ULL;
    UINT64 GuardTailLast  = TotalSectors - PS5_M2_HEAD_SECTORS;
    Ps5M2WriteGuardEntry(DevExt->Overlay.Head[PS5_M2_HEAD_SECTORS - 1ULL] + 2U * PS5_M2_GPT_ENTRY_SIZE,
                         gPs5GameOsHeadPartGuid, GuardHeadFirst, GuardHeadLast, "PS5 GameOS Header (Do Not Delete)");
    Ps5M2WriteGuardEntry(DevExt->Overlay.Head[PS5_M2_HEAD_SECTORS - 1ULL] + 3U * PS5_M2_GPT_ENTRY_SIZE,
                         gPs5GameOsTailPartGuid, GuardTailFirst, GuardTailLast, "PS5 GameOS Tail (Do Not Delete)");
    Ps5M2Copy(DevExt->Overlay.Tail[PS5_M2_ENTRY_SECTORS - 1ULL],
              DevExt->Overlay.Head[PS5_M2_HEAD_SECTORS - 1ULL], PS5_M2_SECTOR_SIZE);
  }

  UINT32 SynthCrc = Ps5M2Crc32(&DevExt->Overlay.Head[2][0], PS5_M2_ENTRY_SECTORS * PS5_M2_SECTOR_SIZE);
  UINT64 Fu = DevExt->NestedFirstUsable;
  UINT64 Lu = DevExt->NestedLastUsable;
  UINT64 LastLba = DevExt->Overlay.NestedLastLba;

  /* Refresh synthetic GPT headers in RAM (with guard range 34..N-34) */
  Ps5M2FormatPmbr(DevExt->Overlay.Head[0], 1U, TotalSectors - 1ULL);
  Ps5M2FormatGptHeader(DevExt->Overlay.Head[1], 1ULL, TotalSectors - 1ULL,
                       PS5_M2_HEAD_SECTORS, TotalSectors - PS5_M2_HEAD_SECTORS, DiskGuid, 2ULL, SynthCrc);
  Ps5M2FormatGptHeader(DevExt->Overlay.Tail[PS5_M2_ENTRY_SECTORS], TotalSectors - 1ULL, 1ULL,
                       PS5_M2_HEAD_SECTORS, TotalSectors - PS5_M2_HEAD_SECTORS,
                       DiskGuid, TotalSectors - 1ULL - PS5_M2_ENTRY_SECTORS, SynthCrc);

  /* Build pure on-disk nested GPT (strip synthetic guard entries so UEFI r5.25.8 sees only real Windows partitions) */
  ULONG HeadBytes = PS5_M2_HEAD_SECTORS * PS5_M2_SECTOR_SIZE;
  ULONG TailBytes = PS5_M2_TAIL_SECTORS * PS5_M2_SECTOR_SIZE;
  PUCHAR OutBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, HeadBytes + TailBytes, PS5_M2_POOL_TAG);
  NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
  if (OutBuf != NULL) {
    PUCHAR NestedHead = OutBuf;
    PUCHAR NestedTail = OutBuf + HeadBytes;
    Ps5M2Zero(OutBuf, HeadBytes + TailBytes);

    UINT32 OutIdx = 0U;
    for (UINTN S = 0; S < PS5_M2_ENTRY_SECTORS; S++) {
      for (UINTN E = 0; E < 4U; E++) {
        const UINT8 *Ent = DevExt->Overlay.Head[2U + S] + E * PS5_M2_GPT_ENTRY_SIZE;
        BOOLEAN NonZero = FALSE;
        for (UINTN K = 0; K < 16U; K++) if (Ent[K] != 0) { NonZero = TRUE; break; }
        if (!NonZero || Ps5M2IsGuardEntry(Ent)) continue;
        if (OutIdx < PS5_M2_GPT_ENTRIES) {
          Ps5M2Copy(NestedHead + 2U * PS5_M2_SECTOR_SIZE + (UINTN)OutIdx * PS5_M2_GPT_ENTRY_SIZE, Ent, PS5_M2_GPT_ENTRY_SIZE);
          Ps5M2Copy(NestedTail + (UINTN)OutIdx * PS5_M2_GPT_ENTRY_SIZE, Ent, PS5_M2_GPT_ENTRY_SIZE);
          OutIdx++;
        }
      }
    }

    UINT32 NestedCrc = Ps5M2Crc32(NestedHead + 2U * PS5_M2_SECTOR_SIZE, PS5_M2_ENTRY_SECTORS * PS5_M2_SECTOR_SIZE);
    Ps5M2FormatPmbr(NestedHead, (UINT32)(PS5_M2_SONY_HEAD + 1ULL), LastLba - PS5_M2_SONY_HEAD);
    Ps5M2FormatGptHeader(NestedHead + PS5_M2_SECTOR_SIZE, PS5_M2_SONY_HEAD + 1ULL, LastLba, Fu, Lu,
                         DiskGuid, PS5_M2_SONY_HEAD + 2ULL, NestedCrc);
    Ps5M2FormatGptHeader(NestedTail + PS5_M2_ENTRY_SECTORS * PS5_M2_SECTOR_SIZE, LastLba, PS5_M2_SONY_HEAD + 1ULL,
                         Fu, Lu, DiskGuid, LastLba - PS5_M2_ENTRY_SECTORS, NestedCrc);

    Status = Ps5M2WriteSectorsSync(DevExt->LowerDevice, PS5_M2_SONY_HEAD, HeadBytes, NestedHead);
    if (NT_SUCCESS(Status)) {
      Status = Ps5M2WriteSectorsSync(DevExt->LowerDevice, LastLba - PS5_M2_ENTRY_SECTORS, TailBytes, NestedTail);
    }
    ExFreePoolWithTag(OutBuf, PS5_M2_POOL_TAG);
  }

  KeReleaseMutex(&DevExt->OverlayLock, FALSE);

  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = NT_SUCCESS(Status) ? Length : 0;
  IoCompleteRequest(Irp, IO_DISK_INCREMENT);
  IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
  return Status;
}

static NTSTATUS Ps5M2DispatchWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PPS5_M2_DEVEXT DevExt = (PPS5_M2_DEVEXT)DeviceObject->DeviceExtension;
  NTSTATUS Status = IoAcquireRemoveLock(&DevExt->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }

  if (DevExt->FailSafeWriteProtect) {
    return Ps5M2CompleteBlocked(Irp, DevExt);
  }

  if (DevExt->Overlay.Valid) {
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG Length = Sp->Parameters.Write.Length;
    LONGLONG ByteOffset = Sp->Parameters.Write.ByteOffset.QuadPart;

    if (Length > 0) {
      if (ByteOffset < 0 || (ByteOffset % PS5_M2_SECTOR_SIZE) != 0 || (Length % PS5_M2_SECTOR_SIZE) != 0) {
        return Ps5M2CompleteBlocked(Irp, DevExt);
      }
      UINT64 StartLba = (UINT64)ByteOffset / PS5_M2_SECTOR_SIZE;
      UINTN SectorCount = (UINTN)(Length / PS5_M2_SECTOR_SIZE);
      UINT64 TotalSectors = DevExt->Overlay.TotalSectors;
      UINT64 TailBase = TotalSectors - PS5_M2_TAIL_SECTORS;

      /* Synthetic GPT writes (LBA 0..33 or N-33..N-1) are safely redirected to the nested GPT */
      if ((StartLba + SectorCount >= StartLba) &&
          ((StartLba + SectorCount <= PS5_M2_HEAD_SECTORS) ||
           (StartLba >= TailBase && StartLba + SectorCount <= TotalSectors))) {
        return Ps5M2HandleSyntheticGptWrite(DevExt, Irp, StartLba, SectorCount, Length);
      }

      if (!Ps5M2CheckWriteAllowed(&DevExt->Overlay, StartLba, SectorCount)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "ps5m2flt: BLOCKED write to GameOS area LBA=%llu count=%llu\n",
                   StartLba, (UINT64)SectorCount);
        return Ps5M2CompleteBlocked(Irp, DevExt);
      }
    }
  }

  IoSkipCurrentIrpStackLocation(Irp);
  Status = IoCallDriver(DevExt->LowerDevice, Irp);
  IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
  return Status;
}

static BOOLEAN Ps5M2ValidateDsmTrim(const PS5_M2_OVERLAY *Ov, const VOID *SysBuf, ULONG InLen) {
  if (SysBuf == NULL || InLen < sizeof(PS5_DSM_INPUT)) return FALSE;
  const PS5_DSM_INPUT *Dsm = (const PS5_DSM_INPUT *)SysBuf;
  if (Dsm->Flags & PS5_DSM_FLAG_ENTIRE_DATA_SET_RANGE) return FALSE;
  if (Dsm->DataSetRangesOffset == 0 || Dsm->DataSetRangesLength == 0) return TRUE;
  if (Dsm->DataSetRangesOffset > InLen || Dsm->DataSetRangesLength > InLen - Dsm->DataSetRangesOffset) return FALSE;
  if ((Dsm->DataSetRangesLength % sizeof(PS5_DSM_RANGE)) != 0) return FALSE;
  ULONG Count = Dsm->DataSetRangesLength / sizeof(PS5_DSM_RANGE);
  const PS5_DSM_RANGE *Ranges = (const PS5_DSM_RANGE *)((const UINT8 *)SysBuf + Dsm->DataSetRangesOffset);
  for (ULONG I = 0; I < Count; I++) {
    LONGLONG Off = Ranges[I].StartingOffset;
    UINT64 Len = Ranges[I].LengthInBytes;
    if (Off < 0 || Len == 0) return FALSE;
    UINT64 StartLba = (UINT64)Off / PS5_M2_SECTOR_SIZE;
    UINT64 EndByte = (UINT64)Off + Len;
    if (EndByte < (UINT64)Off) return FALSE;
    UINT64 EndLba = (EndByte - 1ULL) / PS5_M2_SECTOR_SIZE;
    UINTN Sectors = (UINTN)(EndLba - StartLba + 1ULL);
    if (!Ps5M2CheckWriteAllowed(Ov, StartLba, Sectors)) return FALSE;
  }
  return TRUE;
}

static NTSTATUS Ps5M2DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PPS5_M2_DEVEXT DevExt = (PPS5_M2_DEVEXT)DeviceObject->DeviceExtension;
  NTSTATUS Status = IoAcquireRemoveLock(&DevExt->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }

  if (DevExt->Overlay.Valid || DevExt->FailSafeWriteProtect) {
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG Code = Sp->Parameters.DeviceIoControl.IoControlCode;

    if (DevExt->FailSafeWriteProtect && Code == PS5_IOCTL_DISK_IS_WRITABLE) {
      return Ps5M2CompleteBlocked(Irp, DevExt);
    }

    switch (Code) {
      case PS5_IOCTL_SCSI_PASS_THROUGH:
      case PS5_IOCTL_SCSI_PASS_THROUGH_DIRECT:
      case PS5_IOCTL_SCSI_PASS_THROUGH_EX:
      case PS5_IOCTL_SCSI_PASS_THROUGH_DIRECT_EX:
      case PS5_IOCTL_ATA_PASS_THROUGH:
      case PS5_IOCTL_ATA_PASS_THROUGH_DIRECT:
      case PS5_IOCTL_STORAGE_PROTOCOL_COMMAND:
      case PS5_IOCTL_STORAGE_REINITIALIZE_MEDIA:
      case PS5_IOCTL_DISK_SET_DRIVE_LAYOUT:
      case PS5_IOCTL_DISK_SET_DRIVE_LAYOUT_EX:
      case PS5_IOCTL_DISK_CREATE_DISK:
      case PS5_IOCTL_DISK_FORMAT_TRACKS:
      case PS5_IOCTL_DISK_FORMAT_TRACKS_EX:
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "ps5m2flt: BLOCKED destructive/passthrough IOCTL 0x%08lx\n", Code);
        return Ps5M2CompleteBlocked(Irp, DevExt);

      case PS5_IOCTL_STORAGE_MANAGE_DATA_SET_ATTR:
        if (!DevExt->FailSafeWriteProtect &&
            Irp->AssociatedIrp.SystemBuffer != NULL &&
            Sp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(PS5_DSM_INPUT) &&
            (((const PS5_DSM_INPUT *)Irp->AssociatedIrp.SystemBuffer)->Flags & PS5_DSM_FLAG_ENTIRE_DATA_SET_RANGE)) {
          /* Advisory whole-disk TRIM during format: complete as safe no-op so GameOS stays untouched and format succeeds */
          Irp->IoStatus.Status = STATUS_SUCCESS;
          Irp->IoStatus.Information = 0;
          IoCompleteRequest(Irp, IO_NO_INCREMENT);
          IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
          return STATUS_SUCCESS;
        }
        if (DevExt->FailSafeWriteProtect ||
            !Ps5M2ValidateDsmTrim(&DevExt->Overlay,
                                  Irp->AssociatedIrp.SystemBuffer,
                                  Sp->Parameters.DeviceIoControl.InputBufferLength)) {
          DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                     "ps5m2flt: BLOCKED DSM/TRIM overlapping GameOS area\n");
          return Ps5M2CompleteBlocked(Irp, DevExt);
        }
        break;

      default:
        break;
    }
  }

  IoSkipCurrentIrpStackLocation(Irp);
  Status = IoCallDriver(DevExt->LowerDevice, Irp);
  IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
  return Status;
}

static NTSTATUS Ps5M2DispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PPS5_M2_DEVEXT DevExt = (PPS5_M2_DEVEXT)DeviceObject->DeviceExtension;
  NTSTATUS Status = IoAcquireRemoveLock(&DevExt->RemoveLock, Irp);
  if (!NT_SUCCESS(Status)) {
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  }

  PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
  UCHAR Minor = Sp->MinorFunction;

  if (Minor == IRP_MN_START_DEVICE) {
    Status = Ps5M2ForwardIrpSynchronously(DevExt->LowerDevice, Irp);
    if (NT_SUCCESS(Status)) {
      Ps5M2TryProbeNestedGpt(DevExt);
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
    return Status;
  }

  if (Minor == IRP_MN_REMOVE_DEVICE) {
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(DevExt->LowerDevice, Irp);
    IoReleaseRemoveLockAndWait(&DevExt->RemoveLock, Irp);
    IoDetachDevice(DevExt->LowerDevice);
    IoDeleteDevice(DeviceObject);
    return Status;
  }

  IoSkipCurrentIrpStackLocation(Irp);
  Status = IoCallDriver(DevExt->LowerDevice, Irp);
  IoReleaseRemoveLock(&DevExt->RemoveLock, Irp);
  return Status;
}

static NTSTATUS Ps5M2AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
  PDEVICE_OBJECT FilterDevice = NULL;
  NTSTATUS Status = IoCreateDevice(
      DriverObject,
      sizeof(PS5_M2_DEVEXT),
      NULL,
      FILE_DEVICE_DISK,
      FILE_DEVICE_SECURE_OPEN,
      FALSE,
      &FilterDevice);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  PPS5_M2_DEVEXT DevExt = (PPS5_M2_DEVEXT)FilterDevice->DeviceExtension;
  RtlZeroMemory(DevExt, sizeof(*DevExt));
  DevExt->Self = FilterDevice;
  DevExt->PhysicalDevice = PhysicalDeviceObject;
  IoInitializeRemoveLock(&DevExt->RemoveLock, PS5_M2_POOL_TAG, 0, 0);
  KeInitializeMutex(&DevExt->OverlayLock, 0);

  DevExt->LowerDevice = IoAttachDeviceToDeviceStack(FilterDevice, PhysicalDeviceObject);
  if (DevExt->LowerDevice == NULL) {
    IoDeleteDevice(FilterDevice);
    return STATUS_NO_SUCH_DEVICE;
  }

  FilterDevice->Flags |= (DevExt->LowerDevice->Flags & (DO_DIRECT_IO | DO_BUFFERED_IO | DO_POWER_PAGABLE));
  FilterDevice->DeviceType = DevExt->LowerDevice->DeviceType;
  FilterDevice->Characteristics = DevExt->LowerDevice->Characteristics;
  FilterDevice->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}

static VOID Ps5M2Unload(PDRIVER_OBJECT DriverObject) {
  UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
  UNREFERENCED_PARAMETER(RegistryPath);
  for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
    DriverObject->MajorFunction[i] = Ps5M2DispatchPassThrough;
  }
  DriverObject->MajorFunction[IRP_MJ_READ]           = Ps5M2DispatchRead;
  DriverObject->MajorFunction[IRP_MJ_WRITE]          = Ps5M2DispatchWrite;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Ps5M2DispatchDeviceControl;
  DriverObject->MajorFunction[IRP_MJ_PNP]            = Ps5M2DispatchPnp;
  DriverObject->MajorFunction[IRP_MJ_POWER]          = Ps5M2DispatchPower;
  DriverObject->DriverExtension->AddDevice           = Ps5M2AddDevice;
  DriverObject->DriverUnload                         = Ps5M2Unload;
  return STATUS_SUCCESS;
}
