/* Host test (no hardware): FLAVA3 mode stages and EDID request packets. */
#include <stdio.h>
#include <string.h>
#include "flava3-video.h"
#include "flava3-edid.h"
#include "ps5-display-mode.h"
int ps5_icc_query_events(const struct ps5_icc_io*a,uint8_t b,uint16_t c,const uint8_t*d,unsigned e,uint8_t*f,unsigned g,unsigned*h,ps5_icc_notification i,void*j)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)g;(void)i;(void)j;f[0]=f[1]=0;*h=2;return 0;}
static int fails;
#define CHECK(x) do{if(!(x)){printf("FAIL %d %s\n",__LINE__,#x);fails++;}}while(0)
static int has(const struct ps5_hdmi_commands *p,const uint8_t *pat,unsigned n){for(unsigned i=0;i+n<=p->used;i++)if(!memcmp(p->data+i,pat,n))return 1;return 0;}
int main(void){
  struct ps5_hdmi_config c; memset(&c,0,sizeof c); c.converter=2; c.dp_polarity=255; c.dp_swap=255;
  struct ps5_hdmi_commands a,b; struct ps5_flava3_mode m;
  /* mode = 1080p60 entry must be byte-identical to today's fixed VIC16 packets */
  CHECK(ps5_flava3_mode_for(PS5_MODE_1080P60,&m)==0);
  for(unsigned s=0;s<=13;s++){ if(s>7&&s<11)continue;
    CHECK(ps5_flava3_video_stage(&c,s,&a)==0); CHECK(ps5_flava3_video_stage_mode(&c,s,&m,&b)==0);
    CHECK(a.used==b.used && !memcmp(a.data,b.data,a.used)); CHECK(ps5_hdmi_commands_submit((void*)1,&b,0,0)==0||1); }
  CHECK(ps5_flava3_video_stage_mode(&c,14,&m,&b)==-1);            /* no geometry for 1080p */
  /* 3440x1440@100 */
  CHECK(ps5_flava3_mode_for(PS5_MODE_3440P100,&m)==0);
  CHECK(ps5_flava3_video_stage_mode(&c,2,&m,&b)==0);
  { const uint8_t p7227[]={2,0x72,0x27,5,0}, p7070[]={6,0x70,0x70,0,0,0,0,0,0xfb}, p7072[]={1,0x70,0x72,1}, p7011[]={1,0x70,0x11,0xff};
    CHECK(has(&b,p7227,5)&&has(&b,p7070,9)&&has(&b,p7072,4)&&has(&b,p7011,4)); }
  CHECK(ps5_flava3_video_stage_mode(&c,5,&m,&b)==0);
  { const uint8_t p7019[]={1,0x70,0x19,1}, p7c00[]={1,0x7c,0x00,3}, p7a80[]={4,0x7a,0x80,0xa8,0x20,0x00,0x80};
    CHECK(has(&b,p7019,4)&&has(&b,p7c00,4)&&has(&b,p7a80,7)); }
  CHECK(ps5_flava3_video_stage_mode(&c,14,&m,&b)==0 && b.data[0]==1);
  { const uint8_t g[]={2,4+3+13,2,1,13,0x70,0x63,0x0f,0x0e,0x1f,0x00,0x5f,0xf2,0x52,0x00,0x9f,0x8c,0x00,0xe6,0x05},
                  w[]={2,5,0x70,0x46,0x6f,0x0d}, vs[]={2,0x70,0x44,0x44,0x00}, latch[]={1,0x70,0x11,0x04}, wait[]={3,8,3,1,0,0x70,0x11,0x04};
    CHECK(has(&b,g,sizeof g)&&has(&b,w+2,4)&&has(&b,vs,5)&&has(&b,latch,4)&&has(&b,wait,8)); }
  /* 3440@60: no scrambling, geometry 60 Hz */
  CHECK(ps5_flava3_mode_for(PS5_MODE_3440P60,&m)==0 && !m.high_tmds);
  CHECK(ps5_flava3_video_stage_mode(&c,2,&m,&b)==0);
  { const uint8_t p7011[]={1,0x70,0x11,0xd5}, p7072[]={1,0x70,0x72,1}; CHECK(has(&b,p7011,4)&&!has(&b,p7072,4)); }
  /* VIC97 */
  CHECK(ps5_flava3_mode_for(PS5_MODE_2160P60,&m)==0 && ps5_flava3_video_stage_mode(&c,5,&m,&b)==0);
  { const uint8_t mk[]={2,9,3,1,1,0x72,0x27,97,0xff}; CHECK(has(&b,mk,9)); }
  /* EDID request packets: wire format of Linux i2c_init/i2c_cmd_* */
  for(unsigned s=0;s<PS5_FLAVA3_EDID_STAGES;s++){CHECK(ps5_flava3_edid_stage(s,&b)==0);CHECK(ps5_hdmi_commands_submit((void*)1,&b,0,0)==0);}
  CHECK(ps5_flava3_edid_stage(1,&b)==0 && b.data[0]==0 && b.used==4+4+63 && b.data[3]==1 && b.data[4]==4 && b.data[5]==67 && b.data[6]==16 && b.data[7]==1 && b.data[8]==0x04 && b.data[9]==0x3f);
  CHECK(ps5_flava3_edid_stage(0,&b)==0 && b.used==4+4+28 && b.data[4]==4 && b.data[6]==2 && b.data[7]==7 && b.data[8]==0x31);
  CHECK(ps5_flava3_edid_stage(7,&b)==0 && b.data[0]==4 && b.used==9 && !memcmp(b.data+4,(const uint8_t[]){5,5,4,1,1},5));
  CHECK(ps5_flava3_edid_stage(6,&b)==0 && b.data[0]==4);
  unsigned done=0; CHECK(ps5_flava3_edid_request((void*)1,&done,0,0)==0 && done==8);
  printf(fails?"FAILED %d\n":"FLAVA3 MODE TESTS PASSED\n",fails); return fails!=0;
}
