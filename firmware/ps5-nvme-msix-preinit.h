/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-nvme-msix-preinit.h v1.3 (13.11.2026) - internal-M.2 NVMe under Windows, way (a)/(a2).
 *
 * The SanDisk M.2 (15B7:5063) sits behind Sony's ARI proxy function 0x40:02.5 (ARI 0x15). Before ExitBootServices
 * every config access of this firmware goes through Ps5PciExpressLib -> ps5_pci_access (ARI forwarding via RID@0x150
 * and the SFC window of function 0), i.e. to the REAL device. After ExitBootServices pci.sys reads raw ECAM, i.e. the
 * SHADOW. The s0 hive shows that Windows sees the real capabilities through the shadow (InterruptSupport 0x6 = MSI +
 * MSI-X, 32 messages, boot BAR 0x88000000/16K), but stornvme reports controller errors: the hypothesis is that pci.sys'
 * writes (MSI-X enable, ...) land in the shadow only, so the device never interrupts.
 *
 * PS5_NVME_PREINIT        log only: at registration (before ExitBootServices) dump both views of the header and of
 *                         the MSI/MSI-X capabilities into the UEFI log - the "baseline of both views" of the decision
 *                         tree, read-only (raw ECAM reads are what pci.sys does anyway).
 * PS5_NVME_PREINIT_APPLY  additionally, in an ExitBootServices notification at TPL_CALLBACK (registered last, so it
 *                         runs after the NOTIFY-level handlers): mask every MSI-X vector in the table (MMIO, BAR of the
 *                         real device), set MSI-X Enable with the function mask clear and INTx Disable in the REAL
 *                         config. Memory decode and bus master are NOT changed (no early bus-master enable): if the
 *                         firmware left them off, nothing is written and Windows behaves as before. Windows then
 *                         programs the table entries over MMIO and unmasks them; only the enable bit was missing.
 * PS5_NVME_PREINIT_PROBE  (a2, after the nvlog result of 13.11. 19:21): the shadow has its OWN capability list and its
 *                         MSI-X is a stub (table size 1, table at BIR0+0) while the real MSI-X has 65 vectors at
 *                         BIR0+0x3000, so Windows programs its single entry into the NVMe registers at BAR0+0. Before
 *                         ExitBootServices this variant tries to write the real MSI-X geometry (table size, table
 *                         offset/BIR, PBA offset/BIR) into the shadow capability via raw ECAM and reads it back. ONLY
 *                         if every field reads back as written, the ExitBootServices step of APPLY runs (mask the real
 *                         vectors, real MSI-X enable, INTx disable). Per the PCI spec these fields are read-only; the
 *                         readback shows whether Sony's emulation accepts them. A cold boot restores the shadow.
 * v1.2 (review): the MSI-X block must lie completely below 0x100; table and PBA must use BIR 0 (so a BIR
 * can never name the upper half of a 64-bit BAR) and must fit into the 16 KB BAR0 (size from Windows' requirement list
 * BasicConfigVector, 0x4000); every check fails closed. "ACCEPTED" only means the EBS step is armed - whether it ran
 * is not observable after ExitBootServices; the Windows result decides.
 * v1.3: write order table/PBA location first, size last and only if the location took; anything partial
 * is restored and read back; if the original geometry cannot be verified the Windows start is BLOCKED (dead loop,
 * the harness powers the console off, which resets the shadow) - a half-written descriptor must never reach pci.sys.
 * Note: that Windows writes its single vector to BAR0+0 is an inference from the shadow descriptor, not observed.
 * The EBS step refuses a real MSI-X that is already enabled/masked (someone owns it) and verifies its own config
 * writes by readback. 65 table entries are the table SIZE; how many messages Windows actually allocates is separate.
 * Writes touch only this one function and only when its identity is 15B7:5063; the USB boot path is not involved.
 * Undo: build without the flag (the device is reset by the next power cycle).
 */
#include <Library/PciExpressLib.h>
#include <Library/IoLib.h>
#include <IndustryStandard/Pci.h>

