"""Patch the pinned platform boot manager to select only the Windows test ESP."""
import subprocess

DIRECTORY='UefiPayloadPkg/Library/PlatformBootManagerLib'


def prepare_windows_boot(root,implementation,setup_menu=False):
    source,inf=[subprocess.check_output(['git','show','HEAD:'+DIRECTORY+'/'+name],
                cwd=root,text=True) for name in ('PlatformBootManager.c','PlatformBootManagerLib.inf')]
    declaration='#include "PlatformBootManager.h"'
    anchor='  EfiBootManagerConnectAll ();\n  EfiBootManagerRefreshAllBootOption ();'
    inf_anchor='  PlatformBootManager.c\n'
    protocol_anchor='[Protocols]\n'
    if any(text.count(item)!=1 for text,item in ((source,declaration),(source,anchor),
            (inf,inf_anchor),(inf,protocol_anchor))):
        raise RuntimeError('Unexpected pinned Windows boot manager anchors')
    source=source.replace(declaration,declaration+'\nEFI_STATUS Ps5BootPinnedWindows (VOID);\nVOID Ps5ExtApicOpenIer (VOID);\nVOID Ps5WarmProbePoint (UINTN N);\nVOID Ps5WarmPciDump (VOID);')
    # lab --warm-probe 46-49 (no-op in other builds): before/after PlatformConsoleInit, AfterConsole start, after ConnectAll
    for a,b in (('  PlatformConsoleInit ();\n}','  Ps5WarmProbePoint (46);\n  PlatformConsoleInit ();\n  Ps5WarmProbePoint (47);\n}'),
                ('  Status = gBS->LocateProtocol (&gEdkiiPlatformLogoProtocolGuid','  Ps5WarmProbePoint (48);\n  Status = gBS->LocateProtocol (&gEdkiiPlatformLogoProtocolGuid')):
        if source.count(a)!=1: raise RuntimeError('Unexpected warm probe anchor in PlatformBootManager.c')
        source=source.replace(a,b,1)
    # v1.1: open the AMD extended-APIC IER before anything in BDS waits on a timer (UiApp, setup window)
    before_anchor='  EFI_BOOT_MANAGER_LOAD_OPTION  BootOption;\n\n  //\n  // Register ENTER as CONTINUE key'
    if source.count(before_anchor)!=1:
        raise RuntimeError('Unexpected BeforeConsole anchor')
    source=source.replace(before_anchor,'  EFI_BOOT_MANAGER_LOAD_OPTION  BootOption;\n\n  Ps5ExtApicOpenIer ();\n\n  //\n  // Register ENTER as CONTINUE key')
    source=source.replace(anchor,'  Ps5WarmPciDump ();\n  EfiBootManagerConnectAll ();\n  Ps5WarmProbePoint (49);\n'+('  EfiBootManagerRefreshAllBootOption ();\n' if setup_menu else '')+
        '  Status = Ps5BootPinnedWindows ();\n'
        '  DEBUG ((DEBUG_INFO, "PS5 Windows boot path returned: %r\\n", Status));\n'
        '  EfiBootManagerRefreshAllBootOption ();')
    inf=inf.replace('[Guids]\n','[Guids]\n  gEfiEventExitBootServicesGuid ## CONSUMES\n  gEfiFileInfoGuid ## CONSUMES\n  gEfiGlobalVariableGuid ## CONSUMES\n')
    inf=inf.replace(inf_anchor,inf_anchor+'  Ps5BootWindows.c\n').replace(protocol_anchor,
        protocol_anchor+'  gEfiSimpleFileSystemProtocolGuid ## CONSUMES\n'
        '  gEfiPartitionInfoProtocolGuid ## CONSUMES\n'
        '  gEfiBlockIoProtocolGuid ## CONSUMES\n'
        '  gEfiBlockIo2ProtocolGuid ## CONSUMES\n'
        '  gEfiDevicePathProtocolGuid ## CONSUMES\n'
        '  gEfiMpServiceProtocolGuid ## CONSUMES\n'
        '  gEfiSmbiosProtocolGuid ## CONSUMES\n'
        '  gEfiAcpiTableProtocolGuid ## CONSUMES\n')   # r4-nvme-name; r5.12 pstates SSDT
    if setup_menu:
        inf=inf.replace('  Ps5BootWindows.c\n','  Ps5BootWindows.c\n  dcn201-scanout.c\n  dcn201-memory.c\n  framebuffer-page-map.c\n')
        inf=inf.replace('[Protocols]\n','[Protocols]\n  gEfiPciIoProtocolGuid ## CONSUMES\n')
    directory=root/DIRECTORY
    (directory/'PlatformBootManager.c').write_text(source)
    (directory/'PlatformBootManagerLib.inf').write_text(inf)
    (directory/'Ps5BootWindows.c').write_text(('#define PS5_NATIVE_SETUP 1\n' if setup_menu else '')+implementation)
    from pathlib import Path
    for name in ('ps5-native-gpu-probe.h','dcn201-scanout.c','dcn201-scanout.h','dcn201-memory.c','dcn201-memory.h','ps5-native-framebuffer.h','ps5-gpu-aperture.h','framebuffer-page-map.c','framebuffer-page-map.h','ps5-native-page-map.h','ps5-vbios-export.h'):
        (directory/name).write_text((Path(__file__).parent/name).read_text())
    (directory/'ps5-native-setup.h').write_text((Path(__file__).parent/'ps5-native-setup.h').read_text())
    (directory/'ps5-boot-screen.h').write_text((Path(__file__).parent/'ps5-boot-screen.h').read_text())   # r5.17 PSWloader screen
    (directory/'ps5-post-wim.h').write_text((Path(__file__).parent/'ps5-post-wim.h').read_text())
    (directory/'ps5-time-calendar.h').write_text((Path(__file__).parent/'ps5-time-calendar.h').read_text())
    (directory/'ps5-extapic.h').write_text((Path(__file__).parent/'ps5-extapic.h').read_text())
    (directory/'ps5-cpu-name.h').write_text((Path(__file__).parent/'ps5-cpu-name.h').read_text())   # r4-nvme-name
    (directory/'ps5-warm-probe.h').write_text((Path(__file__).parent/'ps5-warm-probe.h').read_text())   # lab --warm-probe 42-45
    (directory/'ps5-bs-trace.h').write_text((Path(__file__).parent/'ps5-bs-trace.h').read_text())   # lab --warm-bs-trace
    (directory/'Ps5BootNow.h').write_text((Path(__file__).parent/'Ps5PlatformPkg/Include/Ps5BootNow.h').read_text())   # r5.4
    (directory/'Ps5PowerPreset.h').write_text((Path(__file__).parent/'Ps5PlatformPkg/Include/Ps5PowerPreset.h').read_text())   # r5.6
    (directory/'ps5-settings.h').write_text((Path(__file__).parent/'ps5-settings.h').read_text())   # r5.7
    (directory/'ps5-bcd.h').write_text((Path(__file__).parent/'ps5-bcd.h').read_text())   # r5.22 BCD testsigning
    (directory/'ps5-pstate-ssdt.h').write_text((Path(__file__).parent/'ps5-pstate-ssdt.h').read_text())   # r5.12
    (directory/'ps5-fan.h').write_text((Path(__file__).parent/'ps5-fan.h').read_text())   # r5.14
    for name in ('ps5-diag.h','ps5-diag-uefi.h','icc-power.h'):   # r5.20 BIOS system check
        (directory/name).write_text((Path(__file__).parent/name).read_text())
    (directory/'Ps5Diag.h').write_text((Path(__file__).parent/'Ps5PlatformPkg/Include/Ps5Diag.h').read_text())   # r5.20
    (directory/'ps5-hdmi-video-protocol.h').write_text((Path(__file__).parent/'ps5-hdmi-video-protocol.h').read_text())   # r5.14
    (directory/'ps5-m2-overlay.h').write_text((Path(__file__).parent/'ps5-m2-overlay.h').read_text())
    (directory/'ps5-nvme-msix-preinit.h').write_text((Path(__file__).parent/'ps5-nvme-msix-preinit.h').read_text())


