"""Optional single power packet before DXE core initializes exception handlers."""
import subprocess

DXE_C = 'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c'
DXE_INF = 'MdeModulePkg/Core/Dxe/DxeMain.inf'


def prepare_dxe_power(root, enabled, encoder, stage="dxe-entry"):
    originals = {name: subprocess.check_output(['git', 'show', 'HEAD:' + name],
                 cwd=root, text=True) for name in (DXE_C, DXE_INF)}
    source, inf = originals[DXE_C], originals[DXE_INF]
    declaration = '#include "DxeMain.h"'
    stages = {
        'dxe-entry': ('  VectorInfoList = NULL;', 0x704),
        'dxe-exceptions': ('  InitializeDebugAgent (DEBUG_AGENT_INIT_DXE_CORE, HobStart, NULL);', 0x705),
        'dxe-memory': ('  MemoryProfileInit (HobStart);', 0x706),
        'dxe-dispatch': ('  CoreDispatcher ();', 0x707),
    }
    if stage not in stages:
        raise ValueError('Unknown DXE probe stage')
    anchor, code = stages[stage]
    inf_anchor = '  DxeMain/DxeMain.c'
    if any(text.count(item) != 1 for text, item in
           ((source, declaration), (source, anchor), (inf, inf_anchor))):
        raise RuntimeError('Unexpected pinned DXE core probe anchors')
    if enabled:
        source = source.replace(declaration, declaration + '\nVOID ps5_power_report (unsigned code);')
        source = source.replace(anchor, f'  ps5_power_report (0x{code:x});\n' + anchor)
        inf = inf.replace(inf_anchor, inf_anchor + '\n  DxeMain/Ps5DxePower.c')
    (root / DXE_C).write_text(source)
    (root / DXE_INF).write_text(inf)
    if enabled:
        (root / DXE_C).with_name('Ps5DxePower.c').write_text('#define PS5_POWER_EDK2\n' + encoder)
