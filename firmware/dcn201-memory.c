/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independent range arithmetic from AMD DCN201 read_mmhub_vm_setup semantics. */
#include "dcn201-memory.h"
#define LIMIT (1ULL<<48)
static int valid(const struct ps5_dcn201_memory *m) {
    return m && m->fb_base<m->fb_end && m->fb_end<=LIMIT &&
        m->uma_base<m->uma_end && m->uma_end<=LIMIT &&
        m->fb_end-m->fb_base==m->uma_end-m->uma_base;
}
static int contains(uint64_t base,uint64_t end,uint64_t address,uint64_t bytes) {
    return bytes && address>=base && address<end && bytes<=end-address;
}
int ps5_dcn201_memory_decode(uint32_t base,uint32_t top,uint32_t offset,struct ps5_dcn201_memory *out) {
    if(!out || ((base|top|offset)&0xff000000U) || base>top)return -1;
    struct ps5_dcn201_memory m;
    m.fb_base=(uint64_t)base<<24;m.fb_end=((uint64_t)top+1)<<24;
    m.uma_base=(uint64_t)offset<<24;m.uma_end=m.uma_base+m.fb_end-m.fb_base;
    if(!valid(&m))return -1;
    *out=m;return 0;
}
int ps5_dcn201_uma_contains(const struct ps5_dcn201_memory *m,uint64_t address,uint64_t bytes) {
    return valid(m) && contains(m->uma_base,m->uma_end,address,bytes);
}
int ps5_dcn201_gpu_to_uma(const struct ps5_dcn201_memory *m,uint64_t address,uint64_t bytes,uint64_t *out) {
    if(!out || !valid(m))return -1;
    uint64_t result;
    if(contains(m->fb_base,m->fb_end,address,bytes))result=address-m->fb_base+m->uma_base;
    else if(contains(m->uma_base,m->uma_end,address,bytes))result=address;
    else return -1;
    *out=result;return 0;
}

int ps5_dcn201_framebuffer_candidate(const struct ps5_dcn201_memory *m,uint64_t ram_base,
                                    uint64_t ram_bytes,uint64_t bytes,uint64_t *out) {
    if(!out || !valid(m) || !ram_bytes || ram_base>=LIMIT || ram_bytes>LIMIT-ram_base ||
       !bytes || (bytes&4095))return -1;
    uint64_t start=ram_base>m->uma_base?ram_base:m->uma_base;
    uint64_t end=ram_base+ram_bytes;
    if(end>m->uma_end)end=m->uma_end;
    if(start<0x100000)start=0x100000;
    if(end<=start || bytes>end-start)return -1;
    uint64_t address=(end-bytes)&~65535ULL;
    if(address<start)return -1;
    *out=address;return 0;
}

int ps5_dcn201_aperture_framebuffer(const struct ps5_dcn201_memory *m,uint64_t cpu_base,
                                   uint64_t aperture_bytes,uint32_t hdp_low,uint32_t hdp_high,
                                   uint64_t bytes,struct ps5_dcn201_aperture_plan *out) {
    if(!out || !valid(m) || hdp_high>255 || !aperture_bytes || cpu_base<0x100000 || cpu_base>=LIMIT ||
       aperture_bytes>LIMIT-cpu_base || !bytes || (bytes&4095))return -1;
    uint64_t hdp=((uint64_t)hdp_high<<40)|((uint64_t)hdp_low<<8);
    if(hdp<m->fb_base || hdp>=m->fb_end)return -1;
    uint64_t visible=m->fb_end-hdp;
    if(visible>aperture_bytes)visible=aperture_bytes;
    if(bytes>visible)return -1;
    uint64_t offset=(visible-bytes)&~65535ULL;
    uint64_t surface=m->uma_base+(hdp-m->fb_base)+offset;
    if(!ps5_dcn201_uma_contains(m,surface,bytes))return -1;
    struct ps5_dcn201_aperture_plan result={cpu_base+offset,surface,bytes};
    if(result.cpu_address&65535)return -1;
    *out=result;return 0;
}
