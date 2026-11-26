/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Linux v6.12 link_factory.c, bios/dce112/command_table_helper2_dce112.c,
 * command_table2.c and dcn10_link_encoder.c; DCN2.0.3 register definitions.
 * Byte offsets: (DMU_SEG2 0x34c0 + register index) * 4.
 */
#include "dcn201-link.h"
int ps5_dcn201_encoder_setup_params(uint32_t out[3]) {
 if(!out)return -1;
 /* PANEL_8BIT_PER_COLOR=2 is an ATOM enum, not literal bits-per-channel.
  * command_table2.c leaves dplinkrate_270mhz zero in its stream setup. */
 out[0]=0x04000f00;out[1]=14850;out[2]=2;return 0;
}
static int valid(const struct ps5_dcn201_link *l) {
 return l && l->phy<2 && l->hpd==l->phy+1;
}
static int valid_lanes(unsigned n) {return n==1 || n==2 || n==4;}
int ps5_dcn201_link_decode(unsigned connector,unsigned encoder,unsigned hpd,
                          struct ps5_dcn201_link *out) {
 if(!out || hpd<1 || hpd>2 || connector!=(0x3013U+(hpd<<8)) ||
    encoder!=(0x201eU+(hpd<<8)))return -1;
 out->phy=hpd-1;out->hpd=hpd;return 0;
}
int ps5_dcn201_transmitter_params(const struct ps5_dcn201_link *l,unsigned action,
                                unsigned lanes,unsigned clock,uint32_t out[4]) {
 if(!valid(l) || !out || (action!=0 && action!=1 && action!=7))return -1;
 if(action==0 ? (lanes!=0 || clock!=0) : !valid_lanes(lanes))return -1;
 if(action==7 && (lanes!=4 || clock!=0))return -1;
 if(action==1 && clock!=16200 && clock!=27000 && clock!=54000 && clock!=81000)return -1;
 /* DP digmode=0, FE=0. INIT supplies connector id; ENABLE's original AMD
  * caller leaves connector_obj_id zero. No synthetic FE bit mask here. */
 out[0]=l->phy|(action<<8)|(lanes<<24);
 out[1]=clock;
 out[2]=l->hpd|(action!=1 ? 0x13U<<16 : 0);
 out[3]=0;return 0;
}
int ps5_dcn201_link_plan(const struct ps5_dcn201_link *l,unsigned lanes,
                        struct ps5_dcn201_register_write out[3]) {
 if(!valid(l) || !out || !valid_lanes(lanes))return -1;
 uint32_t stride=l->phy*0x400;
 out[0]=(struct ps5_dcn201_register_write){0x155bc+stride,0x70077f00,((l->hpd-1)<<28)|0x100};
 /* AMD REG_SET(DP_CONFIG,0,DP_UDI_LANES,n-1), not REG_UPDATE. */
 out[1]=(struct ps5_dcn201_register_write){0x1572c+stride,0xffffffff,lanes-1};
 out[2]=(struct ps5_dcn201_register_write){0x15778+stride,0x10,0x10};
 return 0;
}
