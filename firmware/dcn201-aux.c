/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Linux dc/dce/dce_aux.c on DCN2.0.3 DP_AUX0 (0x1f50+0x34c0 dwords). */
#include "dcn201-aux.h"
#define AUX_CONTROL 0x00
#define AUX_SW_CONTROL 0x04
#define AUX_ARB_CONTROL 0x08
#define AUX_INTERRUPT_CONTROL 0x0c
#define AUX_SW_STATUS 0x10
#define AUX_SW_DATA 0x18
static int rd(const struct ps5_dcn201_mmio *io,uint32_t o,uint32_t *v){return io->read(io->context,o,v)?-2:0;}
static int wr(const struct ps5_dcn201_mmio *io,uint32_t o,uint32_t v){return io->write(io->context,o,v)?-2:0;}
static int wait(const struct ps5_dcn201_mmio *io,uint32_t o,uint32_t mask,uint32_t want,unsigned us) {
 for(unsigned i=0;i<=us/10;i++){uint32_t v=0;if(rd(io,o,&v))return -2;if((v&mask)==want)return 0;io->delay_us(io->context,10);}
 return -3;
}
static void release(const struct ps5_dcn201_mmio *io,uint32_t b) {
 uint32_t v=0;if(rd(io,b+AUX_ARB_CONTROL,&v))return;
 wr(io,b+AUX_ARB_CONTROL,(v&~0x00010000U)|0x00020000U);
}
int ps5_dcn201_aux_dpcd(const struct ps5_dcn201_mmio *io,unsigned instance,
                        int write,uint32_t address,uint8_t *data,unsigned len,
                        uint32_t *status,uint8_t *reply) {
 if(!io || !io->read || !io->write || !io->delay_us || !data || !status || !reply)return -1;
 if(instance>5 || !len || len>16 || address>0xfffff)return -1;
 const uint32_t b=0x14240+instance*0x70;
 uint32_t v=0;int r;
 *status=0;*reply=0xff;
 /* acquire_engine */
 if((r=rd(io,b+AUX_ARB_CONTROL,&v)))return r;
 if(((v>>2)&3)==2)return -5;
 if((r=rd(io,b+AUX_CONTROL,&v)))return r;
 if(!(v&1)) {
  v|=0x11;if((r=wr(io,b+AUX_CONTROL,v)))return r;
  if((r=wait(io,b+AUX_CONTROL,0x20,0x20,110)))return r;
  v&=~0x10U;if((r=wr(io,b+AUX_CONTROL,v)))return r;
  if((r=wait(io,b+AUX_CONTROL,0x20,0,110)))return r;
 }
 if((r=rd(io,b+AUX_ARB_CONTROL,&v)))return r;
 if((r=wr(io,b+AUX_ARB_CONTROL,v|0x00010000U)))return r;
 if((r=rd(io,b+AUX_ARB_CONTROL,&v)))return r;
 if(((v>>2)&3)!=1){release(io,b);return -5;}
 /* submit_channel_request */
 if((r=rd(io,b+AUX_INTERRUPT_CONTROL,&v)) || (r=wr(io,b+AUX_INTERRUPT_CONTROL,v|2))){release(io,b);return r;}
 if((r=wait(io,b+AUX_SW_STATUS,1,0,5000))){release(io,b);return r;}
 uint32_t length=4+(write?len:0);
 if((r=rd(io,b+AUX_SW_CONTROL,&v)) || (r=wr(io,b+AUX_SW_CONTROL,(v&~0x001f00f0U)|(length<<16)))){release(io,b);return r;}
 uint32_t d=0;if((r=rd(io,b+AUX_SW_DATA,&d))){release(io,b);return r;}
 uint32_t action=write?0x80:0x90;
 d=(d&~0x801fff01U)|0x80000000U|(((action|((address>>16)&0xf))&0xff)<<8);
 if((r=wr(io,b+AUX_SW_DATA,d))){release(io,b);return r;}
 d&=~0x8000ff00U;
 uint8_t bytes[3+16];unsigned n=0;
 bytes[n++]=(uint8_t)(address>>8);bytes[n++]=(uint8_t)address;bytes[n++]=(uint8_t)(len-1);
 if(write)for(unsigned i=0;i<len;i++)bytes[n++]=data[i];
 for(unsigned i=0;i<n;i++){d=(d&~0xff00U)|((uint32_t)bytes[i]<<8);if((r=wr(io,b+AUX_SW_DATA,d))){release(io,b);return r;}}
 if((r=rd(io,b+AUX_SW_CONTROL,&v)) || (r=wr(io,b+AUX_SW_CONTROL,v|1))){release(io,b);return r;}
 /* get_channel_status */
 if((r=wait(io,b+AUX_SW_STATUS,1,1,10000))){rd(io,b+AUX_SW_STATUS,status);release(io,b);return r;}
 if((r=rd(io,b+AUX_SW_STATUS,status))){release(io,b);return r;}
 if(*status&(0x200|0x80|0x100|0x00f00000U)){release(io,b);return -6;}
 unsigned replied=(*status>>24)&0x1f;
 if(!replied){release(io,b);return -6;}
 /* read_channel_reply */
 if((r=rd(io,b+AUX_SW_DATA,&d))){release(io,b);return r;}
 d=(d&~0x801f0001U)|0x80000001U;
 if((r=wr(io,b+AUX_SW_DATA,d))){release(io,b);return r;}
 if((r=rd(io,b+AUX_SW_DATA,&d))){release(io,b);return r;}
 *reply=(uint8_t)(((d>>8)&0xff)>>4);
 if(*reply){release(io,b);return -7;}
 unsigned got=0;
 if(!write) {
  replied--;if(replied>len){release(io,b);return -6;}
  for(;got<replied;got++){if((r=rd(io,b+AUX_SW_DATA,&d))){release(io,b);return r;}data[got]=(uint8_t)(d>>8);}
 } else got=len;
 release(io,b);
 return (int)got;
}
