/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_ICC_POWER_H
#define PS5_ICC_POWER_H
#ifdef PS5_EDK2
#include <Base.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
#else
#include <stdint.h>
#endif
/* Caller owns the controller and excludes competing IRQ handlers/requests.
 * Region0: mailbox (BAR4); region1: doorbell (BAR2+0x108000).
 * ticks is monotonic modulo64; timeout_ticks must be positive and <2^63.
 */
struct ps5_icc_io {
    void *context;
    uint32_t (*read)(void *, unsigned region, unsigned offset, unsigned width);
    void (*write)(void *, unsigned region, unsigned offset, unsigned width, uint32_t value);
    uint64_t (*ticks)(void *);
    void (*relax)(void *);
    uint64_t timeout_ticks;
};
/* Synchronous callback after transport acknowledgement/checksum validation.
 * Payload is borrowed only for this call. Copy bounded data for later handling;
 * never block, recurse into ICC, or retain its pointer. Runtime callers must
 * supply runtime-resident code/data and convert pointers themselves.
 */
typedef void (*ps5_icc_notification)(void *context,uint8_t service,uint16_t type,
                                    const uint8_t *payload,unsigned bytes);
/* Same query contract as below, with notification delivery while waiting. */
int ps5_icc_query_events(const struct ps5_icc_io *io,uint8_t service,uint16_t type,
                  const uint8_t *payload,unsigned payload_bytes,
                  uint8_t *output,unsigned capacity,unsigned *output_bytes,
                  ps5_icc_notification callback,void *context);
/* Consume at most one pending message without submitting a request. Caller
 * must own ICC and have no outstanding query. 0 no message, 1 notification
 * delivered, 2 stale non-notification discarded; -1 arguments, -4 bad wire data.
 * Callback required so event loss is not silently requested by this API.
 */
int ps5_icc_poll(const struct ps5_icc_io *io,ps5_icc_notification callback,void *context);
/* Generic synchronous transport. Caller must serialize every ICC user.
 * Payload excludes the 12-byte header, max 0x7e4 bytes; requests padded to 32.
 * type must not contain response/notification bits. output may be NULL with
 * capacity=0 to discard a reply. output_bytes (optional) receives wire payload
 * length INCLUDING protocol padding. A service parser must check its own length
 * and status fields. Transport success alone is not service-level success.
 * -5: reply consumed/acknowledged but output too small; output unchanged,
 * output_bytes gives required capacity. -1 arguments, -2 idle timeout, -3 reply timeout, -4 malformed/checksum reply.
 */
int ps5_icc_query(const struct ps5_icc_io *io,uint8_t service,uint16_t type,
                  const uint8_t *payload,unsigned payload_bytes,
                  uint8_t *output,unsigned capacity,unsigned *output_bytes);
/* 0: validated replies to all three messages; this DOES NOT prove reset.
 * -1 arguments, -2 idle timeout, -3 reply timeout, -4 malformed/checksum reply.
 * No NVS write, suspend request, interrupt-mask change or unbounded poll.
 */
int ps5_icc_power(const struct ps5_icc_io *io, int reboot);
#ifdef PS5_ICC_LINUX_NOTIFY
/* edid3 (UNTESTED, only the HdmiProbeDxe copy defines this): an inbound notification with a valid
 * length (12..0x7f0) is taken like PS5 Linux icc_interrupt() without magic/checksum test; any other
 * message that fails the wire check is acknowledged and dropped (its body is not read when the
 * length is out of range) instead of failing with -4. A query gives up with -7 after
 * PS5_ICC_MAX_INBOUND_PER_QUERY inbound messages. The first message of each kind is kept for the log. */
#define PS5_ICC_MAX_INBOUND_PER_QUERY 256U
struct ps5_icc_wire_note { uint32_t count; uint16_t wire_length,checksum,expected; uint8_t head[16]; };
extern struct ps5_icc_wire_note ps5_icc_tolerated,ps5_icc_dropped;
#endif
#endif
