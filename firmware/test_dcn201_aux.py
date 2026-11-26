import pathlib,subprocess,tempfile,unittest
SIM=r'''
#include <assert.h>
#include <string.h>
#include "dcn201-aux.h"
#define B 0x14240
static uint32_t ctrl=1,arb=0,intc=0,swctl=0,status=0,dmcu=0,hpd=0;
static uint8_t req[32],rep[32],dpcd[0x1000];static unsigned ri,pi,rn,rw,writes,acks;
static int rd(void*c,uint32_t a,uint32_t*v){(void)c;
 if(a==B)*v=ctrl;else if(a==B+8)*v=(arb&~0xcU)|((dmcu?2:((arb&0x10000)?1:0))<<2);
 else if(a==B+0xc)*v=intc;else if(a==B+4)*v=swctl;else if(a==B+0x10)*v=status;
 else if(a==B+0x18){*v=rw?((uint32_t)rep[pi<rn?pi:0]<<8)|1:0;if(rw)pi++;}
 else return -1;
 return 0;}
static int wr(void*c,uint32_t a,uint32_t v){(void)c;writes++;
 if(a==B)ctrl=v|((v&0x10)?0x20:0);
 else if(a==B+8)arb=v;
 else if(a==B+0xc){intc=v&~2U;if(v&2){status&=~1U;acks++;}}
 else if(a==B+4){swctl=v&~1U;if(v&1){
   unsigned act=req[0]&0xf0,addr=((req[0]&0xf)<<16)|(req[1]<<8)|req[2],len=req[3]+1;
   assert(ri==((swctl>>16)&0x1f));
   if(hpd){status=1|0x200;return 0;}
   rep[0]=0;rn=1;
   if(act==0x90){for(unsigned i=0;i<len;i++)rep[rn++]=dpcd[addr+i];}
   else{assert(act==0x80);for(unsigned i=0;i<len;i++)dpcd[addr+i]=req[4+i];}
   status=1|((uint32_t)rn<<24);}}
 else if(a==B+0x18){if(v&1){rw=1;pi=0;}else{rw=0;if(v&0x80000000U)ri=0;req[ri++]=(v>>8)&0xff;}}
 else return -1;
 return 0;}
static void dl(void*c,unsigned us){(void)c;(void)us;}
int main(void){
 struct ps5_dcn201_mmio io={0,rd,wr,dl};uint8_t buf[16];uint32_t st;uint8_t r;
 for(int i=0;i<16;i++)dpcd[0x200+i]=0x10+i;
 assert(ps5_dcn201_aux_dpcd(&io,0,0,0x200,buf,16,&st,&r)==16 && r==0);
 for(int i=0;i<16;i++)assert(buf[i]==0x10+i);
 assert(!(arb&0x10000) && (arb&0x20000));      /* released */
 uint8_t one=1;assert(ps5_dcn201_aux_dpcd(&io,0,1,0x270,&one,1,&st,&r)==1 && dpcd[0x270]==1);
 assert(ps5_dcn201_aux_dpcd(&io,0,0,0x246,buf,1,&st,&r)==1 && buf[0]==0);
 dmcu=1;writes=0;assert(ps5_dcn201_aux_dpcd(&io,0,0,0,buf,1,&st,&r)==-5 && writes==0);dmcu=0;
 hpd=1;assert(ps5_dcn201_aux_dpcd(&io,0,0,0,buf,1,&st,&r)==-6 && (st&0x200));hpd=0;
 ctrl=0;assert(ps5_dcn201_aux_dpcd(&io,0,0,0x200,buf,2,&st,&r)==2 && (ctrl&1) && !(ctrl&0x10));
 assert(ps5_dcn201_aux_dpcd(&io,0,0,0,buf,17,&st,&r)==-1);
 assert(ps5_dcn201_aux_dpcd(&io,6,0,0,buf,1,&st,&r)==-1);
 return 0;}
'''
class AuxTests(unittest.TestCase):
    def test_linux_dce_aux_sequence_in_simulator(self):
        r=pathlib.Path(__file__).parent
        with tempfile.TemporaryDirectory() as w:
            p=pathlib.Path(w);(p/'t.c').write_text(SIM)
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-I',str(r),str(p/'t.c'),str(r/'dcn201-aux.c'),'-o',str(p/'t')],check=True)
            subprocess.run([str(p/'t')],check=True)
if __name__=='__main__':unittest.main()
