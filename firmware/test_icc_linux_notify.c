/* edid2/edid3 host test (no hardware): inbound ICC handling of icc-power.c with and without
 * PS5_ICC_LINUX_NOTIFY. Build twice:
 *   gcc -std=c11 -Wall -Wextra -Werror test_icc_linux_notify.c icc-power.c icc-edid.c [-DPS5_ICC_LINUX_NOTIFY]
 *   ./a.out <256-byte EDID>
 * Mock mailbox: region1 0x14 = inbound pending (bit0), region1 0x04 doorbell (1 send, 2 ack),
 * region0 0x800.. inbound message, 0x7f0/0x7f4 outbound handshake, 0x06 last id. */
#include "icc-power.h"
#include "icc-edid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t inbound[8][0x900];
static unsigned inbound_len[8], queued, head_index;
static uint8_t outbound[0x800];
static int reply_on_send;          /* queue a valid reply for every request */
static int endless;                /* the same notification stays pending forever */
static unsigned body_reads;
static uint64_t now;

static uint16_t sum(const uint8_t *p, unsigned n) {
    uint16_t s = 0;
    for (unsigned i = 0; i < n; i++) if (i != 10 && i != 11) s = (uint16_t)(s + p[i]);
    return s;
}
static void queue(const uint8_t *msg, unsigned wire_len, unsigned stored) {
    memcpy(inbound[queued], msg, stored);
    inbound_len[queued] = wire_len;
    queued++;
}
static void make(uint8_t *m, uint8_t service, uint16_t type, uint16_t id, const uint8_t *payload,
                 unsigned payload_bytes, unsigned wire_len, int good_checksum, uint8_t magic) {
    memset(m, 0, 0x900);
    m[0] = magic; m[1] = service; m[2] = (uint8_t)type; m[3] = (uint8_t)(type >> 8);
    m[4] = 3; m[6] = (uint8_t)id; m[7] = (uint8_t)(id >> 8);
    m[8] = (uint8_t)wire_len; m[9] = (uint8_t)(wire_len >> 8);
    memcpy(m + 12, payload, payload_bytes);
    unsigned n = wire_len > 0x7f0 ? 0x7f0 : wire_len;
    uint16_t s = sum(m, n);
    if (!good_checksum) s ^= 0x5a5a;
    m[10] = (uint8_t)s; m[11] = (uint8_t)(s >> 8);
}
static uint32_t rd(void *c, unsigned region, unsigned off, unsigned width) {
    (void)c; (void)width;
    if (region == 1 && off == 0x14) return (endless || head_index < queued) ? 1 : 0;
    if (region == 0 && off == 0x808) return (endless || head_index < queued) ? inbound_len[endless ? 0 : head_index] : 0;
    if (region == 0 && off >= 0x800 && off < 0xff0) { body_reads++; return inbound[endless ? 0 : head_index][off - 0x800]; }
    if (region == 0 && off == 0x7f4) return 1;
    if (region == 0 && off == 0x7f0) return 0;
    if (region == 0 && off == 6) return 0x40;
    return 0;
}
static void wr(void *c, unsigned region, unsigned off, unsigned width, uint32_t v) {
    (void)c; (void)width;
    if (region == 0 && off < 0x7f0) outbound[off] = (uint8_t)v;
    if (region == 1 && off == 4 && v == 2 && !endless && head_index < queued) head_index++;      /* ack inbound */
    if (region == 1 && off == 4 && v == 1 && reply_on_send) {                         /* request sent */
        uint8_t r[0x900];
        uint16_t type = (uint16_t)(outbound[2] | outbound[3] << 8) | 0x4000;
        uint16_t id = (uint16_t)(outbound[6] | outbound[7] << 8);
        make(r, outbound[1], type, id, (const uint8_t *)"\0\0\0\0", 4, 32, 1, 0x42);
        queue(r, 32, 32);
    }
}
static uint64_t ticks(void *c) { (void)c; return now += 10; }
static void relax(void *c) { (void)c; }
static const struct ps5_icc_io io = {0, rd, wr, ticks, relax, 100000};

static unsigned notes, last_bytes; static uint8_t last_service;
static void note(void *ctx, uint8_t service, uint16_t type, const uint8_t *p, unsigned bytes) {
    notes++; last_service = service; last_bytes = bytes;
    ps5_edid_notification(ctx, service, type, p, bytes);
}
static void reset(void) { queued = head_index = 0; notes = 0; reply_on_send = 0; endless = 0; body_reads = 0; }

