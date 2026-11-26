import os
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from ps5_exception_power import SOURCE, INF, HOOK, prepare_exception_power

EDK = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2')


class ExceptionPowerTests(unittest.TestCase):
    def test_registered_handlers_continue_unhandled_faults_report_once_and_halt_interrupts_resume(self):
        # Execute the exact inserted hook with host stubs; no privileged IRQs.
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <assert.h>
typedef uintptr_t UINTN;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint8_t UINT8;
#define BIT10 0x400ULL
#define BIT0 0x1U
#define X86_CPU_INTERRUPT_NUM 256
static unsigned eoi;
#define BIT11 0x800ULL
static UINT64 AsmReadMsr64(unsigned m) { assert(m==0x1B); return 0xFEE00000ULL|0x900ULL|BIT10; }   /* x2APIC: no MMIO on the host */
static void AsmWriteMsr64(unsigned m, UINT64 v) { assert(m==0x80B && v==0); eoi++; }
static void *mExternalInterruptHandlerTable[256];
static uint32_t mPs5FaultReporting;
static uint32_t mPs5StrayCount[256];
static unsigned reported, calls, disabled, continued, calibrated;
static jmp_buf stopped;
static void DisableInterrupts(void) { disabled++; }
static unsigned InterlockedCompareExchange32(uint32_t *p, unsigned c, unsigned x) {
 unsigned old=*p; if(old==c)*p=x; return old;
}
static void ps5_fault_power_calibrate(void) { assert(disabled);calibrated++; }
static void ps5_fault_power_report(unsigned code) { assert(disabled); reported=code;calls++; }
static void CpuDeadLoop(void) { longjmp(stopped,1); }
static void run(UINTN ExceptionType) {
HOOK_TEXT
 continued++;
}
int main(void) {
 mExternalInterruptHandlerTable[32]=(void *)1;
 run(32); assert(continued==1 && calls==0 && disabled==0);
 run(256); assert(continued==2 && calls==0);
 /* r5.25.5: device interrupts without a handler are acknowledged and resumed, never reported */
 mExternalInterruptHandlerTable[32]=NULL;
 run(32); run(0xC0); run(255); assert(eoi==3 && calls==0 && disabled==0 && continued==2);
 unsigned vectors[]={0,14,31};
 for(unsigned i=0;i<3;i++) {
  unsigned v=vectors[i];mExternalInterruptHandlerTable[v]=NULL;
  mPs5FaultReporting=0;calls=0;calibrated=0;
  if(setjmp(stopped)==0)run(v);
  assert(calls==1 && calibrated==1 && reported==0x800+v);
  if(setjmp(stopped)==0)run(v);
  assert(calls==1 && calibrated==1 && continued==2);
 }
}
'''.replace('HOOK_TEXT', HOOK)
        with tempfile.TemporaryDirectory() as work:
            src=Path(work)/'hook.c';src.write_text(harness);exe=Path(work)/'hook'
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    @unittest.skipUnless((EDK/SOURCE).exists(),'Pinned EDK required')
    def test_pristine_regeneration_and_disabled_build_remove_fallback(self):
        originals={p:subprocess.check_output(['git','show','HEAD:'+p],cwd=EDK,text=True) for p in (SOURCE,INF)}
        with tempfile.TemporaryDirectory() as work:
            root=Path(work);(root/SOURCE).parent.mkdir(parents=True)
            with patch('ps5_exception_power.subprocess.check_output',side_effect=lambda argv,**kw:originals[argv[-1][5:]]):
                prepare_exception_power(root,True,'ENCODER')
                enabled=(root/SOURCE).read_text()
                self.assertIn(HOOK,enabled)
                self.assertEqual(enabled.count('CommonExceptionHandlerWorker (ExceptionType, SystemContext, &mExceptionHandlerData);'),1)
                prepare_exception_power(root,True,'ENCODER')
                self.assertEqual((root/SOURCE).read_text(),enabled)
                prepare_exception_power(root,False,'ENCODER')
                self.assertEqual((root/SOURCE).read_text(),originals[SOURCE])
                self.assertEqual((root/INF).read_text(),originals[INF])
                self.assertFalse((root/SOURCE).with_name('Ps5FaultPower.c').exists())