BOOT_SOURCE='MdeModulePkg/Library/UefiBootManagerLib/BmBoot.c'
SNAPSHOT_GUID='{0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}}'

def prepare_boot_snapshots(root):
    """Persist bounded boot-service milestones; no hooks in runtime services."""
    source=subprocess.check_output(['git','show','HEAD:'+BOOT_SOURCE],cwd=root,text=True)
    declaration='#include "InternalBm.h"'
    helper='\nSTATIC EFI_GUID mPs5BootSnapshotGuid='+SNAPSHOT_GUID+';\n'
    helper+='STATIC VOID Ps5BootSnapshot (UINT32 Phase, EFI_STATUS Status) {\n'
    helper+='  DEBUG ((DEBUG_INFO, "PS5 boot milestone %u: %r\\n", Phase, Status));\n'
    helper+='  EfiEventGroupSignal (&mPs5BootSnapshotGuid);\n}\n'
    replacements=[(declaration,declaration+helper),
      ('    FileBuffer = BmGetNextLoadOptionBuffer', '    Ps5BootSnapshot (1, EFI_SUCCESS);\n    FileBuffer = BmGetNextLoadOptionBuffer'),
      ('    if (FileBuffer != NULL) {\n      RamDiskDevicePath', '    Ps5BootSnapshot (2, FileBuffer == NULL ? EFI_NOT_FOUND : EFI_SUCCESS);\n    if (FileBuffer != NULL) {\n      RamDiskDevicePath'),
      ('                      &ImageHandle\n                      );', '                      &ImageHandle\n                      );\n      Ps5BootSnapshot (3, Status);'),
      ('  Status = gBS->StartImage (ImageHandle, &BootOption->ExitDataSize, &BootOption->ExitData);',
       '  DEBUG ((DEBUG_INFO, "PS5 image ready: base=%p size=%Lu\\n", ImageInfo->ImageBase, (UINT64)ImageInfo->ImageSize));\n  Ps5BootSnapshot (4, EFI_SUCCESS);\n  Status = gBS->StartImage (ImageHandle, &BootOption->ExitDataSize, &BootOption->ExitData);'),
      ('  DEBUG ((DEBUG_INFO | DEBUG_LOAD, "Image Return Status = %r\\n", Status));',
       '  DEBUG ((DEBUG_INFO | DEBUG_LOAD, "Image Return Status = %r\\n", Status));\n  Ps5BootSnapshot (5, Status);')]
    for old,new in replacements:
        if source.count(old)!=1:raise RuntimeError('Unexpected boot snapshot anchor: '+old[:60])
        source=source.replace(old,new)
    (root/BOOT_SOURCE).write_text(source)
