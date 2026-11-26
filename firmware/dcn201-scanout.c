/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Register definitions: Linux v6.12 AMD dcn_2_0_3_sh_mask.h, used by DCN201.
 * Only the linear, uncompressed ARGB8888 scanout needed for a GOP is accepted.
 */
#include "dcn201-scanout.h"
int ps5_dcn201_linear_scanout(const struct ps5_dcn201_state *s,struct ps5_linear_scanout *out) {
    if(!s || !out)return -1;
    if(!(s->otg_control&1U) || (s->hubp_control&5U))return -2;
    /* Format8 is little-endian B,G,R,A. Reject rotation and mirroring. */
    if((s->surface_config&0x7ffU)!=8U || (s->tiling_config&31U) || (s->surface_control&2U))return -3;
    uint32_t hs=s->h_blank&0x7fffU,he=(s->h_blank>>16)&0x7fffU;
    uint32_t vs=s->v_blank&0x7fffU,ve=(s->v_blank>>16)&0x7fffU;
    if(hs<=he || vs<=ve)return -4;
    uint32_t width=hs-he,height=vs-ve,pitch=(s->pitch&0x3fffU)+1U;
    if(width>8192 || height>8192 || pitch<width)return -4;
    uint64_t address=((uint64_t)(s->address_high&0xffffU)<<32)|s->address_low;
    uint64_t bytes=(uint64_t)pitch*4U*height;
    if(!address || (address&255U) || bytes>512ULL*1024*1024 || address+bytes>1ULL<<48)return -5;
    *out=(struct ps5_linear_scanout){address,bytes,width,height,pitch,pitch*4U};
    return 0;
}
