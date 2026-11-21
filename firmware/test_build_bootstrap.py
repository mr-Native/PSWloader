"""Regression tests for build-bootstrap's MADT IO-APIC address contract (firmware IO-APIC scratch).

The parser must follow RSDP -> XSDT -> the referenced MADT, never a plain 'APIC' signature search:
an unreferenced but checksum-valid decoy MADT in the arena tail must not fool it (found in review)."""
import os
import importlib.util
from pathlib import Path
import struct
import unittest

HERE = Path(__file__).parent
spec = importlib.util.spec_from_file_location('build_bootstrap', HERE / 'build-bootstrap.py')
bb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bb)
BASE = 0x40200000


def _madt(ioapic_addr):
    """A minimal checksum-valid MADT (ACPI header + LAPIC addr/flags + one IO-APIC entry)."""
    body = bytearray(44)
    body[0:4] = b'APIC'
    struct.pack_into('<I', body, 36, 0xfee00000)  # local APIC address
    entry = bytearray(12)
    entry[0] = 1            # type 1 = IO-APIC
    entry[1] = 12           # length
    entry[2] = 16           # IO-APIC id
    struct.pack_into('<I', entry, 4, ioapic_addr)
    table = body + entry
    struct.pack_into('<I', table, 4, len(table))
    table[9] = 0
    table[9] = (-sum(table)) & 0xff
    return bytes(table)


class MadtAddressContract(unittest.TestCase):
    def test_follows_xsdt_and_ignores_unreferenced_decoy(self):
        # Build a tiny arena: RSDP -> XSDT -> real MADT (IO-APIC 0xFEC00000), plus an UNREFERENCED
        # decoy MADT (IO-APIC 0x40201000) parked later in the page. A signature search would return
        # the decoy; the correct parser follows the XSDT and returns the real address.
        arena = bytearray(4096)
        real = _madt(0xfec00000)
        decoy = _madt(0x40201000)
        real_off, decoy_off = 0x100, 0x400
        arena[real_off:real_off + len(real)] = real
        arena[decoy_off:decoy_off + len(decoy)] = decoy
        xsdt = bytearray(36 + 8)
        xsdt[0:4] = b'XSDT'
        struct.pack_into('<I', xsdt, 4, len(xsdt))
        struct.pack_into('<Q', xsdt, 36, BASE + real_off)   # XSDT references ONLY the real MADT
        xsdt[9] = 0
        xsdt[9] = (-sum(xsdt)) & 0xff
        xsdt_off = 0x80
        arena[xsdt_off:xsdt_off + len(xsdt)] = xsdt
        rsdp = bytearray(36)
        rsdp[0:8] = b'RSD PTR '
        rsdp[15] = 2
        struct.pack_into('<Q', rsdp, 24, BASE + xsdt_off)
        rsdp[8] = (-sum(rsdp[:20])) & 0xff
        rsdp[32] = (-sum(rsdp)) & 0xff
        arena[0:36] = rsdp
        self.assertEqual(bb._madt_ioapic_address(bytes(arena)), 0xfec00000)

    def test_rejects_madt_with_two_ioapics(self):
        # A referenced MADT with two IO-APIC (type 1) entries is ambiguous -> parser returns None,
        # so --ioapic-scratch would reject it (found).
        arena = bytearray(4096)
        body = bytearray(44); body[0:4] = b'APIC'; struct.pack_into('<I', body, 36, 0xfee00000)
        e1 = bytearray(12); e1[0] = 1; e1[1] = 12; e1[2] = 16; struct.pack_into('<I', e1, 4, 0x40201000)
        e2 = bytearray(12); e2[0] = 1; e2[1] = 12; e2[2] = 17; struct.pack_into('<I', e2, 4, 0xfec00000)
        madt = body + e1 + e2
        struct.pack_into('<I', madt, 4, len(madt)); madt[9] = 0; madt[9] = (-sum(madt)) & 0xff
        moff = 0x100; arena[moff:moff + len(madt)] = madt
        xsdt = bytearray(44); xsdt[0:4] = b'XSDT'; struct.pack_into('<I', xsdt, 4, len(xsdt))
        struct.pack_into('<Q', xsdt, 36, BASE + moff); xsdt[9] = 0; xsdt[9] = (-sum(xsdt)) & 0xff
        xoff = 0x80; arena[xoff:xoff + len(xsdt)] = xsdt
        rsdp = bytearray(36); rsdp[0:8] = b'RSD PTR '; rsdp[15] = 2
        struct.pack_into('<Q', rsdp, 24, BASE + xoff)
        rsdp[8] = (-sum(rsdp[:20])) & 0xff; rsdp[32] = (-sum(rsdp)) & 0xff
        arena[0:36] = rsdp
        self.assertIsNone(bb._madt_ioapic_address(bytes(arena)))

    def test_real_arenas(self):
        work = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work'))
        pairs = [('acpi-candidate-v131/acpi-arena.bin', 0xfec00000),
                 ('acpi-candidate-ioapic-fw2/acpi-arena.bin', 0x40201000),
                 ('acpi-candidate-v386-phantom/acpi-arena.bin', 0xfee003f0)]
        for rel, expected in pairs:
            p = work / rel
            if p.exists():
                self.assertEqual(bb._madt_ioapic_address(p.read_bytes()), expected, rel)

    def test_phantom_contract(self):
        work = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work'))
        phantom = work / 'acpi-candidate-v386-phantom/acpi-arena.bin'
        if phantom.exists():
            self.assertTrue(bb._madt_phantom_contract(phantom.read_bytes()))
        fw2 = work / 'acpi-candidate-ioapic-fw2/acpi-arena.bin'
        if fw2.exists():
            self.assertFalse(bb._madt_phantom_contract(fw2.read_bytes()))


if __name__ == '__main__':
    unittest.main()
