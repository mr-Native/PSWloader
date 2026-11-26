/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_ICC_EDID_H
#define PS5_ICC_EDID_H
#include "icc-power.h"
/* Largest whole-block EDID fitting a 2020-byte ICC payload + 4-byte prefix. */
#define PS5_EDID_MAX_BYTES 1920
#define PS5_EDID_BAD_HEADER 1U
#define PS5_EDID_BAD_CHECKSUM 2U
#define PS5_EDID_BAD_EXTENSION_COUNT 4U
#define PS5_EDID_PS5_HEADER_QUIRK 8U
struct ps5_edid_state {
    uint8_t raw[PS5_EDID_MAX_BYTES];
    unsigned bytes;
    unsigned flags; /* zero means basic header/count/block checks passed */
    unsigned updates;
    unsigned rejected;
};
/* Zero-initialize state before use. Single serialized ICC owner only.
 * Retains the latest bounded EDID event, including invalid EDID bytes with
 * explicit flags. Never repairs checksums or infers usable display timings.
 * Truncated/oversize events leave the prior EDID intact and increment rejected.
 * This is an ICC notification callback, usable for query_events and poll.
 */
void ps5_edid_notification(void *context,uint8_t service,uint16_t type,
                           const uint8_t *payload,unsigned bytes);
#endif
