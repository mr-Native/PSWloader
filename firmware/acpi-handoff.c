/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "acpi-handoff.h"
#include "acpi-validate.h"
static uint16_t r16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static uint32_t r32(const uint8_t *p) { return r16(p)|(uint32_t)r16(p+2)<<16; }
static uint64_t r64(const uint8_t *p) { return r32(p)|(uint64_t)r32(p+4)<<32; }
static void w16(uint8_t *p,uint16_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void w32(uint8_t *p,uint32_t v) {w16(p,(uint16_t)v);w16(p+2,(uint16_t)(v>>16));}
static void w64(uint8_t *p,uint64_t v) {w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32));}
static void copy(uint8_t *d,const uint8_t *s,size_t n) {while(n--)*d++=*s++;}
static int equal(const uint8_t *a,const uint8_t *b,size_t n) {while(n--)if(*a++!=*b++)return 0;return 1;}
static const uint8_t acpi_guid[16]={0x06,0x95,0x9a,0x9f,0x97,0x55,0x15,0x45,0xba,0xb6,0x8b,0xcd,0xe7,0x84,0xba,0x87};
static const uint8_t pci_guid[16]={0xcb,0xba,0x4e,0xec,0x38,0x26,0x6e,0x41,0xbe,0x80,0xe5,0xfa,0x4b,0x51,0x19,0x01};
int ps5_add_acpi_handoff(const uint8_t *input,size_t size,const uint8_t *arena,
                         size_t arena_size,uint8_t *output,size_t capacity,
                         struct ps5_handoff_result *result) {
    if(!input||!output||!result||input==output||size<64||size>PS5_HOB_CAPACITY
       ||result->hob_bytes!=size||capacity<size||ps5_validate_acpi(arena,arena_size,PS5_ACPI_BASE))return -1;
    if(r16(input)!=1||r16(input+2)!=56||r64(input+48)!=PS5_HOB_BASE+size-8)return -2;
    uint64_t end=PS5_ACPI_BASE+arena_size;
#ifdef PS5_IOAPIC_SCRATCH
    /* Opt-in firmware IO-APIC: reserve one page right after the arena as EfiReserved (Type0).
     * Only defined for the firmware-IO-APIC payload; the default build is byte-for-byte unchanged.
     * The MADT IO-APIC address is fixed at the scratch base, so the arena must be exactly one page. */
    if(arena_size!=PS5_IOAPIC_SCRATCH_SIZE)return -2;
    uint64_t rend=end+PS5_IOAPIC_SCRATCH_SIZE; /* reserved region = ACPI arena (Type9) + IO-APIC scratch (Type0) */
#else
    uint64_t rend=end;
#endif
    unsigned producer=0,pci=0,acpi=0,ram=0;
    size_t off=0,used=0;
    while(off<size-8) {
        const uint8_t *p=input+off;
        unsigned type=r16(p),length=r16(p+2);
        if(length<8||(length&7)||length>size-8-off||type==0xffff)return -2;
        if(type==3) {
            if(length!=48)return -2;
            uint64_t a=r64(p+32),n=r64(p+40);
            if(a+n<a)return -2;
            if(a<rend&&PS5_ACPI_BASE<a+n) {
                if(r32(p+24)!=0||a>PS5_ACPI_BASE||a+n<rend)return -3;
                ram++;
            }
        }
        if(type==2) {
            if(length!=48)return -2;
            uint64_t a=r64(p+24),n=r64(p+32);
            if(a+n<a)return -2;
            if(a<rend&&PS5_ACPI_BASE<a+n) {
                if(a!=PS5_HOB_BASE||a+n!=PS5_WORK_TOP||r32(p+40)!=4||producer++)return -3;
#ifdef PS5_IOAPIC_SCRATCH
                if(rend>=a+n)return -3; /* scratch must fit strictly inside the producer allocation */
                if(capacity-used<192)return -4;
                /* 4 disjoint segments: [a,ACPI)=4, [ACPI,end)=9 (arena), [end,rend)=0 (IO-APIC scratch, reserved), [rend,a+n)=4 */
                uint64_t starts[4]={a,PS5_ACPI_BASE,end,rend},ends[4]={PS5_ACPI_BASE,end,rend,a+n};
                for(unsigned i=0;i<4;i++) {
                    copy(output+used,p,48);w64(output+used+24,starts[i]);w64(output+used+32,ends[i]-starts[i]);
                    w32(output+used+40,i==1?9:(i==2?0:4));used+=48;
                }
#else
                if(capacity-used<144)return -4;
                uint64_t starts[3]={a,PS5_ACPI_BASE,end},ends[3]={PS5_ACPI_BASE,end,a+n};
                for(unsigned i=0;i<3;i++) {
                    copy(output+used,p,48);w64(output+used+24,starts[i]);w64(output+used+32,ends[i]-starts[i]);
                    w32(output+used+40,i==1?9:4);used+=48;
                }
#endif
                off+=length;continue;
            }
        }
        if(type==4&&length>=24&&equal(p+8,acpi_guid,16)) {
            if(acpi++||length!=40)return -2;
            off+=length;continue; // Replace, including when old RSDP was absent.
        }
        if(length>capacity-used)return -4;
        copy(output+used,p,length);
        if(type==4&&length>=24&&equal(p+8,pci_guid,16)) {
            if(pci++||length!=216||p[24]!=1||r16(p+26)!=188||p[29]!=1
               ||r32(p+204)!=0x0a0341d0||r32(p+208))return -2;
            w32(output+used+204,0x0a0841d0);
        }
        used+=length;off+=length;
    }
    if(r16(input+off)!=0xffff||r16(input+off+2)!=8||producer!=1||ram!=1||pci!=1)return -3;
    if(capacity-used<48)return -4;
    for(unsigned i=0;i<48;i++)output[used+i]=0;
    w16(output+used,4);w16(output+used+2,40);copy(output+used+8,acpi_guid,16);
    output[used+24]=1;w16(output+used+26,12);w64(output+used+28,PS5_ACPI_BASE);used+=40;
    w16(output+used,0xffff);w16(output+used+2,8);used+=8;
    w64(output+40,PS5_HOB_BASE+used);w64(output+48,PS5_HOB_BASE+used-8);
    result->hob_bytes=used;return 0;
}
/* r5.26.7 (version independence): the PCI0 _CRS memory windows of the embedded DSDT follow the root ports
 * (ps5_pci_windows) instead of the 13.60 values the arena was compiled with. The three descriptors are found by
 * their compiled 13.60 contents (each exactly once), rewritten, and the DSDT checksum is fixed. With the 13.60 windows
 * the arena stays byte-identical. */
