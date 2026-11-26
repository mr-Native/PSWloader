"""Model tests for rejecting ambiguous power traces, not physical channel proof."""
import math
import random
import unittest
from unittest.mock import patch
import power_trace
from power_trace import PATTERN,METER_MAC,crc16,detect,decode_packet

class PowerTraceTests(unittest.TestCase):
    def trace(self,code=0x203,noise=.04,corrupt=False):
        rng=random.Random(17);levels=[]
        for bit in PATTERN:levels += [bit]*8
        packet=(0xd391<<32)|(code<<16)|(crc16(code)^(1 if corrupt else 0))
        for n in range(48):
            bit=(packet>>(47-n))&1;levels += [bit]*4+[1-bit]*4
        return [{'sampled_at':1000+i,'watts':80+4*level+rng.uniform(-noise,noise),'up':True,'mac':METER_MAC,'query_seconds':.05} for i,level in enumerate(levels)]
    def test_calibration_and_crc_packet(self):
        rows=self.trace();c=detect(rows,start=1000)
        self.assertTrue(c['detected']);self.assertEqual(decode_packet(rows,c)['code'],0x203)
    def test_subsecond_calibration_fallback_preserves_quality_gates(self):
        rng=random.Random(9);rows=[]
        for index in range(85):
            second=index*.9+rng.uniform(-.04,.04)
            level=PATTERN[int((second-.37)//8)] if .37<=second<64.37 else 0
            rows.append({'sampled_at':1000+second,
                'watts':130+.06*second+level+rng.uniform(-.30,.30),
                'up':True,'mac':METER_MAC})
        first=rows[0]['sampled_at']
        coarse_count=int(rows[-1]['sampled_at']-first-64)+1
        self.assertFalse(any(detect(rows,start=first+index)['detected']
                             for index in range(coarse_count)))
        # Even a 0.1s grid misses this narrow valid sample-inclusion interval.
        fine_count=int((rows[-1]['sampled_at']-first-64)*10)+1
        self.assertFalse(any(detect(rows,start=first+index/10)['detected']
                             for index in range(fine_count)))
        calibration=detect(rows)
        self.assertTrue(calibration['detected'])
        self.assertNotEqual((calibration['start']-first)%1,0)
        self.assertEqual(detect(rows,start=calibration['start']),calibration)
        self.assertGreaterEqual(calibration['contrast_watts'],8*calibration['residual_mad_watts'])
        # A valid calibration still supplies no invented packet bits.
        self.assertFalse(decode_packet(rows,calibration)['decoded'])
        for row in rows:row['watts']=130+.06*(row['sampled_at']-1000)
        self.assertFalse(detect(rows)['detected'])

    def test_corrupted_packet_is_not_reported_as_code(self):
        rows=self.trace(corrupt=True)
        self.assertFalse(decode_packet(rows,detect(rows,start=1000))['decoded'])
    def test_constant_power_and_linear_drift_are_not_calibration(self):
        rows=self.trace()
        for slope in (0,.03):
            for i,r in enumerate(rows):r['watts']=80+slope*i
            self.assertFalse(detect(rows,start=1000)['detected'])
    def test_gaps_or_wrong_identity_rejected(self):
        for mode in ('missing','wrong meter','slow query'):
            rows=self.trace()
            if mode=='missing':rows=[r for i,r in enumerate(rows) if not 10<=i<=15]
            if mode=='wrong meter':
                for r in rows:r['mac']='00:00:00:00:00:00'
            if mode=='slow query':
                for r in rows:r['query_seconds']=5
            self.assertFalse(detect(rows,start=1000)['detected'])
    def test_incomplete_packet_or_duplicate_samples_rejected(self):
        rows=self.trace();c=detect(rows,start=1000)
        self.assertFalse(decode_packet(rows[:100],c)['decoded'])
        self.assertFalse(decode_packet(rows+[rows[0]],c)['decoded'])
    def test_unknown_code_rejected_even_with_valid_crc(self):
        for code in (0x777,0x710,0x718):
            rows=self.trace(code=code)
            self.assertFalse(decode_packet(rows,detect(rows,start=1000))['decoded'])
    def test_fine_phase_grid_recovers_jittered_packet_coarse_grid_misses(self):
        # 0.9s meter cadence, 40ms receive jitter and sub-watt sample noise
        # reproduce a valid window between the old 0.5s phase candidates.
        for corrupt in (False,True):
            rng=random.Random(13);levels=[]
            packet=(0xd391<<32)|(0x704<<16)|(crc16(0x704)^(1 if corrupt else 0))
            for index in range(48):
                bit=(packet>>(47-index))&1;levels += [bit]*4+[1-bit]*4
            rows=[]
            for index in range(500):
                second=index*.9+rng.uniform(-.04,.04)
                if second<64:level=PATTERN[int(second)//8] if second>=0 else 0
                else:
                    offset=int(second-64-.2)
                    level=levels[offset] if 0<=offset<len(levels) else 0
                rows.append({'sampled_at':1000+second,
                    'watts':140+level+rng.uniform(-.35,.35),
                    'up':True,'mac':METER_MAC,'query_seconds':.05})
            calibration=detect(rows,start=1000)
            self.assertTrue(calibration['detected'])
            def coarse_range(count):
                return range(0,641,5) if count==641 else range(count)
            with patch.object(power_trace,'range',coarse_range,create=True):
                self.assertFalse(decode_packet(rows,calibration)['decoded'])
            result=decode_packet(rows,calibration)
            self.assertEqual(result['decoded'],not corrupt)
            if not corrupt:
                self.assertEqual(result['code'],0x704)
                self.assertEqual(result['frame_start_candidates'],[1064.9])

    def test_known_crc_vector(self):
        import binascii
        self.assertEqual(crc16(0x203),binascii.crc_hqx(bytes([2,3]),0xffff))

    def test_watcher_decodes_existing_trace_without_human_input(self):
        import tempfile,json
        from pathlib import Path
        from power_trace import watch
        with tempfile.TemporaryDirectory() as work:
            path=Path(work)/'trace.jsonl'
            path.write_text(''.join(json.dumps(r)+'\n' for r in self.trace()))
            self.assertEqual(watch(path,2)['packet']['code'],0x203)
            self.assertFalse(watch(path,0)['packet']['decoded'])

    def test_watcher_complete_bad_crc_or_noise_returns_without_waiting(self):
        import tempfile,json
        from pathlib import Path
        for mode in ('bad crc','noise'):
            rows=self.trace(corrupt=True)
            if mode=='noise':
                rng=random.Random(81)
                for row in rows[64:]:row['watts']=80+rng.uniform(-.04,.04)
            rows += [{'sampled_at':1000+i,'watts':80,'up':True,'mac':METER_MAC}
                     for i in range(448,505)]
            with tempfile.TemporaryDirectory() as work:
                path=Path(work)/'trace.jsonl'
                path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
                with patch.object(power_trace.time,'monotonic',return_value=0), \
                     patch.object(power_trace.time,'sleep',side_effect=AssertionError('must not wait')):
                    result=power_trace.watch(path,850)
                self.assertFalse(result['packet']['decoded'])
                self.assertTrue(result['observation_complete'])
                self.assertIn('complete bounded frame window',result['packet']['reason'])
                self.assertTrue(result['calibration']['detected'])
    def test_watcher_does_not_close_partial_or_uncalibrated_observation(self):
        import tempfile,json
        from pathlib import Path
        for mode in ('partial','no calibration'):
            rows=self.trace(corrupt=True)
            end=503 if mode=='partial' else 850
            rows += [{'sampled_at':1000+i,'watts':80,'up':True,'mac':METER_MAC}
                     for i in range(448,end+1)]
            if mode=='no calibration':
                for row in rows:row['watts']=80
            # A nonfinite receive timestamp cannot complete a partial trace.
            rows.append({'sampled_at':float('inf'),'watts':80,'up':False,'mac':METER_MAC})
            with tempfile.TemporaryDirectory() as work:
                path=Path(work)/'trace.jsonl'
                path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
                with patch.object(power_trace.time,'monotonic',side_effect=[0,0,0,850]), \
                     patch.object(power_trace.time,'sleep') as sleep:
                    result=power_trace.watch(path,850)
                sleep.assert_called_once_with(10)
                self.assertFalse(result['packet']['decoded'])
                self.assertNotIn('observation_complete',result)
                self.assertIn('expired',result['packet']['reason'])

    def test_weak_modulation_with_warmup_drift_requires_valid_crc(self):
        for corrupt in (False,True):
            rows=self.trace(corrupt=corrupt)
            for i,r in enumerate(rows):
                level=(r['watts']-80)/4
                r['watts']=130+.06*i+.9*level
            c=detect(rows,start=1000)
            self.assertTrue(c['detected'])
            self.assertAlmostEqual(c['contrast_watts'],.9,delta=.05)
            self.assertEqual(decode_packet(rows,c)['decoded'],not corrupt)
    def test_nonlinear_warmup_plateaus_without_relaxing_packet_gates(self):
        for corrupt in (False,True):
            rows=self.trace(code=0x701,corrupt=corrupt)
            for i,row in enumerate(rows):
                level=(row['watts']-80)/4
                baseline=.06*min(i,64)+3*(1-math.exp(-max(0,i-64)/50))
                row['watts']=130+baseline+(.9 if i<64 else .5)*level
            calibration=detect(rows,start=1000)
            self.assertTrue(calibration['detected'])
            result=decode_packet(rows,calibration)
            self.assertEqual(result['decoded'],not corrupt)
            if not corrupt:
                self.assertEqual((result['code'],result['kind']),(0x701,'uefi_entry'))
                self.assertIn('native-platform',result['scope'])
        # Common-mode shape alone must never become a packet after a valid
        # calibration; drift estimation supplies no missing Manchester bits.
        for i,row in enumerate(rows):
            if i>=64:row['watts']=130+.06*64+3*(1-math.exp(-(i-64)/50))
        self.assertFalse(decode_packet(rows,calibration)['decoded'])

    def test_inverted_or_heavy_noise_calibration_is_rejected(self):
        rows=self.trace()
        for r in rows:r['watts']=160-r['watts']
        self.assertFalse(detect(rows,start=1000)['detected'])
        rows=self.trace(noise=5)
        self.assertFalse(detect(rows,start=1000)['detected'])

    def test_pci_byte_is_typed_and_bad_register_is_rejected(self):
        for code,bridge in ((0x8406,'sony'),(0xc406,'amd')):
            rows=self.trace(code=code);result=decode_packet(rows,detect(rows,start=1000))
            self.assertTrue(result['decoded'])
            self.assertEqual((result['kind'],result['bridge'],result['register_offset'],result['actual_byte']),('pci_profile_byte',bridge,4,6))
        rows=self.trace(code=0xffff)
        self.assertFalse(decode_packet(rows,detect(rows,start=1000))['decoded'])

    def test_noisy_halfbit_keeps_sign_and_erased_bit_is_not_filled(self):
        for corrupt in (False,True):
            rows=self.trace(corrupt=corrupt)
            left=64+38*8
            for index,delta in zip((left+1,left+2,left+3),(-1.1,0,1.1)):
                rows[index]['watts']+=delta
            result=decode_packet(rows,detect(rows,start=1000))
            self.assertEqual(result['decoded'],not corrupt)
        rows=self.trace();left=64+38*8
        rows=[r for i,r in enumerate(rows) if not left-4<=i<left+12]
        self.assertFalse(decode_packet(rows,detect(rows,start=1000))['decoded'])
