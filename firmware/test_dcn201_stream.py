import pathlib,subprocess,tempfile,unittest

class StreamTests(unittest.TestCase):
 def test_stream_timing_ratio_state_guards_and_failed_writes(self):
  r=pathlib.Path(__file__).parent
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work)
   (p/'test.c').write_text(r'''
#include <assert.h>
#include "dcn201-stream.h"
static uint32_t reference_count=4000,clock_reg=0x80000710,backend=1,clock_ready=1;
static unsigned writes,fail_at,delays;static uint32_t stream,otg=1,addr[32],val[32];
static int rd(void*c,uint32_t a,uint32_t*v){(void)c;*v=a==0x154a0?(clock_ready?0x01000000:0):a==0x580?clock_reg:a==0x155c0?backend:a==0x6c9fc?reference_count:a==0x15730?stream:a==0x14004?otg:0x80000000;return 0;}
static int wr(void*c,uint32_t a,uint32_t v){(void)c;addr[writes]=a;val[writes++]=v;if(writes==fail_at)return -1;if(a==0x580)clock_reg=v;return 0;}
static void delay(void*c,unsigned us){(void)c;assert(us==1||us==10||us==100);delays++;}
int main(void){
 struct ps5_dcn201_mmio io={0,rd,wr,delay};
 stream=0x10000;assert(ps5_dcn201_stream_setup(&io)==-4 && !writes);
 stream=1;assert(ps5_dcn201_stream_setup(&io)==-4 && !writes);
 stream=0;assert(!ps5_dcn201_stream_setup(&io) && writes==13);
 assert(addr[12]==0x157ac && (val[12]&1));
 assert(addr[10]==0x15848 && !(val[10]&0x1fff0003) && addr[11]==0x15870 && val[11]==0);
 assert(addr[4]==0x15830 && val[4]==((2200U<<16)|1125));
 assert(val[5]==((192U<<16)|41) && val[6]==((44U<<16)|5) && val[7]==((1920U<<16)|1080));
 assert(addr[8]==0x13400 && !(val[8]&0x2f0000));
 assert(addr[9]==0x13414 && (val[9]&0x70001)==1);
 for(fail_at=1;fail_at<=13;fail_at++){writes=0;assert(ps5_dcn201_stream_setup(&io)==-2 && writes==fail_at);}
 fail_at=0;writes=0;assert(ps5_dcn201_stream_unblank(&io,0)==-4 && !writes);
 otg=0;assert(ps5_dcn201_stream_unblank(&io,1)==-4 && !writes);
 otg=1;assert(!ps5_dcn201_stream_unblank(&io,1) && writes==18 && delays==3);
 assert(addr[0]==0x14008 && !(val[0]&0x10100));
 assert(addr[1]==0x1574c && (val[1]&0x1103ffff)==0x10002000);
 assert(addr[3]==0x15720 && (val[3]&16));
 assert(addr[4]==0x1575c && !(val[4]&0x10000));
 assert(addr[5]==0x15774 && !(val[5]&1));
 assert(addr[7]==0x15744 && (val[7]&0xffffff)==32768);
 assert(addr[8]==0x15748 && (val[8]&0xffffff)==6007);
 assert(addr[11]==0x154a0 && (val[11]&0x400));
 assert(addr[12]==0x154a0 && !(val[12]&0x400));
 assert(addr[13]==0x15734 && (val[13]&1));
 assert(addr[14]==0x15734 && !(val[14]&1));
 assert(addr[15]==0x154b8 && (val[15]&0x100));
 assert(addr[16]==0x154b8 && !(val[16]&0x100));
 assert(addr[17]==0x15730 && (val[17]&1));
 for(fail_at=1;fail_at<=18;fail_at++){writes=delays=0;assert(ps5_dcn201_stream_unblank(&io,1)==-2 && writes==fail_at);assert(delays==(unsigned)((fail_at>12)+(fail_at>14)+(fail_at>15)));}
 fail_at=writes=delays=0;stream=0x10000;
 assert(ps5_dcn201_stream_unblank(&io,1)==-3 && writes==11 && delays==5000);
 stream=0;
 fail_at=writes=0;uint32_t hz=0;
 assert(!ps5_dcn201_pixel_dto(&io,&hz) && hz==400000000 && writes==4);
 assert(addr[0]==0x500 && !(val[0]&16));
 assert(addr[1]==0x504 && val[1]==148500000);
 assert(addr[2]==0x508 && val[2]==400000000);
 assert(addr[3]==0x500 && (val[3]&16));
 for(fail_at=1;fail_at<=4;fail_at++){writes=0;hz=7;assert(ps5_dcn201_pixel_dto(&io,&hz)==-2&&writes==fail_at&&hz==7);}
 fail_at=writes=0;reference_count=0;assert(ps5_dcn201_pixel_dto(&io,&hz)==-4&&!writes);
 reference_count=12001;assert(ps5_dcn201_pixel_dto(&io,&hz)==-4&&!writes);
 fail_at=writes=0;
 assert(!ps5_dcn201_symbol_clock(&io)&&writes==1&&clock_reg==0x80000011);
 backend=0;writes=0;assert(ps5_dcn201_symbol_clock(&io)==-4&&!writes);
 backend=1;clock_reg=0x80000710;fail_at=1;
 assert(ps5_dcn201_symbol_clock(&io)==-2&&clock_reg==0x80000710);
 fail_at=writes=delays=0;clock_ready=0;
 assert(ps5_dcn201_symbol_clock(&io)==-3&&delays==1000);
 assert(ps5_dcn201_stream_setup(0)==-1);return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(r),str(r/'dcn201-stream.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
