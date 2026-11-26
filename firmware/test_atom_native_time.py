import os,pathlib,subprocess,tempfile,unittest
R=pathlib.Path(__file__).parent
EDK=pathlib.Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))
@unittest.skipUnless((EDK/'MdePkg/Include/Uefi.h').exists(),'EDK headers required')
class NativeTimeTests(unittest.TestCase):
 def test_native_jiffies_tracks_tsc_without_instruction_budget_side_effect(self):
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work);(p/'test.c').write_text(r'''
#include <assert.h>
#undef NULL
#include "uefi-compat.h"
static UINT64 Ticks;
UINT64 EFIAPI AsmReadTsc(VOID){return Ticks;}
VOID EFIAPI LongJump(BASE_LIBRARY_JUMP_BUFFER *B,UINTN V){(void)B;(void)V;assert(0);for(;;){}}
int main(void){
 static UINT8 Arena[ATOM_ARENA_BYTES];mAtomArena=Arena;mAtomArenaUsed=0;
 VOID *A=AtomAllocate(1,17),*B=AtomAllocate(2,16);
 assert(A==Arena && B==Arena+32 && mAtomArenaUsed==64);
 AtomFree(A);AtomFree(B);
 for(unsigned I=0;I<32;I++)assert(!mAtomAllocations[I]);
 assert(mAtomArenaUsed==64);
 host_steps=123;Ticks=1596300ULL*1000;
 assert(jiffies==1000 && jiffies_to_msecs(jiffies)==1000);
 for(unsigned I=0;I<100000;I++)assert(jiffies==1000);
 assert(host_steps==123);
 Ticks+=1596300ULL*5001;assert(jiffies==6001);
 assert(time_after(jiffies,1000) && jiffies_to_msecs(jiffies-1000)==5001);
 return 0;
}''')
   subprocess.run(['gcc','-O2','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(R/'atom-host'),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
