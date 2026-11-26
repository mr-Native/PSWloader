#include <stdio.h>
#include "rtc-emu.h"
static int f;
#define C(x) do{ if(!(x)){printf("FAIL %d %s\n",__LINE__,#x);f++;} }while(0)
int main(void){
  struct rtc_time t={2026,11,16,8,7,59,1,0};
  C(rtc_emu_reg(&t,0)==0x59); C(rtc_emu_reg(&t,2)==0x07); C(rtc_emu_reg(&t,4)==0x08); C(rtc_emu_reg(&t,6)==0x02);
  C(rtc_emu_reg(&t,7)==0x16); C(rtc_emu_reg(&t,8)==0x11); C(rtc_emu_reg(&t,9)==0x26); C(rtc_emu_reg(&t,0x32)==0x20);
  C(rtc_emu_reg(&t,0x8a)==0x26);   /* NMI-disable bit ignored, UIP clear early in the second */
  t.subsec_100ns=9997560; C(rtc_emu_reg(&t,0x0a)==0xa6);   /* exactly 244 us before rollover: UIP */
  t.subsec_100ns=9997559; C(rtc_emu_reg(&t,0x0a)==0x26);
  C(rtc_emu_reg(&t,0x0b)==0x02 && rtc_emu_reg(&t,0x0c)==0 && rtc_emu_reg(&t,0x0d)==0x80 && rtc_emu_reg(&t,0x10)==0);
  C(rtc_emu_port(0x70) && rtc_emu_port(0x71) && !rtc_emu_port(0x80) && !rtc_emu_port(0xcf8) && !rtc_emu_port(0x6f));
  printf("%s (%d)\n", f?"FAILED":"OK", f); return f;
}
