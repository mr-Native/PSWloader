#!/usr/bin/env python3
"""Read-only PS5 meter trace and experimental load-pattern calibration detector.

Does not switch the relay or alter the monitoring service. A detected pattern
is only a channel calibration, never proof of Windows/UEFI or an error code.
"""
import argparse
import asyncio
import bisect
import importlib.util
import json
import math
from pathlib import Path
import statistics
import time

PATTERN=(0,1,0,1,1,0,0,1)
SLOT_SECONDS=8
import labconf;METER_MAC=labconf.get('METER_MAC','02:00:00:00:00:01').lower();METER_HOST=labconf.get('METER_HOST','ps5-linux')

async def record(output,duration,config_path,exporter_path):
    from kasa import Discover,Module
    spec=importlib.util.spec_from_file_location('tapo_reader',exporter_path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    cfg=json.loads(config_path.read_text())
    items=[d for d in cfg['devices'] if d['host']==METER_HOST and d['mac'].lower()==METER_MAC]
    if len(items)!=1:raise ValueError('Meter mapping mismatch')
    auth=module.credentials(cfg['credentials_file'])
    device=await Discover.discover_single(items[0]['address'],username=auth['username'],password=auth['password'],timeout=5,discovery_timeout=3)
    if device is None:raise RuntimeError('Meter unavailable')
    try:
        await device.update()
        if device.mac.replace('-',':').lower()!=METER_MAC:raise RuntimeError('Meter identity mismatch')
        energy=device.modules.get(Module.Energy)
        if energy is None:raise RuntimeError('No energy module')
        started=time.monotonic()
        with output.open('x') as out:
            while time.monotonic()-started<duration:
                before=time.time();tick=time.monotonic()
                try:
                    status=await asyncio.wait_for(energy.get_status(),timeout=4)
                    end=time.time();watts=status.power
                    if watts is None or not math.isfinite(watts):raise ValueError('Invalid power sample')
                    row={'sampled_at':(before+end)/2,'query_seconds':end-before,'watts':watts,'up':True,'mac':METER_MAC}
                except Exception as error:
                    row={'sampled_at':time.time(),'up':False,'error':type(error).__name__,'mac':METER_MAC}
                out.write(json.dumps(row)+'\n');out.flush()
                await asyncio.sleep(max(0,1-(time.monotonic()-tick)))
    finally:await device.disconnect()


def detect(rows,start=None):
    if not rows:return {'detected':False,'reason':'empty trace'}
    samples=[]
    for row in rows:
        if not row.get('up') or row.get('mac','').lower()!=METER_MAC:continue
        t=row.get('sampled_at');p=row.get('watts');latency=row.get('query_seconds',0)
        if not all(isinstance(v,(int,float)) and math.isfinite(v) for v in (t,p,latency)):continue
        if p<0 or latency<0 or latency>2:continue
        samples.append((t,p))
    samples.sort()
    if len(samples)<32:return {'detected':False,'reason':'insufficient fresh samples'}
    if any(b[0]<=a[0] for a,b in zip(samples,samples[1:])):
        return {'detected':False,'reason':'duplicate sample timestamps'}
    timestamps=[t for t,_ in samples]
    count=max(0,int(samples[-1][0]-samples[0][0]-64)+1)
    coarse=[start] if start is not None else [samples[0][0]+i for i in range(count)]
    best=None
    for fine in (False,True):
        if fine:
            if start is not None or best is not None:break
            # Calibration statistics change only when a sample enters/leaves
            # one of the eight inclusive slot windows. Test positive-width
            # interval midpoints, rather than assuming an arbitrary time grid
            # can hit a valid phase. Bound origins exactly as the full search.
            lower,upper=samples[0][0],samples[-1][0]-64
            events={lower,upper}
            for timestamp in timestamps:
                for index in range(8):
                    for edge in (2,7):
                        origin=timestamp-index*8-edge
                        if lower<=origin<=upper:events.add(origin)
            events=sorted(events)
            tested=set(coarse)
            # Exact edges can form isolated sample sets under inclusive bounds;
            # require a positive-width phase interval instead of such coincidences.
            starts=sorted({(a+b)/2 for a,b in zip(events,events[1:]) if a<b}-tested)
        else:
            starts=coarse
        for origin in starts:
            windows=[]
            for index in range(8):
                # Discard transitions/possible meter lag; require coverage per slot.
                left=bisect.bisect_left(timestamps,origin+index*8+2)
                right=bisect.bisect_right(timestamps,origin+(index+1)*8-1)
                points=samples[left:right]
                if len(points)<3 or points[-1][0]-points[0][0]<2 or any(b[0]-a[0]>2.5 for a,b in zip(points,points[1:])):break
                windows.append([p for _,p in points])
            if len(windows)!=8:continue
            medians=[statistics.median(w) for w in windows]
            # Fit baseline + linear warm-up drift + the known modulation. Requiring
            # all eight symbols, strong residual SNR and a later CRC packet prevents
            # interpreting a monotonic warm-up ramp as a transmitted message.
            x=[[1,index*8+4.5,bit] for index,bit in enumerate(PATTERN)]
            matrix=[[sum(v[i]*v[j] for v in x) for j in range(3)]+[sum(v[i]*y for v,y in zip(x,medians))] for i in range(3)]
            for col in range(3):
                pivot=matrix[col][col]
                matrix[col]=[v/pivot for v in matrix[col]]
                for row in range(3):
                    if row==col:continue
                    factor=matrix[row][col]
                    matrix[row]=[v-factor*u for v,u in zip(matrix[row],matrix[col])]
            low,slope,contrast=[matrix[i][3] for i in range(3)]
            residual=[abs(p-(low+slope*(index*8+4.5)+contrast*bit)) for index,(w,bit) in enumerate(zip(windows,PATTERN)) for p in w]
            noise=statistics.median(residual)
            margin=min((m-low-slope*(index*8+4.5)-contrast/2)*(1 if bit else -1) for index,(m,bit) in enumerate(zip(medians,PATTERN)))
            if contrast<.6 or contrast<8*max(noise,.025) or margin<.15:continue
            result={'detected':True,'scope':'Experimental load-channel calibration only, no boot/error code',
                    'start':origin,'contrast_watts':contrast,'residual_mad_watts':noise,'minimum_margin_watts':margin,
                    'drift_watts_per_second':slope,'slot_medians_watts':medians}
            if best is None or result['minimum_margin_watts']>best['minimum_margin_watts']:best=result
    return best or {'detected':False,'reason':'pattern incomplete or contrast/quality too low'}

def crc16(code):
    crc=0xffff
    for value in (code>>8,code&255):
        crc^=value<<8
        for _ in range(8):crc=((crc<<1)^(0x1021 if crc&0x8000 else 0))&65535
    return crc


def decode_packet(rows,calibration,offset_min=56,offset_max=120):
    if not calibration.get('detected'):return {'decoded':False,'reason':'calibration not established'}
    if not (0<=offset_min<=offset_max<=1800):raise ValueError('Invalid bounded packet window')
    # Only timely identity-checked samples. A receive timestamp is not proof of
    # a meter's internal sampling cadence; native calibration must establish it.
    samples=sorted((r['sampled_at'],r['watts']) for r in rows
                   if r.get('up') and r.get('mac','').lower()==METER_MAC
                   and all(isinstance(r.get(k),(int,float)) and math.isfinite(r[k]) for k in ('sampled_at','watts'))
                   and 0<=r.get('query_seconds',0)<=2)
    if len({t for t,_ in samples})!=len(samples):return {'decoded':False,'reason':'duplicate timestamps'}
    threshold=max(.25,calibration['contrast_watts']/4,4*calibration['residual_mad_watts'])
    fallback_slope=calibration.get('drift_watts_per_second',0)
    timestamps=[t for t,_ in samples]
    matches=[]
    for step in range(round((offset_max-offset_min)*10)+1):
        origin=calibration['start']+offset_min+step/10
        pairs=[]
        for index in range(48):
            halves=[]
            for half in range(2):
                left=origin+index*8+half*4
                points=samples[bisect.bisect_left(timestamps,left+.75):
                               bisect.bisect_right(timestamps,left+3.25)]
                if len(points)<2 or points[-1][0]-points[0][0]<.75 or any(b[0]-a[0]>2.5 for a,b in zip(points,points[1:])):break
                values=[p for _,p in points];median=statistics.median(values)
                halves.append((median,statistics.median(abs(p-median) for p in values),statistics.median(t for t,_ in points)))
            if len(halves)!=2:break
            pairs.append(halves)
        if len(pairs)!=48:continue
        # Each Manchester pair contains one high and one low half. Its mean
        # cancels modulation without knowing the bit, exposing common-mode
        # drift. Estimate that drift locally so calibration warmup need not
        # continue throughout the packet. Theil-Sen resists isolated outliers.
        means=[((a[2]+b[2])/2,(a[0]+b[0])/2) for a,b in pairs]
        bits=[]
        for index,halves in enumerate(pairs):
            neighbors=means[max(0,index-4):min(48,index+5)]
            slopes=[(b[1]-a[1])/(b[0]-a[0]) for i,a in enumerate(neighbors)
                    for b in neighbors[i+1:] if b[0]>a[0]]
            slope=statistics.median(slopes) if slopes else fallback_slope
            difference=halves[0][0]-halves[1][0]-slope*(halves[0][2]-halves[1][2])
            if abs(difference)<threshold or max(halves[0][1],halves[1][1])>abs(difference)/3:break
            bits.append(int(difference>0))
        if len(bits)!=48:continue
        packet=0
        for bit in bits:packet=(packet<<1)|bit
        sync=packet>>32;code=(packet>>16)&65535;checksum=packet&65535
        category,detail=code>>8,code&255
        offset=(code>>8)&63
        pci_byte=bool(code&0x8000) and (offset<=4 or offset==0x0e or 0x18<=offset<=0x1a or 0x1c<=offset<=0x1d or 0x20<=offset<=0x33)
        known=category==8 or pci_byte or code in (3,0x701,0x703,0x704,0x705,0x706,0x707,0x708,0x709,0x70a,0x70b,0x70c,0x70d,0x70e) or 0x711<=code<=0x717 or category==1 and detail<=1 or category==2 and 1<=detail<=8 or code==0x300 or category==4 and (detail<=31 or detail==255) or category==5 and 1<=detail<=5 or category==6 and 1<=detail<=4
        if sync==0xd391 and checksum==crc16(code) and known:matches.append((origin,code))
    codes={code for _,code in matches}
    if len(codes)!=1:return {'decoded':False,'reason':'no complete valid packet or conflicting packets'}
    code=codes.pop()
    result={'decoded':True,'code':code,'code_hex':hex(code),'frame_start_candidates':[t for t,c in matches if c==code],
            'scope':'Experimental native-platform diagnostic packet, not Windows boot proof'}
    if code&0x8000:
        result.update(kind='pci_profile_byte',bridge='amd' if code&0x4000 else 'sony',
                      bdf='00:08.1' if code&0x4000 else '00:01.1',register_offset=(code>>8)&63,
                      actual_byte=code&255)
    elif code>>8==8:result.update(kind='dxe_unhandled_vector',vector=code&255)
    elif code==0x701:result['kind']='uefi_entry'
    elif code==0x703:result['kind']='uefi_dxe_ready'
    elif code==0x704:result['kind']='dxe_core_entry'
    elif code==0x705:result['kind']='dxe_exception_setup_passed'
    elif code==0x706:result['kind']='dxe_memory_services_passed'
    elif code==0x707:result['kind']='dxe_before_first_dispatch'
    elif code==0x708:result.update(kind='log_export_entry_protocols_present',missing_protocol_mask=0)
    elif code==0x709:result['kind']='cpu_driver_entry'
    elif code==0x70a:result['kind']='cpu_before_mp'
    elif code==0x70b:result['kind']='cpu_before_gcd_refresh'
    elif code==0x70c:result['kind']='cpu_page_table_initialization_returned'
    elif code==0x70d:result['kind']='cpu_gdt_initialization_returned'
    elif code==0x70e:result['kind']='cpu_idt_initialization_returned'
    elif 0x711<=code<=0x717:
        result.update(kind='log_export_entry_protocols_missing',missing_protocol_mask=code&7,
                      missing_protocols=[name for bit,name in ((1,'cpu'),(2,'rtc'),(4,'timer')) if code&bit])
    else:result['kind']='checkpoint_or_stop'
    return result


class PacketStream:
    """Retain calibration and scan later frames without replaying old packets.

    Each bounded block uses the same CRC, whitelist and signal quality checks.
    Progress is not terminal; callers apply native_recovery_gate to each event.
    """
    def __init__(self):
        self.calibration=None
        self.cursor=56.0

    def update(self,rows):
        if self.calibration is None:
            calibration=detect(rows)
            if not calibration.get('detected'):return []
            self.calibration=calibration
        valid=[r['sampled_at'] for r in rows if r.get('up')
               and r.get('mac','').lower()==METER_MAC
               and isinstance(r.get('sampled_at'),(int,float))
               and math.isfinite(r['sampled_at'])]
        if not valid:return []
        available=max(valid)-self.calibration['start']-383.25
        events=[]
        while self.cursor<=min(available,1800):
            upper=min(self.cursor+64,available,1800)
            packet=decode_packet(rows,self.calibration,self.cursor,upper)
            if packet.get('decoded'):
                events.append({'calibration':self.calibration,'packet':packet})
                # Allow phase uncertainty near the next contiguous frame. This
                # cannot re-accept the old frame (its start is >383s earlier).
                self.cursor=min(packet['frame_start_candidates'])-self.calibration['start']+383
            elif upper>=self.cursor+64:
                self.cursor+=64
            else:
                break
        return events


def watch_chain(path,timeout):
    deadline=time.monotonic()+timeout
    stream=PacketStream()
    while time.monotonic()<deadline:
        rows=[]
        if path.exists():
            for line in path.read_text().splitlines():
                try:rows.append(json.loads(line))
                except json.JSONDecodeError:continue
        for event in stream.update(rows):
            print(json.dumps({'event':'packet',**event}),flush=True)
        time.sleep(min(10,max(0,deadline-time.monotonic())))
    print(json.dumps({'event':'observation_complete','reason':'bounded stream window expired'}),flush=True)



def watch(path,timeout):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        rows=[]
        if path.exists():
            # Recorder appends and flushes complete lines. Ignore a torn final
            # line during a concurrent read; never manufacture a sample.
            for line in path.read_text().splitlines():
                try:rows.append(json.loads(line))
                except json.JSONDecodeError:continue
        calibration=detect(rows)
        packet=decode_packet(rows,calibration)
        if packet.get('decoded'):return {'calibration':calibration,'packet':packet}
        # Every searched frame starts by calibration+120s and its final
        # sampling window ends by +503.25s. Allow a conservative +504s;
        # missing/noisy samples remain a terminal negative, never guessed bits.
        timestamps=[r.get('sampled_at') for r in rows
                    if isinstance(r.get('sampled_at'),(int,float)) and math.isfinite(r['sampled_at'])]
        if calibration.get('detected') and timestamps and max(timestamps)>=calibration['start']+504:
            return {'calibration':calibration,
                    'packet':{'decoded':False,'reason':'complete bounded frame window without a valid packet'},
                    'observation_complete':True}
        time.sleep(min(10,max(0,deadline-time.monotonic())))
    return {'packet':{'decoded':False,'reason':'bounded observation expired without complete valid packet'}}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
    r=sub.add_parser('record');r.add_argument('output',type=Path);r.add_argument('--seconds',type=int,default=120)
    r.add_argument('--config',type=Path,default=Path(labconf.get('TAPO_CONFIG','/etc/pswloader/tapo.json')))
    r.add_argument('--exporter',type=Path,default=Path(labconf.get('TAPO_EXPORTER','/opt/pswloader-tapo/exporter.py')))
    d=sub.add_parser('detect');d.add_argument('trace',type=Path);d.add_argument('--start',type=float)
    w=sub.add_parser('watch');w.add_argument('trace',type=Path);w.add_argument('--seconds',type=int,default=900)
    c=sub.add_parser('watch-chain');c.add_argument('trace',type=Path);c.add_argument('--seconds',type=int,default=1800)
    a=p.parse_args()
    if a.operation=='record':
        # v3.03: full Windows setup runs need up to 1 h of trace (v3.02 recorder refused 2700 s)
        if not 1<=a.seconds<=3600:p.error('duration must be 1..3600 seconds')
        asyncio.run(record(a.output,a.seconds,a.config,a.exporter))
    elif a.operation=='watch':
        if not 1<=a.seconds<=1800:p.error('duration must be 1..1800 seconds')
        print(json.dumps(watch(a.trace,a.seconds),indent=2))
    elif a.operation=='watch-chain':
        if not 1<=a.seconds<=1800:p.error('duration must be 1..1800 seconds')
        watch_chain(a.trace,a.seconds)
    else:
        rows=[json.loads(line) for line in a.trace.read_text().splitlines()]
        calibration=detect(rows,a.start)
        print(json.dumps({'calibration':calibration,'packet':decode_packet(rows,calibration)},indent=2))
