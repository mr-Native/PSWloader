/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Linux v6.12 dio/dcn10 and dio/dcn20 stream encoders; DCN2.0.3 fields.
 */
#include "dcn201-stream.h"
static int apply(const struct ps5_dcn201_mmio *io,const struct ps5_dcn201_register_write *w,unsigned count) {
 for(unsigned i=0;i<count;i++) {
  uint32_t before=0,value;
  if(io->read(io->context,w[i].offset,&before))return -2;
  value=(before&~w[i].mask)|(w[i].value&w[i].mask);
  if(io->write(io->context,w[i].offset,value))return -2;
 }
 return 0;
}
int ps5_dcn201_symbol_clock(const struct ps5_dcn201_mmio *io) {
 if(!io || !io->read || !io->write || !io->delay_us)return -1;
 uint32_t value=0;
 if(io->read(io->context,0x155c0,&value))return -2;
 if(!(value&1))return -4;
 /* DMU_SEG1 0xc0 + SYMCLKA_CLOCK_ENABLE 0xa0, dword to byte. */
 /* Fixed PHY A -> DIGA routing. Explicit FE force/source fields are a
  * bounded experiment on the documented DCN201 register, not a claim that
  * newer-generation clock-management callbacks apply to this platform. */
 const struct ps5_dcn201_register_write enable={0x580,0x711,0x11};
 int result=apply(io,&enable,1);if(result)return result;
 if(io->read(io->context,0x580,&value))return -2;
 if((value&0x711)!=0x11)return -3;
 for(unsigned i=0;i<1000;i++) {
  if(io->read(io->context,0x154a0,&value))return -2;
  if(value&0x01000000)return 0;
  io->delay_us(io->context,1);
 }
 return -3;
}
int ps5_dcn201_stream_setup(const struct ps5_dcn201_mmio *io) {
 static const uint32_t cea16[4]={(2200U<<16)|1125,(192U<<16)|41,(44U<<16)|5 /* positive H/V sync */,(1920U<<16)|1080};
 return ps5_dcn201_stream_setup_msa(io,cea16);
}
int ps5_dcn201_stream_setup_msa(const struct ps5_dcn201_mmio *io,const uint32_t msa[4]) {
 if(!io || !io->read || !io->write || !msa)return -1;
 uint32_t status=0;
 if(io->read(io->context,0x15730,&status))return -2;
 if(status&0x10001)return -4;
 const struct ps5_dcn201_register_write w[]={
  {0x154a0,7,0}, /* DIGA from OTG0 */
  {0x15724,0x07000007,0x01000000}, /* RGB444, 8bpc */
  {0x15728,0xffffffff,0x20000000}, /* MISC0 8bpc full-range sRGB */
  {0x15738,0xc0,0},
  {0x15830,0xffffffff,msa[0]},
  {0x15834,0xffffffff,msa[1]},
  {0x15838,0xffffffff,msa[2]}, /* sync widths, bit 15 = negative polarity */
  {0x1583c,0xffffffff,msa[3]},
  /* opp1_set_pixel_encoding RGB444 and opp1_set_clamping FULL_RANGE.
   * Do not inherit a prior GameOS YCbCr/subsampling or programmable clamp. */
  {0x13400,0x002f0000,0},
  {0x13414,0x00070001,1},
  /* enc2_dp_set_dsc_config(OPTC_DSC_DISABLED): never inherit a GameOS DSC
   * encoder mode while OPTC_DSC_MODE=0 delivers uncompressed pixels. */
  {0x15848,0x1fff0003,0},
  {0x15870,0xffffffff,0},
  /* DP_SEC_CNTL.DP_SEC_STREAM_ENABLE: PS5-Linux testbench (2026-11-05) shows
   * FLAVA3 outputs video only while the secondary stream is enabled; clearing
   * this bit alone blanks a working Linux picture. */
  {0x157ac,0x1,0x1},
 };
 return apply(io,w,sizeof(w)/sizeof(w[0]));
}
int ps5_dcn201_pixel_dto(const struct ps5_dcn201_mmio *io,uint32_t *reference_hz) {
 return ps5_dcn201_pixel_dto_hz(io,148500000,reference_hz);
}
int ps5_dcn201_pixel_dto_hz(const struct ps5_dcn201_mmio *io,uint32_t pixel_hz,uint32_t *reference_hz) {
 if(!io || !io->read || !io->write || !reference_hz || !pixel_hz)return -1;
 uint32_t count=0;
 /* CLK segment0 0x16c00 + CLK4_CLK2_CURRENT_CNT 0x467f, dwords. */
 if(io->read(io->context,0x6c9fc,&count))return -2;
 if(count<1000 || count>12000)return -4;
 const struct ps5_dcn201_register_write w[]={
  {0x500,0x10,0},
  {0x504,0xffffffff,pixel_hz},
  {0x508,0xffffffff,count*100000U},
  {0x500,0x10,0x10},
 };
 if((uint64_t)pixel_hz>=(uint64_t)count*100000U)return -4;   /* DTO phase must stay below modulo */
 int result=apply(io,w,4);
 if(!result)*reference_hz=count*100000U;
 return result;
}
int ps5_dcn201_stream_unblank(const struct ps5_dcn201_mmio *io,int trained) {
 return ps5_dcn201_stream_unblank_m(io,trained,(uint32_t)(32768ULL*148500/810000));
}
int ps5_dcn201_stream_unblank_m(const struct ps5_dcn201_mmio *io,int trained,uint32_t vid_m) {
 if(!io || !io->read || !io->write || !io->delay_us || !vid_m || vid_m>0xffffff)return -1;
 if(trained!=1)return -4;
 uint32_t status=0;
 if(io->read(io->context,0x14004,&status))return -2;
 if(!(status&1))return -4;
 const struct ps5_dcn201_register_write w[]={
  /* optc1_unblank_crtc: the timing counter and DP stream may already run
   * while OTG_BLANK_DATA_EN still forces black pixels. Clear DE mode too. */
  {0x14008,0x10100,0},
  /* AMD set_dp_phy_pattern_passthrough_mode: DP_PANEL_MODE_DEFAULT.
   * DCN201 has no DP_DPHY_INTERNAL_CTRL entry, so that optional write is
   * omitted, as in the AMD implementation's zero-register guard. */
  {0x1574c,0x1103ffff,0x10002000},
  {0x15778,0x3ff00,0x1ff00},
  {0x15720,0x10,0x10},
  {0x1575c,0x10000,0},
  {0x15774,1,0},
  {0x15740,0x100,0},
  {0x15744,0xffffff,0x8000},
  {0x15748,0xffffff,vid_m},
  {0x15740,0xd00,0x100},
 };
 int result=apply(io,w,sizeof(w)/sizeof(w[0]));if(result)return result;
 /* DCN201 constructs dcn20_stream_encoder, whose FIFO reset must be
  * explicitly released. The older enc1 sequence leaves DIG_START asserted. */
 const struct ps5_dcn201_register_write disable={0x15730,1,0};
 result=apply(io,&disable,1);if(result)return result;
 unsigned stopped=0;
 for(unsigned i=0;i<5000;i++) {
  uint32_t value=0;
  if(io->read(io->context,0x15730,&value))return -2;
  if(!(value&0x10000)){stopped=1;break;}
  io->delay_us(io->context,10);
 }
 if(!stopped)return -3;
 const struct ps5_dcn201_register_write reset[]={
  {0x154a0,0x400,0x400},{0x154a0,0x400,0},
  {0x15734,1,1},{0x15734,1,0},
 };
 for(unsigned i=0;i<4;i++) {
  result=apply(io,&reset[i],1);if(result)return result;
  if(i==0)io->delay_us(io->context,1);
  if(i==2)io->delay_us(io->context,10);
 }
 io->delay_us(io->context,100);
 const struct ps5_dcn201_register_write enable[]={
  {0x154b8,0x100,0x100},{0x154b8,0x100,0},
  {0x15730,1,1},
 };
 return apply(io,enable,3);
}
