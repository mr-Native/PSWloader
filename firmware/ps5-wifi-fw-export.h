/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_WIFI_FW_EXPORT_H
#define PS5_WIFI_FW_EXPORT_H
/* PSWloader.elf stages the console's own IW620 Wi-Fi firmware from GameOS kernel memory
 * immediately after the UPL container inside the reserved loader arena (0x100000000..0x111000000),
 * prefixed by a 16-byte header: "PS5WFW01" (8B) | uint32_t size | uint32_t crc32.
 * Here we verify CRC32, copy it to EfiReservedMemoryType, publish the volatile runtime variable
 * Ps5WifiFw ({PhysAddr, Size, Crc32}) for ps5wifi.sys, and optionally mirror \PS5WIFIFW.BIN on ESP. */
#define PS5_WIFI_FW_ARENA_LO 0x100000000ULL
#define PS5_WIFI_FW_ARENA_HI 0x111000000ULL
#define PS5_WIFI_FW_MAX_SIZE 0x100000U

#define PS5_WIFI_FW_VENDOR_GUID_INIT \
  { 0x7c8e51b2, 0x4a19, 0x4e7c, { 0x9b, 0x31, 0x50, 0x53, 0x35, 0x57, 0x46, 0x57 } }

typedef struct {
  UINT64 PhysAddr;
  UINT32 Size;
  UINT32 Crc32;
} PS5_WIFI_FW_VAR;

STATIC UINT32 Ps5WifiCrc32(CONST UINT8 *Data, UINTN Len) {
  UINT32 C = 0xFFFFFFFFU;
  for (UINTN I = 0; I < Len; I++) {
    C ^= Data[I];
    for (UINTN B = 0; B < 8; B++) C = (C >> 1) ^ (0xEDB88320U & (0U - (C & 1U)));
  }
  return ~C;
}

STATIC EFI_STATUS Ps5ExportWifiFw(EFI_HANDLE Selected) {
  STATIC BOOLEAN Done = FALSE;
  if (Done) return EFI_SUCCESS;
  CONST UINT8 *Found = NULL;
  UINT32 Size = 0, Crc = 0;
  for (UINT64 A = PS5_WIFI_FW_ARENA_LO; A < PS5_WIFI_FW_ARENA_HI; A += EFI_PAGE_SIZE) {
    CONST UINT8 *H = (CONST UINT8 *)(UINTN)A;
    if (CompareMem(H, "PS5WFW01", 8) != 0) continue;
    UINT32 S = *(CONST UINT32 *)(H + 8);
    UINT32 Want = *(CONST UINT32 *)(H + 12);
    if (S < 0x1000 || S > PS5_WIFI_FW_MAX_SIZE || A + 16 + S > PS5_WIFI_FW_ARENA_HI) continue;
    if (Ps5WifiCrc32(H + 16, S) != Want) continue;
    Found = H + 16; Size = S; Crc = Want;
    break;
  }
  if (!Found && Selected != NULL) {
    /* Warm reboot from Windows (Ps5ResetSystemLib) does not re-run GameOS PSWloader.elf,
     * so fall back to the console's own \PS5WIFIFW.BIN mirrored on the ESP on cold boot. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
    if (!EFI_ERROR(gBS->HandleProtocol(Selected, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) &&
        !EFI_ERROR(Fs->OpenVolume(Fs, &Root)) &&
        !EFI_ERROR(Root->Open(Root, &File, L"\\PS5WIFIFW.BIN", EFI_FILE_MODE_READ, 0))) {
      EFI_PHYSICAL_ADDRESS EspPhys = 0;
      if (!EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, EFI_SIZE_TO_PAGES(PS5_WIFI_FW_MAX_SIZE), &EspPhys))) {
        UINTN ReadSz = PS5_WIFI_FW_MAX_SIZE;
        if (!EFI_ERROR(File->Read(File, &ReadSz, (VOID *)(UINTN)EspPhys)) &&
            ReadSz >= 0x1000 && ReadSz <= PS5_WIFI_FW_MAX_SIZE) {
          Done = TRUE;
          EFI_GUID Guid = PS5_WIFI_FW_VENDOR_GUID_INIT;
          PS5_WIFI_FW_VAR Var = { EspPhys, (UINT32)ReadSz, Ps5WifiCrc32((CONST UINT8 *)(UINTN)EspPhys, ReadSz) };
          EFI_STATUS VarSt = gRT->SetVariable(
            L"Ps5WifiFw", &Guid,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            sizeof(Var), &Var);
          DEBUG((DEBUG_ERROR, "PS5 WiFi FW export (ESP fallback): %u bytes at 0x%Lx crc=%08x var=%r\n",
                 Var.Size, EspPhys, Var.Crc32, VarSt));
          File->Close(File);
          Root->Close(Root);
          return VarSt;
        }
        gBS->FreePages(EspPhys, EFI_SIZE_TO_PAGES(PS5_WIFI_FW_MAX_SIZE));
      }
      File->Close(File);
      Root->Close(Root);
    }
  }
  if (!Found) {
    DEBUG((DEBUG_INFO, "PS5 WiFi FW export: no staged firmware found in arena or ESP\n"));
    return EFI_NOT_FOUND;
  }
  Done = TRUE;
  EFI_PHYSICAL_ADDRESS Phys = 0;
  EFI_STATUS Status = gBS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, EFI_SIZE_TO_PAGES(Size), &Phys);
  if (EFI_ERROR(Status)) return Status;
  CopyMem((VOID *)(UINTN)Phys, Found, Size);

  EFI_GUID Guid = PS5_WIFI_FW_VENDOR_GUID_INIT;
  PS5_WIFI_FW_VAR Var = { Phys, Size, Crc };
  EFI_STATUS VarSt = gRT->SetVariable(
    L"Ps5WifiFw", &Guid,
    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
    sizeof(Var), &Var);
  DEBUG((DEBUG_ERROR, "PS5 WiFi FW export: %u bytes at 0x%Lx crc=%08x var=%r\n", Size, Phys, Crc, VarSt));

  if (Selected != NULL) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
    if (!EFI_ERROR(gBS->HandleProtocol(Selected, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) &&
        !EFI_ERROR(Fs->OpenVolume(Fs, &Root))) {
      if (!EFI_ERROR(Root->Open(Root, &File, L"\\PS5WIFIFW.BIN", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
        File->Delete(File); File = NULL;
      }
      if (!EFI_ERROR(Root->Open(Root, &File, L"\\PS5WIFIFW.BIN", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
        UINTN Bytes = Size;
        (VOID)File->Write(File, &Bytes, (VOID *)(UINTN)Phys);
        (VOID)File->Flush(File);
        File->Close(File);
      }
      Root->Close(Root);
    }
  }
  return VarSt;
}
#endif
