#!/usr/bin/env python3
"""v1.0 Build a pinned EDK II payload candidate; never install or boot it.

This first platform increment supplies PS5 TSC timing and the measured ECAM
window. ACPI replacement, loader HOBs, framebuffer and NDA handoff remain open.
The resulting binary must not be described as a bootable PS5 firmware yet.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from ps5_entry_fixups import prepare_x64_handoff, prepare_x64_page_tables, prepare_cpu_apic
from ps5_dxe_probe import prepare_dxe_power
from ps5_exception_power import prepare_exception_power
from ps5_inherited_timer import prepare_inherited_timer
from ps5_boot_reads import prepare_boot_reads
from ps5_boot_manager import prepare_windows_boot, prepare_boot_snapshots

REVISION = 'ec82c96152507d64055a8965fe261fb9aaca3279'
HERE = Path(__file__).resolve().parent


RTC_ORIGINAL = 'PcAtChipsetPkg/PcatRealTimeClockRuntimeDxe/PcatRealTimeClockRuntimeDxe.inf'
RTC_PS5 = 'Ps5PlatformPkg/Ps5RtcRuntimeDxe/Ps5RtcRuntimeDxe.inf'


PS5_STORAGE_HELPER = """
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
// PS5 storage-only boot menu (r5.25.4): TRUE = the option is no storage with a medium (network boot, empty card reader,
// a USB device without media) - not listed. Firmware-volume apps and short-form OS paths are kept.
STATIC BOOLEAN Ps5OptionNotStorage (EFI_DEVICE_PATH_PROTOCOL *Dp)
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_HANDLE                H = NULL;
  EFI_BLOCK_IO_PROTOCOL     *B = NULL;
  if ((Dp == NULL) || (DevicePathType (Dp) == MEDIA_DEVICE_PATH)) {
    return FALSE;
  }
  for (Node = Dp; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) && ((DevicePathSubType (Node) == MEDIA_PIWG_FW_FILE_DP) || (DevicePathSubType (Node) == MEDIA_PIWG_FW_VOL_DP))) {
      return FALSE;
    }
  }
  Node = Dp;
  if (!EFI_ERROR (gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Node, &H))) {
    return FALSE;
  }
  Node = Dp;
  if (EFI_ERROR (gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Node, &H)) ||
      EFI_ERROR (gBS->HandleProtocol (H, &gEfiBlockIoProtocolGuid, (VOID **)&B)) || (B == NULL) || (B->Media == NULL)) {
    return TRUE;
  }
  return (BOOLEAN)(!B->Media->MediaPresent || (B->Media->LastBlock == 0));
}
"""


def prepare_boot_menu_storage_only(root):
    """r5.25.4 (user: the boot-once list shows real storage only): BootManagerMenuApp and BootManagerUiLib refresh the boot
    options themselves when they open, so the BDS prune is undone - both skip options that are no storage with a medium."""
    jobs = [
        ('MdeModulePkg/Application/BootManagerMenuApp', 'BootManagerMenu.c', 'BootManagerMenuApp.inf', '#include "BootManagerMenu.h"',
         """  if (((BootOption->Attributes & LOAD_OPTION_HIDDEN) != 0) || ((BootOption->Attributes & LOAD_OPTION_ACTIVE) == 0)) {
    return TRUE;
  }
""", """  if (Ps5OptionNotStorage (BootOption->FilePath)) {
    return TRUE;
  }
"""),
        ('MdeModulePkg/Library/BootManagerUiLib', 'BootManager.c', 'BootManagerUiLib.inf', '#include <Protocol/LoadedImage.h>',
         """    if ((BootOption[Index].Attributes & LOAD_OPTION_HIDDEN) != 0) {
      continue;
    }
""", """    if (Ps5OptionNotStorage (BootOption[Index].FilePath)) {
      continue;
    }