#define PS5_NVME_BUS 0x40
#define PS5_NVME_DEV 2
#define PS5_NVME_FN  5
#define PS5_NVME_ID  0x506315B7U
#define PS5_NVME_REAL(Reg) PCI_EXPRESS_LIB_ADDRESS (PS5_NVME_BUS, PS5_NVME_DEV, PS5_NVME_FN, (Reg))
#define PS5_NVME_ECAM 0xF0000000ULL
#define PS5_NVME_SHADOW32(Reg) MmioRead32 ((UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (Reg)))
#define PS5_NVME_BAR0_SIZE 0x4000U   /* 16 KB: LogConf BasicConfigVector of 15B7:5063 (s0 hive) */

/* MSI-X geometry check (v1.2): block below 0x100, BIR 0 for table and PBA, both inside BAR0 */
STATIC BOOLEAN Ps5NvmeMsixGeometryOk (UINT8 Cap, UINT16 Ctl, UINT32 Tbl, UINT32 Pba)
{
  UINT32 Entries = (UINT32)(Ctl & 0x7FF) + 1;
  if (Cap < 0x40 || Cap > 0xF4) return FALSE;                                  /* cap..cap+11 inside 0x100 */
  if ((Tbl & 7) != 0 || (Pba & 7) != 0) return FALSE;                          /* BIR 0 only */
  if ((UINT64)(Tbl & ~7U) + (UINT64)Entries * 16 > PS5_NVME_BAR0_SIZE) return FALSE;
  if ((UINT64)(Pba & ~7U) + (UINT64)((Entries + 63) / 64) * 8 > PS5_NVME_BAR0_SIZE) return FALSE;
  return TRUE;
}

/* bounded capability walk in the real config; returns the offset or 0 */
STATIC UINT8 Ps5NvmeFindCap (UINT8 Id, BOOLEAN Shadow)
{
  UINT8 Cap = (UINT8)((Shadow ? PS5_NVME_SHADOW32 (0x34) : PciExpressRead32 (PS5_NVME_REAL (0x34))) & 0xFC);
  for (UINTN Count = 0; Count < 48 && Cap >= 0x40; Count++) {
    UINT32 Head = Shadow ? PS5_NVME_SHADOW32 (Cap) : PciExpressRead32 (PS5_NVME_REAL (Cap));
    if ((Head & 0xFF) == Id) return Cap;
    Cap = (UINT8)((Head >> 8) & 0xFC);
  }
  return 0;
}

STATIC VOID Ps5NvmeLogViews (VOID)
{
  for (UINTN Shadow = 0; Shadow < 2; Shadow++) {
    UINT32 H[16];
    for (UINTN I = 0; I < 16; I++) H[I] = Shadow ? PS5_NVME_SHADOW32 (I * 4) : PciExpressRead32 (PS5_NVME_REAL (I * 4));
    DEBUG ((DEBUG_ERROR, "PS5 NVMe %a: id %08x cmdsts %08x class %08x bar0 %08x bar1 %08x bar2 %08x ssid %08x cap %02x\n",
      Shadow ? "shadow" : "real", H[0], H[1], H[2], H[4], H[5], H[6], H[11], H[13] & 0xFF));
    UINT8 X = Ps5NvmeFindCap (0x11, Shadow != 0), M = Ps5NvmeFindCap (0x05, Shadow != 0), E = Ps5NvmeFindCap (0x10, Shadow != 0);
    UINT32 Xc = X ? (Shadow ? PS5_NVME_SHADOW32 (X) : PciExpressRead32 (PS5_NVME_REAL (X))) : 0;
    UINT32 Xt = X ? (Shadow ? PS5_NVME_SHADOW32 (X + 4) : PciExpressRead32 (PS5_NVME_REAL (X + 4))) : 0;
    UINT32 Mc = M ? (Shadow ? PS5_NVME_SHADOW32 (M) : PciExpressRead32 (PS5_NVME_REAL (M))) : 0;
    UINT32 Ari = Shadow ? PS5_NVME_SHADOW32 (0x150) : 0;
    DEBUG ((DEBUG_ERROR, "PS5 NVMe %a: msix@%02x ctl %08x table %08x | msi@%02x ctl %08x | pcie@%02x | rid@150 %08x\n",
      Shadow ? "shadow" : "real", X, Xc, Xt, M, Mc, E, Ari));
  }
}

