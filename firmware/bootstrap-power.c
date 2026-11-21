/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Experimental BSP-only power side channel, no ICC/PCI/relay commands.
 * TSC frequency matches the pinned PS5 loader/kernel. Physical meter contrast
 * must be established by calibration; compiled code is not channel evidence.
 */
#ifdef PS5_POWER_EDK2
#include <Base.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
#define UINT64_C(x) x##ULL
#else
#include <stdint.h>
#endif
#define HZ UINT64_C(1596300000)
#ifndef PS5_POWER_TEST
static volatile uint64_t digest;
static uint64_t ticks(void) {
    uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return (uint64_t)hi<<32|lo;
}
static void slot(unsigned active,unsigned seconds) {
    uint64_t start=ticks(),a=1,b=3,c=5,d=7;
    while(ticks()-start<HZ*seconds) {
        if(active) {
            for(unsigned i=0;i<256;i++)
                __asm__ volatile("imulq $1664525,%0,%0; imulq $22695477,%1,%1; imulq $69069,%2,%2; imulq $214013,%3,%3"
                    : "+r"(a),"+r"(b),"+r"(c),"+r"(d) :: "cc");
        } else {
            for(unsigned i=0;i<256;i++) __asm__ volatile("pause");
        }
    }
    digest=a^b^c^d;
}
#else
extern void ps5_power_test_slot(unsigned active,unsigned seconds);
static void slot(unsigned active,unsigned seconds) { ps5_power_test_slot(active,seconds); }
#endif
void ps5_power_calibrate(void) {
    static const unsigned char pattern[8]={0,1,0,1,1,0,0,1};
    for(unsigned i=0;i<8;i++) slot(pattern[i],8);
}
static uint16_t crc16(uint16_t code) {
    uint16_t crc=0xffff;
    for(unsigned byte=0;byte<2;byte++) {
        crc^=(uint16_t)((code>>(byte?0:8))&255)<<8;
        for(unsigned bit=0;bit<8;bit++) crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    return crc;
}
static void send_packet(uint16_t code) {
    uint64_t packet=(UINT64_C(0xd391)<<32)|((uint64_t)code<<16)|crc16((uint16_t)code);
    /* Manchester, MSB first: 0 low/high, 1 high/low. 384 seconds per packet.
     * No guessed code or repeated unbounded output; CPU faults stay silent. */
    for(unsigned i=0;i<48;i++) {
        unsigned bit=(unsigned)(packet>>(47-i))&1;
        slot(bit,4);slot(!bit,4);
    }
}

void ps5_power_report(unsigned code) {
    unsigned category=code>>8,detail=code&255;
    if(category!=8 && code!=3 && code!=0x701 && code!=0x703 && code!=0x704 && code!=0x705 && code!=0x706 && code!=0x707 && code!=0x708 && code!=0x709 && code!=0x70a && code!=0x70b && code!=0x70c && code!=0x70d && code!=0x70e && !(code>=0x711 && code<=0x717) && !((category==1 && detail<=1) || (category==2 && detail>=1 && detail<=8) ||
        (category==3 && detail==0) || (category==4 && (detail<=31 || detail==255)) || (category==5 && detail>=1 && detail<=5) ||
        (category==6 && detail>=1 && detail<=4))) return;
    send_packet((uint16_t)code);
}
void ps5_power_report_pci_byte(unsigned bridge,unsigned offset,uint8_t actual) {
    if(bridge>1 || !((offset<=4) || offset==0x0e || (offset>=0x18 && offset<=0x1a) ||
        (offset>=0x1c && offset<=0x1d) || (offset>=0x20 && offset<=0x33))) return;
    send_packet((uint16_t)(0x8000U|(bridge<<14)|(offset<<8)|actual));
}
