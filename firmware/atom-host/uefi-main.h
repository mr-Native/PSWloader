/* SPDX-License-Identifier: BSD-2-Clause-Patent */
static EFI_PCI_IO_PROTOCOL *mAtomPci;
static BOOLEAN mAtomBusy;
static UINTN mAtomWrites,mAtomReads;
static UINT32 mAtomLastReadReg,mAtomLastReadValue,mAtomLastWriteReg,mAtomLastWriteValue;
static uint32_t AtomMmioRead(struct card_info *Card,uint32_t Reg) {
 (VOID)Card;UINT32 Value=0;
 if(Reg>=0x20000)host_fail("BAR5 read range");
 EFI_STATUS Status=mAtomPci->Mem.Read(mAtomPci,EfiPciIoWidthUint32,5,(UINT64)Reg*4,1,&Value);
 if(EFI_ERROR(Status))host_fail("BAR5 read failed");
 mAtomLastReadReg=Reg;mAtomLastReadValue=Value;mAtomReads++;return Value;
}
static VOID AtomMmioWrite(struct card_info *Card,uint32_t Reg,uint32_t Value) {
 (VOID)Card;
 if(Reg>=0x20000)host_fail("BAR5 write range");
 EFI_STATUS Status=mAtomPci->Mem.Write(mAtomPci,EfiPciIoWidthUint32,5,(UINT64)Reg*4,1,&Value);
 if(EFI_ERROR(Status))host_fail("BAR5 write failed");
 mAtomLastWriteReg=Reg;mAtomLastWriteValue=Value;mAtomWrites++;
}
static uint32_t AtomUnsupportedRead(struct card_info *Card,uint32_t Reg) {
 (VOID)Card;(VOID)Reg;host_fail("unsupported PLL/MC read");return 0;
}
static VOID AtomUnsupportedWrite(struct card_info *Card,uint32_t Reg,uint32_t Value) {
 (VOID)Card;(VOID)Reg;(VOID)Value;host_fail("unsupported PLL/MC write");
}
EFI_STATUS Ps5AtomExecute(EFI_PCI_IO_PROTOCOL *Pci,const UINT8 *Rom,UINTN RomBytes,
                          UINTN Table,UINT32 *Params,UINTN ParamDwords) {
 if(!Pci || !Rom || RomBytes!=65536 || Table>80 || !Params || !ParamDwords || ParamDwords>64)return EFI_INVALID_PARAMETER;
 if(Rom[0]!=0x55 || Rom[1]!=0xaa)return EFI_COMPROMISED_DATA;
 if(mAtomBusy)return EFI_ALREADY_STARTED;
 /* Allocate before suppressing callbacks/interrupts. No boot-service pool
  * or Stall calls are made by the interpreter inside the critical section. */
 mAtomArena=AllocateZeroPool(ATOM_ARENA_BYTES);
 if(!mAtomArena)return EFI_OUT_OF_RESOURCES;
 mAtomArenaUsed=0;
 mAtomBusy=TRUE;mAtomDelayUsed=0;mAtomStarted=AsmReadTsc();mAtomPci=Pci;mAtomReads=mAtomWrites=host_steps=host_depth=0;mAtomStatus=EFI_ABORTED;
 mAtomLastReadReg=mAtomLastReadValue=mAtomLastWriteReg=mAtomLastWriteValue=0;
 EFI_TPL OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
 BOOLEAN Interrupts=SaveAndDisableInterrupts();
 if(SetJump(&mAtomJump)==0) {
  UINT16 Header=get_u16((VOID *)Rom,0x48);
  if(Header>65536-36 || CompareMem(Rom+Header+4,"ATOM",4))host_fail("ATOM signature");
  struct card_info Card={0};Card.reg_read=AtomMmioRead;Card.reg_write=AtomMmioWrite;
  Card.mc_read=Card.pll_read=AtomUnsupportedRead;Card.mc_write=Card.pll_write=AtomUnsupportedWrite;
  struct atom_context Context={0};Context.card=&Card;Context.bios=(VOID *)Rom;
  Context.cmd_table=get_u16((VOID *)Rom,Header+30);Context.data_table=get_u16((VOID *)Rom,Header+32);
  UINT16 DirectorySize=get_u16((VOID *)Rom,Context.cmd_table);
  if(DirectorySize<4+2*(Table+1) || Context.cmd_table+DirectorySize>65536)host_fail("command directory bounds");
  UINT16 Command=get_u16((VOID *)Rom,Context.cmd_table+4+2*Table);
  UINT16 CommandBytes=get_u16((VOID *)Rom,Command);
  if(Command<0x4a || CommandBytes<6 || (UINTN)Command+CommandBytes>65536)host_fail("command bounds");
  Context.scratch=AtomAllocate(512,sizeof(UINT32));Context.scratch_size_bytes=2048;
  atom_index_iio(&Context,get_u16((VOID *)Rom,Context.data_table+0x32)+4);
  int Result=amdgpu_atom_execute_table(&Context,(int)Table,Params,(int)ParamDwords);
  mAtomStatus=Result?EFI_ABORTED:EFI_SUCCESS;
 }
 for(UINTN I=0;I<32;I++)mAtomAllocations[I]=NULL;
 /* RestoreTPL may enable interrupts; restore the caller's IF afterward. */
 gBS->RestoreTPL(OldTpl);SetInterruptState(Interrupts);
 FreePool(mAtomArena);mAtomArena=NULL;mAtomArenaUsed=0;
 DEBUG((DEBUG_INFO,"PS5 ATOM: table=%u status=%r reads=%u writes=%u steps=%u\n",Table,mAtomStatus,mAtomReads,mAtomWrites,host_steps));
 if(EFI_ERROR(mAtomStatus))DEBUG((DEBUG_ERROR,"PS5 ATOM last MMIO: read reg=%x value=%x write reg=%x value=%x\n",mAtomLastReadReg,mAtomLastReadValue,mAtomLastWriteReg,mAtomLastWriteValue));
 mAtomPci=NULL;mAtomBusy=FALSE;return mAtomStatus;
}
