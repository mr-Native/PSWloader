#!/usr/bin/env python3
"""msi-params-from-probe.py v1.1 (14.-15.11.2026) - derive the three ps5nvmsi parameters from a ps5msiprobe run.

  msi-params-from-probe.py ps5msiprobe.txt [--reference 13ea]   (default: every function with MSI enabled)

v1.1 (first real run 15.11.): the PS5 HAL uses LOGICAL CLUSTER mode (DFR 0x0fffffff), lowest priority and data bit 11
(Linux: dest_mode_logical); the GPU 13FC uses MSI-X, so it is no reference. Such a format gives MsiDestKind = 2:
ps5nvmsi v0.7 then takes address/data 1:1 from the HAL and only checks them against these templates.

Input: C:\\ps5-work\\ps5msiprobe.txt as written by run-ps5msiprobe.ps1 v1.3 (lines "name = value"). It needs
  version = ps5msiprobe v1.2, done = ok, cpu_NN = "apicbase .. x2apic N id .. ldr .. dfr .. cpuid_apic ..",
  msi_BB_DD = "id VVVVDDDD ctl CCCC addr HHHHHHHHLLLLLLLL data DDDD" for every function with MSI enabled.
The reference is the MSI message Windows itself programmed for a device (default: the PS5 GPU 1002:13FC, one message).
From its address/data the HAL's format follows:
  address: 0xFEE in 31:20, destination 19:12, RH bit 3, DM bit 2 -> MsiAddrTemplate = address with 19:12 cleared,
           MsiDestKind = DM (0 physical APIC ID, 1 logical);
  data:    vector 7:0, delivery 10:8, level 14, trigger 15 -> MsiDataTemplate = data with 7:0 cleared.
Refused (exit 1) unless: the probe completed, NO processor runs x2APIC (the filter supports xAPIC only), every
reference message has the same template, the high address dword is 0, the template passes the same checks as the
driver's FormatConsistent()/the installer, and the observed destination is a real processor in that mode (physical:
an APIC ID; logical: an OR of flat-mode LDR bytes, all processors with DFR flat). Output on success: the install line.
"""
import re, sys

args = sys.argv[1:]
if not args:
    print(__doc__); sys.exit(2)
ref = (args[args.index('--reference') + 1] if '--reference' in args else '').lower()     # v1.1: '' = all
vals = {}
for line in open(args[0], encoding='utf-8-sig', errors='replace'):
    m = re.match(r'\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$', line)
    if m:
        vals[m.group(1)] = m.group(2)


def refuse(why):
    print('REFUSED: ' + why); sys.exit(1)


if vals.get('version') != 'ps5msiprobe v1.2' or vals.get('done') != 'ok':
    refuse(f"probe incomplete (version {vals.get('version')!r}, done {vals.get('done')!r})")
cpus = []
for k, v in sorted(vals.items()):
    if k.startswith('cpu_'):
        m = re.search(r'x2apic (\d+) id ([0-9a-f]+) ldr ([0-9a-f]+) dfr ([0-9a-f]+) cpuid_apic ([0-9a-f]+)', v, re.I)
        if not m:
            refuse(f'unreadable {k}: {v}')
        x2, apic, ldr, dfr, cpuid = int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)
        cpus.append({'k': k, 'x2': x2, 'id': apic >> 24, 'ldr': ldr >> 24, 'dfr': dfr, 'cpuid': cpuid})
if not cpus:
    refuse('no cpu_NN lines')
if any(c['x2'] for c in cpus):
    refuse('x2APIC is enabled on at least one processor - the filter supports xAPIC only')
msgs = []
for k, v in vals.items():
    if k.startswith('msi_'):
        m = re.search(r'id ([0-9a-f]{8}) ctl ([0-9a-f]+) addr ([0-9a-f]{16}) data ([0-9a-f]+)', v, re.I)
        if m and (not ref or m.group(1).lower()[:4] == ref):      # id is DDDDVVVV (device in the high half)
            msgs.append({'k': k, 'addr': int(m.group(3), 16), 'data': int(m.group(4), 16)})
