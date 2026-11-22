/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * VERDE HDMI bridge register sequences from PS5 Linux drivers/ps5/hdmi.c. Build-only packets. */
#include "verde-video.h"

static void verde_build_avi(uint8_t avi[18], uint8_t vic) {
    for (unsigned i = 0; i < 18; i++) avi[i] = 0;
    avi[0] = 0x02;
    avi[1] = 0x0d; /* HDMI_AVI_INFOFRAME_SIZE */
    avi[3] = 0x00; /* RGB */
    avi[4] = 0x28; /* 16:9, Rec.709 */
    avi[5] = 0x08; /* Full range RGB */
    avi[6] = vic;
    avi[7] = 0x40; /* IT content / full range */
    uint8_t sum = 0x82; /* HDMI_INFOFRAME_TYPE_AVI */
    for (unsigned i = 0; i < 16; i++) sum = (uint8_t)(sum + avi[i]);
    avi[2] = (uint8_t)(0u - sum);
}

static uint8_t verde_vic_for(const struct ps5_flava3_mode *mode) {
    if (!mode) return 16;
    if (mode->vic97_mask) return 97;
    if (mode->reg7070[0]) return mode->reg7070[0];
    return 0;
}

static int verde_is_1440_stock(const struct ps5_flava3_mode *mode) {
    return mode && mode->reg7227[0] == 0x04 && mode->reg7070[5] == 0xfb;
}

int ps5_verde_edid_stage(struct ps5_hdmi_commands *out) {
    if (!out) return -1;
    struct ps5_hdmi_commands p = {0};
    if (ps5_hdmi_commands_init(&p, 4)) return -1;
    if (ps5_hdmi_commands_event(&p, 4, 1)) return -1; /* sceControlHdmiEvent(1) */
    *out = p;
    return 0;
}

int ps5_verde_video_stage_mode(const struct ps5_hdmi_config *c, unsigned stage,
                               const struct ps5_flava3_mode *mode, struct ps5_hdmi_commands *out) {
    if (!c || !out || c->converter != 3 || stage > 14) return -1;
    if (stage == 4 || stage == 5 || stage == 8 || stage == 9 || stage == 10 || stage == 14)
        return 1; /* no-op on VERDE */

    struct ps5_hdmi_commands p = {0};
    unsigned code = (stage == 11 || stage == 13) ? 4 : (stage == 12 ? 2 : 0);
    if (ps5_hdmi_commands_init(&p, code)) return -1;

    struct ps5_hdmi_register_write writes[16];
    uint8_t values[16][28];
    unsigned count = 0;
    int grouping = 0;

#define BEGIN() do { grouping = 1; count = 0; } while (0)
#define END() do { if (count && ps5_hdmi_commands_write_block(&p, writes, count)) return -1; grouping = 0; count = 0; } while (0)
#define W(address, ...) do { const uint8_t d[] = {__VA_ARGS__}; \
    if (grouping) { if (count >= 16 || sizeof(d) > 28) return -1; \
      for (unsigned n = 0; n < sizeof(d); n++) values[count][n] = d[n]; \
      writes[count].reg = address; writes[count].data = values[count]; writes[count].bytes = sizeof(d); count++; \
    } else if (ps5_hdmi_commands_write(&p, address, d, sizeof(d))) return -1; } while (0)
#define WA(address, arr, n) do { \
    if (grouping) { if (count >= 16 || (n) > 28) return -1; \
      for (unsigned k = 0; k < (n); k++) values[count][k] = (arr)[k]; \
      writes[count].reg = address; writes[count].data = values[count]; writes[count].bytes = (n); count++; \
    } else if (ps5_hdmi_commands_write(&p, address, arr, n)) return -1; } while (0)
