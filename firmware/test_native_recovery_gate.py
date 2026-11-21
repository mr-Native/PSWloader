import unittest
from native_recovery_gate import known_terminal_stop


class RecoveryGateTests(unittest.TestCase):
    def test_progress_unknown_and_malformed_results_do_not_authorize_power_cut(self):
        for code in (True,None,-1,3,0x701,0x708,0x711,0x717,0x420,0x900,
                     0x100,0x101,0x201,0x208,0x501,0x505,0x601,0x604):
            self.assertFalse(known_terminal_stop({'packet':{'decoded':True,'code':code}}))
        for result in (None,{}, {'packet':None}, {'packet':{'decoded':False,'code':0x8ef}}):
            self.assertFalse(known_terminal_stop(result))

    def test_valid_terminal_classes_allow_existing_remote_checked_finisher(self):
        for code in (0x300,0x400,0x41f,0x4ff,*range(0x800,0x900)):
            self.assertTrue(known_terminal_stop({'packet':{'decoded':True,'code':code}}))
