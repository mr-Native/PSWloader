"""Install the guarded inherited-timer drain in the regenerated CpuDxe."""
import subprocess


def prepare_inherited_timer(root, implementation):
    directory = root / 'UefiCpuPkg/CpuDxe'
    source = (directory / 'CpuDxe.c').read_text()
    inf = (directory / 'CpuDxe.inf').read_text()
    header = subprocess.check_output(['git', 'show',
        'HEAD:OvmfPkg/LocalApicTimerDxe/LocalApicTimerDxe.h'], cwd=root, text=True)
    if '#define LOCAL_APIC_TIMER_VECTOR  32' not in header:
        raise RuntimeError('Unexpected pinned UEFI timer vector')
    declaration = '#include "CpuDxe.h"'
    anchor = '  Status = gBS->InstallMultipleProtocolInterfaces ('
    inf_anchor = '\n  CpuDxe.c\n'
    library_anchor = '\n  BaseLib\n'
    if (source.count(declaration) != 1 or source.count(anchor) != 1
            or inf.count(inf_anchor) != 1 or inf.count(library_anchor) != 1
            or 'Ps5PrepareInheritedTimer' in source):
        raise RuntimeError('Unexpected pinned inherited timer anchors')
    source = source.replace(declaration,
        declaration + '\nEFI_STATUS Ps5PrepareInheritedTimer (VOID);')
    source = source.replace(anchor,
        '  Status = Ps5PrepareInheritedTimer ();\n'
        '  if (EFI_ERROR (Status)) {\n    return Status;\n  }\n\n' + anchor)
    (directory / 'CpuDxe.c').write_text(source)
    (directory / 'CpuDxe.inf').write_text(inf.replace(inf_anchor,
        inf_anchor + '  Ps5InheritedTimer.c\n').replace(library_anchor,
        library_anchor + '  SynchronizationLib\n'))
    (directory / 'Ps5InheritedTimer.c').write_text(implementation)
