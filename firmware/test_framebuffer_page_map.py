import ctypes as C
from pathlib import Path
import subprocess,tempfile,unittest
class PageMapTests(unittest.TestCase):
 def test_identity_rights_large_pages_and_crossing(self):
  with tempfile.TemporaryDirectory() as work:
   p=Path(work);r=Path(__file__).parent
   (p/'test.c').write_text(r'''
#include <assert.h>
#include "framebuffer-page-map.h"
static uint64_t entries[4][512];static unsigned reads;static int fail;
static int read_entry(void*c,uint64_t a,uint64_t*out){(void)c;reads++;if(fail)return -1;unsigned t=(unsigned)(a/4096);if(t<1||t>4)return -1;*out=entries[t-1][(a%4096)/8];return 0;}
int main(void){
 uint64_t addr=0x450000000ULL;
 entries[0][0]=0x2003;entries[1][17]=0x3003;
 entries[2][128]=0x450000083ULL;entries[2][129]=0x450200083ULL;
 assert(!ps5_framebuffer_identity_writable(0x1000,addr,0x400000,read_entry,0));
 entries[2][129]&=~2ULL;assert(ps5_framebuffer_identity_writable(0x1000,addr,0x400000,read_entry,0));entries[2][129]|=2;
 entries[1][17]&=~2ULL;assert(ps5_framebuffer_identity_writable(0x1000,addr,4096,read_entry,0));entries[1][17]|=2;
 entries[2][128]=0x450001083ULL;assert(!ps5_framebuffer_identity_writable(0x1000,addr,4096,read_entry,0));
 entries[2][128]=0x450002083ULL;assert(ps5_framebuffer_identity_writable(0x1000,addr,4096,read_entry,0));
 entries[2][128]=0x460000083ULL;assert(ps5_framebuffer_identity_writable(0x1000,addr,4096,read_entry,0));
 entries[1][17]=0x440000083ULL;assert(!ps5_framebuffer_identity_writable(0x1000,addr,4096,read_entry,0));
 entries[1][17]=0x3003;entries[2][128]=0x4003;entries[3][0]=addr|3;
 assert(!ps5_framebuffer_identity_writable(0x1000,addr,4096,read_entry,0));
 fail=1;assert(ps5_framebuffer_identity_writable(0x1000,addr,4096,read_entry,0));
 fail=0;reads=0;assert(ps5_framebuffer_identity_writable(0x1000,addr,0,read_entry,0));assert(!reads);
 assert(ps5_framebuffer_identity_writable(0x1000,addr+1,4096,read_entry,0));
 return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-I',str(r),str(r/'framebuffer-page-map.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