"""),
    ]
    # r5.25.4 (user: a boot from the boot menu that does not start goes back to the PSWloader screen): in the boot menu mode
    # (volatile Ps5OnceMode, set by DEL on the start screen) a returned boot leaves the Boot Manager page at once, no key prompt
    ui = root / 'MdeModulePkg/Library/BootManagerUiLib/BootManager.c'
    if ui.exists():
        t = ui.read_text()
        once_anchor = """  BmSetConsoleMode (TRUE);

  if (EFI_ERROR (BootOption[QuestionId - 1].Status)) {"""
        once_new = """  BmSetConsoleMode (TRUE);

  {
    STATIC EFI_GUID  Ps5Preset = { 0x5efc5ae0, 0xb753, 0x4f7a, { 0xa6, 0x62, 0xe3, 0x4d, 0x15, 0xa6, 0xd8, 0xe5 } };
    UINT8            Ps5Once   = 0;
    UINTN            Ps5OnceSz = sizeof (Ps5Once);
    if (!EFI_ERROR (gRT->GetVariable (L"Ps5OnceMode", &Ps5Preset, NULL, &Ps5OnceSz, &Ps5Once)) && Ps5Once) {
      DEBUG ((DEBUG_ERROR, "PS5 boot menu: boot of option %u returned (%r) - back to the start screen\\n", (UINT32)(QuestionId - 1), BootOption[QuestionId - 1].Status));
      *ActionRequest = EFI_BROWSER_ACTION_REQUEST_EXIT;
      EfiBootManagerFreeLoadOptions (BootOption, BootOptionCount);
      return EFI_SUCCESS;
    }
  }

  if (EFI_ERROR (BootOption[QuestionId - 1].Status)) {"""
        if 'Ps5OnceMode' not in t:
            if t.count(once_anchor) != 1:
                raise RuntimeError('BootManager.c: boot-callback anchor not found')
            ui.write_text(t.replace(once_anchor, once_new, 1))
    for d, c, i, include, anchor, add in jobs:
        src, inf = root / d / c, root / d / i
        if not src.exists():
            continue
        text = src.read_text()
        if 'Ps5OptionNotStorage' in text:
            continue
        if text.count(anchor) != 1 or text.count(include) != 1:
            raise RuntimeError(c + ': storage-only anchors not found')
        text = text.replace(include, include + PS5_STORAGE_HELPER, 1).replace(anchor, anchor + add, 1)
        src.write_text(text)
        t = inf.read_text()
        if 'gEfiSimpleFileSystemProtocolGuid' not in t:
            t = t.replace('[Protocols]\n', '[Protocols]\n  gEfiSimpleFileSystemProtocolGuid              ## CONSUMES\n  gEfiBlockIoProtocolGuid                       ## CONSUMES\n', 1)
            inf.write_text(t)


def prepare_usb_mass_boot(root):
    """Force FIXED media for PS5 NVMe-over-USB so standard Windows 11 installs boot."""
    path = root / 'MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassBoot.c'
    if not path.exists():
        return
    text = path.read_text()
    marker = 'forcing FIXED for Windows boot'
    if marker in text:
        return
    anchor = 'Media->RemovableMedia = (BOOLEAN)(USB_BOOT_REMOVABLE (UsbMass->InquiryData.Removable));'
    patch = (
        'Media->RemovableMedia = (BOOLEAN)(USB_BOOT_REMOVABLE (UsbMass->InquiryData.Removable));\n'
        '  //\n'
        '  // PS5 v1.56: the PS5 exposes its internal NVMe to the firmware as USB mass storage.\n'
        '  // If it reports as a removable medium, a normal (non-To-Go) Windows 11 install refuses\n'
        '  // to boot from it (bootmgfw 0xc000000e). Present it as a FIXED disk so a regular Windows\n'
        '  // install boots like on a PC. Log the original INQUIRY removable bit for the record.\n'
        '  //\n'
        '  DEBUG ((DEBUG_ERROR, "PS5 USB-MSD: INQUIRY removable=%u -> forcing FIXED for Windows boot\\n", (UINT32)Media->RemovableMedia));\n'
        '  Media->RemovableMedia = FALSE;'
    )
    if anchor in text:
        path.write_text(text.replace(anchor, patch))


def replace_rtc(source):
    """Replace the x64 PC CMOS driver in either pinned DSC or FDF."""
    if source.count(RTC_ORIGINAL) != 1:
        raise RuntimeError('Unexpected PC RTC driver selection')
    source = source.replace(RTC_ORIGINAL, RTC_PS5)
    # Removing PcRtc also removes the only consumer/DEC declaration of these
    # dynamic PCDs. Keep neither legacy port configuration in the PS5 DSC.
    for name in ('Index', 'Target'):
        source = re.sub(r'^  gPcAtChipsetPkgTokenSpaceGuid.PcdRtc' + name + r'Register[^\n]*\n', '', source, flags=re.M)
    return source


ENTRY_DIRECTORY = 'UefiPayloadPkg/UefiPayloadEntry'


SHORT_MARKER_STAGES = ('entry', 'post-hobs', 'post-dxe-load')


def prepare_entry_power(root, enabled, stage="entry", short_marker=False,
                        short_marker_stage='entry'):
    """Always regenerate pinned entry files so a later normal build is clean."""
    originals = {}
    for name in ('UniversalPayloadEntry.c', 'UniversalPayloadEntry.inf'):
        originals[name] = subprocess.check_output(
            ['git', 'show', 'HEAD:' + ENTRY_DIRECTORY + '/' + name], cwd=root, text=True)
    source = originals['UniversalPayloadEntry.c']
    inf = originals['UniversalPayloadEntry.inf']
    stages = {
        'entry': ('  mHobList = (VOID *)BootloaderParameter;', 0x701),
        'dxe-ready': ('  Hob.HandoffInformationTable = (EFI_HOB_HANDOFF_INFO_TABLE *)GetFirstHob (EFI_HOB_TYPE_HANDOFF);', 0x703),
    }
    if stage not in stages:
        raise ValueError('Unknown UEFI telemetry stage')
    if short_marker and (not enabled or stage != "entry"):
        raise ValueError("Short marker requires enabled entry telemetry")
    if short_marker_stage not in SHORT_MARKER_STAGES:
        raise ValueError('Unknown short marker stage')
    if short_marker_stage != 'entry' and not short_marker:
        raise ValueError('Short marker stage requires short marker')
    anchor, code = stages[stage]
    declaration = 'extern VOID  *mHobList;'
    inf_anchor = '  UniversalPayloadEntry.c'
    if source.count(anchor) != 1 or source.count(declaration) != 1 or inf.count(inf_anchor) != 1:
        raise RuntimeError('Unexpected pinned payload entry telemetry anchors')
    if enabled:
        if short_marker:
            source = source.replace(declaration, declaration + '\nVOID ps5_short_entry_marker (VOID);')
            if short_marker_stage in ('post-hobs', 'post-dxe-load'):
                build_hobs = '  Status = BuildHobs (BootloaderParameter, &DxeFv);'
                status_assert = '  ASSERT_EFI_ERROR (Status);'
                checked_hobs = build_hobs + '\n' + status_assert
                next_hob = '  if (GetFirstGuidHob (&gEfiMemoryTypeInformationGuid) == NULL) {'
                if (source.count(build_hobs) != 1 or source.count(checked_hobs) != 1
                        or source.count(next_hob) != 1
                        or not source.index(anchor) < source.index(build_hobs) < source.index(next_hob)
                        or source[source.index(build_hobs):source.index(next_hob)].count(status_assert) != 1):
                    raise RuntimeError('Unexpected pinned post-HOB marker anchors')
                hobs_gate = checked_hobs + '\n'
                hobs_gate += (
                    '  // Diagnostic marker requires successful HOB construction even with ASSERT disabled.\n'
                    '  if (EFI_ERROR (Status)) {\n'
                    '    __asm__ volatile("1: cli; hlt; jmp 1b");\n'
                    '  }')
                if short_marker_stage == 'post-hobs':
                    source = source.replace(checked_hobs, hobs_gate + '\n  ps5_short_entry_marker ();')
                else:
                    load_dxe = '  Status = UniversalLoadDxeCore (DxeFv, &DxeCoreEntryPoint);'
                    checked_dxe = load_dxe + '\n' + status_assert
                    handoff = '  Hob.HandoffInformationTable = (EFI_HOB_HANDOFF_INFO_TABLE *)GetFirstHob (EFI_HOB_TYPE_HANDOFF);'
                    if (source.count(load_dxe) != 1 or source.count(checked_dxe) != 1
                            or source.count(handoff) != 1
                            or not source.index(next_hob) < source.index(load_dxe) < source.index(handoff)
                            or source[source.index(load_dxe):source.index(handoff)].count(status_assert) != 1):
                        raise RuntimeError('Unexpected pinned post-DXE-load marker anchors')
                    source = source.replace(checked_hobs, hobs_gate)
                    source = source.replace(checked_dxe, checked_dxe + '\n'
                        '  // Diagnostic marker requires successful DXE loading even with ASSERT disabled.\n'
                        '  if (EFI_ERROR (Status)) {\n'
                        '    __asm__ volatile("1: cli; hlt; jmp 1b");\n'
                        '  }\n'
                        '  ps5_short_entry_marker ();')
            else:
                source = source.replace(anchor, '  ps5_short_entry_marker ();\n' + anchor)
        else:
            source = source.replace(declaration, declaration + '\nVOID ps5_power_report (unsigned code);')
            source = source.replace(anchor, f'  ps5_power_report (0x{code:x});\n' + anchor)
        inf = inf.replace(inf_anchor, inf_anchor + '\n  Ps5EntryPower.c')
    directory = root / ENTRY_DIRECTORY
    for name, content in (('UniversalPayloadEntry.c', source), ('UniversalPayloadEntry.inf', inf)):
        (directory / name).write_text(content)
    if enabled:
        (directory / 'Ps5EntryPower.c').write_text('#define PS5_POWER_EDK2\n' + (HERE / 'bootstrap-power.c').read_text())
        if short_marker:
            with (directory / 'Ps5EntryPower.c').open('a') as out:
                out.write("""
VOID ps5_short_entry_marker (VOID) {
    static const unsigned char pattern[12] = {0,0,1,1,1,0,0,1,0,1,0,1};
    for (unsigned i=0; i<12; ++i) slot(pattern[i],4);
    /* Keep a compiler-visible return path so LTO retains the normal payload
     * relocation metadata. The assembly itself never falls through. */
    __asm__ volatile("1: cli; hlt; jmp 1b");
}
""")


def short_marker_manifest(stage):
    if stage not in SHORT_MARKER_STAGES:
        raise ValueError('Unknown short marker stage')
    return dict(schema=1, pattern='001110010101', slot_seconds=4,
                nominal_duration_seconds=48, terminal=True,
                stage=('UniversalPayloadEntry' if stage == 'entry'
                       else 'UniversalPayloadEntry/' + stage),
                hardware_verified=False)


POWER_STAGES = ('exception-only','entry','dxe-ready','dxe-entry','dxe-exceptions','dxe-memory','dxe-dispatch','log-entry','cpu-entry','cpu-before-mp','cpu-before-gcd','cpu-after-paging','cpu-after-gdt','cpu-after-idt')


def power_targets(enabled, stage, log_export):
    if stage not in POWER_STAGES:
        raise ValueError('Unknown UEFI telemetry stage')
    if stage != 'entry' and not enabled:
        raise ValueError('entry power stage requires entry power telemetry')
    if stage == 'log-entry' and not log_export:
        raise ValueError('log-entry power stage requires log export')
    return (enabled and stage in ('entry','dxe-ready'),
            enabled and stage.startswith('dxe-') and stage != 'dxe-ready',
            enabled and stage == 'log-entry',
            enabled and stage in ('cpu-entry','cpu-before-mp','cpu-before-gcd','cpu-after-paging','cpu-after-gdt','cpu-after-idt'))


def prepare_log_power(package, enabled):
    """Regenerate optional logger sources from repository originals every time."""
    relative = Path('LogExportDxe')
    directory = package / relative
    source = (HERE / 'Ps5PlatformPkg' / relative / 'LogExportDxe.c').read_text()
    inf = (HERE / 'Ps5PlatformPkg' / relative / 'LogExportDxe.inf').read_text()
    declaration = '#include <PiDxe.h>'
    anchor = '  EFI_STATUS Status=gBS->CreateEventEx('
    inf_anchor = '  LogExportDxe.c'
    protocol_anchor = '  gEfiPartitionInfoProtocolGuid'
    if (source.count(declaration) != 1 or source.count(anchor) != 1
            or inf.count(inf_anchor) != 1 or inf.count(protocol_anchor) != 1):
        raise RuntimeError('Unexpected logger power probe anchors')
    if enabled:
        source = source.replace(declaration, declaration + '\nVOID Ps5LogEntryProbe (VOID);')
        source = source.replace(anchor, '  Ps5LogEntryProbe ();\n' + anchor)
        inf = inf.replace(inf_anchor, inf_anchor + '\n  Ps5LogEntryProbe.c\n  Ps5LogEntryPower.c')
        inf = inf.replace(protocol_anchor, '  gEfiCpuArchProtocolGuid ## CONSUMES\n'
            '  gEfiRealTimeClockArchProtocolGuid ## CONSUMES\n'
            '  gEfiTimerArchProtocolGuid ## CONSUMES\n' + protocol_anchor)
    (directory / 'LogExportDxe.c').write_text(source)
    (directory / 'LogExportDxe.inf').write_text(inf)
    encoder = directory / 'Ps5LogEntryPower.c'
    if enabled:
        shutil.copy2(HERE / 'Ps5PlatformPkg' / relative / 'Ps5LogEntryProbe.c', directory / 'Ps5LogEntryProbe.c')
        encoder.write_text('#define PS5_POWER_EDK2\n' + (HERE / 'bootstrap-power.c').read_text())
    else:
        encoder.unlink(missing_ok=True)


CPU_DIRECTORY = 'UefiCpuPkg/CpuDxe'


def prepare_warm_soft_tick_cpu(root):
    """r5.25.6: after a warm re-entry the idle loop must not HLT (IER closed in UEFI - nothing would wake it)."""
    path = root / CPU_DIRECTORY / 'CpuDxe.c'; t = path.read_text()
    decl = '#include "CpuDxe.h"'
    idle = 'IdleLoopEventCallback (\n  IN EFI_EVENT  Event,\n  IN VOID       *Context\n  )\n{\n  CpuSleep ();\n}'
    entry = '  InitializePageTableLib ();\n'
    if t.count(decl) != 1 or t.count(idle) != 1 or t.count(entry) != 1:
        raise RuntimeError('warm soft tick: CpuDxe.c anchors')
    t = t.replace(decl, decl + '\nSTATIC BOOLEAN  mPs5WarmNoHlt;   /* r5.25.6: warm re-entry - idle without HLT */', 1)
    t = t.replace(idle, idle.replace('  CpuSleep ();', '  if (mPs5WarmNoHlt) {\n    CpuPause ();\n  } else {\n    CpuSleep ();\n  }'), 1)
    t = t.replace(entry, '  mPs5WarmNoHlt = (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL);\n' + entry, 1)
    path.write_text(t)

def prepare_warm_soft_tick_timer(root):
    """r5.25.6: software timer after a warm re-entry - idle loop and gBS->Stall feed CoreTimerTick."""
    rel = 'OvmfPkg/LocalApicTimerDxe/'
    t = subprocess.check_output(['git', 'show', 'HEAD:' + rel + 'LocalApicTimerDxe.c'], cwd=root, text=True)
    inf = subprocess.check_output(['git', 'show', 'HEAD:' + rel + 'LocalApicTimerDxe.inf'], cwd=root, text=True)
    inc = '#include <Library/NestedInterruptTplLib.h>\n'
    init = 'EFI_STATUS\nEFIAPI\nTimerDriverInitialize (\n'
    inst = '  //\n  // Install the Timer Architectural Protocol onto a new handle\n  //\n'
    lib = '  LocalApicLib\n'
    if t.count(inc) != 1 or t.count(init) != 1 or t.count(inst) != 1 or inf.count(lib) != 1:
        raise RuntimeError('warm soft tick: LocalApicTimerDxe anchors')
    code = r'''/* r5.25.6: after a warm re-entry the extended-APIC filter stays closed in UEFI - no timer interrupt. The DXE
 * core's tick (mTimerNotifyFunction) is fed from the idle loop and from gBS->Stall instead, at TPL_HIGH_LEVEL like from
 * the interrupt, with the time measured by TimerLib (the same source gBS->Stall uses). */
STATIC BOOLEAN    mPs5Soft, mPs5SoftBusy;
STATIC UINT64     mPs5SoftLast;
STATIC EFI_STALL  mPs5SoftStallOrig;
STATIC EFI_CHECK_EVENT  mPs5SoftCheckOrig;
STATIC EFI_RESTORE_TPL  mPs5SoftRestoreOrig;
STATIC EFI_EXIT_BOOT_SERVICES  mPs5SoftEbsOrig;
STATIC EFI_GUID   mPs5IdleGuid = { 0x3c8d294c, 0x5fc3, 0x4451, { 0xbb, 0x31, 0xc4, 0xc0, 0x32, 0x29, 0x5e, 0x6c } };   /* gIdleLoopEventGuid */
STATIC VOID Ps5SoftTick (VOID)
{
  EFI_TPL  Tpl;
  UINT64   Now, Ns;
  if (!mPs5Soft || mPs5SoftBusy || (mTimerNotifyFunction == NULL)) return;
  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if ((Tpl < TPL_HIGH_LEVEL) && !mPs5SoftBusy) {   /* at TPL_HIGH (IF off) no hardware tick could come either */
    Now = GetPerformanceCounter ();
    Ns  = GetTimeInNanoSecond (Now - mPs5SoftLast);
    if (Ns >= 1000000) {
      mPs5SoftLast = Now; mPs5SoftBusy = TRUE;
      mTimerNotifyFunction (Ns / 100);
      mPs5SoftBusy = FALSE;
      if (*(volatile UINT64 *)(UINTN)0x111860018ULL == 0x4352545342575350ULL) {   /* lab --warm-bs-trace: count ticks */
        (*(volatile UINT64 *)(UINTN)0x111860038ULL)++;
      }
    }
  }
  if (mPs5SoftRestoreOrig != NULL) mPs5SoftRestoreOrig (Tpl); else gBS->RestoreTPL (Tpl);   /* never through our own wrapper */
}
STATIC VOID EFIAPI Ps5SoftIdle (IN EFI_EVENT Event, IN VOID *Context) { Ps5SoftTick (); }
STATIC EFI_STATUS EFIAPI Ps5SoftStall (IN UINTN Microseconds)
{
  EFI_STATUS  Status = mPs5SoftStallOrig (Microseconds);
  Ps5SoftTick ();
  return Status;
}
/* r5.25.5 (HW 19.11. lab79: Windows' boot manager hung before ExitBootServices): boot applications calibrate with a
 * relative timer event polled by CheckEvent - that loop calls neither Stall nor the idle loop, so it ticks too */
STATIC EFI_STATUS EFIAPI Ps5SoftCheckEvent (IN EFI_EVENT Event)
{
  Ps5SoftTick ();
  return mPs5SoftCheckOrig (Event);
}
/* r5.25.5 (HW 20.11. lab81 trace: winload looped GetMemoryMap/ExitBootServices - soft ticks between the two ran timer
 * callbacks that changed the memory map, so every ExitBootServices failed on its map key): the software timer stops at
 * the first ExitBootServices call - the loader's retry then sees a stable map */
STATIC EFI_STATUS EFIAPI Ps5SoftExitBootServices (IN EFI_HANDLE ImageHandle, IN UINTN MapKey)
{
  EFI_STATUS  Status;
  mPs5Soft = FALSE;
  Status = mPs5SoftEbsOrig (ImageHandle, MapKey);
#if defined (PS5_WARM_EBS_STEPS)   /* lab: second row, slot 1 (blue-white) = ExitBootServices returned to the loader with success */
  if (!EFI_ERROR (Status)) { volatile UINT64 *C = (volatile UINT64 *)(UINTN)0x111806300ULL; if (C[0] == 0x3142465153575350ULL && C[1] != 0) { volatile UINT32 *F = (volatile UINT32 *)(UINTN)C[1]; for (UINT32 Y = 60; Y < 108; Y++) for (UINT32 X = 0; X < 48; X++) F[Y * C[2] + 320 + 60 + X] = 0x0080C0FFU; AsmWbinvd (); } }
#endif
  return Status;
}
/* loops that only raise and restore the TPL tick as well - dropping below TPL_HIGH is where a hardware tick
 * would be delivered */
STATIC VOID EFIAPI Ps5SoftRestoreTpl (IN EFI_TPL OldTpl)
{
  mPs5SoftRestoreOrig (OldTpl);
  if (OldTpl < TPL_HIGH_LEVEL) Ps5SoftTick ();
}
STATIC VOID Ps5SoftTickStart (VOID)
{
  EFI_EVENT  Idle;
  if (*(volatile UINT64 *)(UINTN)0x111806040ULL != 0x314D524157575350ULL) return;   /* cold boot: hardware ticks */
  mPs5SoftLast = GetPerformanceCounter ();
  if (EFI_ERROR (gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, Ps5SoftIdle, NULL, &mPs5IdleGuid, &Idle))) return;
  mPs5SoftStallOrig = gBS->Stall;
  gBS->Stall = Ps5SoftStall;
  mPs5SoftCheckOrig = gBS->CheckEvent;
  gBS->CheckEvent = Ps5SoftCheckEvent;
  mPs5SoftRestoreOrig = gBS->RestoreTPL;
  gBS->RestoreTPL = Ps5SoftRestoreTpl;
  mPs5SoftEbsOrig = gBS->ExitBootServices;
  gBS->ExitBootServices = Ps5SoftExitBootServices;
  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  mPs5Soft = TRUE;
  mPs5NoHw = TRUE;
  DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: software timer on (idle loop + Stall + CheckEvent + RestoreTPL feed the DXE tick, no timer interrupt)\n"));
}

'''
    t = t.replace(inc, inc + '#include <Library/TimerLib.h>\n', 1)
    hw = ('    //\n    // Program the timer with the new count value\n    //\n'
          '    InitializeApicTimer (DivideValue, (UINT32)TimerCount, TRUE, LOCAL_APIC_TIMER_VECTOR);\n')
    if t.count(hw) != 1: raise RuntimeError('warm soft tick: SetTimerPeriod anchor')
    # r5.25.7 (HW 20.11. lab84 apic state): in a warm run the LAPIC timer is never started - a masked LVT
    # sets no IRR bit, so no stale 0x20 waits for the moment the filter opens at ExitBootServices
    t = t.replace(hw, '    if (mPs5NoHw || (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL)) {\n'
                      '      DisableApicTimerInterrupt ();   /* r5.25.7: warm run - software ticks only, the LVT stays masked */\n'
                      '      mTimerPeriod = TimerPeriod;\n      return EFI_SUCCESS;\n    }\n' + hw, 1)
    t = t.replace(inc, inc + 'STATIC BOOLEAN mPs5NoHw;   /* r5.25.7: set for good by Ps5SoftTickStart (warm run) */\n', 1)
    t = t.replace(init, code + init, 1)
    t = t.replace(inst, '  Ps5SoftTickStart ();\n\n' + inst, 1)
    inf = inf.replace(lib, lib + '  TimerLib\n', 1)
    (root / rel / 'LocalApicTimerDxe.c').write_text(t)
    (root / rel / 'LocalApicTimerDxe.inf').write_text(inf)

def prepare_nvme_pass_thru_timeout(root):
    """r5.25.6: blocking NVMe pass-thru keeps its timeout without timer ticks (Stall-counted)."""
    rel = 'MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressPassthru.c'
    t = subprocess.check_output(['git', 'show', 'HEAD:' + rel], cwd=root, text=True)
    a = ('  while (EFI_ERROR (gBS->CheckEvent (TimerEvent))) {\n'
         '    if (Cq->Pt != Private->Pt[QueueId]) {\n'
         '      Status = EFI_SUCCESS;\n'
         '      break;\n'
         '    }\n'
         '  }\n')
    if t.count(a) != 1: raise RuntimeError('nvme pass-thru timeout anchor')
    b = ('  {\n'
         '    UINT64  Ps5Us = 0;   /* r5.25.6: without timer ticks (warm re-entry) the timer event never fires */\n'
         '    while (EFI_ERROR (gBS->CheckEvent (TimerEvent))) {\n'
         '      if (Cq->Pt != Private->Pt[QueueId]) {\n'
         '        Status = EFI_SUCCESS;\n'
         '        break;\n'
         '      }\n'
         '      gBS->Stall (1);\n'
         '      if ((Packet->CommandTimeout != 0) && (++Ps5Us * 10 >= Packet->CommandTimeout)) {\n'
         '        break;\n'
         '      }\n'
         '    }\n'
         '  }\n')
    (root / rel).write_text(t.replace(a, b, 1))

def prepare_cpu_power(root, enabled, stage="cpu-entry"):
    """Regenerate pinned CpuDxe with a single optional pre-initialization packet."""
    originals = {name: subprocess.check_output(
        ['git','show','HEAD:' + CPU_DIRECTORY + '/' + name], cwd=root, text=True)
        for name in ('CpuDxe.c','CpuDxe.inf')}
    source, inf = originals['CpuDxe.c'], originals['CpuDxe.inf']
    declaration = '#include "CpuDxe.h"'
    stages = {'cpu-entry': ('  InitializePageTableLib ();', 0x709),
              'cpu-before-mp': ('  InitializeMpSupport ();', 0x70a),
              'cpu-before-gcd': ('  RefreshGcdMemoryAttributes ();', 0x70b),
              'cpu-after-paging': ('  InitializeFloatingPointUnits ();', 0x70c),
              'cpu-after-gdt': ('  InitInterruptDescriptorTable ();', 0x70d),
              'cpu-after-idt': ('  Status = gBS->InstallMultipleProtocolInterfaces (', 0x70e)}
    if stage not in stages:raise ValueError('Unknown CPU probe stage')
    anchor, code = stages[stage]
    entry = 'InitializeCpu ('
    inf_anchor = '\n  CpuDxe.c\n'
    if (source.count(declaration) != 1 or source.count(anchor) != 1
            or source.count(entry) != 1 or inf.count(inf_anchor) != 1
            or source.index(anchor) < source.index(entry)):
        raise RuntimeError('Unexpected pinned CPU entry telemetry anchors')
    if enabled:
        source = source.replace(declaration, declaration + '\nVOID ps5_power_report (unsigned Code);')
        source = source.replace(anchor, f'  ps5_power_report (0x{code:x});\n' + anchor)
        inf = inf.replace(inf_anchor, inf_anchor + '  Ps5CpuEntryPower.c\n')
    directory = root / CPU_DIRECTORY
    (directory / 'CpuDxe.c').write_text(source)
    (directory / 'CpuDxe.inf').write_text(inf)
    encoder = directory / 'Ps5CpuEntryPower.c'
    if enabled:
        encoder.write_text('#define PS5_POWER_EDK2\n' + (HERE / 'bootstrap-power.c').read_text())
    else:
        encoder.unlink(missing_ok=True)


def image_protection_comparison(enabled, telemetry, stage):
    """Experimental isolation of synchronous image protection, never default."""
    if not enabled:
        return []
    if not telemetry or stage != 'log-entry':
        raise ValueError('Image protection comparison requires log-entry telemetry')
    return ['-p', 'gEfiMdeModulePkgTokenSpaceGuid.PcdImageProtectionPolicy=0']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk2', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--entry-power-telemetry', action='store_true', help='Emit calibrated-channel packet 0x701 at payload entry; bootstrap must supply calibration')
    parser.add_argument('--entry-power-stage', choices=POWER_STAGES, default='entry', help='Select diagnostic placement; exception-only omits normal-path packets and retains fatal reporting')
    parser.add_argument('--native-atom-library',action='store_true',help='Link experimental ATOM interpreter; no command invocation; requires native-setup')
    parser.add_argument('--native-dp-source',action='store_true',help='Experimental native DP PHY enable on a unique attached ROM path; requires native-atom-library and hdmi-prepare')
    parser.add_argument('--native-dp-video',action='store_true',help='Experimental actual 1080p GPU red test pattern and fixed converter video path; requires native-dp-source, not a GOP')
    # Stage A edid2 (UNTESTED): EDID over ICC in HdmiProbeDxe only; BDS/DCN/GOP path stays v386b.
    parser.add_argument('--hdmi-edid-request',action='store_true',help='HdmiProbeDxe: send Linux hdmiSystemResume EDID sequences + HDMI event enable (needs --hdmi-prepare); EDID window never fatal, ICC notifications taken like PS5 Linux')
    # Stage B (UNTESTED): boot mode from the EDID within the fixed whitelist (ps5-display-mode.c), BDS part of the display-modes patch.
    parser.add_argument('--display-policy',type=int,choices=(0,1,2,3),default=0,help='0 = fixed 1080p60 (default, unchanged), 1 = best whitelisted mode <= 60 Hz (native resolution first), 2 = best whitelisted mode (native first), 3 = largest offered area with a compiled replay (test policy)')
    parser.add_argument('--dispclk-raise',type=int,choices=(0,1),default=0,help='DRAFT: 0 = only log the DISPCLK/DPPCLK plan (default, unchanged), 1 = raise DENTIST to the replay\'s value before the mode is armed (dcn20_update_clocks_update_dentist order)')
    parser.add_argument('--display-replay',action='append',default=[],choices=('1080p120','1440p60','2160p60','3440p60','3440p100','ugreen1440p60','2560x1440p120','720p144'),help='compile in dcn201-pipe-replay-<mode>.h (generated from a PS5 Linux capture at that mode); repeatable')
    parser.add_argument('--display-force',choices=('720p144',),default=None,help='r5.13 test build: this mode wins when the sink offers it exactly and its replay is compiled in (needs --display-replay of the same mode)')
    parser.add_argument('--diag-graphics',action='store_true',help='v3.30: keep the diagnostic boot paints (colour probe, test image, EBS boxes, winload/NT stripes, probe bit rows); default is a quiet boot')
    parser.add_argument('--native-setup',action='store_true',help='Actual firmware Esc/UiApp entry; requires working GOP and UEFI input')
    parser.add_argument('--edid-window-seconds',type=int,default=None,help='r5.16.2 Dell test: EDID window budget in seconds (default 4 in the source) plus per-rx/update/long-burst timestamps; without it the image is unchanged')
    parser.add_argument('--hdmi-prepare',action='store_true',help='Opt-in FLAVA3 converter preparation writes; implies hdmi-probe; not full video initialization')
    parser.add_argument('--hdmi-probe', action='store_true', help='Include synchronous ICC HDMI diagnostic; no GOP')
    parser.add_argument('--setup-seconds', type=int, default=None, help='Esc/boot-manager window before the UEFI boot continues (5 s ticks; default 30)')
    parser.add_argument('--lab-sandisk', action='store_true', help='lab: boot the internal SanDisk and switch its BCD default to its own Windows once')
    parser.add_argument('--lab-debug-info', action='store_true', help='lab: keep DEBUG_INFO in the log ring')
    parser.add_argument('--warm-no-button-poll', action='store_true', help='lab: no ICC button poller timer after a warm re-entry')
    parser.add_argument('--assert-breakpoint', action='store_true', help='lab: PcdDebugPropertyMask 0x1F (ASSERT -> int3) instead of 0x2F (dead loop)')
    parser.add_argument('--warm-timer-storm', action='store_true', help='lab: 200 local-APIC timer interrupts within 0.5 s after a warm re-entry switch off')
    parser.add_argument('--warm-keep-gpu', action='store_true', help='lab: the warm trampoline leaves vendor 1002 functions (GPU, HDMI audio) as the OS left them')
    parser.add_argument('--warm-bs-trace', action='store_true', help='lab: warm run - every boot service is counted from the bootmgfw start on, AP 1 paints the counters on the logo')
    parser.add_argument('--warm-ebs-watch', action='store_true', help='lab: warm watchdog stays armed through ExitBootServices; off 60 s + (N - 49) * 30 s (nominal) after the last point 50 launch, 51 before-EBS, 52/53 EBS mark, 54/55 IER notify; no off = the APs were parked (EBS notifies done)')
    parser.add_argument('--warm-ebs-steps', action='store_true', help='lab (needs --warm-ebs-mark): 48x48 squares at x 320 + 60*slot after a warm ExitBootServices call - 0 green FAT flush entry, 1 yellow FAT flush done, 2 blue after SetTimerPeriod(0), 3 cyan memory map terminated, 4 orange terminate failed, 5 purple xHCI EBS entry, 6 grey xHCI halted, 7 pink xHCI EBS done, 8 white first EBS notify (newest), 11 light green right after PostWimExit, 9 red EBS mark entry, 10 light red EBS mark after LocateProtocol, 12 magenta / 13 gold before / after the WBINVD in the native GOP EBS marker, 14 teal / 15 olive IER notify entry / end; every square is flushed with WBINVD (WB-cached frame buffer); second row: 0 white first TPL_CALLBACK EBS notify, 1 light blue ExitBootServices returned success')
    parser.add_argument('--warm-ebs-mark', action='store_true', help='lab: white square on the screen at ExitBootServices of a warm run')
    parser.add_argument('--warm-rescue', action='store_true', help='lab: the early watchdog AP rescues the log ring of a hung warm run, re-enters the firmware itself; the rescue run exports it to the PS5UEFI.LOG slot and switches off')
    parser.add_argument('--warm-watchdog-selftest', action='store_true', help='lab: cold-boot self-test - arm the early watchdog without a warm re-entry and stop the BSP at point 44')
    parser.add_argument('--warm-watchdog-early', action='store_true', help='lab: arm the warm watchdog right after the CPU names (points 41-43 around the IER opening); skips the ext-APIC AP part')
    parser.add_argument('--warm-watchdog', action='store_true', help='lab: after a warm re-entry a watchdog AP switches off 60 s after the last BDS progress point N (44..49) with a delay of (N-39)*10 s')
    parser.add_argument('--warm-probe-image', type=int, help='lab: after a warm re-entry switch the console off when the K-th DXE image is started')
    parser.add_argument('--warm-probe', type=int, choices=(1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 16, 21, 22, 23, 24, 25, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 61, 62, 63, 65, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79), help='lab: after a warm re-entry switch the console off in the reset driver (1) or in BDS (2); triple fault right before the firmware entry (3) or right after the trampoline dropped the IDT (4); off right before the trampoline (5); 11-16 = GPU-D3 + halt signal at trampoline steps; 21-25 = ICC off at trampoline steps; 31/32 = payload entry start/hand-off to DXE, 33/34 = DXE core start/after all DXE drivers (before BDS); 35/36 = payload entry after the library constructors/after BuildHobs; 37/38 = hand-off before/after the CR3 load; 39 = first line of the DXE core; 40 = unhandled DXE exception: off after (vector + 1) * 10 s; 41 = hand-off after UpdateStackHob (before SwitchStack); 42-44 = BDS ext-APIC step start/after the CPU names/end; 45 = BDS warm record (stand-alone off); 46/47 = before/after PlatformConsoleInit, 48 = AfterConsole start, 49 = after ConnectAll; 50/51 = BDS ext-APIC after the BSP filter opened / after the AP StartupAllAPs; 52 = 8th device interrupt without a handler: off after (vector / 32 + 1) * 10 s; 53 = a ButtonPoll call slower than 0.5 s; 54 = 200th local-APIC timer interrupt; 55/56 = ButtonPoll entered / returned; 57-59 = BDS ext-APIC after the first DEBUG line / after the IER writes / after TPR+EOI; 61 = pending IRR 16..31 (off after (V-15)*10 s) or stale ISR (200 s) before the IER loop; 62 = DXE timer does not fire within 2 s after the IER opening; 63 = at 46 read the config dword 0 of every function, then off; 65 = at 57 wait 60 s (IER still closed), then off; 67 = after the ISR drain, before the timer vector opens; 68 = first entry of the LAPIC timer handler; 69 = int 0x20 before the timer vector opens (software path into the handler); 70 = after the CPU names: all APs ran -> off now, some -> 60 s, none -> 120 s; 71 = at 58 a load square wave on the BSP (no PCIe/ICC); 72 = cli, write the timer IER bit, off; 73 = IF on with IER[0x20] open but TPR 0xF0 for 2 s, then off; 74 = self-IPI 0x21 with own IDT target: off at once = accepted, 30 s = not taken, stays on = acceptance kills; 75 = winload\'s StartupAllAPs: off at its return, or ~120 s (nominal) after its entry if it never returns (stays on = never called); 76 = off at the entry of winload\'s StartupAllAPs; 77 = LAPIC timer one-shot on 0x21 (own IDT target): off at once = a timer interrupt is accepted, after 30 s = not taken (core alive), stays on = the timer interrupt kills; 78 = software int 0x21 into the probe-74 IDT target (off at once = gate/stack fine, the hardware delivery kills); 79 = self-IPI 0x21 on AP 1 (shared IDT, own target): off at once = the AP takes hardware interrupts (BSP-specific killer), after 30 s = AP did not report, stays on = global')
    parser.add_argument('--warm-off-test', action='store_true', help='lab: ResetSystem (cold/warm/platform) switches the console off first - shows whether the OS calls it')
    parser.add_argument('--warm-spin-test', action='store_true', help='lab: ResetSystem (cold/warm/platform) stops the core instead of restarting - shows whether the OS calls it')
    parser.add_argument('--warm-reentry', action='store_true', help='r5.25.4: a restart (ResetSystem cold/warm) starts the firmware again instead of the ICC reboot to GameOS')
    parser.add_argument('--warm-reset-test', action='store_true', help='lab: restart once through ResetSystem (warm re-entry test)')
    parser.add_argument('--menu-idle-seconds', type=int, default=None, help='r5.4: boot Windows after N s without a key in the setup/boot menu (default 0 = off since r5.25.3)')
    parser.add_argument('--m2-overlay', action='store_true', help='N4: hook the internal M.2 BlockIo when it carries a nested PS5 GPT (absolute LBAs from 65536), so the firmware offers its partitions; GameOS areas write-protected; no-op without nested GPT')
    parser.add_argument('--nvme-preinit', choices=('log','apply','probe'), default=None, help='Way (a) for the internal M.2 under Windows: log = dump the real (ARI/SFC) and shadow (raw ECAM) config views of 0x40:02.5 before ExitBootServices; apply = also mask all MSI-X vectors and set MSI-X enable + INTx disable in the real config at ExitBootServices (decode/bus master unchanged); probe = log + try to write the real MSI-X geometry into the shadow capability (raw ECAM, readback), and only if accepted the apply step at ExitBootServices')
    # LVGL UI (15.11.2026, UNTESTED on HW): LvglPkg (MIT, hamitcan99/LvglPkg) replaces the text DisplayEngineDxe, so
    # UiApp / BootManagerMenuApp are drawn graphically when the user opens the menu; the normal boot path is unchanged.
    parser.add_argument('--usb-mass-quiet', action='store_true', help='r5: after the internal NVMe ESP is selected, UsbMassStorageDxe binds no further USB disks (HID unaffected); without it the image is unchanged')
    parser.add_argument('--boot-ts', action='store_true', help='Boot timing: PS5 TS <phase> <ms> DEBUG lines (TSC) in BDS, DP video and HdmiProbeDxe; without it the image is unchanged')
    parser.add_argument('--lvgl-ui', action='store_true', help='Replace DisplayEngineDxe with LvglPkg LvglDisplayEngineDxe + LvglSetupDxe (graphical setup/boot menu); without it the image is unchanged')
    parser.add_argument('--lvgl-pkg', type=Path, default=Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/lvgl-uefi/src/LvglPkg'), help='LvglPkg checkout (with the lvgl submodule) for --lvgl-ui')
    parser.add_argument('--log-export', action='store_true', help='Write ReadyToBoot ring to preprovisioned slot on pinned Windows ESP')
    parser.add_argument('--experimental-no-image-protection', action='store_true', help='Diagnostic comparison only: omit DXE image protection; not a final platform configuration')
    parser.add_argument('--exception-power-telemetry', action='store_true', help='Report unhandled DXE vectors before the normal fatal UART dump; registered IRQ handlers remain unchanged')
    parser.add_argument('--entry-short-marker', action='store_true', help='Diagnostic only: 48s load marker at entry then halt; never boots Windows')
    parser.add_argument('--entry-short-marker-stage', choices=SHORT_MARKER_STAGES,
                        help='Terminal marker placement (default: entry); requires --entry-short-marker')
    args = parser.parse_args()
    if args.entry_short_marker_stage is not None and not args.entry_short_marker:
        parser.error('entry-short-marker-stage requires --entry-short-marker')
    args.entry_short_marker_stage = args.entry_short_marker_stage or 'entry'
    if args.entry_short_marker and (not args.entry_power_telemetry or args.entry_power_stage != 'entry'):
        parser.error('entry-short-marker requires --entry-power-telemetry --entry-power-stage entry')
    if args.hdmi_prepare:args.hdmi_probe=True
    if args.native_atom_library and not args.native_setup:parser.error("native-atom-library requires native-setup")
    if args.native_dp_source and (not args.native_atom_library or not args.hdmi_prepare):parser.error("native-dp-source requires native-atom-library and hdmi-prepare")
    if args.native_dp_video and not args.native_dp_source:parser.error("native-dp-video requires native-dp-source")
    if args.hdmi_edid_request and not args.hdmi_prepare:parser.error("hdmi-edid-request requires hdmi-prepare")
    if args.edid_window_seconds is not None and not (args.hdmi_edid_request and 5<=args.edid_window_seconds<=60):parser.error("edid-window-seconds needs hdmi-edid-request and 5..60")
    if (args.display_policy or args.display_replay) and not (args.native_dp_video and args.hdmi_edid_request):parser.error("display-policy/display-replay require native-dp-video and hdmi-edid-request")
    if args.display_force and args.display_force not in args.display_replay:parser.error("display-force needs --display-replay of the same mode")
    for mode in args.display_replay:
        if not (HERE/f'dcn201-pipe-replay-{mode}.h').exists():parser.error(f'dcn201-pipe-replay-{mode}.h missing (tools/gen-pipe-replay.py gen ...)')
    if args.exception_power_telemetry and not args.entry_power_telemetry:
        parser.error('Exception power telemetry requires calibrated entry telemetry')
    if args.entry_power_stage == 'exception-only' and not args.exception_power_telemetry:
        parser.error('exception-only stage requires exception power telemetry')
    try:
        image_comparison = image_protection_comparison(args.experimental_no_image_protection, args.entry_power_telemetry, args.entry_power_stage)
        upl_power, dxe_power, log_power, cpu_power = power_targets(args.entry_power_telemetry, args.entry_power_stage, args.log_export)
    except ValueError as error:
        parser.error(str(error))
    root = args.edk2.resolve()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    if revision != REVISION:
        raise RuntimeError('EDK II revision mismatch')
    args.output.mkdir(parents=True, exist_ok=False)
    prepare_entry_power(root, upl_power,
                        args.entry_power_stage if args.entry_power_stage in ('entry','dxe-ready') else 'entry',
                        short_marker=args.entry_short_marker,
                        short_marker_stage=args.entry_short_marker_stage)
    # lab --warm-probe 31/32/35/36: ICC off in the payload entry after a warm re-entry (ps5-warm-probe.h); DxeMain.c is put back
    # to the EDK2 git state on every build (the DXE probes 33/34/39 are inserted after prepare_dxe_power, further below)
    subprocess.run(['git', '-C', str(root), 'checkout', '--', 'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c'], check=True)
    if args.warm_probe in (31, 32, 35, 36):
        inc = '#include "%s"\n' % (HERE / 'ps5-warm-probe.h')
        if args.warm_probe in (31, 32, 35, 36):
            ep = root / ENTRY_DIRECTORY / 'UniversalPayloadEntry.c'; t = ep.read_text()
            a = {31: '  mHobList = (VOID *)BootloaderParameter;\n', 35: '  ProcessLibraryConstructorList ();\n',
                 36: '  Status = BuildHobs (BootloaderParameter, &DxeFv);\n', 32: '  HandOffToDxeCore (DxeCoreEntryPoint, Hob);\n'}[args.warm_probe]
            if t.count(a) != 1: raise RuntimeError('warm probe anchor missing in UniversalPayloadEntry.c')
            t = t.replace(a, ('  Ps5WarmProbeOff ();\n' + a) if args.warm_probe == 32 else (a + '  Ps5WarmProbeOff ();\n'), 1)
            ep.write_text(inc + t)
    if args.warm_reset_test:   # lab: bars 7-9 in the payload entry (entry, after the HOBs, before DXE) during a warm re-entry
        ep = root / ENTRY_DIRECTORY / 'UniversalPayloadEntry.c'; t = ep.read_text()
        helper = """
STATIC VOID Ps5WarmBar (UINTN N, UINT32 Color) {
  volatile UINT64 *W = (volatile UINT64 *)(UINTN)0x111806000ULL;
  if (W[0] != 0x59525445524D5257ULL || W[2] == 0) return;
  volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)W[2];
  for (UINTN Y = 0; Y < 16; Y++) for (UINTN X = 0; X < 480; X++) Fb[(N * 24 + Y) * W[3] + X] = Color;
}
"""
        for a, b in (('extern VOID  *mHobList;', 'extern VOID  *mHobList;' + helper),
                     ('  mHobList = (VOID *)BootloaderParameter;', '  mHobList = (VOID *)BootloaderParameter;\n  Ps5WarmBar (7, 0x00FF00FF);'),
                     ('  Status = BuildHobs (BootloaderParameter, &DxeFv);', '  Status = BuildHobs (BootloaderParameter, &DxeFv);\n  Ps5WarmBar (8, 0x008000FF);'),
                     ('  HandOffToDxeCore (DxeCoreEntryPoint, Hob);', '  Ps5WarmBar (9, 0x00808080);\n  HandOffToDxeCore (DxeCoreEntryPoint, Hob);')):
            if t.count(a) != 1: raise RuntimeError('warm bar anchor: ' + a)
            t = t.replace(a, b, 1)
        ep.write_text(t)
    prepare_dxe_power(root, dxe_power, (HERE / 'bootstrap-power.c').read_text(),
                      args.entry_power_stage if dxe_power else 'dxe-entry')
    prepare_cpu_power(root, cpu_power, args.entry_power_stage if cpu_power else "cpu-entry")
    prepare_cpu_apic(root)
    prepare_inherited_timer(root, (HERE / 'Ps5InheritedTimer.c').read_text())
    prepare_warm_soft_tick_cpu(root)
    prepare_nvme_pass_thru_timeout(root)
    prepare_boot_reads(root)
    prepare_boot_snapshots(root)
    prepare_usb_mass_boot(root)
    if args.native_setup: prepare_boot_menu_storage_only(root)   # r5.25.4
    prepare_windows_boot(root, (HERE / 'Ps5BootWindows.c').read_text(),setup_menu=args.native_setup)
    if args.setup_seconds is not None:
        if args.setup_seconds < 1 or args.setup_seconds > 300: parser.error('--setup-seconds must be 1..300')   # r5: below 5 s allowed
        ss_boot=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        ss_boot.write_text('#define PS5_SETUP_SECONDS %d\n' % args.setup_seconds + ss_boot.read_text())
    if args.lab_sandisk:
        lb=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        lb.write_text('#define PS5_LAB_SANDISK 1\n' + lb.read_text())
    if args.warm_ebs_watch:
        args.warm_watchdog = True
        we=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        we.write_text('#define PS5_WARM_EBS_WATCH 1\n' + we.read_text())
    if args.warm_watchdog or args.warm_watchdog_early or args.warm_watchdog_selftest or args.warm_rescue:
        ww=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        ww.write_text(('#define PS5_WARM_RESCUE 1\n' if args.warm_rescue else '') + ('#define PS5_WARM_WATCHDOG_SELFTEST 1\n' if args.warm_watchdog_selftest else '') + ('#define PS5_WARM_WATCHDOG_EARLY 1\n' if (args.warm_watchdog_early or args.warm_watchdog_selftest or args.warm_rescue) else '') + '#define PS5_WARM_WATCHDOG 1\n' + ww.read_text())
    if args.warm_bs_trace:
        wt=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        wt.write_text('#define PS5_WARM_BS_TRACE 1\n' + wt.read_text())
    if args.warm_ebs_steps:
        ws=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        ws.write_text('#define PS5_WARM_EBS_STEPS 1\n' + ws.read_text())
    if args.warm_ebs_mark:
        wm=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        wm.write_text('#define PS5_WARM_EBS_MARK 1\n' + wm.read_text())
    if args.warm_probe:
        wp=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        wp.write_text('#define PS5_WARM_PROBE %d\n' % args.warm_probe + wp.read_text())
    if getattr(args, 'warm_reset_test', False):   # r5.25.4 lab: one self-restart through the warm re-entry
        wr_boot=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        wr_boot.write_text('#define PS5_WARM_RESET_TEST 1\n' + wr_boot.read_text())
    if args.menu_idle_seconds is not None:
        if args.menu_idle_seconds < 0 or args.menu_idle_seconds > 3600: parser.error('--menu-idle-seconds must be 0..3600')
        mi_boot=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        mi_boot.write_text('#define PS5_MENU_IDLE_S %d\n' % args.menu_idle_seconds + mi_boot.read_text())
    if args.m2_overlay:
        m2_boot=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        m2_boot.write_text('#define PS5_M2_OVERLAY_N4 1\n'+m2_boot.read_text())
    if args.nvme_preinit:
        nv_dir=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'
        nv_boot=nv_dir/'Ps5BootWindows.c'
        nv_boot.write_text('#define PS5_NVME_PREINIT 1\n'+('#define PS5_NVME_PREINIT_APPLY 1\n' if args.nvme_preinit=='apply' else '#define PS5_NVME_PREINIT_PROBE 1\n' if args.nvme_preinit=='probe' else '')+nv_boot.read_text())
        nv_inf=nv_dir/'PlatformBootManagerLib.inf'; inf_text=nv_inf.read_text()
        if inf_text.count('[LibraryClasses]\n')!=1: raise RuntimeError('Unexpected PlatformBootManagerLib [LibraryClasses] anchor')
        nv_inf.write_text(inf_text.replace('[LibraryClasses]\n','[LibraryClasses]\n  PciExpressLib\n  IoLib\n'))
    if args.native_atom_library:
        atom_destination=root/'UefiPayloadPkg/Library/PlatformBootManagerLib'
        for atom_file in ('atom-context.h','atom-bits.h','atom-native.h','uefi-compat.h','uefi-main.h'):
            shutil.copy2(HERE/'atom-host'/atom_file,atom_destination/atom_file)
        (atom_destination/'atom-native.c').write_text('#define PS5_ATOM_EDK2 1\n'+(HERE/'atom-host/atom-host.c').read_text())
        atom_inf=atom_destination/'PlatformBootManagerLib.inf'
        atom_inf.write_text(atom_inf.read_text().replace('  Ps5BootWindows.c\n','  Ps5BootWindows.c\n  atom-native.c\n'))
        if args.native_dp_source:
            for name in ('ps5-native-dp-source.h','dcn201-link.h','dcn201-link.c','dcn201-test-pattern.h'):
                shutil.copy2(HERE/name,atom_destination/name)
            boot=atom_destination/'Ps5BootWindows.c'
            boot.write_text('#define PS5_NATIVE_DP_SOURCE 1\n'+boot.read_text())
            atom_inf.write_text(atom_inf.read_text().replace('  atom-native.c\n','  atom-native.c\n  dcn201-link.c\n'))
            if args.native_dp_video:
                for name in ('ps5-native-dp-video.h','ps5-hdmi-mode-var.h','ps5-hdmi-video-protocol.h','dcn201-stream.h','dcn201-stream.c','dcn201-timing.h','dcn201-timing.c','dcn201-test-pattern.c','dcn201-aux.h','dcn201-aux.c','dcn201-pipe-replay.h','ps5-native-gop.h',
                             'dcn201-mode-replay.h','dcn201-dispclk.h','ps5-display-mode.h','ps5-display-mode.c','flava3-mode.h','flava3-manual.h','icc-power.h'):
                    shutil.copy2(HERE/name,atom_destination/name)
                for mode in args.display_replay:
                    shutil.copy2(HERE/f'dcn201-pipe-replay-{mode}.h',atom_destination/f'dcn201-pipe-replay-{mode}.h')
                boot.write_text('#define PS5_NATIVE_DP_VIDEO 1\n#define PS5_NATIVE_DP_SCANOUT 1\n'+
                                f'#define PS5_DISPLAY_POLICY {args.display_policy}\n#define PS5_DISPCLK_RAISE {args.dispclk_raise}\n'+(f'#define PS5_DISPLAY_FORCE PS5_MODE_{args.display_force.upper()}\n' if args.display_force else '')+''.join(f'#define PS5_REPLAY_{m.upper()} 1\n' for m in args.display_replay)+boot.read_text())
                if args.diag_graphics:
                    boot.write_text('#define PS5_DIAG_GRAPHICS 1\n'+boot.read_text())
                atom_inf.write_text(atom_inf.read_text().replace('  dcn201-link.c\n','  dcn201-link.c\n  dcn201-stream.c\n  dcn201-timing.c\n  dcn201-test-pattern.c\n  dcn201-aux.c\n  ps5-display-mode.c\n'))

    prepare_exception_power(root, args.exception_power_telemetry, (HERE / 'bootstrap-power.c').read_text())
    if args.warm_reset_test:   # lab: an unhandled exception during a warm re-entry paints its vector and RIP as bit rows
        ex = root / 'UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeException.c'; t = ex.read_text()
        a = '  if (((UINTN)ExceptionType < X86_CPU_INTERRUPT_NUM) &&' if args.exception_power_telemetry else '  CommonExceptionHandlerWorker (ExceptionType, SystemContext, &mExceptionHandlerData);'   # before the telemetry dead loop
        paint = """  { volatile UINT64 *W = (volatile UINT64 *)(UINTN)0x111806000ULL;
    if (W[0] == 0x59525445524D5257ULL && W[2] != 0) {
      volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)W[2]; UINT64 S = W[3], Rip = SystemContext.SystemContextX64->Rip;
      for (UINTN B = 0; B < 64; B++) for (UINTN Y = 0; Y < 12; Y++) for (UINTN X = 0; X < 12; X++) {
        Fb[(520 + Y) * S + 20 + B * 16 + X] = (B < 8) ? (((UINT64)ExceptionType >> (7 - B)) & 1 ? 0x00FFFF00 : 0x00402000) : 0x00000000;
        Fb[(540 + Y) * S + 20 + B * 16 + X] = ((Rip >> (63 - B)) & 1) ? 0x00FFFFFF : 0x00400000;
      }
    } }
