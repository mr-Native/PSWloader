/* r5.13 host test: 1280x720@144 (UGREEN DTD) - offered/selection on the real UGREEN EDID, FLAVA3 manual bytes
 * (computation display-modes/ugreen-720p144-proposal.md), stage-2 packet, OTG/MSA.
 * gcc -std=c11 -Wall -Wextra -Werror -I. test_720p144.c ps5-display-mode.c flava3-video.c flava3-edid.c hdmi-commands.c flava3-prepare.c dcn201-timing.c dcn201-test-pattern.c */
#include <stdio.h>
#include <string.h>
#include "flava3-video.h"
#include "ps5-display-mode.h"
int ps5_icc_query_events(const struct ps5_icc_io*a,uint8_t b,uint16_t c,const uint8_t*d,unsigned e,uint8_t*f,unsigned g,unsigned*h,ps5_icc_notification i,void*j)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)g;(void)i;(void)j;f[0]=f[1]=0;*h=2;return 0;}
static int fails;
#define CHECK(x) do{if(!(x)){printf("FAIL %d %s\n",__LINE__,#x);fails++;}}while(0)
static int has(const struct ps5_hdmi_commands *p,const uint8_t *pat,unsigned n){for(unsigned i=0;i+n<=p->used;i++)if(!memcmp(p->data+i,pat,n))return 1;return 0;}
int main(int argc,char**argv){
    static uint8_t e[2048]; unsigned n=0;
    if(argc>1){FILE*f=fopen(argv[1],"rb"); if(f){n=(unsigned)fread(e,1,sizeof e,f);fclose(f);}}
    struct ps5_edid_info info; CHECK(n>=256 && ps5_edid_parse(e,n,&info)==0);
    CHECK(ps5_display_offered(&info,PS5_MODE_720P144));
    uint32_t mask=(1u<<PS5_MODE_720P144)|(1u<<PS5_MODE_UGREEN1440P60);
    printf("policy2 -> %s, policy3 -> %s\n",ps5_display_modes[ps5_display_select(&info,0,mask)].name,ps5_display_modes[ps5_display_select_largest(&info,0,mask)].name);
    CHECK(ps5_display_select(&info,0,mask)==PS5_MODE_1080P60);          /* without force, policy 2 keeps 1080p60 */
    CHECK(ps5_display_select_fastest(&info,0,mask)==PS5_MODE_720P144);  /* r5.16 refresh=max: 144 Hz wins on the UGREEN */
    CHECK(ps5_display_select_fastest(&info,60,mask)==PS5_MODE_1080P60); /* 60-Hz cap: equal refresh -> native 1080 first */
    CHECK(ps5_display_select_fastest(&info,0,1u<<PS5_MODE_UGREEN1440P60)==PS5_MODE_1080P60);   /* no 720p144 replay */
    CHECK(ps5_display_select_fastest(NULL,0,mask)==PS5_MODE_1080P60);
    struct ps5_flava3_mode_x x; CHECK(ps5_flava3_modex_for(PS5_MODE_720P144,&x)==0 && x.manual==1);
    static const uint8_t c7063[11]={0x71,0x06,0x27,0x00,0xfb,0xe1,0x12,0x00,0x39,0x20,0x00};   /* */
    static const uint8_t c7042[8]={0x02,0x01,0x1a,0x00,0xff,0x04,0xcf,0x02};  /* hstart 1650-1390-2=258, vstart 26, w 1279, h 719 */
    CHECK(!memcmp(x.m7063,c7063,11)); CHECK(!memcmp(x.m7042,c7042,8));
    CHECK(x.m7078[0]==178 && x.m7078[1]==0x80);
    CHECK(x.m.reg7227[0]==5 && x.m.reg7070[5]==0 && !x.m.uw_geometry && !x.m.high_tmds);
    struct ps5_flava3_mode_x y; CHECK(ps5_flava3_modex_for(PS5_MODE_3440P60,&y)==0 && y.manual==0 && y.m.uw_geometry==1);
    struct ps5_hdmi_config c; memset(&c,0,sizeof c); c.converter=2; c.dp_polarity=255; c.dp_swap=255;
    struct ps5_hdmi_commands p;
    CHECK(ps5_flava3_video_stage_modex(&c,2,&x,&p)==0);
    static const uint8_t w7042[]={0x70,0x42,0x02,0x01,0x1a,0x00,0xff,0x04,0xcf,0x02};
    printf("stage2 packet %u bytes, 7042 %s\n",p.used,has(&p,w7042+2,8)?"present":"MISSING");
    CHECK(has(&p,x.m7042,8) && has(&p,x.m7063,11));
    struct ps5_hdmi_commands q; CHECK(ps5_flava3_video_stage_mode(&c,2,&x.m,&q)==0); CHECK(!has(&q,x.m7063,11));
    CHECK(ps5_flava3_video_stage_modex(&c,14,&x,&p)==-1);              /* no ultrawide geometry stage */
    struct ps5_dcn201_register_write w[10]; CHECK(ps5_dcn201_otg_sync(&ps5_display_modes[PS5_MODE_720P144].t,w)==0);
    CHECK(w[0].value==1649 && w[4].value==749 && w[3].value==0 && w[9].value==0);
    uint32_t msa[4]; ps5_dp_msa(&ps5_display_modes[PS5_MODE_720P144].t,msa);
    printf("MSA %08x %08x %08x %08x, VID_M %u\n",msa[0],msa[1],msa[2],msa[3],ps5_dp_vid_m(178200));
    puts(fails?"FAILED":"720P144 TESTS PASSED"); return fails!=0;
}
