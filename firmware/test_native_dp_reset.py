import pathlib,subprocess,tempfile,unittest
class ResetTests(unittest.TestCase):
 def test_actual_reset_sequence_and_failure_guards(self):
  r=pathlib.Path(__file__).parent
  source=(r/'ps5-native-dp-video.h').read_text()
  start=source.index('STATIC EFI_STATUS Ps5NativeDpReset(')
  function=source[start:source.index('\n}\n',start)+3]   # just this function (more helpers follow it since r5.14)
  self.assertNotIn('Ps5AtomExecute',function)
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work)
   (p/'test.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include "dcn201-link.h"
#define STATIC static
#define CONST const
#define VOID void
#define EFI_ERROR(x) ((x)!=0)
#define DEBUG(x) ((void)0)
#define NULL ((void*)0)
typedef unsigned EFI_STATUS;typedef unsigned UINTN;typedef uint8_t UINT8;typedef uint32_t UINT32;typedef void EFI_PCI_IO_PROTOCOL;
enum{EFI_SUCCESS,EFI_UNSUPPORTED,EFI_NOT_READY,EFI_INVALID_PARAMETER,EFI_DEVICE_ERROR,EFI_TIMEOUT};
typedef struct{EFI_STATUS(*Stage)(UINTN);}PS5_HDMI_VIDEO_PROTOCOL;
static unsigned stagecalls,reads,mode,mPs5HdmiVideoGuid;
static EFI_STATUS Stage(UINTN n){const unsigned expected[]={6,7,8};assert(n==expected[stagecalls++]);return mode==1?EFI_DEVICE_ERROR:0;}
static PS5_HDMI_VIDEO_PROTOCOL Video={Stage};
static EFI_STATUS Locate(void*g,void*k,void**out){(void)g;(void)k;*out=&Video;return 0;}
static struct{EFI_STATUS(*LocateProtocol)(void*,void*,void**);}BS={Locate},*gBS=&BS;
static void Ps5GpuSnapshot(void){}
static EFI_STATUS NativeVideoStage(PS5_HDMI_VIDEO_PROTOCOL*v,UINTN n){return v->Stage(n);}
static int VideoRead(void*p,uint32_t o,uint32_t*v){(void)p;assert(o==0x155c0);reads++;if(mode==2)return -1;*v=mode==3?0:1;return 0;}
'''+function+r'''
int main(void){
 struct ps5_dcn201_link l={0,1};const unsigned expected[]={0,EFI_DEVICE_ERROR,EFI_DEVICE_ERROR,EFI_NOT_READY};
 for(mode=0;mode<4;mode++){
  stagecalls=reads=0;
  assert(Ps5NativeDpReset(0,0,&l)==expected[mode]);
  assert(stagecalls==(mode==1?1:3));
  assert(reads==(mode==1?0:1));
 }
 l.phy=1;assert(Ps5NativeDpReset(0,0,&l)==EFI_UNSUPPORTED);return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-I',str(r),str(p/'test.c'),str(r/'dcn201-link.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
