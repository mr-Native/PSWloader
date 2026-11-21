import pathlib,subprocess,tempfile,unittest
class ReadTests(unittest.TestCase):
 def test_wire_reply_bounds_and_unchanged_output_on_failure(self):
  r=pathlib.Path(__file__).parent
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work);(p/'test.c').write_text(r'''
#include <assert.h>
#include <string.h>
#include "hdmi-commands.h"
static int error;static unsigned length=30,status,calls;static uint8_t echo1=1,echoreg=0xf8;   /* real EMC: 28+count, echo */
int ps5_icc_query_events(const struct ps5_icc_io*i,uint8_t s,uint16_t t,const uint8_t*p,unsigned n,uint8_t*out,unsigned cap,unsigned*bytes,ps5_icc_notification cb,void*c){
 (void)i;(void)cb;(void)c;
 const uint8_t expected[]={4,12,0,1,1,8,1,1,2,0x60,0xf8,0};
 assert(s==0x10 && t==0 && n==12 && cap==128 && !memcmp(p,expected,12));
 calls++;if(error)return error;memset(out,0,128);out[0]=status;out[4]=echo1;out[5]=2;out[6]=0x60;out[7]=echoreg;out[8]=0xaa;out[9]=0xbb;
 *bytes=length;return 0;
}
int main(void){
 struct ps5_icc_io io={0};uint8_t out[2]={0x11,0x22};
 assert(!ps5_hdmi_register_read(&io,0x60f8,2,out,0,0));assert(out[0]==0xaa&&out[1]==0xbb);
 for(unsigned mode=0;mode<5;mode++){   /* short, longer than the buffer, status, wrong echo marker, wrong echo register */
  out[0]=0x11;out[1]=0x22;length=mode==0?9:mode==1?129:30;status=mode==2;echo1=mode==3?0:1;echoreg=mode==4?0xf9:0xf8;
  assert(ps5_hdmi_register_read(&io,0x60f8,2,out,0,0)==-6);assert(out[0]==0x11&&out[1]==0x22);
 }
 echo1=1;echoreg=0xf8;status=0;length=60;   /* 28+32: a full 32-byte read length fits now (was -5 with 40 bytes) */
 assert(!ps5_hdmi_register_read(&io,0x60f8,2,out,0,0)&&out[0]==0xaa);
 length=30;out[0]=0x11;out[1]=0x22;
 error=-3;assert(ps5_hdmi_register_read(&io,0x60f8,2,out,0,0)==-3&&out[0]==0x11);
 unsigned before=calls;
 assert(ps5_hdmi_register_read(&io,0xffff,2,out,0,0)==-1);
 assert(ps5_hdmi_register_read(&io,0,0,out,0,0)==-1);
 assert(ps5_hdmi_register_read(&io,0,33,out,0,0)==-1);
 assert(ps5_hdmi_register_read(0,0,2,out,0,0)==-1);
 assert(calls==before);return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-I',str(r),str(r/'hdmi-commands.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
