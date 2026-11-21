/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Register facts from PS5 Linux hdmi.c FLAVA3 preparation sequences.
 * No hardware access here; no invented graphics protocol. */
#include "flava3-prepare.h"
int ps5_flava3_prepare_stage(const struct ps5_hdmi_config *c,unsigned stage,
                            struct ps5_hdmi_commands *out) {
    if(!c || !out || c->converter!=2 || c->tmds_polarity>1 || c->tmds_swap>1 || stage>2)return -1;
    if(c->dp_polarity!=255 && c->dp_polarity>15)return -1;
    if(c->dp_swap!=255) {
        unsigned seen=0;
        for(unsigned i=0;i<4;i++)seen|=1U<<((c->dp_swap>>(2*i))&3);
        if(seen!=15)return -1;
    }
    struct ps5_hdmi_commands p={0};
    if(ps5_hdmi_commands_init(&p,stage==2?1:0))return -1;
    struct ps5_hdmi_register_write writes[32];uint8_t values[32][10];unsigned count=0;
    int grouping=1;
#define FLUSH() do {if(ps5_hdmi_commands_write_block(&p,writes,count))return -1;count=0;} while(0)
#define W(address,...) do {const uint8_t d[]={__VA_ARGS__}; \
    if(grouping) {if(count>=32 || sizeof(d)>10)return -1; \
      for(unsigned n=0;n<sizeof(d);n++)values[count][n]=d[n]; \
      writes[count].reg=address;writes[count].data=values[count];writes[count].bytes=sizeof(d);count++; \
    } else if(ps5_hdmi_commands_write(&p,address,d,sizeof(d)))return -1; } while(0)
    if(stage==0) {
        W(0x705f, 0x12);
        W(0x6004, 0x80);
        W(0x6020, 0x00);
        W(0x7007, 0xff);
        W(0x100c, 0x01);
        W(0x6008, 0xc0);
        W(0x6207, 0x00);
        W(0x621b, 0x00);
        W(0x6080, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
        W(0x6090, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
        W(0x10e7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
        W(0x10e9, 0xff, 0xff);
        W(0x10f0, 0xff, 0x07, 0x00, 0x0f, 0x00, 0x56, 0x00, 0x00, 0xd0, 0x00);
        W(0x7018, c->tmds_polarity<<1);
        FLUSH();
        if(c->tmds_swap && ps5_hdmi_commands_mask(&p,0x701a,0xb1,0xff))return -1;
    }
    if(stage==1) {
        W(0x6a03, 0x04);
        W(0x60a2, 0xf1);
        W(0x60bf, 0x03);
        W(0x60c0, 0xef);
        W(0x60c3, 0x4d);
        W(0x60c7, 0x85);
        W(0x60bf, 0x04);
        W(0x60c7, 0x84);
        if(c->dp_polarity!=255) W(0x600e, c->dp_polarity<<4);
        if(c->dp_swap!=255) W(0x600b, c->dp_swap);
        W(0x6220, 0x40, 0x00, 0x80, 0x00, 0x00, 0x01, 0x02);
        W(0x6028, 0x01, 0x02);
        W(0x6058, 0x01, 0x02, 0x03, 0x04);
        W(0x62af, 0x01);
        W(0x6207, 0x00);
        W(0x621b, 0x00);
        W(0x60e0, 0x1e);
        W(0x603c, 0x04);
        W(0x100e, 0x10);
        FLUSH();grouping=0;
        if(ps5_hdmi_commands_delay(&p,2))return -1;
        W(0x6005, 0x01);
        if(ps5_hdmi_commands_delay(&p,2))return -1;
        W(0x6008, 0x00);
    }
    if(stage==2) {
        W(0x1047, 0x00);
        W(0x6064, 0x01);
        W(0x600c, 0x01, 0x00);
        W(0x6c07, 0x00);
        W(0x7214, 0x00, 0x00);
        FLUSH();
    }
    *out=p;return 0;
#undef W
#undef FLUSH
}

int ps5_flava3_prepare(const struct ps5_icc_io *io,const struct ps5_hdmi_config *config,
                      unsigned *completed,ps5_icc_notification callback,void *context) {
    if(completed)*completed=0;
    if(!io)return -1;
    for(unsigned stage=0;stage<3;stage++) {
        struct ps5_hdmi_commands packet;
        int result=ps5_flava3_prepare_stage(config,stage,&packet);
        if(result)return result;
        result=ps5_hdmi_commands_submit(io,&packet,callback,context);
        if(result)return result;
        if(completed)*completed=stage+1;
    }
    return 0;
}
