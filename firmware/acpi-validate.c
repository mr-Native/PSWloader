/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "acpi-validate.h"
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t r64(const uint8_t *p) { return r32(p)|(uint64_t)r32(p+4)<<32; }
static int same(const uint8_t *a,const char *b,size_t n) {
    while(n--)if(*a++!=(uint8_t)*b++)return 0;
    return 1;
}
static int valid_sum(const uint8_t *p,size_t n) {
    unsigned sum=0;while(n--)sum+=*p++;
    return !(sum&255);
}
struct table { size_t offset,length; };
static int table_at(const uint8_t *arena,size_t size,uint64_t base,uint64_t address,
                    const char *signature,struct table *out) {
    if(address<base || address-base<36 || address-base>size || (address&15))return -1;
    size_t offset=(size_t)(address-base);
    if(size-offset<36 || !same(arena+offset,signature,4))return -1;
    size_t length=r32(arena+offset+4);
    if(length<36 || length>size-offset || !valid_sum(arena+offset,length))return -1;
    *out=(struct table){offset,length};return 0;
}
int ps5_validate_acpi(const uint8_t *arena,size_t size,uint64_t base) {
    if(!arena || size<4096 || size>65536 || (size&4095) || (base&4095)
       || base<0x100000 || base>(((uint64_t)1<<44)-size))return -1;
    if(!same(arena,"RSD PTR ",8) || arena[15]!=2 || r32(arena+20)!=36
       || r32(arena+16) || !valid_sum(arena,20) || !valid_sum(arena,36))return -2;
    struct table t[5];
    if(table_at(arena,size,base,r64(arena+24),"XSDT",&t[0]) || t[0].length!=60)return -3;
    const char *names[3]={"FACP","APIC","MCFG"};
    for(unsigned i=0;i<3;i++)
        if(table_at(arena,size,base,r64(arena+t[0].offset+36+i*8),names[i],&t[i+1]))return -3;
    const uint8_t *f=arena+t[1].offset;
    if(t[1].length!=276 || f[8]!=6 || !(r32(f+112)&(1U<<20))
       || r32(f+36) || r64(f+132))return -4;
    uint64_t dsdt=r64(f+140);
    if(r32(f+40)!=(dsdt<((uint64_t)1<<32)?(uint32_t)dsdt:0)
       || table_at(arena,size,base,dsdt,"DSDT",&t[4]))return -4;
    // No overlapping tables, including a DSDT hidden inside an XSDT entry.
    for(unsigned i=0;i<5;i++)for(unsigned j=0;j<i;j++)
        if(t[i].offset<t[j].offset+t[j].length && t[j].offset<t[i].offset+t[i].length)return -5;
    // MCFG must describe the measured segment0/bus00-7f ECAM only.
    const uint8_t *m=arena+t[3].offset;
    if(t[3].length!=60 || r64(m+44)!=0xf0000000 || m[52] || m[53]
       || m[54] || m[55]!=127 || r32(m+56))return -6;
    // Validate MADT record bounds; native AP startup remains separately unproven.
    const uint8_t *a=arena+t[2].offset;
    if(t[2].length<44 || r32(a+36)!=0xfee00000)return -7;
    size_t offset=44;
    while(offset<t[2].length) {
        if(t[2].length-offset<2 || a[offset+1]<2 || a[offset+1]>t[2].length-offset)return -7;
        offset+=a[offset+1];
    }
    return 0;
}