static const uint64_t crs13[3][3]={{0x80000000,0xc43fffff,0x44400000},{0xd0000000,0xe06fffff,0x10700000},
                                    {UINT64_C(0x980000000),UINT64_C(0x99fffffff),0x20000000}};
int ps5_acpi_set_pci_windows(uint8_t *arena,size_t size,const uint64_t cut[6]) {
    if(!arena||!cut||ps5_validate_acpi(arena,size,PS5_ACPI_BASE))return -1;
    if(cut[1]<=cut[0]||cut[3]<=cut[2]||cut[5]<=cut[4]||cut[3]>UINT64_C(0x100000000))return -1;
    size_t x=(size_t)(r64(arena+24)-PS5_ACPI_BASE),f=(size_t)(r64(arena+x+36)-PS5_ACPI_BASE);
    size_t d=(size_t)(r64(arena+f+140)-PS5_ACPI_BASE),len=r32(arena+d+4);
    uint8_t *t=arena+d;size_t at[3]={0,0,0};unsigned found[3]={0,0,0};
    for(size_t i=36;i+46<=len;i++) {
        for(unsigned k=0;k<2;k++)
            if(t[i]==0x87&&t[i+1]==0x17&&t[i+2]==0&&r32(t+i+10)==crs13[k][0]&&r32(t+i+14)==crs13[k][1]&&r32(t+i+22)==crs13[k][2]){at[k]=i;found[k]++;}
        if(t[i]==0x8a&&t[i+1]==0x2b&&t[i+2]==0&&r64(t+i+14)==crs13[2][0]&&r64(t+i+22)==crs13[2][1]&&r64(t+i+38)==crs13[2][2]){at[2]=i;found[2]++;}
    }
    if(found[0]!=1||found[1]!=1||found[2]!=1)return -2;
    for(unsigned k=0;k<2;k++){w32(t+at[k]+10,(uint32_t)cut[k*2]);w32(t+at[k]+14,(uint32_t)(cut[k*2+1]-1));w32(t+at[k]+22,(uint32_t)(cut[k*2+1]-cut[k*2]));}
    w64(t+at[2]+14,cut[4]);w64(t+at[2]+22,cut[5]-1);w64(t+at[2]+38,cut[5]-cut[4]);
    t[9]=0;{unsigned sum=0;for(size_t i=0;i<len;i++)sum+=t[i];t[9]=(uint8_t)(0x100-(sum&255));}
    return ps5_validate_acpi(arena,size,PS5_ACPI_BASE)?-3:0;
}
