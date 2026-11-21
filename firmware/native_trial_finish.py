#!/usr/bin/env python3
"""Finish a completed native measurement; never launch a boot or touch services."""
import argparse
import asyncio
import importlib.util
import json
import math
import os
from pathlib import Path
import time
from power_trace import METER_MAC,METER_HOST,detect;import labconf

CONFIG=Path(labconf.get('TAPO_CONFIG','/etc/pswloader/tapo.json'))
EXPORTER=Path(labconf.get('TAPO_EXPORTER','/opt/pswloader-tapo/exporter.py'))


def trace_evidence(path):
    rows=[json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    if not rows or any(r.get('mac','').replace('-',':').lower()!=METER_MAC for r in rows):
        raise ValueError('Trace meter identity mismatch')
    times=[r.get('sampled_at') for r in rows]
    if any(not isinstance(t,(int,float)) or not math.isfinite(t) for t in times):
        raise ValueError('Invalid trace timestamps')
    if any(b<=a for a,b in zip(times,times[1:])):
        raise ValueError('Trace timestamps must increase')
    span=times[-1]-times[0]
    if span<500:raise ValueError('Measurement is not complete: less than 500 seconds')
    age=time.time()-times[-1]
    if not 0<=age<=120:raise ValueError('Trace must end within the last 120 seconds')
    calibration=detect(rows)
    calibrated=calibration['detected']
    if calibrated:
        start=calibration.get('start')
        if not isinstance(start,(int,float)) or not math.isfinite(start) or times[-1]<start+448:
            raise ValueError('Calibrated packet observation is not complete')
    if not calibrated and span<850:
        raise ValueError('Uncalibrated measurement requires 850 seconds')
    stat=path.stat()
    return {'span_seconds':span,'calibrated':calibrated},(stat.st_ino,stat.st_size,stat.st_mtime_ns)


def remote_available(observation):
    if not isinstance(observation,dict) or not {'linux','console','services_available'}<=observation.keys():
        raise ValueError('Incomplete controller inspection')
    linux=observation['linux'];console=observation['console']
    if (linux is not None and not isinstance(linux,(bool,dict))) or (
            console is not None and not isinstance(console,dict)) or not isinstance(observation['services_available'],bool):
        raise ValueError('Invalid controller inspection')
    return isinstance(linux,dict) or linux is True or console is not None or observation['services_available']


async def inspect_controller():
    # Each invocation constructs a new Hardware object, bypassing its in-process
    # discovery cache. No request to the normal supervisor or boot endpoint.
    source="""import json
from pathlib import Path
from controller import Hardware
h=Hardware(json.loads(Path('/config/config.json').read_text()))
h.start_deadline()
print(json.dumps(h.inspect()))
"""
    process=await asyncio.create_subprocess_exec('docker','exec','-i',labconf.get('REMOTEPLAY_CONTAINER','remoteplay'),'python3','-c',source,
        stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
    try:
        stdout,_=await asyncio.wait_for(process.communicate(),timeout=45)
    except BaseException:
        process.kill();await process.wait();raise
    if process.returncode:raise RuntimeError('Controller inspection failed')
    try:return json.loads(stdout.decode().splitlines()[-1])
    except (ValueError,IndexError):raise RuntimeError('Controller inspection was not JSON') from None


async def discover_meter():
    from kasa import Discover
    cfg=json.loads(CONFIG.read_text())
    items=[d for d in cfg['devices'] if d.get('host')==METER_HOST
           and d.get('mac','').replace('-',':').lower()==METER_MAC]
    if len(items)!=1:raise ValueError('Trusted meter mapping mismatch')
    spec=importlib.util.spec_from_file_location('native_trial_tapo_exporter',EXPORTER)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    auth=module.credentials(cfg['credentials_file'])
    device=await Discover.discover_single(items[0]['address'],username=auth['username'],
        password=auth['password'],timeout=5,discovery_timeout=3)
    if device is None:raise RuntimeError('Meter unavailable')
    return device


class Journal:
    def __init__(self,path):
        self.fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
        try:
            os.fsync(self.fd)
            directory=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
            try:os.fsync(directory)
            finally:os.close(directory)
        except BaseException:
            os.close(self.fd);raise
    def write(self,event,**fields):
        raw=(json.dumps({'event':event,'at':time.time(),**fields})+'\n').encode()
        while raw:
            count=os.write(self.fd,raw)
            if count<=0:raise OSError('Short journal write')
            raw=raw[count:]
        os.fsync(self.fd)
    def close(self):os.close(self.fd)


async def bounded(operation):
    return await asyncio.wait_for(operation,timeout=8)


async def finish(trace,journal_path,execute=False,*,inspect=inspect_controller,
                 discover=discover_meter,sleep=asyncio.sleep,journal_factory=Journal):
    evidence,signature=trace_evidence(trace)
    journal=journal_factory(journal_path)
    try:
        journal.write('measurement_complete',**evidence,execute=execute)
        observations=[]
        for index in range(2):
            if index:await sleep(5)
            observation=await inspect()
            observations.append(remote_available(observation))
            journal.write('fresh_inspection',number=index+1,remote_available=observations[-1])
        stat=trace.stat()
        if signature!=(stat.st_ino,stat.st_size,stat.st_mtime_ns):
            raise ValueError('Recorder is still changing the trace')
        if any(observations):
            journal.write('skip_remote_available');return 'skip_remote_available'
        if not execute:
            journal.write('dry_run_no_relay');return 'dry_run_no_relay'
        device=await discover()
        try:
            await bounded(device.update())
            if device.mac.replace('-',':').lower()!=METER_MAC or device.children:
                raise ValueError('Relay identity mismatch or child devices present')
            journal.write('off_intent')
            # Enter restoration protection before attempting the first off. A
            # timeout/error may mean the relay switched despite no response.
            try:
                await bounded(device.turn_off())
                await bounded(device.update())
                if device.is_on is not False:raise RuntimeError('Off readback failed')
                journal.write('off_confirmed')
                await sleep(10)
            finally:
                restored=False
                for attempt in range(3):
                    try:
                        await bounded(device.turn_on())
                        await bounded(device.update())
                        if device.is_on is not True:raise RuntimeError('On readback failed')
                        restored=True
                        break
                    except Exception:
                        if attempt<2:await sleep(1)
                # Logging is deliberately after restoration attempts, so a
                # failed disk/fsync cannot prevent returning mains power.
                journal.write('on_confirmed' if restored else 'restore_failed')
                if not restored:raise RuntimeError('Relay on could not be confirmed')
            return 'power_cycled'
        finally:
            await bounded(device.disconnect())
    finally:journal.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--journal',type=Path,required=True)
    parser.add_argument('--execute',action='store_true')
    args=parser.parse_args()
    try:result=asyncio.run(finish(args.trace,args.journal,args.execute))
    except Exception as error:
        # Never print exception text from a network library or credentials loader.
        print(json.dumps({'finished':False,'error_type':type(error).__name__}));raise SystemExit(1)
    print(json.dumps({'finished':True,'result':result}))

if __name__=='__main__':main()
