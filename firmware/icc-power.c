/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ICC wire protocol observed in PS5 Linux drivers/ps5/spcie.c and ps5.h.
 * Independent polling transport for firmware requests, including shutdown/reset.
 */
#include "icc-power.h"
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static void put16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n >> 8); }
static uint16_t sum(const uint8_t *p,unsigned n) {
    uint16_t s=0;
    for (unsigned i=0;i<n;i++) if(i!=10 && i!=11) s=(uint16_t)(s+p[i]);
    return s;
}
static int expired(const struct ps5_icc_io *io,uint64_t start) {
    return io->ticks(io->context)-start >= io->timeout_ticks;
}
#ifdef PS5_ICC_LINUX_NOTIFY
struct ps5_icc_wire_note ps5_icc_tolerated,ps5_icc_dropped;
static void remember(struct ps5_icc_wire_note *n,const uint8_t *reply,unsigned length,unsigned wire) {
    if(n->count++) return;
    n->wire_length=(uint16_t)wire;
    if(!reply) return;
    n->checksum=get16(reply+10);n->expected=sum(reply,length);
    for(unsigned i=0;i<16;i++) n->head[i]=reply[i];
}
#endif
/* Copy before acknowledging ownership. Never expose corrupt wire data. */
static int receive(const struct ps5_icc_io *io,uint8_t reply[0x7f0],unsigned *length) {
    uint32_t status=io->read(io->context,1,0x14,4)&3;
    if(status) io->write(io->context,1,0x14,4,status);
    if(!(status&1)) return 0;
    *length=io->read(io->context,0,0x808,2);
    int valid=*length>=12 && *length<=0x7f0;
    if(valid) for(unsigned i=0;i<*length;i++) reply[i]=(uint8_t)io->read(io->context,0,0x800+i,1);
    io->write(io->context,0,0xff0,2,0);
    io->write(io->context,0,0xff4,2,1);
    io->write(io->context,1,4,4,2);
#ifdef PS5_ICC_LINUX_NOTIFY
    if(!valid || reply[0]!=0x42 || get16(reply+10)!=sum(reply,*length)) {
        int note=valid && (get16(reply+2)&0x8000);
        remember(note?&ps5_icc_tolerated:&ps5_icc_dropped,valid?reply:0,*length,*length);
        return note?1:3;   /* 3: consumed and dropped, nothing to deliver */
    }
#else
    if(!valid || reply[0]!=0x42 || get16(reply+10)!=sum(reply,*length)) return -4;
#endif
    return 1;
}
static void notify(ps5_icc_notification callback,void *context,const uint8_t *reply,unsigned length) {
    if(callback && (get16(reply+2)&0x8000))
        callback(context,reply[1],get16(reply+2),reply+12,length-12);
}
static int query(const struct ps5_icc_io *io,uint8_t service,uint16_t type,
                 uint16_t id,const uint8_t *payload,unsigned payload_bytes,
                 uint8_t *output,unsigned capacity,unsigned *output_bytes,
                 ps5_icc_notification callback,void *context) {
    uint8_t request[0x7f0]={0},reply[0x7f0];
    unsigned request_bytes=12+payload_bytes;
    if(request_bytes<32) request_bytes=32;
    uint64_t start=io->ticks(io->context);
    while(io->read(io->context,0,0x7f4,2)!=1 || io->read(io->context,0,0x7f0,2)!=0) {
        if(expired(io,start)) return -2;
        io->relax(io->context);
    }
    request[0]=0x42;request[1]=service;
    put16(request+2,type);put16(request+4,3);put16(request+6,id);put16(request+8,(uint16_t)request_bytes);
    for(unsigned i=0;i<payload_bytes;i++) request[12+i]=payload[i];
    put16(request+10,sum(request,request_bytes));
    // Clear stale transport ACK only, preserving a queued inbound message.
    io->write(io->context,1,0x14,4,2);
    io->write(io->context,0,0x7f4,2,0);
    for(unsigned i=0;i<request_bytes;i++) io->write(io->context,0,i,1,request[i]);
    io->write(io->context,0,0x7f0,2,1);
    io->write(io->context,1,4,4,1);
    start=io->ticks(io->context);
#ifdef PS5_ICC_LINUX_NOTIFY
    unsigned inbound=0;
#endif
    for(;;) {
        unsigned length=0;
        int received=receive(io,reply,&length);
        if(received<0) return received;
#ifdef PS5_ICC_LINUX_NOTIFY
        if(received && ++inbound>PS5_ICC_MAX_INBOUND_PER_QUERY) return -7;
        if(received==3) received=0;
#endif
        if(received) {
            notify(callback,context,reply,length);
            if(reply[1]==service && get16(reply+6)==id && get16(reply+2)==(type|0x4000)) {
                unsigned bytes=length-12;
                if(output_bytes) *output_bytes=bytes;
                if(output) {
                    if(bytes>capacity) return -5;
                    for(unsigned i=0;i<bytes;i++) output[i]=reply[12+i];
                }
                return 0;
            }
            // Valid notifications are delivered, stale replies consumed; neither counts
            // as completion of our request. They do not reset the deadline.
        }
        if(expired(io,start)) return -3;
        io->relax(io->context);
    }
}
static int valid_io(const struct ps5_icc_io *io) {
    return io && io->read && io->write && io->ticks && io->relax
        && io->timeout_ticks && io->timeout_ticks < ((uint64_t)1<<63);
}
int ps5_icc_query_events(const struct ps5_icc_io *io,uint8_t service,uint16_t type,
                  const uint8_t *payload,unsigned payload_bytes,
                  uint8_t *output,unsigned capacity,unsigned *output_bytes,
                  ps5_icc_notification callback,void *context) {
    if(output_bytes) *output_bytes=0;
    if(!valid_io(io) || payload_bytes>0x7f0-12 || (payload_bytes && !payload)
        || (!output && capacity) || (type&0xc000)) return -1;
    uint16_t id=(uint16_t)(io->read(io->context,0,6,2)+1);
    return query(io,service,type,id,payload,payload_bytes,output,capacity,output_bytes,callback,context);
}
int ps5_icc_query(const struct ps5_icc_io *io,uint8_t service,uint16_t type,
                  const uint8_t *payload,unsigned payload_bytes,
                  uint8_t *output,unsigned capacity,unsigned *output_bytes) {
    return ps5_icc_query_events(io,service,type,payload,payload_bytes,
                                output,capacity,output_bytes,0,0);
}
int ps5_icc_poll(const struct ps5_icc_io *io,ps5_icc_notification callback,void *context) {
    uint8_t reply[0x7f0];
    unsigned length=0;
    if(!valid_io(io) || !callback) return -1;
    int result=receive(io,reply,&length);
#ifdef PS5_ICC_LINUX_NOTIFY
    if(result==3) return 0;
#endif
    if(result<=0) return result;
    if(!(get16(reply+2)&0x8000)) return 2;
    notify(callback,context,reply,length);
    return 1;
}
int ps5_icc_power(const struct ps5_icc_io *io,int reboot) {
    if(!valid_io(io) || (reboot!=0 && reboot!=1)) return -1;
    uint16_t id=(uint16_t)(io->read(io->context,0,6,2)+1);
    uint8_t payload[9]={0};
    int status=query(io,2,0x10,id++,payload,9,0,0,0,0,0);
    if(status) return status;
    payload[0]=1;
    status=query(io,2,0x10,id++,payload,9,0,0,0,0,0);
    if(status) return status;
    payload[0]=0;payload[1]=(uint8_t)reboot;payload[2]=2;payload[4]=1;
    return query(io,4,1,id,payload,9,0,0,0,0,0);
}