if not msgs:
    refuse(f'no enabled MSI of a {ref} device in the probe (reference missing)')
temps = set()
for g in msgs:
    if g['addr'] >> 32:
        refuse(f"{g['k']}: high address dword {g['addr'] >> 32:#x} (extended destination) not supported")
    lo = g['addr'] & 0xFFFFFFFF
    temps.add((lo & ~0x000FF000 & 0xFFFFFFFF, g['data'] & ~0xFF & 0xFFFF, (lo >> 2) & 1))
if len(temps) != 1:
    refuse(f'reference messages disagree on the format: {sorted(temps)}')
addr_t, data_t, kind = temps.pop()
# the same rules as ps5nvmsi.c FormatConsistent() and install-ps5nvmsi.ps1 v0.3
if (addr_t & 0xFFF00000) != 0xFEE00000 or (addr_t & 0x000FFFF3) != 0:
    refuse(f'address template {addr_t:#010x} is not a plain xAPIC MSI address')
cluster = kind == 1 and any((c['dfr'] >> 28) != 0xF for c in cpus)
if ((data_t >> 8) & 7) > 1 or (data_t & ~0xCF00) != 0 or (data_t & 0x800 and not kind):
    refuse(f'data template {data_t:#06x} uses bits/delivery modes the filter does not support')
hal = cluster or bool(data_t & 0x800)            # v1.1: kind 2 = HAL message 1:1 (ps5nvmsi v0.7)
if hal and any((c['dfr'] >> 28) not in (0x0, 0xF) or (c['dfr'] >> 28) != (cpus[0]['dfr'] >> 28) for c in cpus):
    refuse('processors disagree on the DFR model')
for g in msgs:
    dest = (g['addr'] >> 12) & 0xFF
    if kind == 0:
        if dest not in {c['id'] for c in cpus}:
            refuse(f"{g['k']}: physical destination {dest:#x} is no processor's APIC ID {sorted({c['id'] for c in cpus})}")
    elif cluster:                                # cluster: dest = cluster id 7:4 | processor bits 3:0
        members = {c['ldr'] & 0xF for c in cpus if c['ldr'] >> 4 == dest >> 4}
        allowed = 0
        for b in members:
            allowed |= b
        if not members or dest & 0xF == 0 or dest & 0xF & ~allowed:
            refuse(f"{g['k']}: cluster destination {dest:#x} does not name processors of cluster {dest >> 4}")
    else:
        if any((c['dfr'] >> 28) != 0xF for c in cpus):
            refuse('logical mode but not every processor has a flat DFR - cluster mode is not supported')
        allowed = 0
        for c in cpus:
            allowed |= c['ldr']
        if dest == 0 or dest & ~allowed:
            refuse(f"{g['k']}: logical destination {dest:#x} is not an OR of the processors' LDR bytes ({allowed:#x})")
if hal:
    kind = 2
print(f"reference {len(msgs)} message(s) of {ref or 'all MSI functions'}: {', '.join(m['k'] for m in msgs)}; {len(cpus)} processors, xAPIC, "
      f"{'logical (cluster)' if cluster else 'logical (flat)' if kind else 'physical'} destination"
      f"{' -> kind 2 (HAL message 1:1, ps5nvmsi v0.7+)' if kind == 2 else ''}")
print(f'MsiAddrTemplate=0x{addr_t:08X} MsiDataTemplate=0x{data_t:04X} MsiDestKind={kind}')
print(f'install line: install-ps5nvmsi.ps1 -Mode apply -AddrTemplate {addr_t:08X} -DataTemplate {data_t:04X} -DestKind {kind}')
print('NOTE: deriving the parameters is not a GO - the install still needs review and an explicit GO.')