int main(int argc, char **argv) {
    uint8_t edid[256], payload[4 + 256], m[0x900];
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : 0;
    assert(f && fread(edid, 1, 256, f) == 256); fclose(f);
    payload[0] = 0; payload[1] = 2; payload[2] = 0; payload[3] = 1;     /* HDMI event 2, size 256 */
    memcpy(payload + 4, edid, 256);
    struct ps5_edid_state st;

    /* A: EDID notification with a wrong checksum (Linux never checks notification checksums). */
    reset(); memset(&st, 0, sizeof st);
    make(m, 0x10, 0x8000, 0, payload, sizeof payload, 12 + sizeof payload, 0, 0x42);
    queue(m, 12 + sizeof payload, 12 + sizeof payload);
    int r = ps5_icc_poll(&io, note, &st);
#ifdef PS5_ICC_LINUX_NOTIFY
    assert(r == 1 && notes == 1 && last_service == 0x10 && st.bytes == 256 && st.updates == 1);
    assert(ps5_icc_tolerated.count == 1 && ps5_icc_tolerated.wire_length == 12 + sizeof payload);
    assert(ps5_icc_tolerated.checksum != ps5_icc_tolerated.expected && ps5_icc_tolerated.head[1] == 0x10);
#else
    assert(r == -4 && notes == 0 && st.bytes == 0);
#endif
    printf("A bad-checksum EDID notification: poll=%d edid bytes=%u\n", r, st.bytes);

    /* B (edid3): a length field beyond the 0x7f0 window is acknowledged and dropped WITHOUT reading
     * the body (edid2 clamped and read 2 KB of MMIO per such message). */
    reset(); memset(&st, 0, sizeof st);
    make(m, 0x10, 0x8000, 0, payload, sizeof payload, 0x810, 1, 0x42);
    queue(m, 0x810, 0x7f0);
    r = ps5_icc_poll(&io, note, &st);
    assert(body_reads == 0 && head_index == 1 && st.bytes == 0);
#ifdef PS5_ICC_LINUX_NOTIFY
    assert(r == 0 && ps5_icc_dropped.count == 1 && ps5_icc_dropped.wire_length == 0x810);
#else
    assert(r == -4);
#endif
    printf("B oversized length: poll=%d body-reads=%u\n", r, body_reads);

    /* C: garbage that is not a notification is consumed and dropped, never fatal. */
    reset();
    make(m, 0x10, 0x4001, 7, payload, 4, 32, 1, 0x00);
    queue(m, 32, 32);
    r = ps5_icc_poll(&io, note, &st);
#ifdef PS5_ICC_LINUX_NOTIFY
    assert(r == 0 && notes == 0 && ps5_icc_dropped.count == 2 && head_index == 1);
#else
    assert(r == -4 && head_index == 1);
#endif
    printf("C bad non-notification: poll=%d (consumed=%u)\n", r, head_index);

    /* D: a query survives a bad-checksum EDID notification that arrives before its reply. */
    reset(); memset(&st, 0, sizeof st);
    make(m, 0x10, 0x8000, 0, payload, sizeof payload, 12 + sizeof payload, 0, 0x42);
    queue(m, 12 + sizeof payload, 12 + sizeof payload);
    reply_on_send = 1;
    r = ps5_icc_query_events(&io, 0x10, 0x05, (const uint8_t *)"\1", 1, 0, 0, 0, note, &st);
#ifdef PS5_ICC_LINUX_NOTIFY
    assert(r == 0 && st.bytes == 256);
#else
    assert(r == -4 && st.bytes == 0);
#endif
    printf("D query with interleaved bad notification: query=%d edid bytes=%u\n", r, st.bytes);

    /* E: valid traffic is identical in both modes. */
    reset(); memset(&st, 0, sizeof st);
    make(m, 0x10, 0x8000, 0, payload, sizeof payload, 12 + sizeof payload, 1, 0x42);
    queue(m, 12 + sizeof payload, 12 + sizeof payload);
    make(m, 0x09, 0x4020, 3, payload, 4, 32, 1, 0x42);
    queue(m, 32, 32);
    assert(ps5_icc_poll(&io, note, &st) == 1 && st.bytes == 256);
    assert(ps5_icc_poll(&io, note, &st) == 2 && ps5_icc_poll(&io, note, &st) == 0);
    reset(); reply_on_send = 1;
    assert(ps5_icc_query_events(&io, 0x09, 0x20, (const uint8_t *)"\1", 1, 0, 0, 0, note, &st) == 0);
    printf("E valid traffic unchanged: ok\n");

    /* F (edid3): an endless notification stream (the same bad-checksum notification stays pending and
     * no reply ever comes) cannot keep a query busy: -7 after PS5_ICC_MAX_INBOUND_PER_QUERY messages,
     * long before the 12 s deadline; each poll consumes exactly one message and returns. */
    reset(); memset(&st, 0, sizeof st);
    make(m, 0x10, 0x8000, 0, payload, sizeof payload, 12 + sizeof payload, 0, 0x42);
    queue(m, 12 + sizeof payload, 12 + sizeof payload);
    endless = 1;
    struct ps5_icc_io slow = io; slow.timeout_ticks = 1ull << 40;      /* deadline never reached */
    uint64_t t0 = now;
    r = ps5_icc_query_events(&slow, 0x10, 0x05, (const uint8_t *)"\1", 1, 0, 0, 0, note, &st);
#ifdef PS5_ICC_LINUX_NOTIFY
    assert(r == -7 && notes == PS5_ICC_MAX_INBOUND_PER_QUERY);
#else
    assert(r == -4 && notes == 0);
#endif
    unsigned query_notes = notes;
    for (unsigned i = 0; i < 1000; i++) assert(ps5_icc_poll(&io, note, &st) == (1
#ifndef PS5_ICC_LINUX_NOTIFY
        - 5
#endif
        ));
    printf("F endless stream: query=%d after %u notifications, %llu ticks; 1000 polls returned\n", r, query_notes,
           (unsigned long long)(now - t0));
#ifdef PS5_ICC_LINUX_NOTIFY
    puts("ICC LINUX-NOTIFY TESTS PASSED");
#else
    puts("ICC STRICT TESTS PASSED");
#endif
    return 0;
}
