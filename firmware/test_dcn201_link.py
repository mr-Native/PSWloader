import pathlib,subprocess,tempfile,unittest

class LinkTests(unittest.TestCase):
 def test_native_link_routing_and_atom_wire_parameters(self):
  r=pathlib.Path(__file__).parent
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work)
   (p/'test.c').write_text(r'''
#include <assert.h>
#include "dcn201-link.h"
int main(void) {
 struct ps5_dcn201_link l;uint32_t p[4];struct ps5_dcn201_register_write w[3];
 assert(!ps5_dcn201_encoder_setup_params(p));
 assert(p[0]==0x04000f00 && p[1]==14850 && p[2]==2);
 assert(ps5_dcn201_encoder_setup_params(0)==-1);
 for(unsigned hpd=1;hpd<=2;hpd++) {
  assert(!ps5_dcn201_link_decode(0x3013+(hpd<<8),0x201e + (hpd<<8),hpd,&l));
  assert(l.phy==hpd-1 && l.hpd==hpd);
  assert(!ps5_dcn201_transmitter_params(&l,0,0,0,p));
  assert(p[0]==l.phy && !p[1] && p[2]==(0x130000|hpd) && !p[3]);
  assert(ps5_dcn201_transmitter_params(&l,0,4,0,p)==-1);
  assert(ps5_dcn201_transmitter_params(&l,0,0,54000,p)==-1);
  assert(!ps5_dcn201_transmitter_params(&l,7,4,0,p));
  assert(p[0]==(0x04000700U|l.phy) && !p[1] && p[2]==(0x130000|hpd) && !p[3]);
  assert(!ps5_dcn201_transmitter_params(&l,1,4,54000,p));
  assert(p[0]==(0x04000100U|l.phy) && p[1]==54000 && p[2]==hpd && !p[3]);
  assert(!ps5_dcn201_link_plan(&l,4,w));
  assert(w[0].offset==(0x34c0+0x20af+0x100*l.phy)*4);
  assert(w[0].mask==0x70077f00 && w[0].value==((hpd-1)<<28|0x100));
  assert(w[1].offset==(0x34c0+0x210b+0x100*l.phy)*4 && w[1].value==3 && w[1].mask==0xffffffff);
  assert(w[2].offset==(0x34c0+0x211e + 0x100*l.phy)*4 && w[2].mask==16 && w[2].value==16);
  for(unsigned n=0;n<8;n++) {
   int ok=n==1||n==2||n==4;
   assert((ps5_dcn201_link_plan(&l,n,w)==0)==ok);
   assert((ps5_dcn201_transmitter_params(&l,1,n,27000,p)==0)==ok);
  }
  assert(!ps5_dcn201_transmitter_params(&l,1,4,81000,p) && p[1]==81000);
  assert(ps5_dcn201_transmitter_params(&l,7,4,54000,p)==-1);
  assert(ps5_dcn201_transmitter_params(&l,1,4,14850,p)==-1);
  assert(ps5_dcn201_transmitter_params(&l,255,4,54000,p)==-1);
 }
 assert(ps5_dcn201_link_decode(0x3113,0x221e,1,&l)==-1);
 assert(ps5_dcn201_link_decode(0x3113,0x211e,2,&l)==-1);
 assert(ps5_dcn201_link_decode(0x3113,0x211e,1,0)==-1);
 l.phy=99;l.hpd=1;assert(ps5_dcn201_link_plan(&l,4,w)==-1);
 return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(r),str(r/'dcn201-link.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
