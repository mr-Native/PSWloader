/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Register facts: Linux v6.12 DCN201 uses opp2_set_disp_pattern_generator;
 * DPG0 DCN2.0.3 offsets + DMU segment2 0x34c0, horizontal-bars mode4.
 * Both bar colors equal, producing a solid color directly in the GPU. */
#include "dcn201-test-pattern.h"
int ps5_dcn201_solid_pattern(unsigned width,unsigned height,unsigned red,unsigned green,
                            unsigned blue,struct ps5_dcn201_register_write out[6]) {
 if(!out || !width || !height || width>0x3fff || height>0x3fff ||
    red>1023 || green>1023 || blue>1023)return -1;
 uint32_t r=red<<6,g=green<<6,b=blue<<6;
 struct ps5_dcn201_register_write writes[6]={
  {0x13458,0xffffffffU,(width<<16)|height}, // dimensions
  {0x13468,0xffffffffU,0}, // offset/segment
  {0x13464,0xffffffffU,b|(b<<16)},
  {0x13460,0xffffffffU,g|(g<<16)},
  {0x1345c,0xffffffffU,r|(r<<16)},
  {0x13450,0x71,0x41} // preserve other control fields; enable, mode4
 };
 for(unsigned i=0;i<6;i++)out[i]=writes[i];
 return 0;
}
