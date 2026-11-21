import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from ps5_inherited_timer import prepare_inherited_timer

HERE=Path(__file__).parent
EDK=Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2')


class InheritedTimerTests(unittest.TestCase):
    def test_pending_timer_is_acknowledged_new_timer_is_not_masked_and_storm_fails(self):
        header='''#include <stdint.h>
#include <stddef.h>
typedef unsigned UINT32; typedef unsigned char UINT8;
typedef unsigned EFI_EXCEPTION_TYPE; typedef void *EFI_SYSTEM_CONTEXT;
typedef unsigned EFI_STATUS;
#define STATIC static
#define VOID void
#define EFIAPI
#define IN
#define EFI_SUCCESS 0
typedef void (*HANDLER)(EFI_EXCEPTION_TYPE,EFI_SYSTEM_CONTEXT);
unsigned InterlockedIncrement(volatile UINT32 *);
void GetApicTimerState(void *,void *,UINT8 *);
void DisableApicTimerInterrupt(void); void SendApicEoi(void); void CpuDeadLoop(void);
EFI_STATUS RegisterCpuInterruptHandler(unsigned,HANDLER);
'''
        harness=r'''
#include "CpuDxe.h"
#include <assert.h>
#include <setjmp.h>
static HANDLER installed; static unsigned vector=239, masks,eoi,reg_status,fatal;
static jmp_buf stopped;
unsigned InterlockedIncrement(volatile UINT32 *p){return ++*p;}
void GetApicTimerState(void *a,void *b,UINT8 *v){(void)a;(void)b;*v=vector;}
void DisableApicTimerInterrupt(void){masks++;}
void SendApicEoi(void){eoi++;}
void CpuDeadLoop(void){assert(0);}
EFI_STATUS RegisterCpuInterruptHandler(unsigned v,HANDLER h){
 assert(v==239);if(reg_status)return reg_status;installed=h;return 0;
}
void CommonExceptionHandler(unsigned t,void *c){(void)c;assert(t==239);fatal++;longjmp(stopped,1);}
extern unsigned Ps5PrepareInheritedTimer(void);
int main(void){
 vector=32;assert(Ps5PrepareInheritedTimer()==0 && !installed && !masks);
 vector=239;reg_status=7;assert(Ps5PrepareInheritedTimer()==7 && !installed);
 reg_status=0;assert(Ps5PrepareInheritedTimer()==0 && installed);
 unsigned before=masks;installed(239,NULL);assert(masks==before+1 && eoi==1);
 vector=32;before=masks;installed(239,NULL);assert(masks==before && eoi==2);
 for(unsigned i=2;i<8;i++)installed(239,NULL);
 assert(eoi==8);
 if(setjmp(stopped)==0)installed(239,NULL);
 assert(fatal==1 && installed==NULL && masks==before && eoi==8);
}
'''
        with tempfile.TemporaryDirectory() as work:
            p=Path(work);(p/'CpuDxe.h').write_text(header)
            (p/'Library').mkdir();(p/'Library/SynchronizationLib.h').write_text('')
            (p/'timer.c').write_text((HERE/'Ps5InheritedTimer.c').read_text())
            (p/'test.c').write_text(harness)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+work,
                str(p/'timer.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    @unittest.skipUnless((EDK/'UefiCpuPkg/CpuDxe/CpuDxe.c').exists(),'EDK required')
    def test_installation_precedes_first_cpu_protocol_notification(self):
        with tempfile.TemporaryDirectory() as work:
            root=Path(work);p=root/'UefiCpuPkg/CpuDxe';p.mkdir(parents=True)
            for name in ('CpuDxe.c','CpuDxe.inf'):
                (p/name).write_text(subprocess.check_output(['git','show','HEAD:UefiCpuPkg/CpuDxe/'+name],cwd=EDK,text=True))
            with patch('ps5_inherited_timer.subprocess.check_output',return_value='#define LOCAL_APIC_TIMER_VECTOR  32'):
                prepare_inherited_timer(root,'IMPLEMENTATION')
                source=(p/'CpuDxe.c').read_text()
                self.assertLess(source.index('  InitInterruptDescriptorTable ();'),source.index('  Status = Ps5PrepareInheritedTimer ();'))
                self.assertLess(source.index('  Status = Ps5PrepareInheritedTimer ();'),source.index('  Status = gBS->InstallMultipleProtocolInterfaces ('))
                self.assertIn('if (EFI_ERROR (Status)) {\n    return Status;',source)
                with self.assertRaisesRegex(RuntimeError,'anchors'):prepare_inherited_timer(root,'IMPLEMENTATION')
            with patch('ps5_inherited_timer.subprocess.check_output',return_value='#define LOCAL_APIC_TIMER_VECTOR  239'):
                with self.assertRaisesRegex(RuntimeError,'timer vector'):prepare_inherited_timer(root,'IMPLEMENTATION')