#if defined (PS5_NVME_PREINIT_PROBE) && !defined (PS5_NVME_PREINIT_APPLY)
#define PS5_NVME_PREINIT_APPLY 1
#endif
#ifdef PS5_NVME_PREINIT_PROBE
/* (a2): copy the real MSI-X geometry into the shadow capability (raw ECAM, i.e. NOT through the ARI forwarding of
 * Ps5PciExpressLib, which would hit the real device). TRUE only if every field reads back exactly. */
#define PS5_NVME_FIX_UNCHANGED 0   /* nothing accepted, original geometry verified */
#define PS5_NVME_FIX_FIXED     1   /* real geometry accepted completely */
#define PS5_NVME_FIX_CORRUPT   2   /* original geometry could NOT be verified - Windows must not start */
STATIC UINTN Ps5NvmeFixShadowMsix (VOID)
{
  if (PciExpressRead32 (PS5_NVME_REAL (0)) != PS5_NVME_ID || PS5_NVME_SHADOW32 (0) != PS5_NVME_ID) return PS5_NVME_FIX_UNCHANGED;
  UINT8 R = Ps5NvmeFindCap (0x11, FALSE), S = Ps5NvmeFindCap (0x11, TRUE);
  if (R == 0 || S == 0) { DEBUG ((DEBUG_ERROR, "PS5 NVMe shadow fix: msix real@%02x shadow@%02x - skipped\n", R, S)); return PS5_NVME_FIX_UNCHANGED; }
  UINT16 RCtl = (UINT16)(PciExpressRead32 (PS5_NVME_REAL (R)) >> 16);
  UINT32 RTbl = PciExpressRead32 (PS5_NVME_REAL (R + 4)), RPba = PciExpressRead32 (PS5_NVME_REAL (R + 8));
  UINTN CtlA = (UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (S + 2));
  UINTN TblA = (UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (S + 4));
  UINTN PbaA = (UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (S + 8));
  UINT16 SCtl = MmioRead16 (CtlA);
  UINT32 STbl = MmioRead32 (TblA), SPba = MmioRead32 (PbaA);
  DEBUG ((DEBUG_ERROR, "PS5 NVMe shadow fix: before ctl %04x tbl %08x pba %08x, real ctl %04x tbl %08x pba %08x\n",
    SCtl, STbl, SPba, RCtl, RTbl, RPba));
  if ((SCtl & (BIT15 | BIT14)) != 0) return PS5_NVME_FIX_UNCHANGED;          /* never touch an enabled/masked shadow */
  if (S < 0x40 || S > 0xF4 || !Ps5NvmeMsixGeometryOk (R, RCtl, RTbl, RPba)) {
    DEBUG ((DEBUG_ERROR, "PS5 NVMe shadow fix: geometry check failed (real@%02x shadow@%02x) - skipped\n", R, S));
    return PS5_NVME_FIX_UNCHANGED;
  }
  /* v1.3 order: the table/PBA location first, the table SIZE last and only if the location took. A shadow
   * with a larger size but the old offset 0 would make Windows write vectors over the NVMe registers at BAR0+0. */
  MmioWrite32 (TblA, RTbl);
  MmioWrite32 (PbaA, RPba);
  UINT32 NTbl = MmioRead32 (TblA), NPba = MmioRead32 (PbaA);
  UINT16 NCtl = MmioRead16 (CtlA);
  if (NTbl == RTbl && NPba == RPba && NCtl == SCtl) {
    MmioWrite16 (CtlA, (UINT16)((SCtl & ~0x7FF) | (RCtl & 0x7FF)));         /* size only; enable/mask stay 0 */
    NCtl = MmioRead16 (CtlA);
    NTbl = MmioRead32 (TblA); NPba = MmioRead32 (PbaA);
    if ((NCtl & 0x7FF) == (RCtl & 0x7FF) && (NCtl & (BIT15 | BIT14)) == 0 && NTbl == RTbl && NPba == RPba) {
      DEBUG ((DEBUG_ERROR, "PS5 NVMe shadow fix: after ctl %04x tbl %08x pba %08x -> ACCEPTED (EBS step armed)\n", NCtl, NTbl, NPba));
      return PS5_NVME_FIX_FIXED;
    }
  }
  /* anything partial: restore all three fields (size first, so the size never outlives a reverted offset) */
  MmioWrite16 (CtlA, SCtl);
  MmioWrite32 (TblA, STbl);
  MmioWrite32 (PbaA, SPba);
  UINT16 BCtl = MmioRead16 (CtlA);
  UINT32 BTbl = MmioRead32 (TblA), BPba = MmioRead32 (PbaA);
  BOOLEAN Restored = BCtl == SCtl && BTbl == STbl && BPba == SPba;
  DEBUG ((DEBUG_ERROR, "PS5 NVMe shadow fix: after ctl %04x tbl %08x pba %08x -> %a\n", BCtl, BTbl, BPba,
    Restored ? "READ-ONLY (original verified, no EBS step)" : "CORRUPT (original NOT verified, Windows start blocked)"));
  return Restored ? PS5_NVME_FIX_UNCHANGED : PS5_NVME_FIX_CORRUPT;
}
STATIC BOOLEAN mPs5NvmeShadowFixed;
#endif
#ifdef PS5_NVME_PREINIT_APPLY
STATIC volatile UINT32 mPs5NvmePreinitResult;
STATIC EFI_EVENT mPs5NvmePreinitEvent;