#define WS(address, val) do { if (ps5_hdmi_commands_write_single(&p, address, val)) return -1; } while (0)
#define M(a, v, m) do { if (ps5_hdmi_commands_mask(&p, a, v, m)) return -1; } while (0)
#define D(ms) do { if (ps5_hdmi_commands_delay(&p, ms)) return -1; } while (0)
#define WAIT(a, m, set) do { if (ps5_hdmi_commands_wait(&p, a, m, set)) return -1; } while (0)
#define WAITEQ(a, v) do { if (ps5_hdmi_commands_wait_eq(&p, a, v)) return -1; } while (0)
#define EV(minor, val) do { if (ps5_hdmi_commands_event(&p, minor, val)) return -1; } while (0)

    if (stage == 6) {
        /* Explicit HDCP off at next output start (verde_send_hdcp_mode(1)). */
        EV(1, 1);
    }
    if (stage == 7) {
        /* initVerde() */
        M(0x105, 0x00, 0x03);
        M(0x104, 0x00, 0x02);
        WS(0x400, 0x00);
        WS(0x3261, 0x02);
        BEGIN();
        W(0x11, 0x03);
        W(0x13, 0xff);
        W(0x8000, 0x00, 0x01, 0x07, 0x00);
        W(0x108, 0x80);
        W(0x9000, 0x01);
        END();
        EV(3, 0);
    }
    if (stage == 0) {
        /* linkConfigVerde() */
        M(0x105, 0x00, 0x40);
        if (verde_is_1440_stock(mode)) WS(0x400, 0x03);
        BEGIN();
        W(0x9003, 0x1e, 0x04, 0x01, 0x01, 0x00, 0x00, 0x00);
        W(0x4002, 0x03);
        END();
    }
    if (stage == 1) {
        /* configDpLinkTrainingVerde(false) */
        WS(0x9002, 0x01);
        WAIT(0x4001, 0x02, 1);
        WS(0x11, 0x01);
    }
    if (stage == 2) {
        /* setTmdsConfigVerde(false) + queueVerdeAudio() */
        WS(0x9200, 0x00);
        BEGIN();
        W(0x9800, 0x00);
        W(0x33b3, 0x1a);
        W(0x9831, 0x80);
        W(0x9810, 0x00, 0x79, 0x00, 0x02, 0x0b, 0x00, 0x00);
        END();
        WAIT(0x9830, 0x80, 0);
        WS(0x9830, 0x80);
        WAIT(0x9830, 0x80, 0);
        WS(0x9830, 0x81);
        WAIT(0x9830, 0x80, 0);
        WAIT(0x108, 0x10, 0);
        M(0x108, 0x11, 0x17);
        WAIT(0x13, 0x40, 1);
        WS(0x13, 0x40);
        WAITEQ(0x4001, 0x03);
        WS(0x11, 0x01);
    }
    if (stage == 3) {
        /* setHdmiBasicVideoConfigVerde(mode, 8, RGB, false, false) */
        static const uint8_t spd[28] = {
            0x01, 0x19, 0x5f, 0x53, 0x43, 0x45, 0x49, 0x00, 0x00, 0x00, 0x00, 0x50, 0x53, 0x35,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08
        };
        static const uint8_t depth[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const uint8_t packets[10] = {0x56, 0xaa, 0x00, 0x24, 0x00, 0xa8, 0x00, 0x00, 0x08, 0x80};
        uint8_t avi[18];
        verde_build_avi(avi, verde_vic_for(mode));
        BEGIN();
        WA(0x3058, avi, 18);
        WA(0x31d0, spd, 28);
        END();
        BEGIN();
        WA(0x3033, depth, 9);
        W(0x307a, 0x01, 0x0a, 0x70, 0x01, 0x00, 0x00, 0x00, 0x00);
        WA(0x3000, packets, 10);
        END();
        WS(0x300a, 0x01);
        WAIT(0x300c, 0xf3, 0);
        WAIT(0x300d, 0xff, 0);
        WAIT(0x300e, 0xff, 0);
        WAIT(0x300f, 0xff, 0);
        WAIT(0x3010, 0xff, 0);
        WAIT(0x3011, 0xff, 0);
        WAIT(0x13, 0x80, 1);
        WS(0x13, 0x80);
        M(0x105, 0x01, 0x03);
        D(700);
        EV(3, 1);
    }
    if (stage == 11) {
        /* sceHdmiDeviceSetVideoMute(0) */
        M(0x105, 0x01, 0x03);
    }
    if (stage == 12) {
        /* setAudioConfigForVerde(2) */
        WAIT(0x102, 0x80, 0);
        WS(0xf1, 0x01);
        WAIT(0xf2, 0x01, 1);
        WAIT(0x16, 0x08, 1);
        WS(0x16, 0x08);
        BEGIN();
        W(0x9800, 0x00);
        W(0x33b3, 0x1a);
        W(0x9831, 0x80);
        W(0x9810, 0x00, 0x79, 0x00, 0x02, 0x0b, 0x00, 0x00);
        END();
        WAIT(0x9830, 0x80, 0);
        WS(0x9830, 0x80);
        WAIT(0x9830, 0x80, 0);
        WS(0x9830, 0x81);
        WAIT(0x9830, 0x80, 0);
        WAIT(0x108, 0x10, 0);
        M(0x108, 0x10, 0x10);
        WAIT(0x108, 0x10, 0);
        W(0x307a, 0x01, 0x0a, 0x70, 0x01, 0x00, 0x00, 0x00, 0x00);
        M(0x3000, 0x40, 0xc0);
        M(0x3005, 0x80, 0xc0);
        WS(0x300a, 0x01);
        WAIT(0x13, 0x80, 1);
        WS(0x13, 0x80);
    }
    if (stage == 13) {
        /* sceHdmiSetAudioMute(0) */
        EV(5, 0);
    }

    *out = p;
    return 0;
#undef EV
#undef WAITEQ
#undef WAIT
#undef D
#undef M
#undef WS
#undef WA
#undef W
#undef END
#undef BEGIN
}