"""
        if t.count(a) != 1: raise RuntimeError('exception paint anchor')
        ex.write_text(t.replace(a, paint + a, 1))
    prepare_x64_handoff(root)
    prepare_x64_page_tables(root)
    subprocess.run(['git', '-C', str(root), 'checkout', '--', 'MdeModulePkg/Core/Dxe/Image/Image.c'], check=True)
    if args.warm_probe_image:   # lab: ICC off when the K-th DXE image starts after a warm re-entry (dispatch order = cold-boot log)
        im = root / 'MdeModulePkg/Core/Dxe/Image/Image.c'; t = im.read_text()
        a = '  Image = CoreLoadedImageInfo (ImageHandle);\n  if ((Image == NULL) ||  Image->Started) {\n    return EFI_INVALID_PARAMETER;\n  }\n'
        if t.count(a) != 1: raise RuntimeError('warm probe image anchor missing in Image.c')
        t = t.replace(a, a + '  { STATIC UINTN Ps5N; if (++Ps5N == %dU) Ps5WarmProbeOff (); }\n' % args.warm_probe_image, 1)
        im.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    if args.warm_probe in (33, 34, 39):   # after prepare_dxe_power, which regenerates DxeMain.c (HW 19.11.: probes 33/39 were wiped)
        dm = root / 'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c'; t = dm.read_text()
        a = {33: '  InitializeDebugAgent (DEBUG_AGENT_INIT_DXE_CORE, HobStart, NULL);\n', 34: '  CoreDispatcher ();\n',
             39: '  VectorInfoList = NULL;\n'}[args.warm_probe]
        if t.count(a) != 1: raise RuntimeError('warm probe anchor missing in DxeMain.c')
        t = t.replace(a, ('  Ps5WarmProbeOff ();\n' + a) if args.warm_probe == 39 else (a + '  Ps5WarmProbeOff ();\n'), 1)
        dm.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    prepare_warm_soft_tick_timer(root)   # r5.25.5: regenerates LocalApicTimerDxe.c/.inf from HEAD (replaces the checkout)
    if args.warm_ebs_steps:
        lt = root / 'OvmfPkg/LocalApicTimerDxe/LocalApicTimerDxe.c'
        lt.write_text('#define PS5_WARM_EBS_STEPS 1\n' + lt.read_text())
    for rel in ('FatPkg/EnhancedFatDxe/Misc.c', 'MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c'):   # lab --warm-ebs-steps edits these: always from HEAD
        (root / rel).write_text(subprocess.check_output(['git', 'show', 'HEAD:' + rel], cwd=root, text=True))
    if args.warm_ebs_steps:
        inc = '#include "%s"\n' % (HERE / 'ps5-warm-probe.h')
        def ps5_steps(rel, pairs):
            f = root / rel; t = f.read_text()
            for a, add, before in pairs:
                if t.count(a) != 1: raise RuntimeError('warm ebs steps anchor missing in ' + rel + ': ' + a[:40])
                t = t.replace(a, (add + a) if before else (a + add), 1)
            f.write_text(inc + t)
        ps5_steps('FatPkg/EnhancedFatDxe/Misc.c', [
            ('  Volume = (FAT_VOLUME *)Context;\n  if ((Volume == NULL) || (Volume->Signature != FAT_VOLUME_SIGNATURE)) {\n', '  Ps5WarmSq (0, 0x0000FF00);\n', True),
            ('  Status = FatFlushDirtyCache (Volume, NULL);\n', '  Ps5WarmSq (1, 0x00FFFF00);\n', False)])
        ps5_steps('MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c', [
            ('  gTimer->SetTimerPeriod (gTimer, 0);\n', '  Ps5WarmSq (2, 0x000000FF);\n', False),
            ('  gMemoryMapTerminated = TRUE;\n', '  Ps5WarmSq (3, 0x0000FFFF);\n', False),
            ('    CoreNotifySignalList (&gEventExitBootServicesFailedGuid);\n', '    Ps5WarmSq (4, 0x00FF8000);\n', True)])
        ps5_steps('MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c', [
            ('  gBS->SetTimer (Xhc->PollTimer, TimerCancel, 0);\n  XhcHaltHC (Xhc, XHC_GENERIC_TIMEOUT);\n', '  Ps5WarmSq (5, 0x00A000FF);\n', True),
            ('  XhcHaltHC (Xhc, XHC_GENERIC_TIMEOUT);\n\n  if (Xhc->PollTimer != NULL) {\n', '  Ps5WarmSq (6, 0x00808080);\n', False),
            ('  XhcClearBiosOwnership (Xhc);\n\n  //\n  // Restore original PCI attributes\n', '  Ps5WarmSq (7, 0x00FF80C0);\n', False)])
    mp = root / 'UefiCpuPkg/Library/MpInitLib/MpLib.c'
    mp.write_text(subprocess.check_output(['git', 'show', 'HEAD:UefiCpuPkg/Library/MpInitLib/MpLib.c'], cwd=root, text=True))
    if args.warm_probe in (75, 76):   # lab: winload's StartupAllAPs (wbinvd on all APs, blocking, no timeout)
        t = mp.read_text()
        fn = 'EFI_STATUS\nStartupAllCPUsWorker (\n'
        a1 = '  BOOLEAN      HasEnabledAp;\n  CPU_STATE    ApState;\n\n  CpuMpData = GetCpuMpData ();\n'
        a2 = ('  Status = EFI_SUCCESS;\n  if (WaitEvent == NULL) {\n    do {\n      Status = CheckAllAPs ();\n'
              '    } while (Status == EFI_NOT_READY);\n  }\n\n  return Status;\n}\n')
        if t.count(fn) != 1 or t.count(a1) != 1 or t.count(a2) != 1: raise RuntimeError('warm probe 75/76 anchors missing in MpLib.c')
        helper = r'''/* lab: is Procedure inside winload (PE subsystem 16 = Windows boot application)? */
STATIC BOOLEAN Ps5ProcIsWinload (VOID *Procedure)
{
  UINTN P = (UINTN)Procedure & ~(UINTN)0xFFF;
  for (UINTN N = 0; N < 1024 && P >= 0x100000; N++, P -= 0x1000) {
    if (*(volatile UINT16 *)P != 0x5A4D) continue;
    UINT32 Lfa = *(volatile UINT32 *)(P + 0x3C);
    if (Lfa >= 0xF00 || *(volatile UINT32 *)(P + Lfa) != 0x00004550) return FALSE;
    return *(volatile UINT16 *)(P + Lfa + 0x18 + 0x44) == 16;
  }
  return FALSE;
}
STATIC BOOLEAN Ps5ProbeWarm (VOID) { return *(volatile UINT64 *)(UINTN)0x1118061B0ULL == 0x314E555257575350ULL; }
STATIC VOID Ps5ProbeOffNow (VOID) { *(volatile UINT64 *)(UINTN)0x111806040ULL = 0x314D524157575350ULL; Ps5WarmProbeOff (); }

'''
        t = t.replace(fn, helper + fn, 1)
        if args.warm_probe == 76:
            t = t.replace(a1, a1 + '  if (Ps5ProbeWarm () && Ps5ProcIsWinload ((VOID *)Procedure)) Ps5ProbeOffNow ();   /* lab 76 */\n', 1)
        else:
            t = t.replace(a1, a1 + '  BOOLEAN Ps5Wl = Ps5ProbeWarm () && Ps5ProcIsWinload ((VOID *)Procedure); UINT64 Ps5T0 = AsmReadTsc ();   /* lab 75 */\n', 1)
            t = t.replace(a2, ('  Status = EFI_SUCCESS;\n  if (WaitEvent == NULL) {\n    do {\n      Status = CheckAllAPs ();\n'
                               '      if (Ps5Wl && (AsmReadTsc () - Ps5T0 > 120ULL * 1596300000ULL)) Ps5ProbeOffNow ();   /* lab 75: stuck waiting */\n'
                               '    } while (Status == EFI_NOT_READY);\n  }\n\n'
                               '  if (Ps5Wl) Ps5ProbeOffNow ();   /* lab 75: came back */\n  return Status;\n}\n'), 1)
        mp.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    if args.warm_probe in (68, 69):   # lab: first entry of the local-APIC timer interrupt handler after a warm re-entry (69: entered by int 0x20)
        lt = root / 'OvmfPkg/LocalApicTimerDxe/LocalApicTimerDxe.c'; t = lt.read_text()
        a = '  OriginalTPL = NestedInterruptRaiseTPL ();\n'
        if t.count(a) != 1: raise RuntimeError('warm probe 68 anchor missing in LocalApicTimerDxe.c')
        t = t.replace(a, '  { STATIC BOOLEAN Ps5First; if (!Ps5First) { Ps5First = TRUE; Ps5WarmProbeOff (); } }\n' + a, 1)
        lt.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    if args.warm_timer_storm:   # lab: timer storm
        lt = root / 'OvmfPkg/LocalApicTimerDxe/LocalApicTimerDxe.c'; t = lt.read_text()
        a = '  OriginalTPL = NestedInterruptRaiseTPL ();\n'
        if t.count(a) != 1: raise RuntimeError('warm timer storm anchor missing in LocalApicTimerDxe.c')
        t = t.replace(a, '  { STATIC UINT64 Ps5T0; STATIC UINT32 Ps5Ticks; if (Ps5Ticks++ == 0) Ps5T0 = AsmReadTsc (); else if (Ps5Ticks == 200 && AsmReadTsc () - Ps5T0 < 1596300000ULL / 2) Ps5WarmProbeOff (); }\n' + a, 1)
        lt.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    if args.warm_probe == 54:   # lab: the 200th local-APIC timer interrupt after a warm re-entry switches off (the timer keeps running)
        lt = root / 'OvmfPkg/LocalApicTimerDxe/LocalApicTimerDxe.c'; t = lt.read_text()
        a = '  SendApicEoi ();\n\n  if (mTimerNotifyFunction != NULL) {\n'
        if t.count(a) != 1: raise RuntimeError('warm probe 54 anchor missing in LocalApicTimerDxe.c')
        t = t.replace(a, '  SendApicEoi ();\n  { STATIC UINTN Ps5Ticks; if (++Ps5Ticks == 200) Ps5WarmProbeOff (); }\n\n  if (mTimerNotifyFunction != NULL) {\n', 1)
        lt.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    if args.warm_probe == 52:   # lab: 8th device interrupt without a handler (below the flood guard's 16) after a warm re-entry -> off after (vector / 32 + 1) * 10 s
        de = root / 'UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeException.c'; t = de.read_text()
        a = '    UINT64 Ps5ApicBase = AsmReadMsr64 (0x1B);\n'
        if t.count(a) != 1: raise RuntimeError('warm probe 52 needs the stray-interrupt hook')
        t = t.replace(a, '    { STATIC UINT32 Ps5WarmStray; if (++Ps5WarmStray == 8) Ps5WarmProbeVectorOff ((unsigned)ExceptionType >> 5); }\n' + a, 1)
        de.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    if args.warm_probe == 40:   # lab: unhandled DXE exception after a warm re-entry -> off after (vector + 1) * 10 s
        de = root / 'UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeException.c'; t = de.read_text()
        a = '    DisableInterrupts ();\n    if (InterlockedCompareExchange32 (&mPs5FaultReporting, 0, 1) == 0) {\n'
        if t.count(a) != 1: raise RuntimeError('warm probe 40 needs --exception-power-telemetry (hook anchor missing)')
        t = t.replace(a, '    DisableInterrupts ();\n    Ps5WarmProbeVectorOff ((unsigned)ExceptionType);\n' + a[len('    DisableInterrupts ();\n'):], 1)
        de.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    if args.warm_probe in (37, 38, 41):   # lab: ICC off in HandOffToDxeCore after a warm re-entry (after the regeneration above)
        hf = root / 'UefiPayloadPkg/UefiPayloadEntry/X64/DxeLoadFunc.c'; t = hf.read_text()
        a = {38: '    AsmWriteCr3 (PageTables);\n', 37: '  if (FeaturePcdGet (PcdDxeIplBuildPageTables)) {\n    AsmWriteCr3 (PageTables);\n',
             41: '  UpdateStackHob ((EFI_PHYSICAL_ADDRESS)(UINTN)BaseOfStack, STACK_SIZE);\n'}[args.warm_probe]
        if t.count(a) != 1: raise RuntimeError('warm probe anchor missing in DxeLoadFunc.c')
        t = t.replace(a, ('  Ps5WarmProbeOff ();\n' + a) if args.warm_probe == 37 else (a + ('    ' if args.warm_probe == 38 else '  ') + 'Ps5WarmProbeOff ();\n'), 1)
        hf.write_text('#include "%s"\n' % (HERE / 'ps5-warm-probe.h') + t)
    source = subprocess.check_output(['git', 'show', 'HEAD:UefiPayloadPkg/UefiPayloadPkg.dsc'],
                                     cwd=root, text=True)
    old = 'TimerLib|UefiCpuPkg/Library/CpuTimerLib/BaseCpuTimerLib.inf'
    if source.count(old) != 1:
        raise RuntimeError('Unexpected DSC timer selection')
    if args.native_setup:
        source=source.replace('DEFINE BOOT_MANAGER_ESCAPE          = FALSE','DEFINE BOOT_MANAGER_ESCAPE          = TRUE')
    source = source.replace(old, 'TimerLib|Ps5PlatformPkg/Library/Ps5TimerLib/Ps5TimerLib.inf')
    # All module-specific serial overrides must use the same PS5 register map.
    source, serial_count = re.subn(r'(SerialPortLib\|)[^\r\n]+',
        r'\1Ps5PlatformPkg/Library/Ps5SerialPortLib/Ps5SerialPortLib.inf', source)
    if serial_count != 6:
        raise RuntimeError(f'Unexpected serial library selections: {serial_count}')
    source = source.replace('PlatformHookLib|UefiPayloadPkg/Library/PlatformHookLib/PlatformHookLib.inf',
        'PlatformHookLib|MdeModulePkg/Library/BasePlatformHookLibNull/BasePlatformHookLibNull.inf')
    source = source.replace('PciExpressLib|MdePkg/Library/BasePciExpressLib/BasePciExpressLib.inf',
        'PciExpressLib|Ps5PlatformPkg/Library/Ps5PciExpressLib/Ps5PciExpressLib.inf')
    source = source.replace('PciSegmentLib|MdePkg/Library/PciSegmentLibSegmentInfo/BasePciSegmentLibSegmentInfo.inf',
        'PciSegmentLib|MdePkg/Library/BasePciSegmentLibPci/BasePciSegmentLibPci.inf')
    source = source.replace('ResetSystemLib|UefiPayloadPkg/Library/ResetSystemLib/ResetSystemLib.inf',
        'ResetSystemLib|Ps5PlatformPkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.inf')
    source = replace_rtc(source)
    # Diagnostic: drop DEBUG_INFO (0x40) from the global print level so our verbose
    # PartitionDxe/GptLib GPT output does not flood the 64KB log ring during
    # bootmgfw's ConnectController enumeration, leaving room for bootmgfw's captured
    # console lines (promoted to DEBUG_ERROR in the WindowsConsoleTap).
    if 'PcdDebugPrintErrorLevel|0x8000004F' not in source:
        raise RuntimeError('Unexpected PcdDebugPrintErrorLevel in DSC')
    if not args.lab_debug_info:   # lab: keep DEBUG_INFO
        source = source.replace('PcdDebugPrintErrorLevel|0x8000004F', 'PcdDebugPrintErrorLevel|0x8000000F')
    # The original DSC is retained. Keep its FDF paths and output layout intact.
    package = root / 'Ps5PlatformPkg'
    shutil.copytree(HERE / 'Ps5PlatformPkg', package, dirs_exist_ok=True)
    shutil.copy2(HERE / 'ps5-time-calendar.h', package / 'Ps5RtcRuntimeDxe/ps5-time-calendar.h')
    prepare_log_power(package, log_power)
    for name in ('pci-transport.c', 'pci-transport.h'):
        shutil.copy2(HERE / name, package / 'Library/Ps5PciExpressLib' / name)
    for name in ('icc-power.c', 'icc-power.h'):
        shutil.copy2(HERE / name, package / 'Library/Ps5ResetSystemLib' / name)
    if args.warm_reset_test:   # lab: squares per log line during the re-entry
        sl = package / 'Library/Ps5SerialPortLib/Ps5SerialPortLib.c'
        sl.write_text('#define PS5_WARM_RESET_TEST 1\n' + sl.read_text())
    if args.warm_reentry or args.warm_reset_test:   # r5.25.4: Windows restart -> PSWloader start screen (warm re-entry)
        rl = package / 'Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c'
        rl.write_text('#define PS5_WARM_REENTRY 1\n' + rl.read_text())
    if args.warm_probe:
        rl = package / 'Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c'
        rl.write_text('#define PS5_WARM_PROBE %d\n' % args.warm_probe + rl.read_text())
    if args.warm_no_button_poll:
        hp = package / 'HdmiProbeDxe/HdmiProbeDxe.c'
        hp.write_text('#define PS5_WARM_NO_BUTTON_POLL 1\n' + hp.read_text())
    if args.warm_probe in (53, 55, 56):   # lab: ButtonPoll (HdmiProbeDxe) slower than 0.5 s / entered / returned after a warm re-entry
        hp = package / 'HdmiProbeDxe/HdmiProbeDxe.c'
        hp.write_text('#define PS5_WARM_PROBE %d\n' % args.warm_probe + hp.read_text())
        shutil.copy2(HERE / 'ps5-warm-probe.h', package / 'HdmiProbeDxe' / 'ps5-warm-probe.h')
    if args.warm_keep_gpu:
        rl = package / 'Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c'
        rl.write_text('#define PS5_WARM_KEEP_GPU 1\n' + rl.read_text())
    if args.warm_rescue:
        rl = package / 'Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c'
        rl.write_text('#define PS5_WARM_RESCUE 1\n' + rl.read_text())
        le = package / 'LogExportDxe/LogExportDxe.c'
        le.write_text('#define PS5_WARM_RESCUE 1\n' + le.read_text())
        shutil.copy2(HERE / 'ps5-warm-probe.h', package / 'LogExportDxe' / 'ps5-warm-probe.h')
    if args.warm_off_test:
        rl = package / 'Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c'
        rl.write_text('#define PS5_WARM_OFF_TEST 1\n' + rl.read_text())
    if args.warm_spin_test:
        rl = package / 'Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c'
        rl.write_text('#define PS5_WARM_SPIN_TEST 1\n' + rl.read_text())
    if args.hdmi_probe:
        module = 'Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.inf'
        anchor = '  UefiPayloadPkg/BlSupportDxe/BlSupportDxe.inf'
        if source.count(anchor) != 1:
            raise RuntimeError('Unexpected DSC HDMI probe insertion point')
        source = source.replace(anchor, anchor + '\n  ' + module)
        fdf = subprocess.check_output(['git', 'show', 'HEAD:UefiPayloadPkg/UefiPayloadPkg.fdf'], cwd=root, text=True)
        anchor = '\nINF UefiPayloadPkg/BlSupportDxe/BlSupportDxe.inf'
        if fdf.count(anchor) != 1:
            raise RuntimeError('Unexpected FDF HDMI probe insertion point')
        fdf = fdf.replace(anchor, anchor + '\nINF ' + module)
        (package / 'Ps5UefiPayload.fdf').write_text(fdf)
        source = source.replace('UefiPayloadPkg/UefiPayloadPkg.fdf', 'Ps5PlatformPkg/Ps5UefiPayload.fdf')
        for stem in ('icc-power', 'icc-hdmi', 'icc-edid', 'hdmi-commands', 'flava3-prepare', 'flava3-video', 'flava3-edid', 'ps5-edid-info', 'button-keys'):
            for suffix in ('.c', '.h'):
                shutil.copy2(HERE / (stem + suffix), package / 'HdmiProbeDxe' / (stem + suffix))
        shutil.copy2(HERE/'ps5-hdmi-video-protocol.h',package/'HdmiProbeDxe/ps5-hdmi-video-protocol.h')
        shutil.copy2(HERE/'ps5-fan.h',package/'HdmiProbeDxe/ps5-fan.h')   # r5.14
        shutil.copy2(HERE/'flava3-mode.h',package/'HdmiProbeDxe/flava3-mode.h')
        if args.hdmi_prepare:
            driver=package/'HdmiProbeDxe/HdmiProbeDxe.c'
            driver.write_text('#define PS5_HDMI_PREPARE 1\n'+('#define PS5_HDMI_EDID_REQUEST 1\n' if args.hdmi_edid_request else '')+driver.read_text())
            if args.edid_window_seconds is not None:
                driver.write_text(f'#define PS5_EDID_WINDOW_SECONDS {args.edid_window_seconds}U\n#define PS5_EDID_TIMESTAMPS 1\n'+driver.read_text())
            if args.hdmi_edid_request:
                # edid2: Linux-style inbound notifications for HdmiProbeDxe only (bootstrap and
                # Ps5ResetSystemLib compile icc-power.c without the define, byte-identical to v386b).
                icc=package/'HdmiProbeDxe/icc-power.c'
                icc.write_text('#define PS5_ICC_LINUX_NOTIFY 1\n'+icc.read_text())
    if args.log_export:
        module = 'Ps5PlatformPkg/LogExportDxe/LogExportDxe.inf'
        anchor = '  UefiPayloadPkg/BlSupportDxe/BlSupportDxe.inf'
        if source.count(anchor) != 1:
            raise RuntimeError('Unexpected DSC log insertion point')
        source = source.replace(anchor, anchor + '\n  ' + module)
        fdf_path = package / 'Ps5UefiPayload.fdf'
        fdf = fdf_path.read_text() if args.hdmi_probe else subprocess.check_output(
            ['git', 'show', 'HEAD:UefiPayloadPkg/UefiPayloadPkg.fdf'], cwd=root, text=True)
        anchor = '\nINF UefiPayloadPkg/BlSupportDxe/BlSupportDxe.inf'
        if fdf.count(anchor) != 1:
            raise RuntimeError('Unexpected FDF log insertion point')
        fdf_path.write_text(fdf.replace(anchor, anchor + '\nINF ' + module))
        source = source.replace('UefiPayloadPkg/UefiPayloadPkg.fdf', 'Ps5PlatformPkg/Ps5UefiPayload.fdf')
    fdf_path = package / 'Ps5UefiPayload.fdf'
    fdf = fdf_path.read_text() if args.hdmi_probe or args.log_export else subprocess.check_output(
        ['git', 'show', 'HEAD:UefiPayloadPkg/UefiPayloadPkg.fdf'], cwd=root, text=True)
    fdf_path.write_text(replace_rtc(fdf))
    source = source.replace('UefiPayloadPkg/UefiPayloadPkg.fdf', 'Ps5PlatformPkg/Ps5UefiPayload.fdf')
    if args.lvgl_ui:
        lvgl_dst = root / 'LvglPkg'
        if lvgl_dst.exists(): shutil.rmtree(lvgl_dst)
        shutil.copytree(args.lvgl_pkg, lvgl_dst, ignore=shutil.ignore_patterns('.git', 'docs'))
        old = '  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf'
        if source.count(old) != 1: raise RuntimeError('Unexpected DSC DisplayEngineDxe')
        source = source.replace(old, '  LvglPkg/LvglDisplayEngineDxe/LvglDisplayEngineDxe.inf\n  LvglPkg/LvglSetupDxe/LvglSetupDxe.inf\n  Ps5PlatformPkg/Ps5InfoDxe/Ps5InfoDxe.inf')
        anchor = '[LibraryClasses]\n'
        if source.count(anchor) < 1: raise RuntimeError('Unexpected DSC LibraryClasses')
        source = source.replace(anchor, anchor + '  LvglLib|LvglPkg/Library/LvglLib/LvglLib.inf\n  LvglThemeLib|LvglPkg/Library/LvglThemeLib/LvglThemeLib.inf\n'
                                '  LvglUiConfigLib|LvglPkg/Library/LvglUiConfigLib/LvglUiConfigLib.inf\n', 1)
        source += ('\n[PcdsFixedAtBuild]\n  gLvglPkgTokenSpaceGuid.PcdLvglAptioHeaderTitle|"UEFI Boot Manager"\n'
                   '  gLvglPkgTokenSpaceGuid.PcdLvglAptioHeaderVendor|"PSWloader v1.0.1"\n'
                   # r5.24 (user: one visible name): system table FirmwareVendor + version string (UiApp, Windows)
                   '  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"PSWloader"\n'
                   '  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"PSWloader v1.0.1"\n'
                   # r5.7 (menu test 16.11.): safe inner margin (TV/capture overscan cut ~15-20 px on the left)
                   '  gLvglPkgTokenSpaceGuid.PcdLvglCenteredFrameEnabled|TRUE\n'
                   '  gLvglPkgTokenSpaceGuid.PcdLvglCenteredFrameHeightPct|94\n'
                   # r5.7: menu languages English (default) + Deutsch (UiApp language list, PlatformLang check)
                   '  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultPlatformLangCodes|"en-US;de-DE"\n'
                   '  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultLangCodes|"engdeu"\n')
        # r5.7: chrome tweaks on the copied LvglPkg (the checkout stays untouched): clock only with a real time source
        # (BDS sets volatile Ps5TimeValid=1 after an accepted UTC seed), footer chip for the power button.
        chrome = lvgl_dst / 'LvglDisplayEngineDxe/LvglAptioChrome.c'; c = chrome.read_text()
        old_clock = '  Status = gRT->GetTime (&Time, NULL);\n  if (EFI_ERROR (Status)) {\n    lv_label_set_text (mClockLabel, "--:--:--");\n    return;\n  }\n'
        new_clock = ('  {\n    STATIC EFI_GUID Ps5Guid = {0x5efc5ae0,0xb753,0x4f7a,{0xa6,0x62,0xe3,0x4d,0x15,0xa6,0xd8,0xe5}};\n'
                     '    UINT32 Valid = 0; UINTN VSize = sizeof (Valid);\n'
                     '    if (EFI_ERROR (gRT->GetVariable (L"Ps5TimeValid", &Ps5Guid, NULL, &VSize, &Valid)) || Valid != 1) {\n'
                     '      lv_label_set_text (mClockLabel, "Time not set");\n      return;\n    }\n  }\n' + old_clock)
        old_foot = '  AddHotKeyChip (Bar, "Esc",   "Exit");\n'
        if c.count(old_clock) != 1 or c.count(old_foot) != 1: raise RuntimeError('Unexpected LvglAptioChrome.c for r5.7 tweaks')
        c = c.replace(old_clock, new_clock).replace(old_foot, old_foot + '  AddHotKeyChip (Bar, "Power", "short: next  long: OK  double: back");\n'
                                                                       '  AddHotKeyChip (Bar, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "tabs");\n')   # r5.22
        # r5.8: tab strip in the subtitle bar (active tab in the accent colour, underlined); r5.22: the six PSWloader tabs
        # in the order of lvgl-tabs.py (Overview, Performance & Fan, Display, Boot, Diagnostics, Save & Exit)
        old_sub = '  Label = lv_label_create (Bar);\n  lv_label_set_text (Label, Utf8 != NULL ? Utf8 : "Setup");\n'
        new_sub = ('  {\n'
                   '    STATIC CONST UINT8 TabId[6] = { 0x01, 0x0a, 0x10, 0x03, 0x12, 0x0c };\n'
                   '    STATIC CONST EFI_GUID Base = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x00}};\n'
                   '    STATIC EFI_GUID GlobalVar = {0x8BE4DF61,0x93CA,0x11d2,{0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}};\n'
                   '    STATIC CONST CHAR8 *En[6] = { "Overview", "Performance & Fan", "Display", "Boot", "Diagnostics", "Save & Exit" };\n'
                   '    STATIC CONST CHAR8 *De[6] = { "\\xc3\\x9c" "bersicht", "Leistung & L" "\\xc3\\xbc" "fter", "Anzeige", "Start", "Diagnose", "Speichern & Beenden" };\n'
                   '    INTN Active = -1;\n'
                   '    if ((FormData != NULL) && (CompareMem (&FormData->FormSetGuid, &Base, 15) == 0)) {\n'
                   '      for (UINTN T = 0; T < 6; T++) { if (((UINT8 *)&FormData->FormSetGuid)[15] == TabId[T]) { Active = (INTN)T; } }\n'
                   '    }\n'
                   '    if (Active >= 0) {\n'
                   '      CHAR8 Lang[8]; UINTN Ls = sizeof (Lang) - 1; BOOLEAN Ger;\n'
                   '      ZeroMem (Lang, sizeof (Lang));\n'
                   '      Ger = !EFI_ERROR (gRT->GetVariable (L"PlatformLang", &GlobalVar, NULL, &Ls, Lang)) && (Lang[0] == \'d\');\n'
                   '      lv_obj_set_flex_flow (Bar, LV_FLEX_FLOW_ROW);\n'
                   '      lv_obj_set_flex_align (Bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);\n'
                   '      lv_obj_set_style_pad_column (Bar, 32, 0);\n'
                   '      for (UINTN T = 0; T < 6; T++) {\n'
                   '        lv_obj_t *Tab = lv_label_create (Bar);\n'
                   '        lv_label_set_text (Tab, Ger ? De[T] : En[T]);\n'
                   '        THEME_APPLY_BODY_FONT (Tab);\n'
                   '        lv_obj_set_style_text_color (Tab, lv_color_hex (((INTN)T == Active) ? 0x3C9BFF : THEME_COLOR_SUBTITLE_TEXT), 0);\n'
                   '        if ((INTN)T == Active) {\n'
                   '          lv_obj_set_style_border_side (Tab, LV_BORDER_SIDE_BOTTOM, 0);\n'
                   '          lv_obj_set_style_border_width (Tab, 2, 0);\n'
                   '          lv_obj_set_style_border_color (Tab, lv_color_hex (0x3C9BFF), 0);\n'
                   '        }\n'
                   '      }\n'
                   '      if (Utf8 != NULL) { FreePool (Utf8); }\n'
                   '      return;\n'
                   '    }\n'
                   '  }\n' + old_sub)
        if c.count(old_sub) != 1: raise RuntimeError('Unexpected LvglAptioChrome.c subtitle for r5.8 tabs')
        c = c.replace(old_sub, new_sub)
        chrome.write_text(c)
        # r5.8: Montserrat with Latin-1 (umlauts for the German menu), regenerated by lv_font_conv with the LVGL options
        for font in sorted((HERE / 'lvgl-fonts').glob('lv_font_montserrat_*.c')):
            shutil.copy2(font, lvgl_dst / 'Library/LvglLib/lvgl/src/font' / font.name)
        # r5.21c: keyboard grab for ordered lists (Boot order): Enter grabs, Up/Down move, Enter drops, Esc restores
        subprocess.run([sys.executable, str(HERE / 'lvgl-ordered-grab.py'), str(lvgl_dst / 'LvglDisplayEngineDxe/LvglFormRenderer.c')], check=True)
        # r5.22: six tabs - Left/Right switch, the setup opens on the first tab (PROPOSAL uefi-gui)
        subprocess.run([sys.executable, str(HERE / 'lvgl-tabs.py'), str(lvgl_dst / 'LvglDisplayEngineDxe/LvglFormRenderer.c')], check=True)
        subprocess.run([sys.executable, str(HERE / 'lvgl-lang.py'), str(lvgl_dst / 'LvglDisplayEngineDxe')], check=True)   # r5.25.3: German renderer texts
        subprocess.run([sys.executable, str(HERE / 'lvgl-keys.py'), str(lvgl_dst / 'LvglDisplayEngineDxe/LvglFormRenderer.c')], check=True)   # r5.25.4: Enter opens, arrows only select
        # r5.22: front page = only the PSWloader tabs. EDK2 Device Manager and Boot Maintenance Manager (a second,
        # ineffective boot order) leave UiApp; the EDK2 Boot Manager (Enter boots at once) leaves the front page and is
        # reached only as "Boot once from ..." from the Boot tab; language/continue/reset items of the front page go.
        for lib in ('MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf', 'MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf'):
            line = '      NULL|' + lib + '\n'
            if source.count(line) != 1: raise RuntimeError('Unexpected UiApp library line ' + lib)
            source = source.replace(line, '')
        for rel in ('MdeModulePkg/Application/UiApp/FrontPageCustomizedUi.c', 'MdeModulePkg/Library/BootManagerUiLib/BootManagerVfr.Vfr',
                    'MdeModulePkg/Library/BootManagerUiLib/BootManagerStrings.uni'):
            (root / rel).write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + rel], cwd=root))
        fp = root / 'MdeModulePkg/Application/UiApp/FrontPageCustomizedUi.c'; t = fp.read_text()
        for call in ('  UiCreateLanguageMenu (HiiHandle, StartOpCodeHandle);\n', '  UiCreateContinueMenu (HiiHandle, StartOpCodeHandle);\n',
                     '  UiCreateResetMenu (HiiHandle, StartOpCodeHandle);\n', '  UiCreateEmptyLine (HiiHandle, StartOpCodeHandle);\n'):
            if t.count(call) < 1: raise RuntimeError('Unexpected FrontPageCustomizedUi.c call ' + call.strip())
            t = t.replace(call, '')
        fp.write_text(t)
        bv = root / 'MdeModulePkg/Library/BootManagerUiLib/BootManagerVfr.Vfr'; t = bv.read_text()
        if t.count('classguid = gEfiIfrFrontPageGuid,') != 1: raise RuntimeError('Unexpected BootManagerVfr classguid')
        bv.write_text(t.replace('classguid = gEfiIfrFrontPageGuid,', 'classguid = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x20}},   // r5.22: not on the front page'))
        bs = root / 'MdeModulePkg/Library/BootManagerUiLib/BootManagerStrings.uni'; raw = bs.read_bytes()
        t = raw.decode('utf-8-sig') if raw[:3] == b'\xef\xbb\xbf' else raw.decode('utf-8')
        old_b = '#string STR_BM_BANNER                  #language en-US  "Boot Manager"'
        if t.count(old_b) != 1: raise RuntimeError('Unexpected BootManagerStrings.uni banner')
        t = t.replace(old_b, '#string STR_BM_BANNER                  #language en-US  "Boot once from ..."')
        bs.write_bytes((b'\xef\xbb\xbf' if raw[:3] == b'\xef\xbb\xbf' else b'') + t.encode('utf-8'))
        # r5.21 (user: no mouse in the setup): the LVGL display engine draws a pointer only for EFI_ABSOLUTE_POINTER_PROTOCOL,
        # which UsbMouseAbsolutePointerDxe provides; UsbMouseDxe (simple pointer only) is replaced, both would bind the same mouse
        m_old = 'MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf'
        m_new = 'MdeModulePkg/Bus/Usb/UsbMouseAbsolutePointerDxe/UsbMouseAbsolutePointerDxe.inf'
        if source.count(m_old) != 1: raise RuntimeError('Unexpected DSC USB mouse entry')
        source = source.replace(m_old, m_new)
        fdf = fdf_path.read_text()
        if fdf.count(m_old) != 1: raise RuntimeError('Unexpected FDF USB mouse entry')
        fdf_path.write_text(fdf.replace(m_old, m_new))
        fdf = fdf_path.read_text(); old = 'INF MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf'
        if fdf.count(old) != 1: raise RuntimeError('Unexpected FDF DisplayEngineDxe')
        fdf_path.write_text(fdf.replace(old, 'INF LvglPkg/LvglDisplayEngineDxe/LvglDisplayEngineDxe.inf\nINF LvglPkg/LvglSetupDxe/LvglSetupDxe.inf\nINF Ps5PlatformPkg/Ps5InfoDxe/Ps5InfoDxe.inf'))
    usbmass = root / 'MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassImpl.c'
    usbmass.write_text(subprocess.check_output(['git', 'show', 'HEAD:MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassImpl.c'], cwd=root, text=True))
    if args.usb_mass_quiet:
        text = usbmass.read_text()
        anchor = '  UINTN                         Index;\n\n  Status = gBS->OpenProtocol (\n                  Controller,\n                  &gEfiUsbIoProtocolGuid,'
        if text.count(anchor) != 1: raise RuntimeError('Unexpected UsbMassImpl.c Supported() anchor (%d)' % text.count(anchor))
        guard = ('  UINTN                         Index;\n'
                 '  {  /* PS5 r5 --usb-mass-quiet: marker installed by the BDS once the internal NVMe ESP is selected */\n'
                 '    STATIC EFI_GUID  Ps5UsbMassStop = {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x05}};\n'
                 '    VOID             *Ps5Mark = NULL;\n'
                 '    if (!EFI_ERROR (gBS->LocateProtocol (&Ps5UsbMassStop, NULL, &Ps5Mark))) return EFI_UNSUPPORTED;\n'
                 '  }\n\n  Status = gBS->OpenProtocol (\n                  Controller,\n                  &gEfiUsbIoProtocolGuid,')
        usbmass.write_text(text.replace(anchor, guard))
        bw = root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c'
        bw.write_text('#define PS5_USB_MASS_QUIET 1\n' + bw.read_text())
    if args.boot_ts:
        for ts_file in (root/'UefiPayloadPkg/Library/PlatformBootManagerLib'/'Ps5BootWindows.c', package/'HdmiProbeDxe'/'HdmiProbeDxe.c'):
            if not ts_file.exists(): raise RuntimeError('boot-ts target missing: %s' % ts_file)
            ts_file.write_text('#define PS5_BOOT_TS 1\n' + ts_file.read_text())
    if args.assert_breakpoint:   # lab: a failed ASSERT executes int3 (vector 3 -> fault hook -> probe 40: 40 s) instead of a dead loop
        if source.count('gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2F') != 1: raise RuntimeError('Unexpected DSC debug property mask')
        source = source.replace('gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2F', 'gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x1F')
    dsc = package / 'Ps5UefiPayload.dsc'
    dsc.write_text(source)
    # EDK II reads this from Conf/target.txt, not the environment.
    target = root / 'Conf/target.txt'
    target_text = target.read_text()
    (args.output / 'original-target.txt').write_text(target_text)
    target_text, replaced = re.subn(r'^MAX_CONCURRENT_THREAD_NUMBER\s*=.*$',
                                    'MAX_CONCURRENT_THREAD_NUMBER = 2', target_text, flags=re.M)
    if not replaced:
        target_text += '\nMAX_CONCURRENT_THREAD_NUMBER = 2\n'
    target.write_text(target_text)
    env = os.environ.copy()
    command = ['python3', 'UefiPayloadPkg/UniversalPayloadBuild.py', '-t', 'GCC', '-b', 'DEBUG',
               '-a', 'X64', '-c', 'Ps5PlatformPkg/Ps5UefiPayload.dsc',
               '-D', 'PCIE_BASE_SUPPORT=TRUE', '-D', 'TIMER_SUPPORT=LAPIC',
               '-D', 'DISABLE_RESET_SYSTEM=FALSE', '-D', 'MAX_LOGICAL_PROCESSORS=16',
               '-p', 'gEfiMdePkgTokenSpaceGuid.PcdPciExpressBaseAddress=0xF0000000',
               '-p', 'gEfiMdePkgTokenSpaceGuid.PcdPciExpressBaseSize=0x08000000']
    command += image_comparison
    display_build_input_paths = (boot, atom_inf) if args.native_dp_video else ()
    display_build_input_hashes = {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in display_build_input_paths
    }
    # Arguments are passed as argv, never interpolated into shell source.
    with (args.output / 'build.log').open('w') as log:
        subprocess.run(['bash', '-c',
                        'ps5_build_args=("$@"); set --; source edksetup.sh && exec "${ps5_build_args[@]}"',
                        'ps5-build', *command],
                       cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    display_generated_source_hashes = {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in display_build_input_paths
    }
    if display_build_input_hashes != display_generated_source_hashes:
        raise RuntimeError('Generated display build inputs changed during the EDK2 build')
    binary = args.output / 'UniversalPayload.elf'
    shutil.copy2(root / 'Build/UefiPayloadPkgX64/UniversalPayload.elf', binary)
    manifest = dict(usb_mass_quiet=args.usb_mass_quiet, boot_ts=args.boot_ts, lvgl_ui=args.lvgl_ui, diag_graphics=args.diag_graphics, native_dp_video_enabled=args.native_dp_video, native_dp_video_hashes={name:hashlib.sha256((HERE/name).read_bytes()).hexdigest() for name in ("ps5-native-dp-video.h","ps5-hdmi-video-protocol.h","dcn201-stream.h","dcn201-stream.c","dcn201-timing.h","dcn201-timing.c","dcn201-test-pattern.c","dcn201-aux.h","dcn201-aux.c","dcn201-pipe-replay.h","ps5-native-gop.h")} if args.native_dp_video else {}, native_dp_source_enabled=args.native_dp_source, native_dp_source_hashes={name:hashlib.sha256((HERE/name).read_bytes()).hexdigest() for name in ("ps5-native-dp-source.h","dcn201-link.h","dcn201-link.c","dcn201-test-pattern.h")} if args.native_dp_source else {}, native_atom_library_enabled=args.native_atom_library, native_atom_source_hashes={name:hashlib.sha256((HERE/"atom-host"/name).read_bytes()).hexdigest() for name in ("atom-host.c","atom-context.h","atom-bits.h","atom-native.h","uefi-compat.h","uefi-main.h")} if args.native_atom_library else {}, exception_power_telemetry=args.exception_power_telemetry, experimental_no_image_protection=args.experimental_no_image_protection, edk2_revision=revision, command=command, hdmi_probe_enabled=args.hdmi_probe, hdmi_prepare_enabled=args.hdmi_prepare, log_export_enabled=args.log_export, entry_power_telemetry=args.entry_power_telemetry, entry_power_stage=args.entry_power_stage if args.entry_power_telemetry else None, legacy_pic_ports_disabled=True, preserve_mmio_pat3=True,
                    sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    bytes=binary.stat().st_size, ps5_hardware_boot_verified=False,
                    windows_boot_verified=False,
                    icc_power_hashes={name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                                      for name in ('icc-power.c', 'icc-power.h')},
                    hdmi_source_hashes={name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                                        for name in ('icc-hdmi.c', 'icc-hdmi.h', 'icc-edid.c', 'icc-edid.h','hdmi-commands.c','hdmi-commands.h','flava3-prepare.c','flava3-prepare.h','flava3-video.c','flava3-video.h','button-keys.c','button-keys.h','ps5-hdmi-video-protocol.h','Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.c','Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.inf')} if args.hdmi_probe else {},
                    pci_transport_hashes={name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                                          for name in ('pci-transport.c', 'pci-transport.h')},
                    m2_overlay_enabled=args.m2_overlay,
                    setup_seconds=args.setup_seconds,
                    menu_idle_seconds=args.menu_idle_seconds,
                    nvme_preinit=args.nvme_preinit,
                    nvme_preinit_hash=hashlib.sha256((HERE / 'ps5-nvme-msix-preinit.h').read_bytes()).hexdigest() if args.nvme_preinit else None,
                    m2_overlay_hash=hashlib.sha256((HERE / 'ps5-m2-overlay.h').read_bytes()).hexdigest() if args.m2_overlay else None,
                    boot_path_hashes={name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                                      for name in ('Ps5BootWindows.c','ps5-native-gpu-probe.h','dcn201-scanout.c','dcn201-scanout.h','dcn201-memory.c','dcn201-memory.h','ps5-native-framebuffer.h','ps5-gpu-aperture.h','framebuffer-page-map.c','framebuffer-page-map.h','ps5-native-page-map.h','ps5-vbios-export.h','ps5-native-setup.h','ps5-post-wim.h','ps5-extapic.h','ps5_boot_manager.py','ps5_boot_reads.py')},
                    source_hashes={str(p.relative_to(HERE)): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in (HERE / 'Ps5PlatformPkg').rglob('*') if p.is_file()})
    display_sources = (
        ('ps5-native-dp-video.h', 'dcn201-pipe-replay.h', 'dcn201-mode-replay.h',
         'dcn201-dispclk.h', 'ps5-display-mode.h', 'ps5-display-mode.c',
         'flava3-mode.h', 'icc-power.h')
        + tuple(f'dcn201-pipe-replay-{mode}.h' for mode in args.display_replay)
    ) if args.native_dp_video else ()
    manifest.update(
        display_policy=args.display_policy,
        dispclk_raise=args.dispclk_raise,
        display_replay=list(args.display_replay),
        display_source_hashes={name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                               for name in display_sources},
        display_generated_source_hashes=display_generated_source_hashes
    )
    manifest['entry_short_marker'] = (short_marker_manifest(args.entry_short_marker_stage)
        if args.entry_short_marker else None)
    (args.output / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