STATIC UINT32 Ps5NvmePreinitCore (VOID)
{
  if (PciExpressRead32 (PS5_NVME_REAL (0)) != PS5_NVME_ID) return 1;
  UINT16 Cmd = PciExpressRead16 (PS5_NVME_REAL (PCI_COMMAND_OFFSET));
  if ((Cmd & (EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER)) !=
      (EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER)) return 2;      /* never enable decode/DMA here */
  UINT8 Cap = Ps5NvmeFindCap (0x11, FALSE);
  if (Cap == 0) return 3;
  UINT16 Ctl = PciExpressRead16 (PS5_NVME_REAL (Cap + 2));
  if (Ctl == 0xFFFF || (Ctl & (BIT15 | BIT14)) != 0) return 8;                 /* unreadable or already owned: hands off */
  UINT32 Tbl = PciExpressRead32 (PS5_NVME_REAL (Cap + 4));
  UINTN Bir = Tbl & 7, Entries = (UINTN)(Ctl & 0x7FF) + 1;
  if (!Ps5NvmeMsixGeometryOk (Cap, Ctl, Tbl, PciExpressRead32 (PS5_NVME_REAL (Cap + 8)))) return 4;
  UINT32 Lo = PciExpressRead32 (PS5_NVME_REAL (0x10 + Bir * 4));
  if ((Lo & 1) != 0) return 5;
  UINT64 Base = Lo & ~(UINT64)0xF;
  if (((Lo >> 1) & 3) == 2) {
    if (Bir == 5) return 6;
    Base |= LShiftU64 (PciExpressRead32 (PS5_NVME_REAL (0x14 + Bir * 4)), 32);
  }
  if (Base == 0) return 7;
  UINT64 Table = Base + (Tbl & ~(UINT32)7);
  for (UINTN I = 0; I < Entries; I++) MmioOr32 ((UINTN)(Table + I * 16 + 12), BIT0);   /* vector control: masked */
  PciExpressWrite16 (PS5_NVME_REAL (PCI_COMMAND_OFFSET), (UINT16)(Cmd | BIT10));          /* INTx disable only */
  PciExpressWrite16 (PS5_NVME_REAL (Cap + 2), (UINT16)((Ctl & ~BIT14) | BIT15));          /* MSI-X on, unmasked fn */
  /* v1.3: verify by readback - a refused write (lock timeout returns all ones) is not a success */
  UINT16 NCmd = PciExpressRead16 (PS5_NVME_REAL (PCI_COMMAND_OFFSET));
  UINT16 NCtl = PciExpressRead16 (PS5_NVME_REAL (Cap + 2));
  if (NCmd == 0xFFFF || NCtl == 0xFFFF || (NCmd & BIT10) == 0 || (NCtl & (BIT15 | BIT14)) != BIT15) return 9;
  return 0x100 | (UINT32)Entries;
}

STATIC VOID EFIAPI Ps5NvmePreinitAtEbs (IN EFI_EVENT Event, IN VOID *Context)
{
#ifdef PS5_NVME_PREINIT_PROBE
  if (!mPs5NvmeShadowFixed) return;
#endif
  mPs5NvmePreinitResult = Ps5NvmePreinitCore ();
}
#endif

