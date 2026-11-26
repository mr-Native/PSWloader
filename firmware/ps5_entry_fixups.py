"""Narrow transformations of the pinned EDK payload entry for PS5."""
import subprocess

CPU_SOURCE = 'UefiCpuPkg/CpuDxe/CpuDxe.c'


def quiesce_inherited_apic(source):
    """Mask inherited BSP timer/LINT sources before DXE can first enable IF.

    CPU architectural protocol notification publishes gCpu. CoreRestoreTpl can
    then enable interrupts inside the protocol installation itself, before
    InitializeMpSupport or the UEFI timer driver owns the interrupt sources.
    Use the existing LocalApicLib accessors (xAPIC/x2APIC aware), never PIC I/O.
    This does not drain IRR or mask device MSI/IOAPIC sources.
    """
    anchor = '  Status = gBS->InstallMultipleProtocolInterfaces ('
    order = ('InitializeCpu (', '  // Make sure interrupts are disabled',
             '  InitInterruptDescriptorTable ();', anchor,
             '  InitializeMpSupport ();')
    if (any(source.count(item) != 1 for item in order)
            or '  DisableApicTimerInterrupt ();' in source
            or '  DisableLvtInterrupts ();' in source):
        raise RuntimeError('Unexpected pinned CPU APIC anchors')
    positions = [source.index(item) for item in order]
    if positions != sorted(positions):
        raise RuntimeError('Unexpected pinned CPU APIC order')
    return source.replace(anchor,
        '  // Quiesce inherited BSP sources before CPU protocol notify can enable IF.\n'
        '  DisableApicTimerInterrupt ();\n'
        '  DisableLvtInterrupts ();\n\n' + anchor)


def prepare_cpu_apic(root):
    """Apply after pristine CPU regeneration, including optional probe insertion."""
    path = root / CPU_SOURCE
    path.write_text(quiesce_inherited_apic(path.read_text()))

X64_HANDOFF = 'UefiPayloadPkg/UefiPayloadEntry/X64/DxeLoadFunc.c'
PIC_WRITES = ('  IoWrite8 (LEGACY_8259_MASK_REGISTER_MASTER, 0xFF);',
              '  IoWrite8 (LEGACY_8259_MASK_REGISTER_SLAVE, 0xFF);')


def without_legacy_pic(source):
    """PS5 has no legacy 8259; do not access its PC mask ports at handoff.

    Preserve all CPU, stack, HOB, and page-table operations. In particular this
    neither enables interrupts nor changes the existing loader interrupt state.
    Refuse changed source structure rather than silently patching another path.
    """
    for anchor in ('HandOffToDxeCore (', '  InitializeFloatingPointUnits ();',
                   '  BaseOfStack = AllocatePages (EFI_SIZE_TO_PAGES (STACK_SIZE));',
                   *PIC_WRITES):
        if source.count(anchor) != 1:
            raise RuntimeError('Unexpected pinned x64 handoff legacy PIC anchors')
    first,second=(source.index(line) for line in PIC_WRITES)
    if not source.index('  InitializeFloatingPointUnits ();') < first < second < source.index('  BaseOfStack = AllocatePages'):
        raise RuntimeError('Unexpected pinned x64 handoff legacy PIC order')
    for line in PIC_WRITES:
        source=source.replace(line, '')
    return source.replace('  // Mask off all legacy 8259 interrupt sources',
                          '  // PS5 has no legacy 8259 mask ports; preserve loader interrupt state.')


def prepare_x64_handoff(root):
    """Regenerate from pinned git source on every build; never patch cumulatively."""
    source=subprocess.check_output(['git','show','HEAD:'+X64_HANDOFF],cwd=root,text=True)
    (root/X64_HANDOFF).write_text(without_legacy_pic(source))

X64_PAGE_TABLES = 'UefiPayloadPkg/UefiPayloadEntry/X64/VirtualMemory.c'
# All four physical leaf initializers, including both split paths. Do not set
# cache bits on pointer entries: only the final translation selects the PAT.
CACHE_LEAVES = (
    ('PageTableEntry->Uint64 = (UINT64)PhysicalAddress4K;', 'PhysicalAddress4K'),
    ('PageDirectoryEntry->Uint64         = (UINT64)PhysicalAddress2M | AddressEncMask;', 'PhysicalAddress2M'),
    ('PageDirectory1GEntry->Uint64         = (UINT64)PageAddress | AddressEncMask;', 'PageAddress'),
    ('PageDirectoryEntry->Uint64         = (UINT64)PageAddress | AddressEncMask;', 'PageAddress'),
)


def preserve_mmio_cache(source):
    """Preserve bootstrap PAT index 3 for 2..4GiB without altering IA32_PAT.

    The power/signal bootstrap verifies inherited PAT entry 3 is UC. Both
    boundaries align with 1GiB leaves; every leaf size can select it directly.
    RAM, high PMem, encryption, protection and non-leaf mappings stay intact.
    """
    for anchor,address in CACHE_LEAVES:
        if source.count(anchor) != 1:
            raise RuntimeError('Unexpected pinned x64 page-table leaf anchors')
    for anchor,address in CACHE_LEAVES:
        replacement=anchor[:-1] + (' | (((UINT64)'+address+
            ' >= 0x80000000ULL && (UINT64)'+address+
            ' < 0x100000000ULL) ? 0x18ULL : 0);')
        source=source.replace(anchor,replacement)
    return source


def prepare_x64_page_tables(root):
    """Regenerate the page-table source from pinned HEAD, never cumulatively."""
    source=subprocess.check_output(['git','show','HEAD:'+X64_PAGE_TABLES],cwd=root,text=True)
    (root/X64_PAGE_TABLES).write_text(preserve_mmio_cache(source))
