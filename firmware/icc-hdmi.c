/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independent implementation from the wire layout used by Linux
 * drivers/ps5/hdmi.c:getHdmiConfiguration. No converter programming here.
 */
#include "icc-hdmi.h"
int ps5_hdmi_read_config_events(const struct ps5_icc_io *io,struct ps5_hdmi_config *out,
                                 ps5_icc_notification callback,void *context) {
    uint8_t selector=0x10,reply[0x7e4];
    unsigned bytes=0;
    if(!out) return -1;
    int status=ps5_icc_query_events(io,2,0x16,&selector,1,reply,sizeof(reply),&bytes,callback,context);
    if(status) return status;
    if(bytes<8) return -6;
    struct ps5_hdmi_config config;
    for(unsigned i=0;i<8;i++) config.raw[i]=reply[i];
    config.converter=(uint8_t)(2|(reply[3]==1));
    config.tmds_polarity=(uint8_t)(reply[4]==1);config.tmds_swap=(uint8_t)(reply[5]==1);
    config.dp_polarity=reply[6];config.dp_swap=reply[7];
    *out=config;
    return 0;
}

int ps5_hdmi_read_config(const struct ps5_icc_io *io,struct ps5_hdmi_config *out) {
    return ps5_hdmi_read_config_events(io,out,0,0);
}
