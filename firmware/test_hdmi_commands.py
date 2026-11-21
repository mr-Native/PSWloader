import pathlib,subprocess,tempfile,unittest

class HdmiCommandsTests(unittest.TestCase):
    def test_wire_bounds_and_service_errors(self):
        here=pathlib.Path(__file__).parent
        source=r'''
#include <assert.h>
#include <string.h>
#include "hdmi-commands.h"
static int result=0,calls=0;static unsigned length=2;static unsigned char status=0;
int ps5_icc_query_events(const struct ps5_icc_io *io,uint8_t s,uint16_t t,const uint8_t *p,unsigned n,uint8_t *o,unsigned cap,unsigned *out,ps5_icc_notification cb,void *ctx) {
 (void)io;(void)cb;(void)ctx;assert(s==0x10&&t==0&&n==20&&cap>=2);assert(p[3]==2);calls++;if(result)return result;*out=length;o[0]=status;o[1]=0;return 0;
}
int main(void) {
 struct ps5_hdmi_commands p,old;unsigned char v=0x12;
 assert(ps5_hdmi_commands_init(&p,0)==0);
 assert(ps5_hdmi_commands_submit(0,&p,0,0)==-1&&calls==0);
 assert(ps5_hdmi_commands_write(&p,0x705f,&v,1)==0);
 assert(ps5_hdmi_commands_delay(&p,0x1234)==0);
 const unsigned char expected[]={0,20,0,2,2,8,2,1,1,0x70,0x5f,0x12,3,8,1,1,0,0x34,0x12,0};
 assert(p.used==sizeof(expected)&&!memcmp(p.data,expected,sizeof(expected)));
 assert(ps5_hdmi_commands_submit(0,&p,0,0)==0);
 length=1;assert(ps5_hdmi_commands_submit(0,&p,0,0)==-6);
 length=2;status=1;assert(ps5_hdmi_commands_submit(0,&p,0,0)==-6);
 result=-3;assert(ps5_hdmi_commands_submit(0,&p,0,0)==-3);assert(calls==4);
 old=p;p.data[5]=255;assert(ps5_hdmi_commands_submit(0,&p,0,0)==-1&&calls==4);p=old;
 old=p;assert(ps5_hdmi_commands_write(&p,0,&v,249)==-1);assert(!memcmp(&p,&old,sizeof(p)));
 assert(ps5_hdmi_commands_delay(&p,65536)==-1);assert(!memcmp(&p,&old,sizeof(p)));
 assert(ps5_hdmi_commands_init(&p,3)==-1);assert(!memcmp(&p,&old,sizeof(p)));
 assert(ps5_hdmi_commands_init(&p,1)==0);
 while(ps5_hdmi_commands_write(&p,0,&v,1)==0){}old=p;
 assert(ps5_hdmi_commands_write(&p,0,&v,1)==-1);assert(!memcmp(&p,&old,sizeof(p)));
 p.used=sizeof(p.data)+1;assert(ps5_hdmi_commands_delay(&p,1)==-1);
 return 0;
}'''
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'test.c').write_text(source)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(here),str(here/'hdmi-commands.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
