/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independent serializer from drivers/ps5/hdmi.c wire layouts. */
#include "hdmi-commands.h"
int ps5_hdmi_register_read(const struct ps5_icc_io *io,uint16_t reg,unsigned count,
                           uint8_t *out,ps5_icc_notification callback,void *context) {
 if(!io || !out || !count || count>32 || count>65536U-reg)return -1;
 const uint8_t body[]={4,12,0,1,1,8,1,1,(uint8_t)count,(uint8_t)(reg>>8),(uint8_t)reg,0};
 /* 17.11.2026 (diag9): the EMC pads the reply to 28+count bytes (PS5 Linux ps5_hdmi_read.c logs: count 6 -> 34,
  * 32 -> 60), so the former 40-byte buffer failed every read above 12 bytes with -5. Same echo checks as Linux:
  * data[4]==1, data[5]==count, data[6..7]==register (big endian). */
 uint8_t reply[128];unsigned bytes=0;
 int result=ps5_icc_query_events(io,0x10,0,body,sizeof(body),reply,sizeof(reply),&bytes,callback,context);
 if(result)return result;
 if(bytes<8+count || bytes>sizeof(reply) || reply[0] || reply[1] || reply[4]!=1 || reply[5]!=count ||
    reply[6]!=(uint8_t)(reg>>8) || reply[7]!=(uint8_t)reg)return -6;
 for(unsigned i=0;i<count;i++)out[i]=reply[8+i];
 return 0;
}
static int valid(const struct ps5_hdmi_commands *p) {
    return p && p->used>=4 && p->used<=sizeof(p->data) && (p->data[0]<=2 || p->data[0]==4) &&
        ((unsigned)p->data[1]|((unsigned)p->data[2]<<8))==p->used;
}
static int reserve(struct ps5_hdmi_commands *p,unsigned n) {
    return valid(p) && p->data[3]<255 && n<=sizeof(p->data)-p->used;
}
static void finish(struct ps5_hdmi_commands *p,unsigned n) {
    p->used+=n;p->data[1]=(uint8_t)p->used;p->data[2]=(uint8_t)(p->used>>8);p->data[3]++;
}
int ps5_hdmi_commands_init(struct ps5_hdmi_commands *p,unsigned code) {
    if(!p || (code>2 && code!=4))return -1;
    p->data[0]=(uint8_t)code;p->data[1]=4;p->data[2]=0;p->data[3]=0;p->used=4;return 0;
}
int ps5_hdmi_commands_write_block(struct ps5_hdmi_commands *p,const struct ps5_hdmi_register_write *writes,unsigned count) {
    if(!writes || !count || count>255)return -1;
    unsigned size=4;
    for(unsigned i=0;i<count;i++) {
        if(!writes[i].data || !writes[i].bytes || writes[i].bytes>248 ||
           size+3+writes[i].bytes>255)return -1;
        size+=3+writes[i].bytes;
    }
    if(!reserve(p,size))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=2;d[1]=(uint8_t)size;d[2]=2;d[3]=(uint8_t)count;
    unsigned at=4;
    for(unsigned i=0;i<count;i++) {
        d[at++]=(uint8_t)writes[i].bytes;d[at++]=(uint8_t)(writes[i].reg>>8);d[at++]=(uint8_t)writes[i].reg;
        for(unsigned j=0;j<writes[i].bytes;j++)d[at++]=writes[i].data[j];
    }
    finish(p,size);return 0;
}
int ps5_hdmi_commands_write(struct ps5_hdmi_commands *p,uint16_t reg,const uint8_t *data,unsigned bytes) {
    struct ps5_hdmi_register_write w={reg,data,bytes};
    return ps5_hdmi_commands_write_block(p,&w,1);
}
int ps5_hdmi_commands_write_single(struct ps5_hdmi_commands *p,uint16_t reg,uint8_t value) {
    if(!reserve(p,8))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=2;d[1]=8;d[2]=1;d[3]=1;d[4]=1;
    d[5]=(uint8_t)(reg>>8);d[6]=(uint8_t)reg;d[7]=value;
    finish(p,8);return 0;
}
int ps5_hdmi_commands_event(struct ps5_hdmi_commands *p,unsigned minor,uint8_t value) {
    if(!((minor==1 && value<=3)||(minor==3 && value<=1)||(minor==4 && value<=1)||(minor==5 && value<=1)) || !reserve(p,5))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=5;d[1]=5;d[2]=(uint8_t)minor;d[3]=1;d[4]=value;
    finish(p,5);return 0;
}
int ps5_hdmi_commands_sequence(struct ps5_hdmi_commands *p,const uint8_t *data,unsigned bytes) {
    if(!data || !bytes || bytes>251 || !reserve(p,4+bytes))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=4;d[1]=(uint8_t)(4+bytes);d[2]=16;d[3]=1;
    for(unsigned i=0;i<bytes;i++)d[4+i]=data[i];
    finish(p,4+bytes);return 0;
}
int ps5_hdmi_commands_table(struct ps5_hdmi_commands *p,const uint8_t *data,unsigned count) {
    if(!data || !count || count>62 || !reserve(p,4+4*count))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=4;d[1]=(uint8_t)(4+4*count);d[2]=2;d[3]=(uint8_t)count;
    for(unsigned i=0;i<4*count;i++)d[4+i]=data[i];
    finish(p,4+4*count);return 0;
}
int ps5_hdmi_commands_mask(struct ps5_hdmi_commands *p,uint16_t reg,uint8_t value,uint8_t mask) {
    if(!reserve(p,9))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=2;d[1]=9;d[2]=3;d[3]=1;d[4]=1;
    d[5]=(uint8_t)(reg>>8);d[6]=(uint8_t)reg;d[7]=value;d[8]=mask;
    finish(p,9);return 0;
}
int ps5_hdmi_commands_wait(struct ps5_hdmi_commands *p,uint16_t reg,uint8_t mask,int set) {
    if((set!=0 && set!=1) || !reserve(p,8))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=3;d[1]=8;d[2]=(uint8_t)(set?2:3);d[3]=1;d[4]=0;
    d[5]=(uint8_t)(reg>>8);d[6]=(uint8_t)reg;d[7]=mask;
    finish(p,8);return 0;
}
int ps5_hdmi_commands_wait_eq(struct ps5_hdmi_commands *p,uint16_t reg,uint8_t value) {
    if(!reserve(p,8))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=3;d[1]=8;d[2]=5;d[3]=1;d[4]=0;
    d[5]=(uint8_t)(reg>>8);d[6]=(uint8_t)reg;d[7]=value;
    finish(p,8);return 0;
}
int ps5_hdmi_commands_delay(struct ps5_hdmi_commands *p,unsigned milliseconds) {
    if(milliseconds>65535 || !reserve(p,8))return -1;
    uint8_t *d=p->data+p->used;
    d[0]=3;d[1]=8;d[2]=1;d[3]=1;d[4]=0;
    d[5]=(uint8_t)milliseconds;d[6]=(uint8_t)(milliseconds>>8);d[7]=0;
    finish(p,8);return 0;
}
int ps5_hdmi_commands_submit(const struct ps5_icc_io *io,const struct ps5_hdmi_commands *p,
                             ps5_icc_notification callback,void *context) {
    if(!valid(p) || !p->data[3])return -1;
    unsigned offset=4,count=0;
    while(offset<p->used) {
        if(p->used-offset<4)return -1;
        const uint8_t *d=p->data+offset;unsigned n=d[1];
        if(n<4 || n>p->used-offset || !d[3])return -1;
        if(d[0]==2 && d[2]==2) {
            unsigned at=4,entries=0;
            while(at<n) {
                if(n-at<3 || !d[at] || (unsigned)d[at]>n-at-3)return -1;
                at+=3+d[at];entries++;
            }
            if(entries!=d[3])return -1;
        } else if(d[0]==2 && d[2]==1) {
            if(n!=8 || d[3]!=1 || d[4]!=1)return -1;
        } else if(d[0]==5 && (d[2]==1 || d[2]==3 || d[2]==4 || d[2]==5)) {
            if(n!=5 || d[3]!=1 || d[4]>(d[2]==1?3:1))return -1;
        } else if(d[0]==4 && d[2]==16) {
            if(n<5 || d[3]!=1)return -1;
        } else if(d[0]==4 && d[2]==2) {
            if(n!=4+4u*d[3])return -1;
        } else if(d[0]==2 && d[2]==3) {
            if(n!=9 || d[3]!=1 || d[4]!=1)return -1;
        } else if(d[0]==3 && d[2]==1) {
            if(n!=8 || d[3]!=1 || d[4]!=0 || d[7]!=0)return -1;
        } else if(d[0]==3 && (d[2]==2 || d[2]==3 || d[2]==5)) {
            if(n!=8 || d[3]!=1 || d[4]!=0)return -1;
        } else return -1;
        offset+=n;count++;
    }
    if(count!=p->data[3])return -1;
    uint8_t reply[0x7e4];unsigned bytes=0;
    int status=ps5_icc_query_events(io,0x10,0,p->data,p->used,reply,sizeof(reply),&bytes,callback,context);
    if(status)return status;
    if(bytes<2 || reply[0] || reply[1])return -6;
    return 0;
}
