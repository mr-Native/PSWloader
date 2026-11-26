import pathlib,subprocess,tempfile,unittest
class StartTests(unittest.TestCase):
 def test_activation_order_and_error_boundaries(self):
  with tempfile.TemporaryDirectory() as work:
   r=pathlib.Path(__file__).parent;p=pathlib.Path(work)
   (p/'test.c').write_text(r'''
#include <assert.h>
#include <string.h>
#include "dcn201-timing.h"
static uint32_t regs[0x15000/4],offsets[64];static unsigned writes,fail_at;
static int read_reg(void*c,uint32_t a,uint32_t*v){(void)c;assert(a<0x15000&&!(a&3));*v=regs[a/4];if(a==0x13e3c)*v|=4;if(a==0x14118)*v|=0x100;if(a==0x13530)*v|=2;return 0;}
static int write_reg(void*c,uint32_t a,uint32_t v){(void)c;offsets[writes++]=a;if(writes==fail_at)return -1;regs[a/4]=v;return 0;}
static void delay(void*c,unsigned n){(void)c;(void)n;assert(0);}
int main(void){
 struct ps5_dcn201_mmio io={0,read_reg,write_reg,delay};
 /* Synthetic global-sync parameters exercise arithmetic, not validated PS5 values. */
 struct ps5_dcn201_global_sync sync={43,2,3,4};
 assert(!ps5_dcn201_start_solid_1080p(&io,&sync,1023,0,0));
 assert(writes==35);assert(offsets[0]==0x13e3c&&offsets[1]==0x14118&&offsets[2]==0x13530);
 assert(offsets[32]==0xe7a0&&offsets[33]==0x14004&&offsets[34]==0x14004);
 assert(regs[0x14004/4]&1);assert(regs[0xe7a0/4]&0x80000000U);
 assert(regs[0x13450/4]==0x41);assert(regs[0x1345c/4]==0xffc0ffc0);
 unsigned prior=writes;assert(ps5_dcn201_start_solid_1080p(&io,&sync,0,0,0)==-4&&writes==prior);
 for(fail_at=1;fail_at<=35;fail_at++){
  memset(regs,0,sizeof(regs));writes=0;
  assert(ps5_dcn201_start_solid_1080p(&io,&sync,0,0,0)==-2);
  assert(writes==fail_at);if(fail_at<35)assert(!(regs[0x14004/4]&1));
 }
 memset(regs,0,sizeof(regs));writes=0;sync.startup=0;
 assert(ps5_dcn201_start_solid_1080p(&io,&sync,0,0,0)==-1&&!writes);
 return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-I',str(r),str(r/'dcn201-timing.c'),str(r/'dcn201-test-pattern.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
