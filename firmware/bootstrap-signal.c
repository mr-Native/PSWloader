/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Optional BSP-only milestone tones, before UEFI owns ICC. Interrupts are off.
 * Protocol: local Linux drivers/ps5/buzzer.c, indicator service 9, type 0.
 * Failure disables later tones; absence of sound never proves absence of entry.
 */
#include <stdint.h>
#include "icc-power.h"
#include "pci-transport.h"
#define ECAM 0xf0000000ULL
/* r5.25.15 (GitHub issue 4, GameOS 6.00; user: "like ps5-linux"): the ICC mailbox and doorbell come from the BARs of
 * the Salina function 104D:9107 (BAR4 = mailbox, BAR2 + 0x108000 = doorbell), as PS5 Linux maps them; 13.60 gives
 * 0x85400000 / 0x85308000. The values below are only the fallback until available() has read the BARs. */
static uint64_t mailbox=0x85400000ULL,doorbell=0x85308000ULL;
#define MAILBOX mailbox
#define DOORBELL doorbell
#define TSC_HZ 1596300000ULL
#ifndef PS5_SIGNAL_TEST
static uint32_t raw_read(uint64_t a,unsigned w) {
    if(w==1)return *(volatile uint8_t *)(uintptr_t)a;
    if(w==2)return *(volatile uint16_t *)(uintptr_t)a;
    return *(volatile uint32_t *)(uintptr_t)a;
}
static void raw_write(uint64_t a,unsigned w,uint32_t v) {
    if(w==1)*(volatile uint8_t *)(uintptr_t)a=(uint8_t)v;
    else if(w==2)*(volatile uint16_t *)(uintptr_t)a=(uint16_t)v;
    else *(volatile uint32_t *)(uintptr_t)a=v;
}
static uint64_t ticks(void *c) {
    (void)c;uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return (uint64_t)hi<<32|lo;
}
static void relax(void *c) {(void)c;__asm__ volatile("pause");}
static uint64_t pat(void) {
    uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x277));
    return (uint64_t)hi<<32|lo;
}
#endif
static int enter(void *c) {(void)c;return 0;}
static void leave(void *c) {(void)c;}
static int pci_read(void *c,uint32_t a,unsigned w,uint32_t *v) {
    (void)c;*v=raw_read(ECAM+a,w);return 0;
}
static int pci_write(void *c,uint32_t a,unsigned w,uint32_t v) {
    (void)c;raw_write(ECAM+a,w,v);return 0;
}
static const struct ps5_pci_io pci={0,enter,leave,pci_read,pci_write};
static int ari;
static int config(uint32_t a,unsigned w,uint32_t *v) {
    return ps5_pci_access(&pci,a,w,ari,0,v);
}
static int available(void) {
    /* PCD=1,PWT=1,PAT=0 in bootstrap's 2 MiB MMIO leaves -> PAT entry 3. */
    unsigned type=(unsigned)(pat()>>24)&255;
    if(type!=0)return 0;
    if(raw_read(ECAM+0x9000,4)!=0x13df1022 || raw_read(ECAM+0x9019,1)!=0x40)return 0;
    ari=0;
    if(raw_read(ECAM+0x9006,2)&16) {
        unsigned cap=raw_read(ECAM+0x9034,1)&0xfc,count=0;
        for(;count<48 && cap>=0x40 && cap<=0xfc;count++) {
            if(raw_read(ECAM+0x9000+cap,1)==0x10) {
                unsigned flags=raw_read(ECAM+0x9000+cap+2,2);
                if((flags&15)<2 || ((flags>>4)&15)!=4 || cap>0xd4)return 0;
                ari=(raw_read(ECAM+0x9000+cap+0x28,2)&32)!=0;
                break;
            }
            cap=raw_read(ECAM+0x9000+cap+1,1)&0xfc;
        }
        if(count==48)return 0;
    }
    uint32_t v,b2l,b2h,b4l,b4h;
    if(config(0x04005000,4,&v) || v!=0x9107104d)return 0;
    if(config(0x04005018,4,&b2l) || config(0x0400501c,4,&b2h) || config(0x04005020,4,&b4l) || config(0x04005024,4,&b4h))return 0;
    uint64_t b2=((uint64_t)b2h<<32)|(b2l&~0xfu),b4=((uint64_t)b4h<<32)|(b4l&~0xfu);
    /* only inside the 32-bit MMIO window the bootstrap maps uncached (0x8000_0000..0xFFFF_FFFF) */
    if(b2<0x80000000ULL || b4<0x80000000ULL || b2+0x110000>0x100000000ULL || b4+0x1000>0x100000000ULL)return 0;
    mailbox=b4;doorbell=b2+0x108000;
    return !config(0x04005004,2,&v) && (v&2);
}
static uint32_t icc_read(void *c,unsigned region,unsigned offset,unsigned width) {
    (void)c;return raw_read((region?DOORBELL:MAILBOX)+offset,width);
}
static void icc_write(void *c,unsigned region,unsigned offset,unsigned width,uint32_t v) {
    (void)c;raw_write((region?DOORBELL:MAILBOX)+offset,width,v);
}
static int state;
static void tone_group(unsigned milestone) {
    if(milestone<1 || milestone>9 || state<0)return;
    if(!state)state=available()?1:-1;
    if(state<0)return;
    const struct ps5_icc_io io={0,icc_read,icc_write,ticks,relax,TSC_HZ};
    const uint8_t payload=1; /* One short tone per successful transport request. */
    for(unsigned i=0;i<milestone;i++) {
        if(ps5_icc_query(&io,9,0,&payload,1,0,0,0)) {state=-1;return;}
        uint64_t start=ticks(0);
        while(ticks(0)-start<TSC_HZ/2)relax(0);
    }
    /* Separate adjacent milestone groups even when the following work is fast. */
    uint64_t start=ticks(0);
    while(ticks(0)-start<TSC_HZ*3)relax(0);
}

void ps5_boot_signal(unsigned milestone) {
    if(milestone>=1 && milestone<=3) tone_group(milestone);
}
void ps5_boot_error(unsigned code) {
    unsigned category=code>>8,detail=code&255;
    /* Never touch ICC from an architectural exception handler. Known regular
     * abort codes only: 0x100..101, 0x201..208, 0x300, 0x501..505, 0x601..604. */
    if(!((category==1 && detail<=1) || (category==2 && detail>=1 && detail<=8) ||
         (category==3 && detail==0) || (category==5 && detail>=1 && detail<=5) ||
         (category==6 && detail>=1 && detail<=4))) return;
    /* Three bounded reports make an otherwise one-shot abort countable.
     * A failed transport disables all remaining reports, as for milestones. */
    for(unsigned repeat=0;repeat<3 && state>=0;repeat++) {
        tone_group(4);          /* Explicit-abort prefix. */
        tone_group(category);   /* Same high byte as RAM checkpoint. */
        tone_group(detail+1);   /* Zero is represented by one tone. */
        if(repeat<2 && state>=0) {
            uint64_t start=ticks(0);
            while(ticks(0)-start<TSC_HZ*6)relax(0);
        }
    }
}

/* Only called after a pre-UEFI diagnostic packet. No OS/filesystem is running.
 * A reply is not reset proof; caller halts if firmware does not restart. */
int ps5_boot_reboot(void) {
    if(!available()) return -10;
    const struct ps5_icc_io io={0,icc_read,icc_write,ticks,relax,TSC_HZ*3};
    return ps5_icc_power(&io,1);
}
