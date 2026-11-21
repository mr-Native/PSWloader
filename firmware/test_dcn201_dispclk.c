/* r5.16.6: host test for the DPPCLK ladder in dcn201-dispclk.h. The fake DENTIST models HW 17.11.2026: a DPP
 * divider change that jumps from the GameOS DID 0x64 straight below DID 0x40 is acknowledged but not applied (the
 * DPPCLK counter keeps the old clock); steps that stay within 0x64..0x45 or start at <= 0x45 apply. */
#include <stdio.h>
#include <stdint.h>
#include "dcn201-dispclk.h"
static uint32_t dentist=0x64190064u, applied_dpp=0x64, writes;
static unsigned jumps_refused;
static int rd(void *c,uint32_t o,uint32_t *v){(void)c;
 if(o==PS5_DENTIST_DISPCLK_CNTL){*v=dentist;return 0;}
 if(o==PS5_CLK4_CLK_PLL_REQ){*v=30;return 0;}
 if(o==PS5_CLK4_DPPCLK_CNT){*v=120000u/ps5_dentist_divider(applied_dpp);return 0;}
 *v=0;return 0;}
static int wr(void *c,uint32_t o,uint32_t v){(void)c;if(o!=PS5_DENTIST_DISPCLK_CNTL)return 0;writes++;
 unsigned nd=(v>>24)&0x7f,od=(dentist>>24)&0x7f;
 if(nd!=od){ if(od>0x45 && nd<0x40)jumps_refused++; else applied_dpp=nd;
   dentist=(dentist&~0x7f000000u)|(nd<<24); dentist^=(1u<<21); dentist^=(1u<<22); dentist|=PS5_DENTIST_DPP_CHG_DONE; }
 if((v&0x7f)!=(dentist&0x7f)){dentist=(dentist&~0x7fu)|(v&0x7f);dentist|=PS5_DENTIST_DISP_CHG_DONE;}
 return 0;}
static void dl(void *c,unsigned us){(void)c;(void)us;}
int main(void){
 struct ps5_dcn201_mmio io={0,rd,wr,dl};struct ps5_dcn201_dispclk_result r;int fail=0;
 int rc=ps5_dcn201_dispclk_raise(&io,0x2c191515u,1,&r);
 printf("rc=%d after=%08x dpp_steps=%u dpp_cnt=%u refused=%u\n",rc,(unsigned)r.after,r.dpp_written,(unsigned)r.dpp_cnt,jumps_refused);
 if(rc!=0 || ((r.after>>24)&0x7f)!=0x2c || (r.after&0x7f)!=0x15){puts("FAIL dividers");fail++;}
 if(applied_dpp!=0x2c || jumps_refused){puts("FAIL ladder: a refused jump");fail++;}
 if(r.dpp_cnt!=120000u/44u){puts("FAIL counter");fail++;}
 if(r.dpp_written<3){puts("FAIL expected several steps");fail++;}
 /* 3440p60: 0x64 -> 0x45 is one step */
 dentist=0x64190064u;applied_dpp=0x64;jumps_refused=0;
 rc=ps5_dcn201_dispclk_raise(&io,0x45192525u,1,&r);
 if(rc!=0 || applied_dpp!=0x45 || r.dpp_written!=1){printf("FAIL p60 rc=%d dpp=%x steps=%u\n",rc,applied_dpp,r.dpp_written);fail++;}
 /* nothing to do */
 rc=ps5_dcn201_dispclk_raise(&io,0x45192525u,1,&r);
 if(rc!=1){printf("FAIL idempotent rc=%d\n",rc);fail++;}
 puts(fail?"FAILED":"OK");return fail;}
