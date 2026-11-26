#!/usr/bin/env python3
"""Host tests for VERDE HDMI bridge command packets (PS5 Slim / late revisions)."""
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "verde-video.h"

static uint8_t last_body[0x800];
static unsigned last_len;

int ps5_icc_query_events(const struct ps5_icc_io *io, uint8_t svc, uint16_t type,
                         const uint8_t *req, unsigned req_bytes,
                         uint8_t *reply, unsigned reply_cap, unsigned *reply_bytes,
                         ps5_icc_notification cb, void *ctx) {
    (void)io; (void)cb; (void)ctx;
    assert(svc == 0x10 && type == 0);
    assert(req && req_bytes > 0 && req_bytes <= sizeof(last_body));
    memcpy(last_body, req, req_bytes);
    last_len = req_bytes;
    if (reply && reply_cap >= 2) { reply[0] = 0; reply[1] = 0; }
    if (reply_bytes) *reply_bytes = 2;
    return 0;
}

static int find_bytes(const uint8_t *hay, unsigned hlen, const uint8_t *ndl, unsigned nlen) {
    if (nlen > hlen) return 0;
    for (unsigned i = 0; i <= hlen - nlen; i++) {
        if (memcmp(hay + i, ndl, nlen) == 0) return 1;
    }
    return 0;
}

int main(void) {
    struct ps5_hdmi_config cfg = {0};
    cfg.converter = 3;
    struct ps5_hdmi_commands p;
    struct ps5_icc_io io = {0};

    /* Reject FLAVA3 config and out-of-range stage. */
    struct ps5_hdmi_config f3 = {0};
    f3.converter = 2;
    assert(ps5_verde_video_stage_mode(&f3, 0, NULL, &p) == -1);
    assert(ps5_verde_video_stage_mode(&cfg, 15, NULL, &p) == -1);

    /* EDID stage: code 4, event 5/4 = 1. */
    assert(ps5_verde_edid_stage(&p) == 0);
    assert(p.data[0] == 4);
    assert(ps5_hdmi_commands_submit(&io, &p, NULL, NULL) == 0);
    {
        const uint8_t ev[] = {5, 5, 4, 1, 1};
        assert(find_bytes(last_body, last_len, ev, sizeof(ev)));
    }

    /* No-op stages return 1. */
    const unsigned noops[] = {4, 5, 8, 9, 10, 14};
    for (unsigned i = 0; i < sizeof(noops)/sizeof(noops[0]); i++)
        assert(ps5_verde_video_stage_mode(&cfg, noops[i], NULL, &p) == 1);

    /* Active stages 0,1,2,3,6,7,11,12,13 build and pass submit validation. */
    const unsigned active[] = {6, 7, 0, 1, 2, 3, 11, 12, 13};
    for (unsigned i = 0; i < sizeof(active)/sizeof(active[0]); i++) {
        assert(ps5_verde_video_stage_mode(&cfg, active[i], NULL, &p) == 0);
        assert(ps5_hdmi_commands_submit(&io, &p, NULL, NULL) == 0);
    }

    /* Stage 2 includes i2c_cmd_3_5(0x4001, 0x03). */
    assert(ps5_verde_video_stage_mode(&cfg, 2, NULL, &p) == 0);
    {
        const uint8_t cmd35[] = {3, 8, 5, 1, 0, 0x40, 0x01, 0x03};
        assert(find_bytes(p.data, p.used, cmd35, sizeof(cmd35)));
    }

    /* Stage 3 AVI for VIC16 (NULL mode). */
    assert(ps5_verde_video_stage_mode(&cfg, 3, NULL, &p) == 0);
    {
        const uint8_t avi16[] = {0x02, 0x0d, 0xef, 0x00, 0x28, 0x08, 0x10, 0x40};
        assert(find_bytes(p.data, p.used, avi16, sizeof(avi16)));
    }

    /* Stage 0 + 3 for 1440p stock: 0x0400=0x03 in stage 0, VIC0 AVI (checksum 0xff) in stage 3. */
    struct ps5_flava3_mode m1440 = {0};
    m1440.reg7227[0] = 0x04;
    m1440.reg7070[5] = 0xfb;
    assert(ps5_verde_video_stage_mode(&cfg, 0, &m1440, &p) == 0);
    {
        const uint8_t r400[] = {2, 8, 1, 1, 1, 0x04, 0x00, 0x03};
        assert(find_bytes(p.data, p.used, r400, sizeof(r400)));
    }
    assert(ps5_verde_video_stage_mode(&cfg, 3, &m1440, &p) == 0);
    {
        const uint8_t avi1440[] = {0x02, 0x0d, 0xff, 0x00, 0x28, 0x08, 0x00, 0x40};
        assert(find_bytes(p.data, p.used, avi1440, sizeof(avi1440)));
    }

    /* Stage 3 AVI for 4K60 (VIC97, checksum 0x9e). */
    struct ps5_flava3_mode m4k = {0};
    m4k.reg7227[0] = 0x06;
    m4k.vic97_mask = 1;
    assert(ps5_verde_video_stage_mode(&cfg, 3, &m4k, &p) == 0);
    {
        const uint8_t avi97[] = {0x02, 0x0d, 0x9e, 0x00, 0x28, 0x08, 0x61, 0x40};
        assert(find_bytes(p.data, p.used, avi97, sizeof(avi97)));
    }

    /* Stage 13 audio unmute uses event 5/5 = 0. */
    assert(ps5_verde_video_stage_mode(&cfg, 13, NULL, &p) == 0);
    {
        const uint8_t ev55[] = {5, 5, 5, 1, 0};
        assert(find_bytes(p.data, p.used, ev55, sizeof(ev55)));
    }

    puts("VERDE_OK");
    return 0;
}
"""


class VerdeVideoTest(unittest.TestCase):
    def test_verde_packets(self):
        with tempfile.TemporaryDirectory() as td:
            src = pathlib.Path(td) / "harness.c"
            exe = pathlib.Path(td) / "harness"
            src.write_text(HARNESS)
            subprocess.run(
                [
                    "gcc", "-Wall", "-Wextra", "-Werror",
                    f"-I{HERE}",
                    str(src),
                    str(HERE / "verde-video.c"),
                    str(HERE / "hdmi-commands.c"),
                    "-o", str(exe),
                ],
                check=True,
            )
            out = subprocess.check_output([str(exe)], text=True)
            self.assertIn("VERDE_OK", out)


if __name__ == "__main__":
    unittest.main()
