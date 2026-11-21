import unittest
import test_power_trace as fixtures
from power_trace import PacketStream,decode_packet,detect
from native_recovery_gate import known_terminal_stop


class PowerStreamTests(unittest.TestCase):
    def chain(self,corrupt=False,gap=150):
        fixture=fixtures.PowerTraceTests()
        first=fixture.trace(code=0x714)
        second=fixture.trace(code=0x8ef,corrupt=corrupt)[64:]
        delta=first[-1]['sampled_at']+1+gap-second[0]['sampled_at']
        for row in second:row['sampled_at']+=delta
        rows=first+second
        for _ in range(3):
            row=dict(rows[-1]);row['sampled_at']+=1;row['watts']=80;rows.append(row)
        return first,rows

    def test_progress_followed_by_later_fault_is_retained_once(self):
        first,rows=self.chain();stream=PacketStream()
        events=stream.update(first)
        self.assertEqual([e['packet']['code'] for e in events],[0x714])
        self.assertFalse(known_terminal_stop(events[0]))
        later=stream.update(rows)
        self.assertEqual([e['packet']['code'] for e in later],[0x8ef])
        self.assertEqual(later[0]['packet']['vector'],239)
        self.assertTrue(known_terminal_stop(later[0]))
        self.assertEqual(stream.update(rows),[])

    def test_corrupt_later_packet_never_authorizes_recovery(self):
        _,rows=self.chain(corrupt=True,gap=30)
        events=PacketStream().update(rows)
        self.assertEqual([e['packet']['code'] for e in events],[0x714])
        self.assertFalse(any(known_terminal_stop(e) for e in events))

    def test_out_of_bounds_search_is_rejected(self):
        rows=fixtures.PowerTraceTests().trace()
        calibration=detect(rows,start=1000)
        for lower,upper in ((-1,64),(100,99),(0,1801)):
            with self.assertRaises(ValueError):decode_packet(rows,calibration,lower,upper)
