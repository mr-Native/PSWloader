/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_VBIOS_EXPORT_H
#define PS5_VBIOS_EXPORT_H
/* PS5 Linux loader supplies the platform VBIOS at physical 0xc0000.
 * Copy only; do not execute ROM code or change GPU registers. Destination
 * is the already pinned external ESP, never internal Sony storage.
 * r5.19: export exactly the ROM image (byte 2 x 512 bytes, 55296 on the PS5), not 64 KiB: the bytes behind the
 * image are unrelated memory and change from boot to boot. The shadow copy always carries 0xFF in the checksum byte
 * (last image byte, sum 196 instead of 0, seen since the first export); the KMD refuses such an image (ATOM status
 * CHECKSUM). The checksum byte is recomputed so the 8-bit sum over the image is 0 - the same single-byte fix as the
 * known-good ps5-vbios-cksum.bin. Any older, longer \PS5VBIOS.BIN is deleted first (Open/CREATE does not truncate). */
STATIC EFI_STATUS Ps5ExportVbios(EFI_HANDLE Selected) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  EFI_STATUS Status=gBS->HandleProtocol(Selected,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if(EFI_ERROR(Status))return Status;
  volatile UINT8 *Rom=(VOID *)(UINTN)0xc0000;
  if(Rom[0]!=0x55 || Rom[1]!=0xaa)return EFI_UNSUPPORTED;
  UINTN Length=(UINTN)Rom[2]*512;
  if(Length<0x200 || Length>65536)return EFI_COMPROMISED_DATA;
  UINT8 *Copy=AllocatePool(Length);
  if(Copy==NULL)return EFI_OUT_OF_RESOURCES;
  for(UINTN N=0;N<Length;N++)Copy[N]=Rom[N];
  UINTN Header=(UINTN)Copy[0x48]|((UINTN)Copy[0x49]<<8);
  if(Header>Length-36 || CompareMem(Copy+Header+4,"ATOM",4)!=0){FreePool(Copy);return EFI_COMPROMISED_DATA;}
  UINT8 Sum=0;
  for(UINTN N=0;N+1<Length;N++)Sum=(UINT8)(Sum+Copy[N]);
  DEBUG((DEBUG_ERROR,"PS5 VBIOS export: image %u bytes, checksum byte %02x -> %02x\n",(UINT32)Length,Copy[Length-1],(UINT8)(0-Sum)));
  Copy[Length-1]=(UINT8)(0-Sum);
  Status=Fs->OpenVolume(Fs,&Root);
  if(!EFI_ERROR(Status) && !EFI_ERROR(Root->Open(Root,&File,L"\\PS5VBIOS.BIN",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0))){
    File->Delete(File);File=NULL;   /* Delete also closes the handle */
  }
  if(!EFI_ERROR(Status))Status=Root->Open(Root,&File,L"\\PS5VBIOS.BIN",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(!EFI_ERROR(Status)) {
    UINTN Bytes=Length;
    Status=File->Write(File,&Bytes,Copy);
    if(!EFI_ERROR(Status) && Bytes!=Length)Status=EFI_DEVICE_ERROR;
    if(!EFI_ERROR(Status))Status=File->Flush(File);
  }
  if(File)File->Close(File);
  if(Root)Root->Close(Root);
  FreePool(Copy);return Status;
}
#endif
