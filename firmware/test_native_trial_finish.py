"""Post-measurement recovery tests with injected devices; never use a relay."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import native_trial_finish as finish

class Device:
    mac=finish.METER_MAC
    children={}
    def __init__(self,events,off_error=False,on_failures=0):
        self.events=events;self.is_on=True;self.off_error=off_error;self.on_failures=on_failures
    async def update(self):self.events.append('read')
    async def turn_off(self):
        self.events.append('off');self.is_on=False
        if self.off_error:raise TimeoutError('must not be printed')
    async def turn_on(self):
        self.events.append('on')
        if self.on_failures:self.on_failures-=1;raise TimeoutError()
        self.is_on=True
    async def disconnect(self):self.events.append('disconnect')

class FinishTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.work=tempfile.TemporaryDirectory();self.root=Path(self.work.name)
        self.trace=self.root/'trace.jsonl';self.journal=self.root/'finish.jsonl'
        self.trace.write_text(''.join(json.dumps({'sampled_at':1000+i,'watts':80,
            'mac':finish.METER_MAC,'up':True})+'\n' for i in range(851)))
        self.events=[];self.device=Device(self.events);self.available=False
        self.patch=patch.object(finish,'detect',return_value={'detected':False});self.patch.start()
        self.clock=patch.object(finish.time,'time',return_value=1850);self.clock.start()
    def tearDown(self):self.clock.stop();self.patch.stop();self.work.cleanup()
    async def inspect(self):
        self.events.append('inspect')
        return {'linux':self.available,'console':None,'services_available':False}
    async def discover(self):self.events.append('discover');return self.device
    async def sleep(self,seconds):self.events.append(('sleep',seconds))
    async def run_finish(self,**kwargs):
        return await finish.finish(self.trace,self.journal,True,inspect=self.inspect,
            discover=self.discover,sleep=self.sleep,**kwargs)
    async def test_remote_access_prevents_discovery_and_off(self):
        self.available=True
        self.assertEqual(await self.run_finish(),'skip_remote_available')
        self.assertEqual(self.events,['inspect',('sleep',5),'inspect'])
        self.assertIn('skip_remote_available',self.journal.read_text())
    async def test_console_or_services_also_prevent_discovery(self):
        for field,value in (('console',{'state':'standby'}),('services_available',True)):
            self.journal.unlink(missing_ok=True)
            async def inspect():
                result={'linux':False,'console':None,'services_available':False};result[field]=value
                return result
            result=await finish.finish(self.trace,self.journal,True,inspect=inspect,
                discover=self.discover,sleep=self.sleep)
            self.assertEqual(result,'skip_remote_available')
        self.assertNotIn('discover',self.events)
    async def test_successful_off_confirm_ten_seconds_and_on(self):
        self.assertEqual(await self.run_finish(),'power_cycled')
        self.assertEqual(self.events,['inspect',('sleep',5),'inspect','discover','read',
            'off','read',('sleep',10),'on','read','disconnect'])
        self.assertEqual(self.journal.stat().st_mode&0o777,0o600)
        self.assertIn('on_confirmed',self.journal.read_text())
    async def test_wrong_mac_or_children_never_switch(self):
        for mode in ('mac','children'):
            self.journal.unlink(missing_ok=True);self.device=Device(self.events)
            if mode=='mac':self.device.mac='00:00:00:00:00:00'
            else:self.device.children={'child':object()}
            with self.assertRaises(ValueError):await self.run_finish()
        self.assertNotIn('off',self.events);self.assertNotIn('on',self.events)
    async def test_existing_journal_is_never_replayed(self):
        await self.run_finish();self.events.clear()
        with self.assertRaises(FileExistsError):await self.run_finish()
        self.assertEqual(self.events,[])
    async def test_off_error_still_restores_on(self):
        self.device.off_error=True
        with self.assertRaises(TimeoutError):await self.run_finish()
        self.assertEqual(self.events.count('off'),1);self.assertEqual(self.events.count('on'),1)
        self.assertTrue(self.device.is_on)
    async def test_on_retries_bounded_without_repeating_off(self):
        self.device.on_failures=2
        self.assertEqual(await self.run_finish(),'power_cycled')
        self.assertEqual(self.events.count('off'),1);self.assertEqual(self.events.count('on'),3)
        self.assertEqual(self.events.count(('sleep',1)),2)
    async def test_failed_on_readback_is_recorded_after_three_attempts(self):
        self.device.on_failures=4
        with self.assertRaisesRegex(RuntimeError,'could not be confirmed'):await self.run_finish()
        self.assertEqual(self.events.count('on'),3)
        self.assertIn('restore_failed',self.journal.read_text())
    async def test_logging_failure_after_off_cannot_prevent_restore(self):
        class BadJournal(finish.Journal):
            def write(self,event,**fields):
                if event=='off_confirmed':raise OSError('disk failed')
                super().write(event,**fields)
        with self.assertRaises(OSError):await self.run_finish(journal_factory=BadJournal)
        self.assertEqual(self.events.count('on'),1);self.assertTrue(self.device.is_on)
    async def test_changing_trace_prevents_discovery(self):
        async def sleep(seconds):
            self.trace.write_text(self.trace.read_text()+'\n')
        with self.assertRaisesRegex(ValueError,'still changing'):
            await finish.finish(self.trace,self.journal,True,inspect=self.inspect,
                discover=self.discover,sleep=sleep)
        self.assertNotIn('discover',self.events)
    async def test_short_or_wrong_identity_measurements_are_rejected(self):
        rows=self.trace.read_text().splitlines()
        for data in (rows[:500],[json.dumps({'sampled_at':1000,'mac':'other'})]):
            self.trace.write_text('\n'.join(data))
            with self.assertRaises(ValueError):await self.run_finish()
        self.assertFalse(self.journal.exists());self.assertEqual(self.events,[])
    async def test_uncalibrated_needs_850_seconds_calibrated_needs_500(self):
        self.trace.write_text('\n'.join(self.trace.read_text().splitlines()[:501]))
        with patch.object(finish.time,'time',return_value=1500):
            with self.assertRaisesRegex(ValueError,'850'):await self.run_finish()
        with patch.object(finish.time,'time',return_value=1500),patch.object(finish,'detect',return_value={'detected':True,'start':1000}):
            self.assertEqual(await self.run_finish(),'power_cycled')
    async def test_actual_controller_null_and_dict_linux_values(self):
        for linux,expected in ((None,'power_cycled'),({'status':'ready'},'skip_remote_available'),({},'skip_remote_available')):
            self.journal.unlink(missing_ok=True);self.events.clear()
            async def inspect():
                return {'linux':linux,'console':None,'services_available':False}
            result=await finish.finish(self.trace,self.journal,True,inspect=inspect,
                discover=self.discover,sleep=self.sleep)
            self.assertEqual(result,expected)
            if linux is not None:self.assertNotIn('discover',self.events)
        for field,value in (('linux','unknown'),('linux',[]),('console',False),
                            ('console','ready'),('services_available',None)):
            observation={'linux':None,'console':None,'services_available':False}
            observation[field]=value
            with self.assertRaises(ValueError):finish.remote_available(observation)
    async def test_late_calibration_requires_complete_packet_window(self):
        with patch.object(finish,'detect',return_value={'detected':True,'start':1500}):
            with self.assertRaisesRegex(ValueError,'packet observation'):await self.run_finish()
        self.assertFalse(self.journal.exists());self.assertEqual(self.events,[])
        with patch.object(finish,'detect',return_value={'detected':True,'start':1402}):
            self.assertEqual(await self.run_finish(),'power_cycled')
    async def test_old_and_future_traces_cannot_cut_a_later_trial(self):
        for now in (1849,1971):
            with patch.object(finish.time,'time',return_value=now):
                with self.assertRaisesRegex(ValueError,'120 seconds'):await self.run_finish()
        self.assertFalse(self.journal.exists());self.assertEqual(self.events,[])

    async def test_dry_run_never_discovers_relay(self):
        self.assertEqual(await finish.finish(self.trace,self.journal,False,
            inspect=self.inspect,discover=self.discover,sleep=self.sleep),'dry_run_no_relay')
        self.assertNotIn('discover',self.events)

if __name__=='__main__':unittest.main()
