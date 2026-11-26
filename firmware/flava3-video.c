/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed-mode register facts from PS5 Linux hdmi.c. Build-only packets. */
#include "flava3-video.h"
int ps5_flava3_video_stage(const struct ps5_hdmi_config *c,unsigned stage,
                           struct ps5_hdmi_commands *out) {
    return ps5_flava3_video_stage_mode(c,stage,0,out);
}
int ps5_flava3_video_stage_mode(const struct ps5_hdmi_config *c,unsigned stage,
                                const struct ps5_flava3_mode *mode,struct ps5_hdmi_commands *out) {
    if(!mode)return ps5_flava3_video_stage_modex(c,stage,0,out);
    struct ps5_flava3_mode_x x={0};x.m=*mode;
    return ps5_flava3_video_stage_modex(c,stage,&x,out);
}
int ps5_flava3_video_stage_modex(const struct ps5_hdmi_config *c,unsigned stage,
                                 const struct ps5_flava3_mode_x *modex,struct ps5_hdmi_commands *out) {
    const struct ps5_flava3_mode *mode=modex?&modex->m:0;
    const int manual=modex && modex->manual;
    if(!out || (stage>7 && stage!=11 && stage!=12 && stage!=13 && stage!=14))return -1;
    if(stage==14 && (!mode || !mode->uw_geometry))return -1;
    const int high=mode && mode->high_tmds;
    struct ps5_hdmi_commands p={0};
    /* Reuse strict converter/wiring validation without accessing hardware. */
    if(ps5_flava3_prepare_stage(c,0,&p))return -1;
    if(ps5_hdmi_commands_init(&p,stage==14?1:(stage==12?2:(stage>=6?4:((stage==3 || stage==4)?0:1)))))return -1;
    struct ps5_hdmi_register_write writes[64];uint8_t values[64][12];unsigned count=0;
    int grouping=0;
#define BEGIN() do {grouping=1;count=0;} while(0)
#define END() do {if(count && ps5_hdmi_commands_write_block(&p,writes,count))return -1;grouping=0;count=0;} while(0)
#define W(address,...) do {const uint8_t d[]={__VA_ARGS__}; \
    if(grouping) {if(count>=64 || sizeof(d)>12)return -1; \
      for(unsigned n=0;n<sizeof(d);n++)values[count][n]=d[n]; \
      writes[count].reg=address;writes[count].data=values[count];writes[count].bytes=sizeof(d);count++; \
    } else if(ps5_hdmi_commands_write(&p,address,d,sizeof(d)))return -1; } while(0)
#define WA(address,arr,n) do {if(grouping) {if(count>=64 || (n)>12)return -1; \
      for(unsigned k=0;k<(n);k++)values[count][k]=(arr)[k]; \
      writes[count].reg=address;writes[count].data=values[count];writes[count].bytes=(n);count++; \
    } else if(ps5_hdmi_commands_write(&p,address,arr,n))return -1; } while(0)
