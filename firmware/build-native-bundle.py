#!/usr/bin/env python3
"""Assemble checked local native-test assets; does not deploy or start anything."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def telemetry_config(bootstrap_manifest, packed_manifest, uefi):
    deferred = bootstrap_manifest.get('defer_success_report', False)
    build_path = uefi / 'build.json'
    entry = False
    stage = None
    experimental_no_image_protection = False
    exception_power = False
    if build_path.exists():
        provenance = json.loads(build_path.read_text())
        if provenance.get('sha256') != packed_manifest.get('source_sha256'):
            raise ValueError('UEFI build provenance does not match packed payload')
        experimental_no_image_protection = provenance.get('experimental_no_image_protection', False)
        exception_power = provenance.get('exception_power_telemetry', False)
        entry = provenance.get('entry_power_telemetry', False)
        stage = provenance.get('entry_power_stage', 'entry') if entry else None
        if entry and stage not in ('exception-only', 'entry', 'dxe-ready', 'dxe-entry', 'dxe-exceptions', 'dxe-memory', 'dxe-dispatch', 'log-entry', 'cpu-entry', 'cpu-before-mp', 'cpu-before-gcd', 'cpu-after-paging', 'cpu-after-gdt', 'cpu-after-idt'):
            raise ValueError('Unknown UEFI telemetry stage')
        if entry and stage == 'log-entry' and not provenance.get('log_export_enabled'):
            raise ValueError('Log entry telemetry requires log export')
    if experimental_no_image_protection and (not entry or stage != 'log-entry'):
        raise ValueError('Experimental image protection comparison requires log-entry telemetry')
    if bootstrap_manifest.get('defer_power_calibration') and stage!='exception-only':
        raise ValueError('Deferred calibration requires exception-only firmware telemetry')
    if stage == 'exception-only' and not exception_power:
        raise ValueError('Exception-only stage requires exception power telemetry')
    if exception_power and not entry:
        raise ValueError('Exception power telemetry requires calibrated entry telemetry')
    if deferred and (not bootstrap_manifest.get('power_telemetry') or not entry):
        raise ValueError('Deferred success report requires calibrated UEFI entry telemetry')
    if entry and (not bootstrap_manifest.get('power_telemetry') or not deferred):
        raise ValueError('UEFI entry telemetry requires deferred calibrated bootstrap')
    return dict(bootstrap_calibration_deferred=bootstrap_manifest.get('defer_power_calibration',False), defer_success_report=deferred, entry_power_telemetry=entry, entry_power_stage=stage, experimental_no_image_protection=experimental_no_image_protection, exception_power_telemetry=exception_power)


def build(bootstrap,uefi,loader,output):
    b=json.loads((bootstrap/'manifest.json').read_text())
    u=json.loads((uefi/'ps5-upl.bin.json').read_text())
    l=json.loads((loader/'manifest.json').read_text())
    telemetry=telemetry_config(b,u,uefi)
    if b.get('test_only') is not False or b.get('pci_fixture_sha256') is not None:
        raise ValueError('Refusing test-only PCI bootstrap')
    if not b.get('acpi_arena_sha256'):raise ValueError('ACPI candidate required')
    names=[(bootstrap/'bzImage.ps5-uefi',b['files']['bzImage.ps5-uefi']['sha256']),
           (uefi/'ps5-upl.bin',u['container_sha256']),
           (loader/'ps5-windows-test-loader.elf',l['loader_sha256'])]
    blobs=[]
    for path,expected in names:
        data=path.read_bytes()
        if hashlib.sha256(data).hexdigest()!=expected:raise ValueError('Artifact hash mismatch: '+str(path))
        blobs.append(data)
    body,payload,entry=blobs
    if body[0x1f1]!=3 or body[0x202:0x206]!=b'HdrS' or struct.unpack_from('<I',body,0x1f4)[0]*16+2048!=len(body):
        raise ValueError('Invalid bootstrap transport')
    if payload[:8]!=b'PS5UPL01' or len(payload)<4096 or zlib.crc32(payload[:4092])!=struct.unpack_from('<I',payload,4092)[0]:
        raise ValueError('Invalid payload header')
    if entry[:4]!=b'\x7fELF':raise ValueError('Invalid GameOS loader ELF')
    output.mkdir(parents=True,exist_ok=False)
    assets=output/'PS5/Windows11/native-test';assets.mkdir(parents=True)
    paths=[assets/'bzImage.ps5-uefi',assets/'ps5-upl.bin',output/'ps5-windows-test-loader.elf']
    for path,data in zip(paths,blobs):path.write_bytes(data)
    manifest={'scope':'Native diagnostic candidate, not a verified Windows boot; no deployment',
              'bootstrap_test_only':False,'audible_checkpoints':b.get('audible_checkpoints',False),'power_telemetry':b.get('power_telemetry',False),'early_abort_reboot_attempt':b.get('power_telemetry',False),'acpi_arena_sha256':b['acpi_arena_sha256'],
              'loader_revision':l['revision'],**telemetry,
              'files':{str(path.relative_to(output)):{'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for path,data in zip(paths,blobs)},
              'limitations':['Native NDA/CPU entry unverified','RTC replacement unresolved','No GOP/working scanout','Windows kernel/PCI drivers unverified','Log export requires working USB filesystem and an enabled export event']}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('bootstrap','uefi','loader','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();print(json.dumps(build(a.bootstrap,a.uefi,a.loader,a.output),indent=2))
