#!/usr/bin/env python3
"""Stage verified native-test inputs on pinned Windows ESP from PS5 Linux.

Does not register or execute a payload, change boot defaults, or replace an
existing test directory. Requires the previously provisioned diagnostic slot.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

PARTUUID='bd21c511-2781-4058-b847-f625a7eccbb6'
import labconf;SERIAL=labconf.get('WINDOWS_DISK_SERIAL','SET-WINDOWS_DISK_SERIAL-IN-LAB-CONF')   # placeholder never matches a disk -> refuses
ASSETS='PS5/Windows11/native-test'
FILES=(ASSETS+'/bzImage.ps5-uefi',ASSETS+'/ps5-upl.bin','ps5-windows-test-loader.elf')

def run(*args):return subprocess.check_output(args,text=True).strip()
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def verify(source):
    manifest=json.loads((source/'manifest.json').read_text())
    if manifest.get('bootstrap_test_only') is not False or set(manifest['files'])!=set(FILES):
        raise ValueError('Unexpected native bundle file set or test-only bootstrap')
    for name in FILES:
        path=source/name
        if path.is_symlink() or not path.is_file() or not 0<path.stat().st_size<=16*1024*1024:
            raise ValueError('Invalid bundle file: '+name)
        record=manifest['files'][name]
        if path.stat().st_size!=record['bytes'] or sha(path)!=record['sha256']:
            raise ValueError('Bundle hash/size mismatch: '+name)
    return manifest

def main(source,apply=False):
    source=source.resolve();manifest=verify(source)
    device=(Path('/dev/disk/by-partuuid')/PARTUUID).resolve(strict=True)
    rows=json.loads(run('lsblk','-J','-o','PATH,PKNAME,PARTUUID,FSTYPE,LABEL,MOUNTPOINTS',str(device)))['blockdevices']
    if len(rows)!=1:raise RuntimeError('Ambiguous partition')
    row=rows[0]
    if (row['partuuid']!=PARTUUID or row['fstype']!='vfat' or row['label']!='PS5WINBOOT'
            or any(row['mountpoints']) or not row['pkname']):raise RuntimeError('Partition mismatch or mounted')
    if run('lsblk','-dnro','SERIAL',str(Path('/dev')/row['pkname']))!=SERIAL:raise RuntimeError('Wrong disk serial')
    report={'scope':'Native diagnostic assets staged only; no boot selection or native execution',
            'device':str(device),'serial':SERIAL,'partuuid':PARTUUID,'applied':apply,
            'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
    if not apply:return report
    directory=Path(tempfile.mkdtemp(prefix='ps5-native-stage-'));mounted=False
    try:
        subprocess.run(['mount','-o','rw,nosuid,nodev,noexec',str(device),str(directory)],check=True);mounted=True
        slot=(directory/'PS5UEFI.LOG').read_bytes()
        if len(slot)!=65568 or slot[:16]!=b'PS5LOGFILE1'.ljust(16,b'\0'):
            raise RuntimeError('Missing/invalid diagnostic slot')
        target=directory/ASSETS
        if target.exists():raise RuntimeError('Existing test directory preserved; refusing replacement')
        parent=target.parent;parent.mkdir(parents=True,exist_ok=True)
        staging=Path(tempfile.mkdtemp(prefix='native-test-staging-',dir=parent))
        for name in FILES:
            data=(source/name).read_bytes()
            with (staging/Path(name).name).open('xb') as out:
                out.write(data);out.flush();os.fsync(out.fileno())
            if sha(staging/Path(name).name)!=manifest['files'][name]['sha256']:
                raise RuntimeError('Staged copy differs')
        with (staging/'manifest.json').open('x') as out:
            json.dump(manifest,out,indent=2);out.flush();os.fsync(out.fileno())
        staging.rename(target)
        subprocess.run(['umount',str(directory)],check=True);mounted=False
        subprocess.run(['mount','-o','ro,nosuid,nodev,noexec',str(device),str(directory)],check=True);mounted=True
        readback={}
        for name in FILES:
            path=directory/ASSETS/Path(name).name
            record={'bytes':path.stat().st_size,'sha256':sha(path)}
            if record!=manifest['files'][name]:raise RuntimeError('Readback after remount differs')
            readback[str(Path(ASSETS)/Path(name).name)]=record
        if (directory/'PS5UEFI.LOG').read_bytes()!=slot:raise RuntimeError('Diagnostic slot changed')
        report.update(files=readback,readback_after_ro_remount=True,diagnostic_slot_unchanged=True)
    finally:
        if mounted:subprocess.run(['umount',str(directory)],check=True)
        directory.rmdir()
    report['unmounted']=True
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--apply',action='store_true')
    a=p.parse_args();print(json.dumps(main(a.source,a.apply),indent=2))
