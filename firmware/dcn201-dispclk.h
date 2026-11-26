/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * DRAFT v1.1, UNTESTED ON HARDWARE (14.11.2026; v1.1: PLL/DENTIST read before the want==0 exit). DCN201 DISPCLK/DPPCLK raise before the mode is armed,
 * as PS5 Linux does it: dcn201_update_clocks (clk_mgr/dcn201/dcn201_clk_mgr.c:155-168, clock raised ->
 * dentist first) -> dcn20_update_clocks_update_dentist (clk_mgr/dcn20/dcn20_clk_mgr.c:127-212):
 *   DISPCLK_WDIVIDER := did, wait DENTIST_DISPCLK_CHG_DONE (50 us x 2000), then
 *   DPPCLK_WDIVIDER  := did, wait DENTIST_DPPCLK_CHG_DONE  (5 us x 100).
 * DCN201 has no per-pipe DPPCLK DTO work (dccg201_update_dpp_dto: "vbios handles it") and no SMU call here.
 * The wanted value comes from the replay (Linux at that mode, or generic-dcn.py's DML result, 6/6 equal to Linux).
 * Raise only: a field whose current divider already gives >= the wanted clock is left alone (never lower a
 * clock under a running pipe; dcn20 lowers only when safe_to_lower, after the pipes changed).
 * Refused: VCO != 3 GHz (the DIDs assume it; Linux reads FbMult_int at construct, dcn201_clk_mgr.c:206-210),
 * and any DID 0x7f transition (needs the OTG FIFO pixel drop/add sequence, dcn20_clk_mgr.c:150-205). */
#ifndef PS5_DCN201_DISPCLK_H
#define PS5_DCN201_DISPCLK_H
#include "dcn201-timing.h"
#define PS5_DENTIST_DISPCLK_CNTL 0x490u   /* DCN base1 0x300 + mmDENTIST_DISPCLK_CNTL 0x64, dword->byte (read today at the same offset) */
#define PS5_CLK4_CLK_PLL_REQ     0x6c838u /* clk base 0x5b000 + mmCLK4_0_CLK4_CLK_PLL_REQ 0x460e*4 (same base as CLK2_CURRENT_CNT 0x6c9fc) */
#define PS5_DENTIST_DISP_CHG_DONE (1u<<19)
#define PS5_DENTIST_DPP_CHG_DONE  (1u<<20)
#define PS5_CLK4_DPPCLK_CNT      0x6c9f8u /* r5.16.6: DPPCLK counter in 0.1 MHz (HW 17.11.: 83.4 at DID 0x64, 162.2 at 0x45, 272.7 at 0x2C) */
/* r5.16.6 (HW 17.11.2026 diag14): a direct DPPCLK change from the GameOS DID 0x64 (83 MHz) to 0x2C (273 MHz)
 * reads back as done but the DPPCLK counter stays at 83 MHz (PS5 Linux the same: no 3440p100 picture). Stepping
 * 0x64 -> 0x45 -> 0x40 -> 0x3C ... -> 0x2C takes effect at every step. So the DPP divider goes up a ladder: first to
 * at most DID 0x45, then 4 DIDs per step, each with the change toggle/done wait. */
#define PS5_DPP_LADDER_FIRST 0x45u
#define PS5_DPP_LADDER_STEP  4u
/* inverse of dentist_get_did_from_divider (clk_mgr_internal.h:42-58); divider in quarter steps, clock = 4*VCO/divider */
static inline unsigned ps5_dentist_divider(unsigned did) {
 if(did<0x08 || did>=0x7f)return 0;           /* 0x00-0x07 invalid, 0x7f special */
 if(did<0x40)return did;                       /* 8..63,   step 1 */
 if(did<0x60)return 64+(did-0x40)*2;           /* 64..126, step 2 */
 if(did<0x7e)return 128+(did-0x60)*4;          /* 128..244, step 4 */
 return 248;                                   /* 0x7e */
}
struct ps5_dcn201_dispclk_result {
 uint32_t before,after,pll_req;
 unsigned disp_written,dpp_written;   /* 1 = that divider was raised; dpp_written = number of DPP ladder steps */
 uint32_t dpp_cnt;                    /* r5.16.6: CLK4 DPPCLK counter after the raise, 0.1 MHz (0 = not read) */
};
/* want = DENTIST_DISPCLK_CNTL of the target mode (0 = replay carries none -> nothing done, returns 1).
 * write=0: plan only (reads, no writes) - result->*_written says what WOULD be written.
 * 0 ok, 1 nothing to do, -1 bad input, -2 MMIO error, -3 CHG_DONE timeout, -5 VCO != 3 GHz, -6 DID 0x7f involved. */
static inline int ps5_dcn201_dispclk_raise(const struct ps5_dcn201_mmio *io,uint32_t want,int write,
                                           struct ps5_dcn201_dispclk_result *r) {
 if(!io || !io->read || !io->write || !io->delay_us || !r)return -1;
 r->before=r->after=r->pll_req=0;r->disp_written=r->dpp_written=0;r->dpp_cnt=0;
 uint32_t v=0;                                 /* v1.1: read first, so the log shows them even without a target */
 if(io->read(io->context,PS5_CLK4_CLK_PLL_REQ,&r->pll_req))return -2;
 if(io->read(io->context,PS5_DENTIST_DISPCLK_CNTL,&v))return -2;
 r->before=r->after=v;
 if(!want)return 1;
 const unsigned wd=want&0x7f,wp=(want>>24)&0x7f;
 if(!ps5_dentist_divider(wd) || !ps5_dentist_divider(wp))return wd==0x7f||wp==0x7f?-6:-1;
 if((r->pll_req&0x1ff)!=30)return -5;          /* FbMult_int x 100 MHz must be the measured 3 GHz */
 const unsigned cd=v&0x7f,cp=(v>>24)&0x7f;
 if(cd==0x7f || cp==0x7f)return -6;
 /* larger divider = lower clock; an invalid current DID (0..7) counts as "must set" */
 const unsigned need_d=!ps5_dentist_divider(cd) || ps5_dentist_divider(cd)>ps5_dentist_divider(wd);
 const unsigned need_p=!ps5_dentist_divider(cp) || ps5_dentist_divider(cp)>ps5_dentist_divider(wp);
 if(!need_d && !need_p)return 1;
 if(!write){r->disp_written=need_d;r->dpp_written=need_p;return 0;}
 if(need_d) {
  if(io->write(io->context,PS5_DENTIST_DISPCLK_CNTL,(v&~0x7fu)|wd))return -2;
  r->disp_written=1;
  unsigned i;for(i=0;i<2000;i++){if(io->read(io->context,PS5_DENTIST_DISPCLK_CNTL,&v))return -2;if(v&PS5_DENTIST_DISP_CHG_DONE)break;io->delay_us(io->context,50);}
  if(i==2000)return -3;
 }
 if(need_p) {
  unsigned cur=cp,steps=0;
  while(!ps5_dentist_divider(cur) || ps5_dentist_divider(cur)>ps5_dentist_divider(wp)) {
   /* next rung: the GameOS DID (or anything slower than 0x45) goes to 0x45 first, then 4 DIDs per step */
   unsigned next=(!ps5_dentist_divider(cur) || ps5_dentist_divider(cur)>ps5_dentist_divider(PS5_DPP_LADDER_FIRST))?PS5_DPP_LADDER_FIRST:
                 (cur>=wp+PS5_DPP_LADDER_STEP?cur-PS5_DPP_LADDER_STEP:wp);
   if(ps5_dentist_divider(next)<ps5_dentist_divider(wp))next=wp;          /* never past the target */
   if(io->read(io->context,PS5_DENTIST_DISPCLK_CNTL,&v))return -2;
   if(io->write(io->context,PS5_DENTIST_DISPCLK_CNTL,(v&~0x7f000000u)|((uint32_t)next<<24)))return -2;
   unsigned i;
   for(i=0;i<200;i++) {
    if(io->read(io->context,PS5_DENTIST_DISPCLK_CNTL,&v))return -2;
    if((v&PS5_DENTIST_DPP_CHG_DONE) && ((v>>21)&1)==((v>>22)&1))break;
    io->delay_us(io->context,50);
   }
   if(i==200)return -3;
   io->delay_us(io->context,1000);
   cur=next;steps++;
   if(steps>32)return -3;
  }
  r->dpp_written=steps;
 }
 if(io->read(io->context,PS5_DENTIST_DISPCLK_CNTL,&r->after))return -2;
 if(need_p)(void)io->read(io->context,PS5_CLK4_DPPCLK_CNT,&r->dpp_cnt);
 return 0;
}
#endif
