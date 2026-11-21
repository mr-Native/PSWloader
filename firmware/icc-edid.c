/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independent bounded collector for HDMI service notification subtype 2,
 * based on the wire layout used by Linux drivers/ps5/hdmi.c.
 */
#include "icc-edid.h"
static void increment(unsigned *value) { if(*value!=~0U) ++*value; }
void ps5_edid_notification(void *context,uint8_t service,uint16_t type,
                           const uint8_t *payload,unsigned bytes) {
    struct ps5_edid_state *state=context;
    if(!state || service!=0x10 || !(type&0x8000)) return;
    if(!payload || bytes<2) { increment(&state->rejected);return; }
    if(payload[1]!=2) return;
    if(bytes<4) { increment(&state->rejected);return; }
    unsigned length=(unsigned)payload[2]|((unsigned)payload[3]<<8);
    if(length<128 || length>PS5_EDID_MAX_BYTES || length%128 || length>bytes-4) {
        increment(&state->rejected);return;
    }
    const uint8_t *edid=payload+4;
    unsigned flags=0;
    if(edid[0]!=0 || edid[7]!=0) flags|=PS5_EDID_BAD_HEADER;
    for(unsigned i=1;i<7;i++) if(edid[i]!=0xff) flags|=PS5_EDID_BAD_HEADER;
    if(edid[0]==1) flags|=PS5_EDID_PS5_HEADER_QUIRK;
    for(unsigned block=0;block<length;block+=128) {
        unsigned sum=0;
        for(unsigned i=0;i<128;i++) sum+=edid[block+i];
        if(sum&255) flags|=PS5_EDID_BAD_CHECKSUM;
    }
    if(((unsigned)edid[126]+1)*128!=length) flags|=PS5_EDID_BAD_EXTENSION_COUNT;
    for(unsigned i=0;i<length;i++) state->raw[i]=edid[i];
    for(unsigned i=length;i<PS5_EDID_MAX_BYTES;i++) state->raw[i]=0;
    state->bytes=length;state->flags=flags;
    increment(&state->updates);
}
