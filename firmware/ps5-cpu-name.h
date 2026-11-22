/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-cpu-name.h v1.0 (16.11.2026, user naming rule "ps5-hardware-naming"):
 *  - Processor Name String MSRs C001_0030..0035 (AMD PPR Fam17h: read-write, they back CPUID 0x80000002..4) set to
 *    "AMD Zen 2 (PS5 Custom CPU)" on the BSP and every AP (MP services, sequential, no DEBUG from AP context). Only on
 *    family 17h; the old brand is logged first, the BSP write is verified through CPUID before the APs are touched.
 *  - SMBIOS Type 4 (processor) and Type 41 (onboard device: "AMD Oberon (PS5 Custom GPU)") through EFI_SMBIOS_PROTOCOL,
 *    if present (SmbiosDxe is in the payload; the PS5 loader hands over no SMBIOS table, so these are new records).
 *  - r5.24: SMBIOS Type 0 (BIOS information: vendor "PSWloader", version "PSWloader v1.0") - there was none, so Windows
 *    showed an empty BIOS vendor/version.
 * Cosmetic only: no clock, voltage or feature bit is changed. */
#ifndef PS5_CPU_NAME_H
#define PS5_CPU_NAME_H
#include <Protocol/Smbios.h>
#include <IndustryStandard/SmBios.h>
#define PS5_CPU_NAME "AMD Zen 2 (PS5 Custom CPU)"
#define PS5_GPU_NAME "AMD Oberon (PS5 Custom GPU)"
STATIC UINT8 mPs5ApNameState[16];                     /* 0 not run, 1 written+verified, 2 family mismatch, 3 verify failed */
STATIC BOOLEAN Ps5IsFamily17 (VOID)
{
  UINT32 Eax = 0; AsmCpuid (1, &Eax, NULL, NULL, NULL);
  UINT32 Fam = (Eax >> 8) & 0xF; if (Fam == 0xF) Fam += (Eax >> 20) & 0xFF;
  return Fam == 0x17;
}
STATIC VOID Ps5ReadBrand (CHAR8 *Out)                 /* 48 bytes + NUL */
{
  UINT32 *W = (UINT32 *)Out;
  for (UINT32 I = 0; I < 3; I++) AsmCpuid (0x80000002 + I, &W[I*4], &W[I*4+1], &W[I*4+2], &W[I*4+3]);
  Out[48] = 0;
}
STATIC UINT8 Ps5WriteBrand (VOID)                     /* runs on BSP and APs: MSR only, no DEBUG/allocation */
{
  if (!Ps5IsFamily17 ()) return 2;
  CHAR8 Name[48];
  for (UINTN I = 0; I < 48; I++) Name[I] = 0;
  CONST CHAR8 *S = PS5_CPU_NAME;
  for (UINTN I = 0; I < 47 && S[I]; I++) Name[I] = S[I];
  for (UINT32 I = 0; I < 6; I++) {
    UINT64 V = 0;
    for (UINT32 B = 0; B < 8; B++) V |= (UINT64)(UINT8)Name[I * 8 + B] << (B * 8);
    AsmWriteMsr64 (0xC0010030 + I, V);
  }
  CHAR8 Back[49]; Ps5ReadBrand (Back);
  for (UINTN I = 0; I < 48; I++) if (Back[I] != Name[I]) return 3;
  return 1;
}
STATIC VOID EFIAPI Ps5CpuNameApWorker (IN VOID *Context)
{
  EFI_MP_SERVICES_PROTOCOL *Mp = Context; UINTN Who = 0;
  if (Mp == NULL || EFI_ERROR (Mp->WhoAmI (Mp, &Who)) || Who >= 16) return;
  mPs5ApNameState[Who] = Ps5WriteBrand ();
}
STATIC VOID Ps5AddSmbiosString (UINT8 **P, CONST CHAR8 *S)
{
  UINTN L = AsciiStrLen (S); CopyMem (*P, S, L); *P += L; *(*P)++ = 0;
}
STATIC VOID Ps5CpuNameSmbios (VOID)
{
  EFI_SMBIOS_PROTOCOL *Smbios = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status) || Smbios == NULL) { DEBUG ((DEBUG_ERROR, "PS5 name: no SMBIOS protocol (%r)\n", Status)); return; }
  UINT8 Buf[256]; EFI_SMBIOS_HANDLE H;
  { /* r5.24: Type 0 - strings 1 vendor, 2 version, 3 release date (mm/dd/yyyy) */
    ZeroMem (Buf, sizeof (Buf));
    SMBIOS_TABLE_TYPE0 *T0 = (SMBIOS_TABLE_TYPE0 *)Buf; UINT64 Bc = BIT3;   /* "BIOS characteristics not supported" */
    T0->Hdr.Type = EFI_SMBIOS_TYPE_BIOS_INFORMATION; T0->Hdr.Length = sizeof (SMBIOS_TABLE_TYPE0); T0->Hdr.Handle = SMBIOS_HANDLE_PI_RESERVED;
    T0->Vendor = 1; T0->BiosVersion = 2; T0->BiosReleaseDate = 3; T0->BiosSegment = 0; T0->BiosSize = 0xFF;
    CopyMem (&T0->BiosCharacteristics, &Bc, sizeof (Bc));
    T0->BIOSCharacteristicsExtensionBytes[1] = BIT3;                        /* UEFI specification supported */
    T0->SystemBiosMajorRelease = 1; T0->SystemBiosMinorRelease = 0;
    T0->EmbeddedControllerFirmwareMajorRelease = 0xFF; T0->EmbeddedControllerFirmwareMinorRelease = 0xFF;
    UINT8 *P0 = Buf + T0->Hdr.Length;
    Ps5AddSmbiosString (&P0, "PSWloader"); Ps5AddSmbiosString (&P0, "PSWloader v1.0.4"); Ps5AddSmbiosString (&P0, "11/22/2026"); *P0++ = 0;
    H = SMBIOS_HANDLE_PI_RESERVED;
    Status = Smbios->Add (Smbios, NULL, &H, (EFI_SMBIOS_TABLE_HEADER *)Buf);
    DEBUG ((DEBUG_ERROR, "PS5 name: SMBIOS type 0 add %r (handle %x)\n", Status, H));
  }
  /* Type 4: strings 1 socket, 2 manufacturer, 3 version */
  ZeroMem (Buf, sizeof (Buf));
  SMBIOS_TABLE_TYPE4 *T4 = (SMBIOS_TABLE_TYPE4 *)Buf;
  T4->Hdr.Type = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION; T4->Hdr.Length = sizeof (SMBIOS_TABLE_TYPE4); T4->Hdr.Handle = SMBIOS_HANDLE_PI_RESERVED;
  T4->Socket = 1; T4->ProcessorType = CentralProcessor; T4->ProcessorFamily = ProcessorFamilyIndicatorFamily2;
  T4->ProcessorManufacturer = 2; T4->ProcessorVersion = 3;
  UINT32 Eax = 0, Edx = 0; AsmCpuid (1, &Eax, NULL, NULL, &Edx);
  CopyMem (&T4->ProcessorId, &Eax, 4); CopyMem ((UINT8 *)&T4->ProcessorId + 4, &Edx, 4);
  T4->MaxSpeed = 3500; T4->CurrentSpeed = 3500; T4->ExternalClock = 100;
  T4->Status = 0x41;                                   /* socket populated, CPU enabled */
  T4->ProcessorUpgrade = ProcessorUpgradeNone;
  T4->L1CacheHandle = T4->L2CacheHandle = T4->L3CacheHandle = 0xFFFF;
  T4->CoreCount = 8; T4->EnabledCoreCount = 8; T4->ThreadCount = 16;
  T4->ProcessorCharacteristics = 0x00EC;               /* 64-bit, multi-core, HW thread, execute protection, enhanced virt. */
  T4->ProcessorFamily2 = 0x6B;                         /* Zen */
  T4->CoreCount2 = 8; T4->EnabledCoreCount2 = 8; T4->ThreadCount2 = 16;
  UINT8 *P = Buf + T4->Hdr.Length;
  Ps5AddSmbiosString (&P, "PS5 SoC"); Ps5AddSmbiosString (&P, "AMD"); Ps5AddSmbiosString (&P, PS5_CPU_NAME); *P++ = 0;
  H = SMBIOS_HANDLE_PI_RESERVED;
  Status = Smbios->Add (Smbios, NULL, &H, (EFI_SMBIOS_TABLE_HEADER *)Buf);
  DEBUG ((DEBUG_ERROR, "PS5 name: SMBIOS type 4 add %r (handle %x)\n", Status, H));
  /* Type 41: onboard GPU */
  ZeroMem (Buf, sizeof (Buf));
  SMBIOS_TABLE_TYPE41 *T41 = (SMBIOS_TABLE_TYPE41 *)Buf;
  T41->Hdr.Type = EFI_SMBIOS_TYPE_ONBOARD_DEVICES_EXTENDED_INFORMATION; T41->Hdr.Length = sizeof (SMBIOS_TABLE_TYPE41); T41->Hdr.Handle = SMBIOS_HANDLE_PI_RESERVED;
  T41->ReferenceDesignation = 1; T41->DeviceType = 0x80 | OnBoardDeviceExtendedTypeVideo; T41->DeviceTypeInstance = 1;
  T41->SegmentGroupNum = 0xFFFF; T41->BusNum = 0xFF; T41->DevFuncNum = 0xFF;   /* not tied to a PCI address here */
  P = Buf + T41->Hdr.Length;
  Ps5AddSmbiosString (&P, PS5_GPU_NAME); *P++ = 0;
  H = SMBIOS_HANDLE_PI_RESERVED;
  Status = Smbios->Add (Smbios, NULL, &H, (EFI_SMBIOS_TABLE_HEADER *)Buf);
  DEBUG ((DEBUG_ERROR, "PS5 name: SMBIOS type 41 add %r (handle %x)\n", Status, H));
  /* r5.3 (16.11.): memory records - UiApp shows "0 MB RAM" without type 19; Windows reads type 16/17 too.
   * PS5: 16 GiB unified GDDR6 (CPU+GPU), one array, one "device", physical 0..16 GiB. */
  ZeroMem (Buf, sizeof (Buf));
  SMBIOS_TABLE_TYPE16 *T16 = (SMBIOS_TABLE_TYPE16 *)Buf;
  T16->Hdr.Type = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY; T16->Hdr.Length = sizeof (SMBIOS_TABLE_TYPE16); T16->Hdr.Handle = SMBIOS_HANDLE_PI_RESERVED;
  T16->Location = MemoryArrayLocationSystemBoard; T16->Use = MemoryArrayUseSystemMemory; T16->MemoryErrorCorrection = MemoryErrorCorrectionNone;
  T16->MaximumCapacity = 0x80000000; T16->ExtendedMaximumCapacity = 16ULL << 30; T16->MemoryErrorInformationHandle = 0xFFFE; T16->NumberOfMemoryDevices = 1;
  P = Buf + T16->Hdr.Length; *P++ = 0; *P++ = 0;
  EFI_SMBIOS_HANDLE H16 = SMBIOS_HANDLE_PI_RESERVED;
  Status = Smbios->Add (Smbios, NULL, &H16, (EFI_SMBIOS_TABLE_HEADER *)Buf);
  DEBUG ((DEBUG_ERROR, "PS5 name: SMBIOS type 16 add %r (handle %x)\n", Status, H16));
  ZeroMem (Buf, sizeof (Buf));
  SMBIOS_TABLE_TYPE17 *T17 = (SMBIOS_TABLE_TYPE17 *)Buf;
  T17->Hdr.Type = EFI_SMBIOS_TYPE_MEMORY_DEVICE; T17->Hdr.Length = sizeof (SMBIOS_TABLE_TYPE17); T17->Hdr.Handle = SMBIOS_HANDLE_PI_RESERVED;
  T17->MemoryArrayHandle = H16; T17->MemoryErrorInformationHandle = 0xFFFE; T17->TotalWidth = 256; T17->DataWidth = 256;
  T17->Size = 0x7FFF; T17->ExtendedSize = 16384;               /* MiB */
  T17->FormFactor = MemoryFormFactorChip; T17->DeviceLocator = 1; T17->BankLocator = 2; T17->MemoryType = MemoryTypeOther;
  T17->Manufacturer = 3; T17->PartNumber = 4; T17->Speed = 14000; T17->ConfiguredMemoryClockSpeed = 14000;
  P = Buf + T17->Hdr.Length;
  Ps5AddSmbiosString (&P, "PS5 GDDR6 (unified)"); Ps5AddSmbiosString (&P, "SoC"); Ps5AddSmbiosString (&P, "AMD (PS5)"); Ps5AddSmbiosString (&P, "GDDR6 16 GB, 256-bit"); *P++ = 0;
  EFI_SMBIOS_HANDLE H17 = SMBIOS_HANDLE_PI_RESERVED;
  Status = Smbios->Add (Smbios, NULL, &H17, (EFI_SMBIOS_TABLE_HEADER *)Buf);
  DEBUG ((DEBUG_ERROR, "PS5 name: SMBIOS type 17 add %r (handle %x)\n", Status, H17));
  ZeroMem (Buf, sizeof (Buf));
  SMBIOS_TABLE_TYPE19 *T19 = (SMBIOS_TABLE_TYPE19 *)Buf;
  T19->Hdr.Type = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS; T19->Hdr.Length = sizeof (SMBIOS_TABLE_TYPE19); T19->Hdr.Handle = SMBIOS_HANDLE_PI_RESERVED;
  T19->StartingAddress = 0xFFFFFFFF; T19->EndingAddress = 0xFFFFFFFF; T19->MemoryArrayHandle = H16; T19->PartitionWidth = 1;
  T19->ExtendedStartingAddress = 0; T19->ExtendedEndingAddress = (16ULL << 30) - 1;
  P = Buf + T19->Hdr.Length; *P++ = 0; *P++ = 0;
  H = SMBIOS_HANDLE_PI_RESERVED;
  Status = Smbios->Add (Smbios, NULL, &H, (EFI_SMBIOS_TABLE_HEADER *)Buf);
  DEBUG ((DEBUG_ERROR, "PS5 name: SMBIOS type 19 add %r (handle %x)\n", Status, H));
}
STATIC VOID Ps5CpuNameApply (VOID)
{
  CHAR8 Old[49]; Ps5ReadBrand (Old);
  DEBUG ((DEBUG_ERROR, "PS5 name: CPUID brand before \"%a\", family17 %u\n", Old, (UINT32)Ps5IsFamily17 ()));
  UINT8 Bsp = Ps5WriteBrand ();
  CHAR8 New[49]; Ps5ReadBrand (New);
  DEBUG ((DEBUG_ERROR, "PS5 name: BSP result %u, brand now \"%a\"\n", (UINT32)Bsp, New));
  if (Bsp == 1) {                                      /* APs only after the BSP proved the MSRs writable */
    EFI_MP_SERVICES_PROTOCOL *Mp = NULL; UINTN *Failed = NULL, N = 0, E = 0;
    if (!EFI_ERROR (gBS->LocateProtocol (&gEfiMpServiceProtocolGuid, NULL, (VOID **)&Mp)) && Mp != NULL) {
      Mp->GetNumberOfProcessors (Mp, &N, &E);
      if (N > 1) {
        EFI_STATUS S = Mp->StartupAllAPs (Mp, Ps5CpuNameApWorker, TRUE, NULL, 2000000, (VOID *)Mp, &Failed);
        UINT32 Ok = 0; for (UINTN I = 1; I < N && I < 16; I++) if (mPs5ApNameState[I] == 1) Ok++;
        DEBUG ((DEBUG_ERROR, "PS5 name: APs %r, %u of %u written+verified\n", S, Ok, (UINT32)(N - 1)));
        for (UINTN I = 1; I < N && I < 16; I++) if (mPs5ApNameState[I] != 1) DEBUG ((DEBUG_ERROR, "PS5 name: AP %u state %u\n", (UINT32)I, mPs5ApNameState[I]));
      }
    }
  }
  Ps5CpuNameSmbios ();
}
#endif
