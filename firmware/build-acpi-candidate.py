#!/usr/bin/env python3
"""Compile a reviewable ACPI candidate. Does not install or boot tables."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

HERE=Path(__file__).resolve().parent
EXPECTED={'APIC':('32ae6aae06b9abb362912edef128ecbed5ee122264312e179f065cc7b7c490ad',
                  'e8a3722b9a65e96f444b5ceedc7ed8f49b4b5b40588bb232b849bb8e286b0ef0',
                  'b80865d0a3392ac94c0fe1e608ecca591ee2a3388aefbbd4cb7751a3b2bc50dd'),
          'MCFG':('f843494293a07b46cbbbc49793b91e9d4460b6dc320420e0aa6fba8a3e91f1f5',)}

def checksum(data,offset=9):
    data[offset]=0
    data[offset]=(-sum(data))&255
    return data

def header(signature,size,revision):
    data=bytearray(size)
    struct.pack_into('<4sIBB6s8sI4sI',data,0,signature,size,revision,0,b'PSWLDR',b'PS5WIN11',1,b'PSWL',1)
    return data

def assemble(tables, base):
    """Relocate into a page-aligned arena; caller must reserve actual RAM."""
    if base < 0x100000 or base & 4095 or base > (1 << 44) - 65536:
        raise ValueError('Invalid ACPI arena base')
    for name in ('FACP','DSDT','APIC','MCFG'):
        data=tables[name]
        if len(data)<36 or data[:4]!=name.encode() or struct.unpack_from('<I',data,4)[0]!=len(data) or sum(data)&255:
            raise ValueError('Invalid ACPI table: '+name)
    if len(tables['FACP'])!=276:
        raise ValueError('Unsupported FADT layout')
    offsets={};cursor=64
    for name in ('DSDT','FACP','APIC','MCFG','XSDT'):
        cursor=(cursor+15)&~15
        offsets[name]=cursor
        cursor+=60 if name=='XSDT' else len(tables[name])
    if cursor>65536:raise ValueError('ACPI arena overflow')
    fadt=bytearray(tables['FACP']);dsdt=base+offsets['DSDT']
    struct.pack_into('<I',fadt,40,dsdt if dsdt<1<<32 else 0)
    struct.pack_into('<Q',fadt,140,dsdt)
    relocated=dict(tables);relocated['FACP']=bytes(checksum(fadt))
    xsdt=header(b'XSDT',60,1)
    for i,name in enumerate(('FACP','APIC','MCFG')):
        struct.pack_into('<Q',xsdt,36+8*i,base+offsets[name])
    relocated['XSDT']=bytes(checksum(xsdt))
    rsdp=bytearray(36)
    struct.pack_into('<8sB6sBI',rsdp,0,b'RSD PTR ',0,b'PSWLDR',2,0)
    struct.pack_into('<IQ',rsdp,20,36,base+offsets['XSDT'])
    rsdp[8]=(-sum(rsdp[:20]))&255
    rsdp[32]=(-sum(rsdp))&255
    image=bytearray((cursor+4095)&~4095);image[:36]=rsdp
    for name,data in relocated.items():image[offsets[name]:offsets[name]+len(data)]=data
    return bytes(image),{'base':base,'bytes':len(image),'rsdp':base,
                         'addresses':{name:base+off for name,off in offsets.items()},
                         'requires_reserved_acpi_reclaim_arena':True}

def phantom_madt(data):
    """v386 phantom IO-APIC: move the single Type-1 entry to 0xFEE003F0, GSI base 24 (ID kept), fix the checksum."""
    data=bytearray(data);entries=[];o=44
    while o+2<=len(data) and data[o+1]>=2:
        entries.append(o);o+=data[o+1]
    io=[e for e in entries if data[e]==1]
    if o!=len(data) or len(io)!=1 or data[io[0]+1]!=12:
        raise ValueError('MADT needs exactly one 12-byte IO-APIC entry')
    struct.pack_into('<II',data,io[0]+4,0xFEE003F0,24)
    return bytes(checksum(data))

def build(source,output, base=0x40200000, ioapic_phantom=False):
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    tables={}
    for name,expected in EXPECTED.items():
        data=(Path(source)/(name+'.dat')).read_bytes()
        digest=hashlib.sha256(data).hexdigest()
        valid=(digest in expected) if isinstance(expected,tuple) else (digest==expected)
        if not valid or sum(data)&255:
            raise ValueError('Captured table mismatch: '+name)
        tables[name]=data
    if ioapic_phantom:tables['APIC']=phantom_madt(tables['APIC'])
    subprocess.run(['iasl','-p',str(output/'dsdt'),str(HERE/'acpi/ps5-dsdt.asl')],check=True,capture_output=True)
    tables['DSDT']=(output/'dsdt.aml').read_bytes()
    # No FACS/global lock, legacy PM blocks, reset GAS or Sx registers.
    # Pointer fields must be relocated by a future ACPI installation stage.
    fadt=header(b'FACP',276,6)
    fadt[45]=1 # desktop profile
    struct.pack_into('<H',fadt,109,(1<<2)|(1<<5)) # VGA absent, CMOS RTC absent
    struct.pack_into('<I',fadt,112,1<<20) # HW_REDUCED_ACPI; no invented S0 idle support
    fadt[131]=5 # ACPI 6.5 minor revision
    tables['FACP']=bytes(checksum(fadt))
    for name,data in tables.items():
        (output/(name+'.dat')).write_bytes(data)
    image, layout=assemble(tables,base)
    (output/'acpi-arena.bin').write_bytes(image)
    manifest={'arena':dict(layout,sha256=hashlib.sha256(image).hexdigest()), 'scope':'Uninstalled ACPI development candidate, not a bootable table set',
              'tables':{n:{'bytes':len(d),'sha256':hashlib.sha256(d).hexdigest(),'checksum_valid':sum(d)%256==0} for n,d in tables.items()},
              'unresolved':['Install into ACPI reclaim memory and synchronize PCI HOB HID to PNP0A08',
                            'UEFI PcRtc driver must be replaced or disabled consistently with RTC absent flag',
                            'AP startup/NDA, Windows timer requirements and native kernel validation',
                            'No _OSC ownership negotiation or validated INTx routing; MSI path unverified',
                            'Shutdown via ICC UEFI runtime only; no ACPI S5 or sleep registers',
                            'No IVRS advertised: loader disables IOMMU; protection enablement remains open'],
              'references':['https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-system-description-tables',
                            'https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html']}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--captured-tables',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--base',type=lambda x:int(x,0),default=0x40200000)
    p.add_argument('--ioapic-phantom',action='store_true',help='Move the MADT IO-APIC entry to the v386 phantom (0xFEE003F0, GSI 24)')
    a=p.parse_args();print(json.dumps(build(a.captured_tables,a.output,a.base,a.ioapic_phantom),indent=2))
