/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "framebuffer-page-map.h"
#define PHYS 0x000ffffffffff000ULL
int ps5_framebuffer_identity_writable(uint64_t cr3,uint64_t address,uint64_t bytes,
                                      ps5_page_entry_read read,void *context) {
    if(!read || !bytes || address>=(1ULL<<47) || bytes>(1ULL<<47)-address ||
       (address&4095) || (bytes&4095) || bytes>512ULL*1024*1024)return -1;
    for(uint64_t at=address;at<address+bytes;at+=4096) {
        uint64_t table=cr3&PHYS;int leaf=0;
        for(int level=4;level>=1;level--) {
            if(!table)return -1;
            unsigned shift=12+9*(level-1);uint64_t entry=0;
            if(read(context,table+8*((at>>shift)&511),&entry) || (entry&3)!=3)return -1;
            if(level==4 && (entry&128))return -1;
            if(level==1 || (entry&128)) {
                uint64_t mask=(1ULL<<shift)-1;
                /* Large-page PAT bit12 is not part of its physical base. */
                if(level>1 && (entry&PHYS&mask&~4096ULL))return -1;
                uint64_t physical=(entry&PHYS&~mask)|(at&mask);
                if(physical!=at)return -1;
                leaf=1;break;
            }
            table=entry&PHYS;
        }
        if(!leaf)return -1;
    }
    return 0;
}
