#!/usr/bin/env python3
"""Build a PS5-loader transport image, not a BIOS/Linux bootable kernel."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parent
BASE = 0x100000
SETUP_BYTES = 2048


def run(argv):
    return subprocess.check_output([str(x) for x in argv], stderr=subprocess.STDOUT)


def _madt_ioapic_address(arena, base=0x40200000):
    """Follow RSDP -> XSDT -> the *referenced* MADT and return its IO-APIC (type 1) address, or None.

    Every step is bounds- and checksum-checked. A stray or unreferenced checksum-valid 'APIC' blob in
    the arena is ignored (a plain signature search could be fooled by such a decoy)."""
    def table(off):
        if off < 0 or off + 36 > len(arena):
            return None
        length = int.from_bytes(arena[off + 4:off + 8], 'little')
        if length < 36 or off + length > len(arena) or sum(arena[off:off + length]) % 256 != 0:
            return None
        return length
    if len(arena) < 36 or arena[:8] != b'RSD PTR ' or sum(arena[:20]) % 256 != 0 or sum(arena[:36]) % 256 != 0:
        return None
    xoff = int.from_bytes(arena[24:32], 'little') - base
    xlen = table(xoff)
    if xlen is None or arena[xoff:xoff + 4] != b'XSDT':
        return None
    e = xoff + 36
    while e + 8 <= xoff + xlen:
        off = int.from_bytes(arena[e:e + 8], 'little') - base
        tlen = table(off)
        if tlen is not None and arena[off:off + 4] == b'APIC':
            p, found = off + 44, []
            while p + 2 <= off + tlen:
                t, l = arena[p], arena[p + 1]
                if l < 2 or p + l > off + tlen:
                    break
                if t == 1 and l >= 12:
                    found.append(int.from_bytes(arena[p + 4:p + 8], 'little'))
                p += l
            # Exactly one IO-APIC: a second entry (e.g. a decoy 0xFEC00000) is ambiguous and rejected.
            return found[0] if len(found) == 1 else None
        e += 8
    return None


def _madt_phantom_contract(arena, base=0x40200000):
    """Verify the v386 Phantom IO-APIC contract via RSDP -> XSDT -> referenced tables.

    Requires:
      - Valid RSDP + XSDT, all referenced tables checksum-valid, no HPET table.
      - Referenced MADT (APIC): LAPIC=0xFEE00000, PCAT_COMPAT=1 (flags & 1).
      - Exactly one Type-1 IO-APIC record with (id=0x10, addr=0xFEE003F0, gsi_base=24).
      - Zero Type-2 (ISO) or Type-3 (NMI Source) records targeting GSI 24..28."""
    def table(off):
        if off < 0 or off + 36 > len(arena):
            return None
        length = int.from_bytes(arena[off + 4:off + 8], 'little')
        if length < 36 or off + length > len(arena) or sum(arena[off:off + length]) % 256 != 0:
            return None
        return length
    if len(arena) < 36 or arena[:8] != b'RSD PTR ' or sum(arena[:20]) % 256 != 0 or sum(arena[:36]) % 256 != 0:
        return False
    xoff = int.from_bytes(arena[24:32], 'little') - base
    xlen = table(xoff)
    if xlen is None or arena[xoff:xoff + 4] != b'XSDT':
        return False
    e = xoff + 36
    madt_bounds = None
    while e + 8 <= xoff + xlen:
        off = int.from_bytes(arena[e:e + 8], 'little') - base
        tlen = table(off)
        if tlen is None:
            return False
        sig = arena[off:off + 4]
        if sig == b'HPET':
            return False
        if sig == b'APIC':
            if madt_bounds is not None:
                return False
            madt_bounds = (off, tlen)
        e += 8
    if madt_bounds is None:
        return False
    off, tlen = madt_bounds
    if tlen < 44:
        return False
    lapic = int.from_bytes(arena[off + 36:off + 40], 'little')
    flags = int.from_bytes(arena[off + 40:off + 44], 'little')
    if lapic != 0xfee00000 or not (flags & 1):
        return False
    p = off + 44
    ioapics = []
    while p + 2 <= off + tlen:
        t, l = arena[p], arena[p + 1]
        if l < 2 or p + l > off + tlen:
            return False
        if t == 1:
            if l < 12:
                return False
            ioapics.append((arena[p + 2],
                            int.from_bytes(arena[p + 4:p + 8], 'little'),
                            int.from_bytes(arena[p + 8:p + 12], 'little')))
        elif t in (2, 3):
            if l < 8:
                return False
            gsi = int.from_bytes(arena[p + 4:p + 8], 'little')
            if 24 <= gsi <= 28:
                return False
        p += l
    if p != off + tlen:
        return False
    return ioapics == [(0x10, 0xfee003f0, 24)]


def build(output, pci_fixture=None, acpi_arena=None, audible_checkpoints=False, power_telemetry=False, defer_success_report=False, defer_power_calibration=False, ioapic_scratch=False, ioapic_phantom=False):
    if defer_power_calibration and not (power_telemetry and defer_success_report):
        raise ValueError('Deferred calibration requires power telemetry and deferred normal success report')
    if defer_success_report and not power_telemetry:
        raise ValueError('Deferred success report requires power telemetry')
    if ioapic_scratch and ioapic_phantom:
        raise ValueError('IO-APIC scratch and IO-APIC phantom contracts are mutually exclusive')
    if ioapic_scratch and not acpi_arena:
        raise ValueError('IO-APIC scratch requires an embedded ACPI arena')
    if ioapic_phantom and not acpi_arena:
        raise ValueError('IO-APIC phantom requires an embedded ACPI arena')
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ps5-bootstrap-") as temporary:
        work = Path(temporary)
        objects = []
        flags = ["-Wall", "-Wextra", "-Werror", "-O2", "-ffreestanding", "-fno-pie",
                 "-fno-stack-protector", "-mno-red-zone", "-mgeneral-regs-only",
                 "-maccumulate-outgoing-args"]
        if pci_fixture:
            capture = json.loads(Path(pci_fixture).read_text())
            header = ""
            for name, bdf in (("sony", "0000:00:01.1"), ("amd", "0000:00:08.1")):
                data = base64.b64decode(capture["detail"][bdf]["config_base64"])
                if len(data) < 64: raise ValueError("Truncated test PCI capture")
                header += f"static const unsigned char test_{name}[64] = {{" + ",".join(map(str,data[:64])) + "};\n"
            (work / "pci-config-test.h").write_text(header)
            flags += ["-DPS5_PCI_CONFIG_TEST", "-I" + str(work)]
        if acpi_arena:
            arena = Path(acpi_arena).read_bytes()
            if not 4096 <= len(arena) <= 65536 or len(arena) % 4096:
                raise ValueError("Invalid ACPI arena size")
            if ioapic_scratch:
                if len(arena) != 4096:
                    raise ValueError("IO-APIC scratch requires a single-page ACPI arena")
                # Address contract: the MADT IO-APIC must point at the reserved scratch page the handoff
                # carves out (one page right after the arena), not at 0xFEC00000 or any stale address.
                scratch = 0x40200000 + len(arena)
                if _madt_ioapic_address(arena) != scratch:
                    raise ValueError("IO-APIC scratch requires the MADT IO-APIC address to be %#x" % scratch)
            if ioapic_phantom:
                if len(arena) != 4096:
                    raise ValueError("IO-APIC phantom requires a single-page ACPI arena")
                if not _madt_phantom_contract(arena):
                    raise ValueError("IO-APIC phantom contract violated (expected single Type-1 at 0xFEE003F0, ID 0x10, GSI 24, PCAT_COMPAT=1, no HPET, no ISO/NMI on GSI 24-28)")
            (work / "acpi-image.h").write_text("static const unsigned char acpi_image[] = {" + ",".join(map(str,arena)) + "};\n")
            flags += ["-DPS5_EMBED_ACPI", "-I" + str(work)]
            if ioapic_scratch:
                flags += ["-DPS5_IOAPIC_SCRATCH"]
        extra = []
        if power_telemetry:
            if audible_checkpoints or pci_fixture:
                raise ValueError('Power telemetry cannot mix with audible or captured PCI paths')
            flags += ['-DPS5_POWER_TELEMETRY']
            if defer_power_calibration:
                flags += ['-DPS5_DEFER_POWER_CALIBRATION']
            if defer_success_report:
                flags += ['-DPS5_DEFER_SUCCESS_REPORT']
            extra = ['bootstrap-power.c','bootstrap-signal.c','icc-power.c','pci-transport.c']
        if audible_checkpoints:
            if pci_fixture:
                raise ValueError("Audible hardware checkpoints cannot use a captured PCI fixture")
            flags += ["-DPS5_AUDIBLE_CHECKPOINTS"]
            extra = ["bootstrap-signal.c", "icc-power.c", "pci-transport.c"]
        for name in (*extra, "bootstrap.S", "bootstrap.c", "handoff.c", "pci-handoff.c", "acpi-handoff.c", "acpi-validate.c"):
            obj = work / (name + ".o")
            run(["gcc", *flags, "-c", SOURCE / name, "-o", obj])
            objects.append(obj)
        elf = work / "bootstrap.elf"
        run(["ld", "-nostdlib", "--fatal-warnings", "-T", SOURCE / "bootstrap.ld",
             "-o", elf, *objects])
        if run(["nm", "-u", elf]).strip():
            raise ValueError("Bootstrap has unresolved symbols")
        symbols = {}
        for line in run(["nm", "--defined-only", elf]).decode().splitlines():
            address, kind, name = line.split()
            symbols[name] = int(address, 16)
        end = symbols["ps5_bootstrap_end"]
        if symbols["ps5_start64"] != BASE + 512 or not BASE < end < 0x1000000:
            raise ValueError("Unexpected loader entry or memory footprint")
        raw = work / "bootstrap-body.bin"
        run(["objcopy", "-O", "binary", elf, raw])
        body = raw.read_bytes()
        if len(body) > end - BASE:
            raise ValueError("Binary extends past reserved memory")
        # The PS5 loader only copies syssize bytes; it does NOT clear ELF BSS.
        # Include explicit zeros for stack, IDT and prepared HOB storage.
        body += bytes(end - BASE - len(body))
        header = bytearray(SETUP_BYTES)
        header[0x1f1] = SETUP_BYTES // 512 - 1
        struct.pack_into("<I", header, 0x1f4, len(body) // 16)
        struct.pack_into("<H", header, 0x1fe, 0xaa55)
        header[0x202:0x206] = b"HdrS"
        struct.pack_into("<H", header, 0x206, 0x20c)
        header[0x211] = 1  # LOADED_HIGH; only informational to this loader.
        struct.pack_into("<I", header, 0x214, BASE)
        struct.pack_into("<I", header, 0x230, 0x1000)
        struct.pack_into("<H", header, 0x236, 3)  # x64, above-4G initrd allowed.
        image = bytes(header) + body
        files = {"bootstrap.elf": elf.read_bytes(), "bootstrap-body.bin": body,
                 ("bzImage.ps5-uefi.TEST-ONLY" if pci_fixture else "bzImage.ps5-uefi"): image}
        manifest = {
            "acpi_arena_sha256": hashlib.sha256(Path(acpi_arena).read_bytes()).hexdigest() if acpi_arena else None,
            "audible_checkpoints": bool(audible_checkpoints),
            "power_telemetry": bool(power_telemetry),
            "defer_success_report": bool(defer_success_report),
            "defer_power_calibration": bool(defer_power_calibration),
            "ioapic_scratch": bool(ioapic_scratch),
            "ioapic_phantom": bool(ioapic_phantom),
            "schema": 1, "scope": "TEST-ONLY captured PCI configuration" if pci_fixture else "PS5 loader transport; not executed on hardware",
            "test_only": bool(pci_fixture),
            "pci_fixture_sha256": hashlib.sha256(Path(pci_fixture).read_bytes()).hexdigest() if pci_fixture else None,
            "entry": symbols["ps5_start64"], "physical_base": BASE,
            "physical_end": end, "setup_bytes": SETUP_BYTES,
            "body_bytes": len(body), "symbols": symbols,
            "compiler": run(["gcc", "--version"]).decode().splitlines()[0],
            "sources_sha256": {name: hashlib.sha256((SOURCE / name).read_bytes()).hexdigest()
                               for name in (*extra, *(["icc-power.h", "pci-transport.h"] if (audible_checkpoints or power_telemetry) else []), "bootstrap.S", "bootstrap.c", "bootstrap.ld", "handoff.c", "handoff.h", "pci-handoff.c", "pci-handoff.h", "acpi-handoff.c", "acpi-handoff.h", "acpi-validate.c", "acpi-validate.h")},
            "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                      for name, data in files.items()},
        }
        for name, data in files.items():
            (output / name).write_bytes(data)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--acpi-arena", type=Path, help="Embed candidate relocated to 0x40200000")
    parser.add_argument("--audible-checkpoints", action="store_true", help="Opt-in guarded ICC milestone tones before UEFI")
    parser.add_argument('--defer-success-report', action='store_true', help='Skip only bootstrap success packet; requires power telemetry')
    parser.add_argument('--power-telemetry', action='store_true', help='Experimental meter calibration and Manchester packet output')
    parser.add_argument('--defer-power-calibration',action='store_true',help='Skip 64-second calibration on normal boot; retain it on bootstrap errors')
    parser.add_argument('--ioapic-scratch',action='store_true',help='Reserve a dedicated EfiReserved page after the (1-page) arena for a firmware IO-APIC; requires --acpi-arena')
    parser.add_argument('--ioapic-phantom',action='store_true',help='Enforce the v386 Phantom IO-APIC contract (0xFEE003F0, ID 0x10, GSI 24) without Type-0 scratch reservation; requires --acpi-arena')
    args = parser.parse_args()
    print(json.dumps(build(args.output, acpi_arena=args.acpi_arena, audible_checkpoints=args.audible_checkpoints, power_telemetry=args.power_telemetry, defer_success_report=args.defer_success_report, defer_power_calibration=args.defer_power_calibration, ioapic_scratch=args.ioapic_scratch, ioapic_phantom=args.ioapic_phantom), indent=2))
