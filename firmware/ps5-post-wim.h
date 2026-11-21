/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded post-WIM boot-service entry snapshots. Never persist after returning
 * a memory-map key: filesystem I/O can allocate and invalidate that key.
 */
STATIC EFI_GUID mWimEofGuid={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x53}};
STATIC EFI_GET_MEMORY_MAP mOriginalMemoryMap;
STATIC EFI_ALLOCATE_PAGES mOriginalAllocatePages;
STATIC EFI_FREE_PAGES mOriginalFreePages;
STATIC EFI_EVENT mWimEofEvent,mPostWimExitEvent;
STATIC BOOLEAN mWimEof,mPostWimBusy,mPostWimActive;
STATIC BOOLEAN mPostWimMapDumped;
STATIC UINTN mPostWimSpecial;
STATIC UINTN mPostWimRequests,mPostWimAllocations,mPostWimMaps,mPostWimMapCalls;
STATIC EFI_PHYSICAL_ADDRESS mKernelCandidate;
STATIC UINTN mKernelChecks;
STATIC BOOLEAN mKernelVerified;
STATIC UINT32 KernelCrc32(CONST UINT8 *Data,UINTN Size) {
  UINT32 Crc=0xffffffffU;
  for(UINTN I=0;I<Size;I++) {
    Crc^=Data[I];
    for(UINTN Bit=0;Bit<8;Bit++)Crc=(Crc>>1)^(0xedb88320U&(0U-(Crc&1U)));
  }
  return ~Crc;
}
STATIC VOID KernelCandidateProbe(VOID) {
  if(!mPostWimActive || mPostWimBusy || mKernelCandidate==0 || mKernelVerified || mKernelChecks>=4) return;
  EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Previous);
  if(Previous>TPL_CALLBACK) return;
  CONST UINT8 *Data=(CONST UINT8 *)(UINTN)mKernelCandidate;
  // Read only a live allocation returned by AllocatePages, never a map scan.
  if(Data[0]!='M' || Data[1]!='Z' || KernelCrc32(Data,1024)!=0x8afe97b4U) return;
  mKernelChecks++;
  UINT32 Crc=KernelCrc32(Data,13104632);
  mPostWimBusy=TRUE;
  DEBUG((DEBUG_INFO,"PS5 kernel file candidate: base=%Lx bytes=13104632 header_crc=8afe97b4 file_crc=%x expected=74921986 match=%u\n",
    mKernelCandidate,Crc,(UINT32)(Crc==0x74921986U)));
  if(Crc==0x74921986U) {
    mKernelVerified=TRUE;
    // The early winload initialization used the first budgets before kernel
    // contents arrived. Open exactly one later window after verified contents.
    mPostWimAllocations=0;mPostWimMaps=0;mPostWimMapCalls=0;
    DEBUG((DEBUG_INFO,"PS5 kernel file verified: post-kernel service window opened\n"));
  }
  ConsoleSnapshot();mPostWimBusy=FALSE;
}
STATIC VOID EFIAPI WimEof(EFI_EVENT Event,VOID *Context) { mWimEof=TRUE; }
STATIC VOID EFIAPI PostWimExit(EFI_EVENT Event,VOID *Context) { mPostWimActive=FALSE;mKernelCandidate=0; }
STATIC BOOLEAN PostWimEntry(BOOLEAN Map,CONST CHAR8 *Service,VOID *Caller,UINT64 A,UINT64 B,UINT64 C) {
  if(!mPostWimActive || !mWimEof || mPostWimBusy ||
     (Map?mPostWimMaps>=6:mPostWimAllocations>=6)) return FALSE;
  EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Previous);
  if(Previous>TPL_CALLBACK) return FALSE;
  mPostWimBusy=TRUE;mPostWimRequests++;
  if(Map)mPostWimMaps++;else mPostWimAllocations++;
  DEBUG((DEBUG_INFO,"PS5 post-WIM service %u %a caller=%p a=%Lx b=%Lx c=%Lx tsc=%Lu\n",
    (UINT32)mPostWimRequests,Service,Caller,A,B,C,AsmReadTsc()));
  ConsoleSnapshot();mPostWimBusy=FALSE;return TRUE;
}
STATIC BOOLEAN PostWimDumpMap(UINTN Size,CONST EFI_MEMORY_DESCRIPTOR *Map,UINTN Stride) {
  if(Map==NULL || Stride<sizeof(EFI_MEMORY_DESCRIPTOR) || Stride>256 ||
     Size==0 || Size%Stride!=0 || Size/Stride>128) return FALSE;
  DEBUG((DEBUG_INFO,"PS5 winload memory descriptors: count=%Lu stride=%Lu\n",(UINT64)(Size/Stride),(UINT64)Stride));
  for(UINTN Offset=0;Offset<Size;Offset+=Stride) {
    CONST EFI_MEMORY_DESCRIPTOR *D=(CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map+Offset);
    DEBUG((DEBUG_INFO,"PS5 memdesc: type=%u physical=%Lx virtual=%Lx pages=%Lx attributes=%Lx\n",
      D->Type,D->PhysicalStart,D->VirtualStart,D->NumberOfPages,D->Attribute));
  }
  return TRUE;
}
STATIC EFI_STATUS EFIAPI PostWimMemoryMap(UINTN *Size,EFI_MEMORY_DESCRIPTOR *Map,
  UINTN *Key,UINTN *DescriptorSize,UINT32 *Version) {
  KernelCandidateProbe(); // Before the map call; cannot invalidate its returned key.
  BOOLEAN Selected=FALSE;
  if(mPostWimActive && mWimEof && !mPostWimBusy) {
    // First two query/fill pairs plus later samples; do not let the first
    // repeated initialization consume every view of subsequent loader work.
    EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Previous);
    if(Previous<=TPL_CALLBACK) {
      mPostWimMapCalls++;
      Selected=mPostWimMapCalls<=4 || mPostWimMapCalls==8 || mPostWimMapCalls==16;
    }
  }
  BOOLEAN Traced=Selected && PostWimEntry(TRUE,"GetMemoryMap",__builtin_return_address(0),Size!=NULL?*Size:0,(UINTN)Map,mPostWimMapCalls);
  EFI_STATUS Status=mOriginalMemoryMap(Size,Map,Key,DescriptorSize,Version);
  if(Traced) {
    // Ring-only diagnostics: absolutely no event/file export or allocation
    // after obtaining the key. The next entry snapshot persists this result.
    DEBUG((DEBUG_INFO,"PS5 post-WIM GetMemoryMap result: status=%r bytes=%Lu key=%Lx descriptor=%Lu version=%u\n",
      Status,Size!=NULL?(UINT64)*Size:0,!EFI_ERROR(Status) && Key!=NULL?(UINT64)*Key:0,
      !EFI_ERROR(Status) && DescriptorSize!=NULL?(UINT64)*DescriptorSize:0,
      !EFI_ERROR(Status) && Version!=NULL?*Version:0));
    if(!EFI_ERROR(Status) && !mPostWimMapDumped && Size!=NULL && DescriptorSize!=NULL)
      mPostWimMapDumped=PostWimDumpMap(*Size,Map,*DescriptorSize);
  }
  return Status;
}
STATIC EFI_STATUS EFIAPI PostWimAllocatePages(EFI_ALLOCATE_TYPE Type,EFI_MEMORY_TYPE MemoryType,
  UINTN Pages,EFI_PHYSICAL_ADDRESS *Address) {
  KernelCandidateProbe();
  BOOLEAN Traced=PostWimEntry(FALSE,"AllocatePages",__builtin_return_address(0),Type,Pages,Type!=AllocateAnyPages && Address!=NULL?*Address:0);
  EFI_STATUS Status=mOriginalAllocatePages(Type,MemoryType,Pages,Address);
  if(!EFI_ERROR(Status) && mPostWimActive && mWimEof && !mPostWimBusy && Pages==3200 &&
     Address!=NULL && mKernelCandidate==0 && !mKernelVerified) mKernelCandidate=*Address;
  if(Traced) {
    // Carried into the next normal snapshot, never export after GetMemoryMap.
    DEBUG((DEBUG_INFO,"PS5 post-WIM AllocatePages result: status=%r type=%u pages=%Lu address=%Lx\n",
      Status,(UINT32)MemoryType,(UINT64)Pages,!EFI_ERROR(Status) && Address!=NULL?*Address:0));
    if(EFI_ERROR(Status)) {mPostWimBusy=TRUE;ConsoleSnapshot();mPostWimBusy=FALSE;}
  }
  // Exact sizes come from the current pinned ntoskrnl.exe PE reference.
  // A matching allocation is only a candidate, not proof of loaded contents.
  if(!Traced && mPostWimActive && mWimEof && !mPostWimBusy && mPostWimSpecial<4 &&
     (EFI_ERROR(Status) || Pages==3200 || Pages==5200)) {
    EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Previous);
    if(Previous<=TPL_CALLBACK) {
      mPostWimBusy=TRUE;mPostWimSpecial++;
      DEBUG((DEBUG_INFO,"PS5 later allocation: caller=%p status=%r type=%u pages=%Lu address=%Lx kernel_size_match=%u\n",
        __builtin_return_address(0),Status,(UINT32)MemoryType,(UINT64)Pages,
        !EFI_ERROR(Status) && Address!=NULL?*Address:0,(UINT32)(Pages==3200 || Pages==5200)));
      ConsoleSnapshot();mPostWimBusy=FALSE;
    }
  }
  return Status;
}
STATIC EFI_STATUS EFIAPI PostWimFreePages(EFI_PHYSICAL_ADDRESS Address,UINTN Pages) {
  if(mKernelCandidate!=0) {
    UINT64 CandidateEnd=mKernelCandidate+3200ULL*4096;
    BOOLEAN Overflow=Pages>MAX_UINT64/4096 || Address>MAX_UINT64-(UINT64)Pages*4096;
    if(Overflow || (Address<CandidateEnd && mKernelCandidate<Address+(UINT64)Pages*4096)) {
      KernelCandidateProbe(); // Still live: original FreePages has not been called.
      if(mPostWimActive && mKernelVerified && !mPostWimBusy) {
        EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Previous);
        if(Previous<=TPL_CALLBACK) {
          mPostWimBusy=TRUE;
          DEBUG((DEBUG_INFO,"PS5 kernel file buffer release: caller=%p address=%Lx pages=%Lu\n",
            __builtin_return_address(0),Address,(UINT64)Pages));
          ConsoleSnapshot();mPostWimBusy=FALSE;
        }
      }
      // Invalidate before the original free, even if it later reports an error.
      mKernelCandidate=0;
    }
  }
  return mOriginalFreePages(Address,Pages);
}
STATIC VOID PostWimTableCrc(VOID) {
  gBS->Hdr.CRC32=0;
  gBS->CalculateCrc32(gBS,gBS->Hdr.HeaderSize,&gBS->Hdr.CRC32);
}
STATIC BOOLEAN InstallPostWimTap(VOID) {
  if(gBS->GetMemoryMap==NULL || gBS->AllocatePages==NULL || gBS->FreePages==NULL || gBS->CreateEventEx==NULL ||
     gBS->CloseEvent==NULL || gBS->CalculateCrc32==NULL || mPostWimActive) return FALSE;
  mKernelCandidate=0;mKernelChecks=0;mKernelVerified=FALSE;mWimEof=FALSE;mPostWimBusy=FALSE;mPostWimMapDumped=FALSE;mPostWimSpecial=0;mPostWimRequests=0;mPostWimAllocations=0;mPostWimMaps=0;mPostWimMapCalls=0;
  EFI_STATUS Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,WimEof,NULL,&mWimEofGuid,&mWimEofEvent);
  if(EFI_ERROR(Status)) return FALSE;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,PostWimExit,NULL,&gEfiEventExitBootServicesGuid,&mPostWimExitEvent);
  if(EFI_ERROR(Status)) {gBS->CloseEvent(mWimEofEvent);return FALSE;}
  mOriginalMemoryMap=gBS->GetMemoryMap;mOriginalAllocatePages=gBS->AllocatePages;mOriginalFreePages=gBS->FreePages;
  gBS->GetMemoryMap=PostWimMemoryMap;gBS->AllocatePages=PostWimAllocatePages;gBS->FreePages=PostWimFreePages;
  PostWimTableCrc();mPostWimActive=TRUE;return TRUE;
}
STATIC VOID RestorePostWimTap(BOOLEAN Installed) {
  if(!Installed) return;
  mPostWimActive=FALSE;
  if(gBS->GetMemoryMap==PostWimMemoryMap) gBS->GetMemoryMap=mOriginalMemoryMap;
  if(gBS->AllocatePages==PostWimAllocatePages) gBS->AllocatePages=mOriginalAllocatePages;
  if(gBS->FreePages==PostWimFreePages)gBS->FreePages=mOriginalFreePages;
  mKernelCandidate=0;
  PostWimTableCrc();gBS->CloseEvent(mWimEofEvent);gBS->CloseEvent(mPostWimExitEvent);
}
