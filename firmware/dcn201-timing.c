/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * AMD DCN201 optc1_program_timing and optc1_enable_optc_clock facts.
 * Register offsets/masks generated against Linux v6.12 DCN2.0.3 definitions. */
#include "dcn201-timing.h"
int ps5_dcn201_1080p_sync(struct ps5_dcn201_register_write out[10]) {
 if(!out)return -1;
 const struct ps5_dcn201_register_write values[10]={
  {0x13fa8,0x7fff,0x897},
  {0x13fb0,0x7fff7fff,0x2c0000},
  {0x13fac,0x7fff7fff,0xc00840},
  {0x13fb4,0x1,0x0},
  {0x13fbc,0x7fff,0x464},
  {0x13fc4,0x7fff,0x464},
  {0x13fc0,0x7fff,0x464},
  {0x13fdc,0x7fff7fff,0x50000},
  {0x13fd8,0x7fff7fff,0x290461},
  {0x13fe0,0x1,0x0},
 };
 for(unsigned i=0;i<10;i++)out[i]=values[i];
 return 0;
}
int ps5_dcn201_enable_timing_clocks(const struct ps5_dcn201_mmio *io) {
 if(!io || !io->read || !io->write || !io->delay_us)return -1;
 const uint32_t regs[3][4]={
  {0x13e3c,0x3,0x3,0x4},
  {0x14118,0x3,0x3,0x100},
  {0x13530,0x1,0x1,0x2}, /* opp1_pipe_clock_control; DPG/formatter clock */
 };
 for(unsigned i=0;i<3;i++) {
  uint32_t before=0;
  if(io->read(io->context,regs[i][0],&before) ||
     io->write(io->context,regs[i][0],(before&~regs[i][1])|regs[i][2]))return -2;
  unsigned ready=0;
  for(unsigned n=0;n<1000;n++) {
   uint32_t value=0;
   if(io->read(io->context,regs[i][0],&value))return -2;
   if((value&regs[i][3])==regs[i][3]){ready=1;break;}
   io->delay_us(io->context,1);
  }
  if(!ready)return -3;
 }
 return 0;
}

static int apply(const struct ps5_dcn201_mmio *io,const struct ps5_dcn201_register_write *w,unsigned n) {
 for(unsigned i=0;i<n;i++) {
  uint32_t before=0;
  if(io->read(io->context,w[i].offset,&before) ||
     io->write(io->context,w[i].offset,(before&~w[i].mask)|(w[i].value&w[i].mask)))return -2;
 }
 return 0;
}
int ps5_dcn201_start_solid_mode(const struct ps5_dcn201_mmio *io,const struct ps5_dcn201_register_write otg[10],
                               unsigned width,unsigned height,unsigned vtotal,unsigned vcount_init,unsigned fp2,
                               const struct ps5_dcn201_global_sync *s,
                               unsigned red,unsigned green,unsigned blue) {
 if(!io || !io->read || !io->write || !io->delay_us || !s || !otg || vtotal<2 || vtotal>0x8000 ||
    vcount_init>0x7fff || fp2>0x7fff || !s->startup || s->startup>vtotal-1 || s->update_offset>vtotal-1 ||
    s->update_width>vtotal-1 || s->ready_offset>vtotal-1)return -1;
 struct ps5_dcn201_register_write color[6];
 if(ps5_dcn201_solid_pattern(width,height,red,green,blue,color))return -1;
 uint32_t control=0;
 if(io->read(io->context,0x14004,&control))return -2;
 if(control&1)return -4;
 int status=ps5_dcn201_enable_timing_clocks(io);if(status)return status;
 const struct ps5_dcn201_register_write global[]={
  {0xe7a0,0x80000000,0x0},
  {0x14010,0x1,0x0},
  {0x14004,0x1000,0x1000},
  {0x14004,0x2000,0x0},
  {0x1411c,0x3ff,((s->startup)<<0)},
  {0x14120,0xffff,((s->update_offset)<<0)},
  {0x14120,0x3ff0000,((s->update_width)<<16)},
  {0x14124,0xffff,((s->ready_offset)<<0)},
  {0xe7a0,0x7fff,fp2},
  {0xe7a0,0x7fff0000,vcount_init<<16},
  {0x13e30,0x3,0x0},
  {0x13fb8,0x1,0x0},
  {0x13e2c,0xf00,0x0},
 };
 status=apply(io,otg,10);if(status)return status;
 status=apply(io,global,sizeof(global)/sizeof(global[0]));if(status)return status;
 status=apply(io,color,6);if(status)return status;
 const struct ps5_dcn201_register_write start[]={
  {0xe7a0,0x80000000,0x80000000},
  {0x14004,0x300,0x300},
  {0x14004,0x1,0x1},
 };
 return apply(io,start,3);
}
int ps5_dcn201_start_solid_1080p(const struct ps5_dcn201_mmio *io,
                                const struct ps5_dcn201_global_sync *s,
                                unsigned red,unsigned green,unsigned blue) {
 struct ps5_dcn201_register_write timing[10];
 if(!s || !s->startup || s->startup>1124 || s->update_offset>1124 ||
    s->update_width>1124 || s->ready_offset>1124 || ps5_dcn201_1080p_sync(timing))return -1;
 return ps5_dcn201_start_solid_mode(io,timing,1920,1080,1125,0x461,s->startup>42?s->startup-42:0,s,red,green,blue);
}