/* r5.25.11 (HW 23.11. lab123: Windows after a restart ended in INACCESSIBLE_BOOT_DEVICE): back to the GameOS hand-over
 * state of both views before the OS starts. A restart leaves the REAL function with Windows' MSI-X enabled and INTx
 * disabled (ps5nvmsi armed it, no STOP/REMOVE at a restart), and the warm trampoline cleared bus mastering in the SHADOW
 * (only the real function is turned on again by the firmware). ps5nvmsi needs the hand-over state: shadow decode + bus
 * master on, MSI-X off in the real config. On a cold boot every value already matches and nothing is written. The
 * firmware's NVMe driver polls, so the real MSI-X going off does not affect it. */
STATIC VOID Ps5NvmeWarmHandover (VOID)
{
  if (PciExpressRead32 (PS5_NVME_REAL (0)) != PS5_NVME_ID || PS5_NVME_SHADOW32 (0) != PS5_NVME_ID) return;
  UINTN Fixed = 0;
  UINT16 SCmd = MmioRead16 ((UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (PCI_COMMAND_OFFSET)));
  if ((SCmd & EFI_PCI_COMMAND_MEMORY_SPACE) && !(SCmd & EFI_PCI_COMMAND_BUS_MASTER)) {
    MmioWrite16 ((UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (PCI_COMMAND_OFFSET)), (UINT16)(SCmd | EFI_PCI_COMMAND_BUS_MASTER)); Fixed |= 1;
  }
  UINT8 S = Ps5NvmeFindCap (0x11, TRUE);
  if (S) {
    UINT16 SCtl = MmioRead16 ((UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (S + 2)));
    if (SCtl != 0xFFFF && (SCtl & BIT14)) { MmioWrite16 ((UINTN)(PS5_NVME_ECAM + PS5_NVME_REAL (S + 2)), (UINT16)(SCtl & ~BIT14)); Fixed |= 2; }
  }
  UINT8 R = Ps5NvmeFindCap (0x11, FALSE);
  if (R) {
    UINT16 RCtl = PciExpressRead16 (PS5_NVME_REAL (R + 2));
    if (RCtl != 0xFFFF && (RCtl & (BIT15 | BIT14))) { PciExpressWrite16 (PS5_NVME_REAL (R + 2), (UINT16)(RCtl & ~(BIT15 | BIT14))); Fixed |= 4; }
  }
  UINT16 RCmd = PciExpressRead16 (PS5_NVME_REAL (PCI_COMMAND_OFFSET));
  if (RCmd != 0xFFFF && (RCmd & BIT10)) { PciExpressWrite16 (PS5_NVME_REAL (PCI_COMMAND_OFFSET), (UINT16)(RCmd & ~BIT10)); Fixed |= 8; }
  if (Fixed) {
    DEBUG ((DEBUG_ERROR, "PS5 NVMe warm hand-over: restored (mask %x: 1 shadow bus master, 2 shadow MSI-X fn mask, 4 real MSI-X off, 8 real INTx)\n", (UINT32)Fixed));
    Ps5NvmeLogViews ();
  }
}

STATIC VOID Ps5RegisterNvmePreinit (VOID)
{
  Ps5NvmeLogViews ();
  Ps5NvmeWarmHandover ();
#ifdef PS5_NVME_PREINIT_PROBE
  UINTN Fix = Ps5NvmeFixShadowMsix ();
  if (Fix == PS5_NVME_FIX_CORRUPT) {
    DEBUG ((DEBUG_ERROR, "PS5 NVMe probe: shadow geometry unverified - Windows start BLOCKED, power the console off\n"));
    CpuDeadLoop ();
  }
  mPs5NvmeShadowFixed = (BOOLEAN)(Fix == PS5_NVME_FIX_FIXED);
  if (!mPs5NvmeShadowFixed) { DEBUG ((DEBUG_ERROR, "PS5 NVMe probe: shadow not fixed, EBS step disabled\n")); return; }
#endif
#ifdef PS5_NVME_PREINIT_APPLY
  EFI_STATUS Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Ps5NvmePreinitAtEbs, NULL,
                                          &gEfiEventExitBootServicesGuid, &mPs5NvmePreinitEvent);
  DEBUG ((DEBUG_ERROR, "PS5 NVMe MSI-X preinit armed: %r\n", Status));
#else
  DEBUG ((DEBUG_ERROR, "PS5 NVMe preinit: log only\n"));
#endif
}