#define M(a,v,m) do {if(ps5_hdmi_commands_mask(&p,a,v,m))return -1;} while(0)
#define D(ms) do {if(ps5_hdmi_commands_delay(&p,ms))return -1;} while(0)
#define WAIT(a,m,set) do {if(ps5_hdmi_commands_wait(&p,a,m,set))return -1;} while(0)
    if(stage==0) {
        BEGIN();
        W(0x6224, 0x00, 0x01);
        W(0x1047, 0x00);
        W(0x1050, 0x00, 0x00, 0x00, 0x00);
        W(0x7215, 0x00);
        W(0x7077, 0x00);
        W(0x7079, 0x80);
        END();
    }
    if(stage==1) {
        BEGIN();
        W(0x600c, 0x01, 0x00);
        W(0x6c00, 0x1e, 0x84, 0x00);
        END();
        M(0x6005, 0x01, 0x01);
        D(2);
        M(0x6006, 0x04, 0x04);
        D(2);
        W(0x6a03, 0x47);
        D(10);
        WAIT(0x60f8, 0xff, 1);
        WAIT(0x60f9, 0x01, 1);
        W(0x6a01, 0x4d);
        WAIT(0x60f9, 0x1a, 1);
        WAIT(0x6083, 0x02, 1);
    }
    if(stage==3) {
        BEGIN();
        W(0x7022, 0x03);
        W(0x7030, 0x40);
        W(0x7024, 0x04, 0x54);
        W(0x7030, 0x41);
        W(0x7024, 0x04, 0x54);
        W(0x7030, 0x42);
        W(0x7024, 0x04, 0x34);
        W(0x7030, 0x43);
        W(0x7024, 0x04, 0x34);
        W(0x7030, 0x04);
        W(0x7024, 0x04, 0x71);
        W(0x7030, 0x14);
        W(0x7025, 0x71);
        W(0x7030, 0x24);
        W(0x7025, 0x70);
        W(0x7030, 0x34);
        W(0x7025, 0x77);
        W(0x7030, 0x05);
        W(0x7024, 0x04, 0x71);
        W(0x7030, 0x15);
        W(0x7025, 0x71);
        W(0x7030, 0x25);
        W(0x7025, 0x70);
        W(0x7030, 0x35);
        W(0x7025, 0x77);
        W(0x7030, 0x06);
        W(0x7024, 0x04, 0x71);
        W(0x7030, 0x16);
        W(0x7025, 0x70);
        W(0x7030, 0x26);
        W(0x7025, 0x70);
        W(0x7030, 0x36);
        W(0x7025, 0x77);
        END();
    }
    if(stage==4) {
        BEGIN();
        W(0x7030, 0x47);
        W(0x7024, 0x04);
        W(0x7025, 0x37);
        W(0x7027, 0x17);
        W(0x7030, 0x37);
        W(0x7025, 0x71);
        W(0x7027, 0x14);
        W(0x7030, 0x48);
        W(0x7024, 0x04);
        W(0x7025, 0x37);
        W(0x7027, 0x17);
        W(0x7030, 0x38);
        W(0x7025, 0x77);
        W(0x7027, 0x14);
        W(0x7030, 0x49);
        W(0x7024, 0x01);
        W(0x7030, 0x19);
        W(0x7026, 0xcc);
        W(0x7030, 0x29);
        W(0x7026, 0xdd);
        W(0x7030, 0x39);
        W(0x7026, 0xef);
        W(0x7030, 0x4a);
        W(0x7024, 0x01);
        W(0x7030, 0x1a);
        W(0x7026, 0xcc);
        W(0x7030, 0x2a);
        W(0x7026, 0xdd);
        W(0x7030, 0x3a);
        W(0x7026, 0xef);
        W(0x7030, 0x4b);
        W(0x7024, 0x01);
        W(0x7030, 0x1b);
        W(0x7026, 0xcc);
        W(0x7030, 0x2b);
        W(0x7026, 0xdd);
        W(0x7030, 0x3b);
        W(0x7026, 0xef);
        END();
    }
    if(stage==2) {
        M(0x7021,0x00,0xf0);D(500);BEGIN();
        W(0x100c,0x01);W(0x68a0,0x96,0x04);W(0x7005,0x80);W(0x7019,0x00);
        W(0x100c,0x01);W(0x7005,0x80);W(0x7009,0x00);W(0x7040,0x42);W(0x7225,0x28);
        if(mode){WA(0x7227,mode->reg7227,2);WA(0x7070,mode->reg7070,6);}
        else {W(0x7227,16,0);W(0x7070,16,16,0,0,0,0);}
        W(0x70c0,0xdc);W(0x621b,0);W(0x629a,0);W(0x70c4,8,8);
        W(0x70c2,0);W(0x70fe,2);W(0x70c3,0);W(0x7018,c->tmds_polarity<<1);
        if(c->tmds_swap){END();M(0x701a,0xb1,0xff);BEGIN();}
        W(0x10e7,0xff);W(0x7202,0x20);
        if(manual){WA(0x7042,modex->m7042,8);W(0x704f,0x12);WA(0x7063,modex->m7063,11);WA(0x7078,modex->m7078,2);}   /* r5.13: setManualTimingFlava3 */
        if(high){W(0x7072,0x01);W(0x7074,0x07);W(0x7206,0x80);W(0x7203,0x60);W(0x7011,0xff);}
        else {W(0x7203,0x60);W(0x7011,0xd5);}
        END();
        WAIT(0x7011,0xff,0);
    }
    if(stage==5) {
        /* Fixed SDR game content, ALLM off; explicitly clear inherited scrambling. */
        M(0x7203,0,0x80);WAIT(0x10e7,0x80,1);
        if(mode && mode->vic97_mask)M(0x7227,97,0xff);
        M(0x7226,0x80,0x80);M(0x7228,0x30,0x30);W(0x7204,0x40);WAIT(0x7204,0x40,0);
        D(10);BEGIN();W(0x7019,high?1:0);W(0x7419,5);W(0x740a,0xff);END();WAIT(0x740a,0xff,0);
        BEGIN();W(0x7404,0);W(0x7a88,0xff);END();WAIT(0x7a84,1,0);
        BEGIN();W(0x7a8b,5);if(high){W(0x7c00,0x03);W(0x7a80,0xa8,0x20,0x00,0x80);}END();
        if(high)WAIT(0x7a84,1,0);
        BEGIN();W(0x7021,0xff);W(0x700a,2);W(0x705f,0x80);END();D(700);W(0x7a8b,0);
        if(ps5_hdmi_commands_event(&p,3,1))return -1;
    }
    if(stage==6) {
        /* Explicit HDCP off at next output start; never writes NVS. */
        if(ps5_hdmi_commands_event(&p,1,1))return -1;
        M(0x7021,0,0xf0);
    }
    if(stage==11) {
        W(0x705f, 0);
    }
    if(stage==14) {
        /* setNativeUltrawideGeometryFlava3: counters after all automatic profile updates, then
         * latch only VIF (0x7011=0x04); ALLUPD would regenerate the 2560 profile window. */
        WA(0x7044,mode->uw_vstart,2);WA(0x7056,mode->uw_vtotal,2);
        WA(0x7063,mode->uw_counters,13);WA(0x7046,mode->uw_width,2);
        W(0x7011,0x04);WAIT(0x7011,0x04,0);
    }
    if(stage==12) {
        /* Linux sceHdmiSetAudioConfigFlava3(2): setHdmiAudioConfigBasic +
         * setAudioConfigAdditional, run after video unmute in amdgpu_dm. */
        BEGIN();W(0x62a0,0x06);W(0x62a7,0x13);W(0x62ac,0x82);W(0x62cb,0x02);W(0x62cb,0x03);W(0x62cb,0x00);
        W(0x70ad,0x00);W(0x70af,0x07);W(0x70a9,0x5e);END();
        M(0x70af,0x06,0x06);M(0x70b3,0x02,0x0f);M(0x70ae,0x80,0xe0);M(0x70ae,0x01,0x07);
        M(0x70ac,0x01,0x21);M(0x70ab,0x81,0x89);M(0x70a9,0x08,0x08);
        M(0x70b0,0x00,0xff);M(0x70b1,0x79,0xff);M(0x70b2,0x00,0xff);M(0x70b3,0x02,0xff);
        M(0x70b4,0x0b,0x0f);M(0x70b5,0x00,0xff);M(0x70b6,0x00,0xff);
        BEGIN();W(0x10e7,0xff);W(0x7011,0xa2);END();WAIT(0x10e7,0xa2,1);
        M(0x7267,0x00,0xff);W(0x7204,0x10);WAIT(0x7204,0x10,0);W(0x10e7,0xff);
        M(0x7203,0x10,0x10);D(30);W(0x70a8,0xc0);
    }
    if(stage==13) {
        /* Linux sceHdmiSetAudioMute(0). */
        M(0x70a8,0x00,0x04);
    }
    if(stage==7) {
        /* Encoder quiesce packet; sink SCDC state still requires observation. */
        M(0x705f,0x80,0x80);
        if(ps5_hdmi_commands_event(&p,3,0))return -1;
        M(0x7021,0,0xf0);
        if(ps5_hdmi_commands_write_single(&p,0x7a88,0xff))return -1;
        WAIT(0x7a84,1,0);
        if(ps5_hdmi_commands_write_single(&p,0x740a,0xff))return -1;
        WAIT(0x740a,0xff,0);M(0x7419,5,7);M(0x7a8b,0,7);D(100);
    }
    *out=p;return 0;
#undef BEGIN
#undef END
#undef W
#undef WA
#undef M
#undef D
#undef WAIT
}
