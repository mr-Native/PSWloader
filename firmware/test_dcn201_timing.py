import pathlib,subprocess,tempfile,unittest
class TimingTests(unittest.TestCase):
 def test_real_timing_fields_and_clock_waits(self):
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work);r=pathlib.Path(__file__).parent
   (p/'test.c').write_text(r'''
#include <assert.h>
#include "dcn201-timing.h"
static unsigned mode,reads,writes,delays;static uint32_t values[3];
static unsigned index_of(uint32_t a){assert(a==0x13e3c||a==0x14118||a==0x13530);return a==0x13530?2:a==0x14118;}
static int read_reg(void*c,uint32_t addr,uint32_t*out){
 (void)c;unsigned i=index_of(addr);reads++;
 if(mode==2)return -1;
 *out=values[i];
 if(!mode || (mode==4&&delays>=3))*out|=i==2?2:i==1?0x100:4;
 return 0;
}
static int write_reg(void*c,uint32_t addr,uint32_t v){
 (void)c;unsigned n=index_of(addr);assert((v&(n==2?1:3))==(n==2?1:3)&&(v&0x80000000));writes++;values[n]=v;return mode==3?-1:0;
}
static void delay(void*c,unsigned us){(void)c;assert(us==1);delays++;}
int main(void){
 struct ps5_dcn201_register_write out[10];assert(!ps5_dcn201_1080p_sync(out));
 assert(out[0].offset==0x13fa8&&out[0].value==2199);
 assert(out[2].offset==0x13fac);assert((out[2].value&0x7fff)-((out[2].value>>16)&0x7fff)==1920);
 assert(out[4].value==1124);assert((out[8].value&0x7fff)-((out[8].value>>16)&0x7fff)==1080);
 assert(!out[3].value&&!out[9].value);
 struct ps5_dcn201_mmio io={0,read_reg,write_reg,delay};
 for(mode=0;mode<5;mode++){
  reads=writes=delays=0;values[0]=values[1]=values[2]=0x80000000;
  int result=ps5_dcn201_enable_timing_clocks(&io);
  if(mode==0)assert(result==0&&writes==3&&!delays);
  if(mode==1)assert(result==-3&&writes==1&&delays==1000);
  if(mode==2)assert(result==-2&&!writes);
  if(mode==3)assert(result==-2&&writes==1);
  if(mode==4)assert(!result&&writes==3&&delays==3);
 }
 assert(ps5_dcn201_enable_timing_clocks(0)==-1);return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-I',str(r),str(r/'dcn201-timing.c'),str(r/'dcn201-test-pattern.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
