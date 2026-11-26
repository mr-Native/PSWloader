/*
 * ps5gbe v1.0.10 - NDIS 6.20 miniport for the PS5 Gigabit Ethernet MAC (Sony 104D:9104,
 * v1.0.10 (07.11.2026): back to the proven 1.0.8 data path (no 1.1.x TX watchdog / per-poll re-kick). With the 1.1.x
 *   binaries active (fast44-50) Windows repeatedly froze with the Windows-To-Go 'USB-Laufwerk angeschlossen lassen' notice
 *   (German Windows text of "keep the USB drive connected"; boot USB drive lost) at shutdown or after a power-scheme switch; fast1-43 with 1.0.8 never did. Only change vs 1.0.8:
 *   the framebuffer status boxes are drawn only with DiagMarks=1 and never after initialize (no desktop boxes).
 * "Salina GBE", a MediaTek Star EMAC derivative) on native Windows 11.
 *
 * Hardware programming model follows the Linux "mts" driver (GPL-2.0-only,
 * Armandas Kvietkus) in drivers/net/phy/mts of the PS5 Linux kernel; this file is a
 * re-implementation for NDIS and is distributed under GPL-2.0-only as well.
 *
 * Design (v1.0): polling only. The chip has no INTx pin and its MSI is known to stay
 * silent on some boots, so a periodic NDIS timer services TX completion, RX and link.
 * TX frames are copied into one physically contiguous 32-bit bounce region (the chip
 * wants contiguous buffers); RX frames are indicated straight from the slotted pool
 * with NDIS_RECEIVE_FLAGS_RESOURCES, so every slot is re-armed right after the
 * indication. PCS/SerDes, MAC address and chip revision live in BAR4 of the Salina
 * glue function 104D:9107, which is found through ECAM (no driver owns it on Windows).
 */
#include <ndis.h>

#define PS5GBE_TAG 'ebg5'
#define PS5GBE_VERSION 0x00010000

#define SONY_VID 0x104D
#define GBE_DID 0x9104
#define GLUE_DID 0x9107
#define ECAM_BASE 0xF0000000ULL       /* MCFG of the native ACPI tables, bus 0..0x7f */

#define GBE_REG_SMI 0x00
#define GBE_REG_LINK 0x04
#define GBE_REG_CLK_CTRL 0x08
#define GBE_REG_CTRL 0x0c
#define GBE_REG_MODE 0x10
#define GBE_REG_MAC_HI 0x14
#define GBE_REG_MAC_LO 0x18
#define GBE_REG_RXBUF 0x30
#define GBE_REG_TX_DMA_CTRL 0x34
#define GBE_REG_RX_DMA_CTRL 0x38
#define GBE_REG_TX_RING_CUR 0x3c
#define GBE_REG_RX_RING_CUR 0x40
#define GBE_REG_TX_RING_BASE 0x44
#define GBE_REG_RX_RING_BASE 0x48
#define GBE_REG_ISR 0x50
#define GBE_REG_IER 0x54
#define GBE_REG_SERDES 0x74
#define GBE_REG_RX_PCODE 0x78
#define GBE_REG_TXKICK 0x9c
#define GBE_REG_RESET 0xac
#define GBE_REG_COAL 0x204
#define GBE_PCS_BASE 0x4000
#define GBE_CLK_ENABLE 0x7597c00
#define GBE_SERDES_1G 0x303277
#define GBE_COAL_VAL 0x10001388
#define GBE_DMA_STOP 0x2
#define GBE_DMA_KICK 0x4
#define GBE_ISR_LSO_RECOVER 0x500000

#define DESC_DONE 0x80000000u
#define DESC_WRAP 0x40000000u
#define DESC_SOP 0x20000000u
#define DESC_EOP 0x10000000u
#define DESC_LEN_MASK 0xffffu
#define DESC_RX_LEN 0x600u
#define DESC_RX_LEN_MASK 0x7ffu
#define DESC_W2_ARM 0xffff0000u
#define DESC_W2_TX 0x4u

#define RING_SIZE 256
#define RING_BYTES (RING_SIZE * 16)
#define TX_SCRATCH_SIZE 0xa0000
#define RX_POOL_SIZE (RING_SIZE * DESC_RX_LEN)
#define MAX_FRAME 1514
#define POLL_MS 2

typedef struct _GBE_DESC { ULONG flags, buf, w2, w3; } GBE_DESC;

typedef struct _ADAPTER {
    NDIS_HANDLE handle;
    NDIS_HANDLE dma;
    NDIS_HANDLE nbl_pool;
    NDIS_HANDLE timer;
    NDIS_SPIN_LOCK lock;
    volatile UCHAR *base;          /* GBE BAR0 */
    ULONG base_len;
    volatile UCHAR *glue;          /* glue BAR4 ("pervasive0" in Linux spcie.c) */
    ULONG glue_len;
    volatile UCHAR *glue2;         /* glue BAR2 */
    ULONG glue2_len;
    ULONG chip;                    /* (rev & 0xff000000 -> 0x110000/0x120000) | rev & 0xffff */
    GBE_DESC *tx_ring, *rx_ring;
    NDIS_PHYSICAL_ADDRESS tx_ring_pa, rx_ring_pa;
    UCHAR *tx_scratch, *rx_pool;
    NDIS_PHYSICAL_ADDRESS tx_scratch_pa, rx_pool_pa;
    ULONG tx_head, tx_tail, tx_off, rx_head;
    PMDL rx_mdl[RING_SIZE];
    PNET_BUFFER_LIST rx_nbl[RING_SIZE];
    UCHAR perm_mac[6], cur_mac[6];
    ULONG packet_filter, lookahead;
    LONG paused, halting;
    ULONG link_up, link_tick;
    ULONG64 tx_ok, rx_ok, tx_bytes, rx_bytes, rx_err, tx_drop;
} ADAPTER, *PADAPTER;

static NDIS_HANDLE g_driver;

static NDIS_OID g_oids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_XMIT_OK, OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER, OID_GEN_STATISTICS,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS, OID_PNP_CAPABILITIES, OID_PNP_SET_POWER, OID_PNP_QUERY_POWER,
};

/* ---------------------------------------------------------------- register access */
static __forceinline ULONG rd(PADAPTER a, ULONG r) { return READ_REGISTER_ULONG((volatile ULONG *)(a->base + r)); }
static __forceinline void wr(PADAPTER a, ULONG r, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)(a->base + r), v); }
static __forceinline void setb(PADAPTER a, ULONG r, ULONG b) { wr(a, r, rd(a, r) | b); }
static __forceinline void clrb(PADAPTER a, ULONG r, ULONG b) { wr(a, r, rd(a, r) & ~b); }
static __forceinline ULONG pcs(PADAPTER a, ULONG off) { return READ_REGISTER_ULONG((volatile ULONG *)(a->glue + GBE_PCS_BASE + off)); }

/* ---------------------------------------------------------------- v1.0.4 step painter
 * The GOP/Basic Display scanout is linear BGRX 1920x1080 at 0x46C000000 (pitch 1920 px).
 * Each init step paints a 28x28 box at y=1000, x=8+32*step (green = reached, red = failed)
 * so a hard hang leaves the last reached step visible on the HDMI capture. */
#define FB_PHYS 0x46C000000ULL
#define FB_W 1920
static volatile ULONG *g_fb;
static void fb_map(void)
{
    PHYSICAL_ADDRESS pa; pa.QuadPart = (LONGLONG)(FB_PHYS + 990ULL * FB_W * 4);
    if (!g_fb) g_fb = MmMapIoSpaceEx(pa, 50 * FB_W * 4, PAGE_READWRITE | PAGE_WRITECOMBINE);
}
static void fb_unmap(void)
{
    volatile ULONG *fb = g_fb;
    g_fb = NULL;
    if (fb) MmUnmapIoSpace((PVOID)fb, 50 * FB_W * 4);
}
static void mark(ULONG step, ULONG color)
{
    if (!g_fb || step > 58) return;
    for (ULONG y = 10; y < 38; y++)
        for (ULONG x = 0; x < 28; x++)
            g_fb[y * FB_W + 8 + step * 32 + x] = color;
    KeMemoryBarrier();
}
#define OK(s) mark((s), 0x0000d000)
#define BAD(s) mark((s), 0x00e00000)

/* ---------------------------------------------------------------- SMI / PHY (mts_phy.c) */
static int smi_wait(PADAPTER a)
{
    for (int i = 0; i < 10000; i++) {
        if (rd(a, GBE_REG_SMI) & 0x8000) return 0;
        KeStallExecutionProcessor(1);
    }
    return -1;
}
static USHORT cl22_rd(PADAPTER a, UCHAR reg)
{
    wr(a, GBE_REG_SMI, 0x8000);
    wr(a, GBE_REG_SMI, ((reg & 0x1f) << 8) | 0x4000);
    if (smi_wait(a)) return 0;
    return (USHORT)(rd(a, GBE_REG_SMI) >> 16);
}
static void cl22_wr(PADAPTER a, UCHAR reg, USHORT v)
{
    wr(a, GBE_REG_SMI, 0x8000);
    wr(a, GBE_REG_SMI, ((ULONG)v << 16) | ((reg & 0x1f) << 8) | 0x2000);
    smi_wait(a);
}
static int cl45_addr(PADAPTER a, ULONG sel)
{
    wr(a, GBE_REG_SMI, 0x8000);
    wr(a, GBE_REG_SMI, (sel & 0xffff0000) | ((sel & 0x1f) << 8) | 0x20);
    return smi_wait(a);
}
static void cl45_wr(PADAPTER a, ULONG sel, USHORT v)
{
    if (cl45_addr(a, sel)) return;
    wr(a, GBE_REG_SMI, 0x8000);
    wr(a, GBE_REG_SMI, ((ULONG)v << 16) | ((sel & 0x1f) << 8) | 0x60);
    smi_wait(a);
}
static USHORT cl45_rd(PADAPTER a, ULONG sel)
{
    if (cl45_addr(a, sel)) return 0;
    wr(a, GBE_REG_SMI, 0x8000);
    wr(a, GBE_REG_SMI, ((sel & 0x1f) << 8) | 0xe0);
    if (smi_wait(a)) return 0;
    return (USHORT)(rd(a, GBE_REG_SMI) >> 16);
}
static const UCHAR cal_lut[64] = {
    0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,
    0x7b,0x7a,0x75,0x73,0x70,0x67,0x64,0x62,0x57,0x55,0x53,0x51,0x48,0x46,0x44,0x42,
    0x40,0x37,0x35,0x34,0x32,0x31,0x30,0x26,0x24,0x23,0x22,0x21,0x20,0x16,0x15,0x14,
    0x13,0x12,0x11,0x10,0x07,0x06,0x05,0x04,0x03,0x02,0x01,0x00,0x00,0x00,0x00,0x00,
};
static UCHAR cal_idx(ULONG f) { f &= 0x7f; if (f < 2) f = 1; if (f >= 0x40) f = 0x40; return cal_lut[f - 1]; }
static void cal_lane(PADAPTER a, ULONG sel, UCHAR cal, BOOLEAN high)
{
    USHORT cur = cl45_rd(a, sel);
    if (high) cl45_wr(a, sel, (USHORT)((cur & 0x00ff) | (cal << 8) | 0x8000));
    else cl45_wr(a, sel, (USHORT)((cur & 0xff00) | cal | 0x80));
}
static USHORT eq_preset(ULONG f) { ULONG v = f & 0x3f; if (v > 0x3c) v = 0x3c; return (USHORT)((v + 3) & 0xff); }
static void phy_calibrate(PADAPTER a)
{
    ULONG v;
    v = pcs(a, 0x68); cl45_wr(a, 0x000e001e, (USHORT)((v & 0x3f) << 8));
    v = pcs(a, 0x68); cl45_wr(a, 0x0115001f, (USHORT)((v >> 6) & 7));
    v = pcs(a, 0x60); cal_lane(a, 0x0174001e, cal_idx(v), TRUE);
    v = pcs(a, 0x60); cal_lane(a, 0x0174001e, cal_idx(v >> 0x13), FALSE);
    v = pcs(a, 0x64); cal_lane(a, 0x0175001e, cal_idx(v >> 6), TRUE);
    v = pcs(a, 0x64); cal_lane(a, 0x0175001e, cal_idx(v >> 0x19), FALSE);
    v = pcs(a, 0x5c); cl45_wr(a, 0x0172001e, (USHORT)((cl45_rd(a, 0x0172001e) & 0xc0ff) | ((v >> 0xc) & 0x3f00)));
    v = pcs(a, 0x60); cl45_wr(a, 0x0172001e, (USHORT)((cl45_rd(a, 0x0172001e) & 0xffc0) | ((v >> 7) & 0x3f)));
    v = pcs(a, 0x60); cl45_wr(a, 0x0173001e, (USHORT)((cl45_rd(a, 0x0173001e) & 0xc0ff) | ((v >> 0x12) & 0x3f00)));
    v = pcs(a, 0x64); cl45_wr(a, 0x0173001e, (USHORT)((cl45_rd(a, 0x0173001e) & 0xffc0) | ((v >> 0xd) & 0x3f)));
    v = pcs(a, 0x5c) >> 0x1a;
    cl45_wr(a, 0x0012001e, (USHORT)((v << 0xa) | v));
    cl45_wr(a, 0x0016001e, (USHORT)((eq_preset(v) << 0xa) | v));
    v = pcs(a, 0x60) >> 0xd;
    cl45_wr(a, 0x0017001e, (USHORT)(((v & 0x3f) << 8) | (v & 0x3f)));
    cl45_wr(a, 0x0018001e, (USHORT)((eq_preset(v) << 8) | (v & 0x3f)));
    v = pcs(a, 0x64);
    cl45_wr(a, 0x0019001e, (USHORT)(((v & 0x3f) << 8) | (v & 0x3f)));
    cl45_wr(a, 0x0020001e, (USHORT)((eq_preset(v) << 8) | (v & 0x3f)));
    v = pcs(a, 0x64) >> 0x13;
    cl45_wr(a, 0x0021001e, (USHORT)(((v & 0x3f) << 8) | (v & 0x3f)));
    cl45_wr(a, 0x0022001e, (USHORT)((eq_preset(v) << 8) | (v & 0x3f)));
    cl45_wr(a, 0x0096001e, 0x8000);
    cl45_wr(a, 0x0037001e, 0x0033);
    cl45_wr(a, 0x0039001e, cl45_rd(a, 0x0039001e) & 0xb7ff);
    cl45_wr(a, 0x0107001f, cl45_rd(a, 0x0107001f) & 0xefff);
    cl45_wr(a, 0x0171001e, cl45_rd(a, 0x0171001e) | 0x0180);
    cl45_wr(a, 0x0039001e, cl45_rd(a, 0x0039001e) | 0x2000);
    cl45_wr(a, 0x0039001e, cl45_rd(a, 0x0039001e) & 0xdfff);
    KeStallExecutionProcessor(50);
    cl45_wr(a, 0x0171001e, cl45_rd(a, 0x0171001e) & 0xfe7f);
}
static void smi_paged(PADAPTER a, USHORT r11, USHORT r12, USHORT r10)
{
    USHORT page = cl22_rd(a, 0x1f);
    cl22_wr(a, 0x1f, 0x52b5); cl22_wr(a, 0x11, r11); cl22_wr(a, 0x12, r12); cl22_wr(a, 0x10, r10);
    cl22_wr(a, 0x1f, page);
}
static void phy_static_init(PADAPTER a)
{
    ULONG chip = a->chip & 0xff0000, rev = a->chip & 0xffff;
    cl45_wr(a, 0x003e001e, 0xf8f8);
    cl45_wr(a, 0x0189001e, 0x0110);
    smi_paged(a, 0xb90a, 0x006f, 0x8f82);
    smi_paged(a, 0xbaef, 0x002e, 0x968c);
    cl22_wr(a, 0x1f, 0x0003); cl22_wr(a, 0x1c, 0x0c92); cl22_wr(a, 0x1f, 0x0000);
    wr(a, GBE_REG_RX_PCODE + 4, 25000000);
    cl45_wr(a, 0x0122001e, 0xffff);
    cl45_wr(a, 0x0234001e, 0x0180);
    smi_paged(a, 0x2e00, 0x000e, 0x8fb0);
    cl45_wr(a, 0x0238001e, 0x0120);
    if (chip == 0x110000) cl45_wr(a, 0x0120001e, 0x9014);
    cl45_wr(a, 0x0239001e, 0x0117);
    smi_paged(a, 0x0001, 0x0004, 0x96a2);
    clrb(a, GBE_REG_RX_PCODE, 1);
    cl45_wr(a, 0x003c0007, 0x0000);
    cl45_wr(a, 0x0033001e, cl45_rd(a, 0x0033001e) & 0xefff);
    cl45_wr(a, 0x0268001f, 0x07f4);
    smi_paged(a, 0x0004, 0x0000, 0x9686);
    smi_paged(a, 0x0671, 0x0006, 0x8fae);
    smi_paged(a, 0x55a0, 0x0000, 0x83aa);
    cl45_wr(a, 0x0123001e, 0xffff);
    smi_paged(a, 0x8670, 0x0001, 0x96a6);
    smi_paged(a, 0x0072, 0x0000, 0x96b6);
    smi_paged(a, 0x3210, 0x0000, 0x96b8);
    smi_paged(a, 0x024a, 0x0000, 0x96a8);
    if (chip == 0x110000 && rev == 0x0100) {
        smi_paged(a, 0x704d, 0x0000, 0x9698);
        smi_paged(a, 0x314f, 0x0002, 0x969a);
        smi_paged(a, 0x4444, 0x0044, 0x8ecc);
    } else if (chip == 0x110000 && rev == 0x0200) {
        smi_paged(a, 0x5010, 0x0000, 0x96a0);
        smi_paged(a, 0x3028, 0x0000, 0x969e);
        smi_paged(a, 0x504d, 0x0000, 0x9698);
        smi_paged(a, 0x194f, 0x0002, 0x969a);
    } else if (chip == 0x110000) {
        smi_paged(a, 0x5010, 0x0000, 0x96a0);
        smi_paged(a, 0x3028, 0x0000, 0x969e);
        smi_paged(a, 0x504d, 0x0000, 0x9698);
        smi_paged(a, 0x194f, 0x0002, 0x969a);
        cl45_wr(a, 0x014a001e, 0xee20);
        cl45_wr(a, 0x019b001e, 0x0111);
    } else {
        smi_paged(a, 0x5010, 0x0000, 0x96a0);
        smi_paged(a, 0x3028, 0x0000, 0x969e);
        smi_paged(a, 0x504d, 0x0000, 0x9698);
        smi_paged(a, 0x194f, 0x0002, 0x969a);
        cl45_wr(a, 0x014a001e, 0xee20);
        cl45_wr(a, 0x019b001e, 0x0111);
        cl45_wr(a, 0x0144001e, 0x0200);
        cl22_wr(a, 0x1f, 0x0003); cl22_wr(a, 0x1d, 0x03fb); cl22_wr(a, 0x1f, 0x0000);
        cl45_wr(a, 0x0323001e, 0x0011);
        smi_paged(a, 0x0036, 0x0000, 0x8f80);
        cl45_wr(a, 0x0120001e, 0x8014);
        smi_paged(a, 0xff3f, 0x0000, 0x83ae);
        static const USHORT tbl[12] = {0x0186,0x01c5,0x01c8,0x010e,0x0202,0x0207,0x0386,0x03c5,0x03c8,0x030a,0x0005,0x0008};
        for (ULONG i = 0; i < 12; i++) cl45_wr(a, (i << 16) | 0x1e, tbl[i]);
    }
    cl45_wr(a, 0x003e001e, 0x0000);
}
static void phy_init(PADAPTER a)
{
    cl22_rd(a, 2); cl22_rd(a, 3);
    OK(30);
    wr(a, GBE_REG_RESET, 9);
    KeStallExecutionProcessor(1000);
    OK(31);
    if ((~pcs(a, 0x6c) & 0x80800000) == 0) phy_calibrate(a);   /* both SerDes lanes locked */
    OK(32);
    phy_static_init(a);
    OK(33);
    cl22_wr(a, 4, cl22_rd(a, 4) & 0xf3ff);
    wr(a, GBE_REG_LINK, rd(a, GBE_REG_LINK) & 0x7fffcfff);
    cl22_wr(a, 0, 0x0800);                                         /* BMCR_PDOWN */
    cl22_wr(a, 4, (USHORT)((cl22_rd(a, 4) & 0xfe1f) | 0x01e0));
    cl22_wr(a, 9, (USHORT)((cl22_rd(a, 9) & 0xfcff) | 0x0200));
    cl22_wr(a, 0, 0x1340);
    cl22_wr(a, 0, cl22_rd(a, 0) | 0x1000 | 0x0200);                /* ANENABLE | ANRESTART */
}

/* ---------------------------------------------------------------- MAC bring-up (mts_main.c) */
static void write_mac(PADAPTER a, const UCHAR *m)
{
    wr(a, GBE_REG_MAC_HI, (m[0] << 8) | m[1]);
    wr(a, GBE_REG_MAC_LO, ((ULONG)m[2] << 24) | (m[3] << 16) | (m[4] << 8) | m[5]);
}
static void init_tx_ring(PADAPTER a)
{
    for (ULONG i = 0; i < RING_SIZE; i++) {
        a->tx_ring[i].flags = DESC_DONE | (i == RING_SIZE - 1 ? DESC_WRAP : 0);
        a->tx_ring[i].buf = 0;
        a->tx_ring[i].w2 = DESC_W2_ARM;
        a->tx_ring[i].w3 = 0;
    }
    a->tx_head = a->tx_tail = a->tx_off = 0;
}
static void rx_fill(PADAPTER a)
{
    for (ULONG i = 0; i < RING_SIZE; i++) {
        a->rx_ring[i].buf = a->rx_pool_pa.LowPart + i * DESC_RX_LEN;
        a->rx_ring[i].w2 = 0;
        a->rx_ring[i].w3 = 0;
        KeMemoryBarrier();
        a->rx_ring[i].flags = DESC_RX_LEN | (i == RING_SIZE - 1 ? DESC_WRAP : 0);
    }
    a->rx_head = 0;
}
static void init_rings_hw(PADAPTER a)
{
    init_tx_ring(a);
    KeMemoryBarrier();
    wr(a, GBE_REG_TX_RING_BASE, a->tx_ring_pa.LowPart);
    wr(a, GBE_REG_TX_RING_CUR, a->tx_ring_pa.LowPart);
    wr(a, GBE_REG_RX_RING_BASE, a->rx_ring_pa.LowPart);
    wr(a, GBE_REG_RX_RING_CUR, a->rx_ring_pa.LowPart);
    clrb(a, GBE_REG_TXKICK, 1u << 6);
    setb(a, GBE_REG_TXKICK, 1u << 6);
    setb(a, GBE_REG_TX_DMA_CTRL, 1);
    setb(a, GBE_REG_RX_DMA_CTRL, 1);
    wr(a, GBE_REG_IER, 0);           /* v1.0 polls: keep the MSI source quiet */
}
static void stop_dma(PADAPTER a)
{
    wr(a, GBE_REG_IER, 0);
    setb(a, GBE_REG_TX_DMA_CTRL, GBE_DMA_STOP);
    for (int i = 0; i < 100000 && (rd(a, GBE_REG_TX_DMA_CTRL) & GBE_DMA_STOP); i++) KeStallExecutionProcessor(1);
    setb(a, GBE_REG_RX_DMA_CTRL, GBE_DMA_STOP);
    for (int i = 0; i < 100000 && (rd(a, GBE_REG_RX_DMA_CTRL) & GBE_DMA_STOP); i++) KeStallExecutionProcessor(1);
}
/* ---------------------------------------------------------------- v1.0.5 Salina glue init
 * Exact port of spcie_init()/spcie_init_dev() from the PS5 Linux drivers/ps5/spcie.c,
 * which runs before mts may touch the MAC: per Salina IP block stop pending transactions,
 * pulse its reset/enable and release it again. v3.18 hung right after the MAC DMA engines
 * were started without it. Bounded polls instead of Linux's endless loops. */
static __forceinline ULONG g2r(PADAPTER a, ULONG o) { return READ_REGISTER_ULONG((volatile ULONG *)(a->glue2 + o)); }
static __forceinline void g2w(PADAPTER a, ULONG o, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)(a->glue2 + o), v); }
static __forceinline ULONG g4r(PADAPTER a, ULONG o) { return READ_REGISTER_ULONG((volatile ULONG *)(a->glue + o)); }
static __forceinline void g4w(PADAPTER a, ULONG o, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)(a->glue + o), v); }
static void spcie_init_dev(PADAPTER a, int ip)
{
    static const ULONG o0[5] = {0x142020, 0x143820, 0x144820, 0x144020, 0x42420};
    static const ULONG o1[5] = {0x142028, 0x143828, 0x144828, 0x144028, 0x42428};
    static const ULONG o2[5] = {0x180020, 0x18002c, 0x180030, 0x180024, 0x7010};
    ULONG (*br)(PADAPTER, ULONG) = ip == 4 ? g4r : g2r;
    void (*bw)(PADAPTER, ULONG, ULONG) = ip == 4 ? g4w : g2w;
    bw(a, o0[ip], br(a, o0[ip]) | 0x10);
    for (int i = 0; i < 10000; i++) {
        if (!(br(a, o1[ip]) & 0xf0)) break;
        KeStallExecutionProcessor(10);
    }
    bw(a, o0[ip], br(a, o0[ip]) | 0x1);
    if (ip == 4) {
        g4w(a, o2[ip], 1);
        g4w(a, 0x74b4, 3);
    } else {
        g2w(a, o2[ip], 3);
        switch (ip) {
        case 0: g4w(a, 0x74c8, 1); g4w(a, 0x7c28, 0x1f); break;
        case 1: g4w(a, 0x74c4, 3); g4w(a, 0x7c14, 7); break;
        case 2: g4w(a, 0x7498, 3); g4w(a, 0x7c18, 7); break;
        case 3: g4w(a, 0x74cc, (a->chip & 0xff0000) == 0x110000 ? 7 : 1); g4w(a, 0x7c1c, 0xf); break;
        }
    }
    bw(a, o0[ip], br(a, o0[ip]) & ~0x11u);
    if (ip == 4) {
        g4w(a, o2[ip], 0);
    } else if (ip == 0) {
        g2w(a, o2[ip], 0);
        g4w(a, 0x7c28, g4r(a, 0x7c28) & ~8u);
        g4w(a, 0x7c28, g4r(a, 0x7c28) & ~1u);
        g2w(a, o2[ip], 1);
        g2w(a, o2[ip], 0);
        g4w(a, 0x7c28, g4r(a, 0x7c28) | 8);
        g4w(a, 0x7c28, g4r(a, 0x7c28) | 1);
        KeStallExecutionProcessor(680);
    }
}
/* v1.0.8: once per boot only. A live driver update (v3.23, pnputil /install) re-ran the
 * glue init while the Salina fabric was in use and the system froze. */
static LONG g_spcie_done;
static BOOLEAN spcie_init(PADAPTER a)
{
    int i;
    if (g_spcie_done) { OK(16); return TRUE; }
    if (!a->glue || !a->glue2) return FALSE;
    g4w(a, 0x22004, 1);
    for (i = 0; i < 100000 && (g4r(a, 0x22000) & 7) != 4; i++) KeStallExecutionProcessor(10);
    if (i == 100000) return FALSE;
    OK(14);
    spcie_init_dev(a, 0);
    if ((a->chip & 0xff0000) == 0x110000) spcie_init_dev(a, 1);
    spcie_init_dev(a, 2);
    spcie_init_dev(a, 3);
    spcie_init_dev(a, 4);
    OK(15);
    g2w(a, 0x18004c, 1);
    g4w(a, 0x7c08, 1);
    g2w(a, 0x18004c, 0);
    g4w(a, 0x22004, 0);
    for (i = 0; i < 100000 && (g4r(a, 0x22000) & 4); i++) KeStallExecutionProcessor(10);
    OK(16);
    InterlockedExchange(&g_spcie_done, 1);
    return TRUE;
}

static void init_hw(PADAPTER a)
{
    if (!spcie_init(a)) BAD(14);
    OK(20);
    wr(a, GBE_REG_RESET, 9);
    KeStallExecutionProcessor(1000);
    OK(21);
    setb(a, GBE_REG_CLK_CTRL, GBE_CLK_ENABLE);
    OK(22);
    phy_init(a);
    OK(23);
    clrb(a, GBE_REG_CTRL, 1u << 7);
    wr(a, GBE_REG_SERDES, GBE_SERDES_1G);
    wr(a, GBE_REG_MODE, (rd(a, GBE_REG_MODE) & 0xffffff6e) | 0x81);
    wr(a, GBE_REG_RXBUF, 0x10100);
    write_mac(a, a->cur_mac);
    wr(a, GBE_REG_COAL, GBE_COAL_VAL);
}
/* the chip latches an LSO error and refuses TX until the TX ring is rebuilt */
static void tx_rebuild(PADAPTER a)
{
    clrb(a, GBE_REG_TXKICK, 1u << 6);
    setb(a, GBE_REG_TXKICK, 1u << 6);
    init_tx_ring(a);
    KeMemoryBarrier();
    wr(a, GBE_REG_TX_RING_BASE, a->tx_ring_pa.LowPart);
    wr(a, GBE_REG_TX_RING_CUR, a->tx_ring_pa.LowPart);
    setb(a, GBE_REG_TX_DMA_CTRL, 1);
}
static void tx_complete(PADAPTER a)
{
    ULONG t = a->tx_tail;
    while (t != a->tx_head) {
        GBE_DESC *d = &a->tx_ring[t];
        if (!(d->flags & DESC_DONE) || d->w2 >= 0xffff0000u) break;
        d->w2 |= 0xffff0000u;
        t = (t + 1) % RING_SIZE;
    }
    a->tx_tail = t;
}

/* ---------------------------------------------------------------- glue (104D:9107) via ECAM */
static NDIS_STATUS map_glue(PADAPTER a)
{
    for (ULONG bus = 0x40; bus <= 0x40; bus++) {
        PHYSICAL_ADDRESS pa; pa.QuadPart = ECAM_BASE + ((ULONG64)bus << 20);
        OK(10);
        volatile UCHAR *cfg = MmMapIoSpaceEx(pa, 1u << 20, PAGE_READWRITE | PAGE_NOCACHE);
        if (!cfg) { BAD(10); return NDIS_STATUS_RESOURCES; }
        OK(11);
        /* GBE: make sure memory decode + bus mastering are on (pci.sys normally does it) */
        for (ULONG devfn = 0; devfn < 256; devfn++) {
            volatile UCHAR *f = cfg + (devfn << 12);
            if (READ_REGISTER_ULONG((volatile ULONG *)f) != ((GBE_DID << 16) | SONY_VID)) continue;
            USHORT cmd = READ_REGISTER_USHORT((volatile USHORT *)(f + 4));
            if ((cmd & 6) != 6) WRITE_REGISTER_USHORT((volatile USHORT *)(f + 4), (USHORT)(cmd | 6));
        }
        for (ULONG devfn = 0; devfn < 256; devfn++) {
            volatile UCHAR *f = cfg + (devfn << 12);
            ULONG id = READ_REGISTER_ULONG((volatile ULONG *)f);
            if (id != ((GLUE_DID << 16) | SONY_VID)) continue;
            OK(12);
            /* glue has no Windows driver: enable memory decode for its BAR4 (read-mostly use) */
            USHORT gcmd = READ_REGISTER_USHORT((volatile USHORT *)(f + 4));
            if (!(gcmd & 2)) WRITE_REGISTER_USHORT((volatile USHORT *)(f + 4), (USHORT)(gcmd | 2));
            ULONG lo = READ_REGISTER_ULONG((volatile ULONG *)(f + 0x10 + 4 * 4));
            ULONG64 bar = lo & ~0xfULL;
            if ((lo & 0x6) == 0x4) bar |= (ULONG64)READ_REGISTER_ULONG((volatile ULONG *)(f + 0x10 + 5 * 4)) << 32;
            ULONG lo2 = READ_REGISTER_ULONG((volatile ULONG *)(f + 0x10 + 2 * 4));
            ULONG64 bar2 = lo2 & ~0xfULL;
            if ((lo2 & 0x6) == 0x4) bar2 |= (ULONG64)READ_REGISTER_ULONG((volatile ULONG *)(f + 0x10 + 3 * 4)) << 32;
            /* size probe would need writes; Linux maps BAR4 whole, we only need < 0x8000 */
            MmUnmapIoSpace((PVOID)cfg, 1u << 20);
            if (!bar || (lo & 1)) return NDIS_STATUS_ADAPTER_NOT_FOUND;
            PHYSICAL_ADDRESS gp; gp.QuadPart = (LONGLONG)bar;
            a->glue_len = 0x80000;                        /* BAR4 = 512K (Linux maps it whole) */
            a->glue = MmMapIoSpaceEx(gp, a->glue_len, PAGE_READWRITE | PAGE_NOCACHE);
            if (bar2 && !(lo2 & 1)) {
                PHYSICAL_ADDRESS gp2; gp2.QuadPart = (LONGLONG)bar2;
                a->glue2_len = 0x200000;                  /* BAR2 = 2M */
                a->glue2 = MmMapIoSpaceEx(gp2, a->glue2_len, PAGE_READWRITE | PAGE_NOCACHE);
            }
            if (a->glue) OK(13); else BAD(13);
            return a->glue ? NDIS_STATUS_SUCCESS : NDIS_STATUS_RESOURCES;
        }
        MmUnmapIoSpace((PVOID)cfg, 1u << 20);
    }
    return NDIS_STATUS_ADAPTER_NOT_FOUND;
}

/* ---------------------------------------------------------------- link + polling */
static void indicate_link(PADAPTER a)
{
    NDIS_LINK_STATE ls;
    NDIS_STATUS_INDICATION si;
    NdisZeroMemory(&ls, sizeof(ls));
    ls.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    ls.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    ls.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    ls.MediaConnectState = a->link_up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ls.MediaDuplexState = a->link_up ? MediaDuplexStateFull : MediaDuplexStateUnknown;
    ls.XmitLinkSpeed = ls.RcvLinkSpeed = a->link_up ? 1000000000ULL : NDIS_LINK_SPEED_UNKNOWN;
    ls.PauseFunctions = NdisPauseFunctionsUnsupported;
    NdisZeroMemory(&si, sizeof(si));
    si.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    si.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    si.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    si.SourceHandle = a->handle;
    si.StatusCode = NDIS_STATUS_LINK_STATE;
    si.StatusBuffer = &ls;
    si.StatusBufferSize = sizeof(ls);
    NdisMIndicateStatusEx(a->handle, &si);
}
static void rx_poll(PADAPTER a)
{
    PNET_BUFFER_LIST head = NULL, *tail = &head;
    ULONG n = 0, i = a->rx_head;
    while (n < 64 && (a->rx_ring[i].flags & DESC_DONE)) {
        ULONG len = a->rx_ring[i].flags & DESC_RX_LEN_MASK;
        if (len >= 60 && len <= DESC_RX_LEN && !a->paused && a->packet_filter) {
            PNET_BUFFER_LIST nbl = a->rx_nbl[i];
            NET_BUFFER_DATA_LENGTH(NET_BUFFER_LIST_FIRST_NB(nbl)) = len;
            NET_BUFFER_DATA_OFFSET(NET_BUFFER_LIST_FIRST_NB(nbl)) = 0;
            NET_BUFFER_CURRENT_MDL_OFFSET(NET_BUFFER_LIST_FIRST_NB(nbl)) = 0;
            NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
            nbl->SourceHandle = a->handle;
            nbl->Status = NDIS_STATUS_SUCCESS;
            *tail = nbl; tail = &NET_BUFFER_LIST_NEXT_NBL(nbl);
            a->rx_ok++; a->rx_bytes += len;
            mark(51 + (a->rx_ok & 1), 0x00e000e0);   /* magenta: frame received */
        } else if (len < 60 || len > DESC_RX_LEN) {
            a->rx_err++;
        }
        n++;
        i = (i + 1) % RING_SIZE;
    }
    if (!n) return;
    if (head) {
        ULONG cnt = 0;
        for (PNET_BUFFER_LIST p = head; p; p = NET_BUFFER_LIST_NEXT_NBL(p)) cnt++;
        /* RESOURCES: NDIS copies before returning, the slots stay ours */
        NdisMIndicateReceiveNetBufferLists(a->handle, head, NDIS_DEFAULT_PORT_NUMBER, cnt,
            NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL | NDIS_RECEIVE_FLAGS_RESOURCES);
    }
    for (ULONG k = 0, j = a->rx_head; k < n; k++, j = (j + 1) % RING_SIZE) {
        a->rx_ring[j].w2 = 0;
        a->rx_ring[j].w3 = 0;
        KeMemoryBarrier();
        a->rx_ring[j].flags = DESC_RX_LEN | (j == RING_SIZE - 1 ? DESC_WRAP : 0);
    }
    a->rx_head = i;
    setb(a, GBE_REG_RX_DMA_CTRL, GBE_DMA_KICK);
}
/* v1.0.6: NDIS_TIMER_FUNCTION is (SystemSpecific1, FunctionContext, SystemSpecific2,
 * SystemSpecific3) - the context is the SECOND argument. v1.0.5 used the first one
 * (NULL) -> bugcheck 0xA at NULL+0x10c8 on the first poll tick (v3.20). */
static NDIS_TIMER_FUNCTION timer_fn;
static VOID timer_fn(PVOID s1, PVOID ctx, PVOID s2, PVOID s3)
{
    PADAPTER a = ctx;
    UNREFERENCED_PARAMETER(s1); UNREFERENCED_PARAMETER(s2); UNREFERENCED_PARAMETER(s3);
    if (a->halting) return;
    mark(45 + ((a->link_tick >> 6) & 1), 0x000000e0);   /* blue blink: poll timer alive */
    ULONG isr = rd(a, GBE_REG_ISR);
    if (isr) wr(a, GBE_REG_ISR, isr);
    NdisDprAcquireSpinLock(&a->lock);
    if (isr & GBE_ISR_LSO_RECOVER) tx_rebuild(a);
    tx_complete(a);
    NdisDprReleaseSpinLock(&a->lock);
    rx_poll(a);
    if (++a->link_tick >= 500 / POLL_MS) {
        a->link_tick = 0;
        ULONG up = rd(a, GBE_REG_LINK) & 1;
        if (up != a->link_up) { a->link_up = up; indicate_link(a); }
        mark(54, up ? 0x0000ffff : 0x00404040);   /* cyan: link up, grey: down */
    }
}

/* ---------------------------------------------------------------- send */
static VOID send_nbls(NDIS_HANDLE ctx, PNET_BUFFER_LIST nbls, NDIS_PORT_NUMBER port, ULONG flags)
{
    PADAPTER a = ctx;
    BOOLEAN dispatch = NDIS_TEST_SEND_AT_DISPATCH_LEVEL(flags);
    UNREFERENCED_PARAMETER(port);
    if (dispatch) { NdisDprAcquireSpinLock(&a->lock); } else { NdisAcquireSpinLock(&a->lock); }
    for (PNET_BUFFER_LIST nbl = nbls; nbl; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        NDIS_STATUS st = NDIS_STATUS_SUCCESS;
        if (a->paused) st = NDIS_STATUS_PAUSED;
        else if (!a->link_up) st = NDIS_STATUS_MEDIA_DISCONNECTED;
        for (PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb && st == NDIS_STATUS_SUCCESS; nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG len = NET_BUFFER_DATA_LENGTH(nb);
            if (len > MAX_FRAME) { a->tx_drop++; st = NDIS_STATUS_INVALID_LENGTH; break; }
            tx_complete(a);
            ULONG head = a->tx_head;
            if ((head + 1) % RING_SIZE == a->tx_tail) { a->tx_drop++; st = NDIS_STATUS_RESOURCES; break; }
            ULONG plen = len < 60 ? 60 : len;
            ULONG aligned = (plen + 0x7f) & ~0x7fu, off = a->tx_off;
            if (off + aligned > TX_SCRATCH_SIZE) off = 0;
            a->tx_off = off + aligned;
            if (a->tx_off >= TX_SCRATCH_SIZE) a->tx_off = 0;
            UCHAR *dst = a->tx_scratch + off;
            /* copy the NET_BUFFER data out of its MDL chain */
            PMDL mdl = NET_BUFFER_CURRENT_MDL(nb);
            ULONG mo = NET_BUFFER_CURRENT_MDL_OFFSET(nb), left = len, copied = 0;
            while (mdl && left) {
                UCHAR *src = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
                if (!src) break;
                ULONG avail = MmGetMdlByteCount(mdl) - mo, c = avail < left ? avail : left;
                NdisMoveMemory(dst + copied, src + mo, c);
                copied += c; left -= c; mo = 0;
                mdl = mdl->Next;
            }
            if (left) { a->tx_drop++; st = NDIS_STATUS_FAILURE; break; }
            if (plen > len) NdisZeroMemory(dst + len, plen - len);
            a->tx_ring[head].buf = a->tx_scratch_pa.LowPart + off;
            a->tx_ring[head].w2 = DESC_W2_TX;
            a->tx_ring[head].w3 = 0;
            KeMemoryBarrier();
            a->tx_ring[head].flags = (plen & DESC_LEN_MASK) | DESC_SOP | DESC_EOP | (head == RING_SIZE - 1 ? DESC_WRAP : 0);
            a->tx_head = (head + 1) % RING_SIZE;
            mark(47, 0x00ff8000);   /* orange: about to kick TX DMA */
            setb(a, GBE_REG_TX_DMA_CTRL, GBE_DMA_KICK);
            a->tx_ok++; a->tx_bytes += len;
            mark(48 + (a->tx_ok & 1), 0x00e0e000);   /* yellow: frame queued */
        }
        NET_BUFFER_LIST_STATUS(nbl) = st;
    }
    if (dispatch) { NdisDprReleaseSpinLock(&a->lock); } else { NdisReleaseSpinLock(&a->lock); }
    /* data was copied into the bounce region: complete immediately */
    NdisMSendNetBufferListsComplete(a->handle, nbls, dispatch ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
}
static VOID return_nbls(NDIS_HANDLE ctx, PNET_BUFFER_LIST nbls, ULONG flags)
{
    /* never called: receives are indicated with NDIS_RECEIVE_FLAGS_RESOURCES */
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(nbls); UNREFERENCED_PARAMETER(flags);
}
static VOID cancel_send(NDIS_HANDLE ctx, PVOID id) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id); }

/* ---------------------------------------------------------------- OIDs */
static NDIS_STATUS put(NDIS_OID_REQUEST *r, const void *src, ULONG len)
{
    if (r->DATA.QUERY_INFORMATION.InformationBufferLength < len) {
        r->DATA.QUERY_INFORMATION.BytesNeeded = len;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    NdisMoveMemory(r->DATA.QUERY_INFORMATION.InformationBuffer, src, len);
    r->DATA.QUERY_INFORMATION.BytesWritten = len;
    return NDIS_STATUS_SUCCESS;
}
static NDIS_STATUS query(PADAPTER a, NDIS_OID_REQUEST *r)
{
    ULONG u = 0; ULONG64 q = 0;
    switch (r->DATA.QUERY_INFORMATION.Oid) {
    case OID_GEN_SUPPORTED_LIST: return put(r, g_oids, sizeof(g_oids));
    case OID_GEN_HARDWARE_STATUS: u = NdisHardwareStatusReady; break;
    case OID_GEN_MEDIA_SUPPORTED: case OID_GEN_MEDIA_IN_USE: u = NdisMedium802_3; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD: case OID_GEN_MAXIMUM_FRAME_SIZE: u = 1500; break;
    case OID_GEN_CURRENT_LOOKAHEAD: u = a->lookahead; break;
    case OID_GEN_LINK_SPEED: u = a->link_up ? 10000000 : 0; break;   /* 100 bps units */
    case OID_GEN_TRANSMIT_BUFFER_SPACE: u = TX_SCRATCH_SIZE; break;
    case OID_GEN_RECEIVE_BUFFER_SPACE: u = RX_POOL_SIZE; break;
    case OID_GEN_TRANSMIT_BLOCK_SIZE: case OID_GEN_RECEIVE_BLOCK_SIZE: u = DESC_RX_LEN; break;
    case OID_GEN_VENDOR_ID: u = 0x00ffffff; break;
    case OID_GEN_VENDOR_DESCRIPTION: return put(r, "PS5 Gigabit Ethernet (ps5gbe)", 30);
    case OID_GEN_VENDOR_DRIVER_VERSION: u = PS5GBE_VERSION; break;
    case OID_GEN_DRIVER_VERSION: { USHORT v = 0x0614; return put(r, &v, 2); }
    case OID_GEN_CURRENT_PACKET_FILTER: u = a->packet_filter; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE: u = MAX_FRAME; break;
    case OID_GEN_MAC_OPTIONS: u = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK; break;
    case OID_GEN_MEDIA_CONNECT_STATUS: u = a->link_up ? NdisMediaStateConnected : NdisMediaStateDisconnected; break;
    case OID_GEN_MAXIMUM_SEND_PACKETS: u = RING_SIZE / 2; break;
    case OID_GEN_XMIT_OK: q = a->tx_ok; return put(r, &q, 8);
    case OID_GEN_RCV_OK: q = a->rx_ok; return put(r, &q, 8);
    case OID_GEN_XMIT_ERROR: q = a->tx_drop; return put(r, &q, 8);
    case OID_GEN_RCV_ERROR: q = a->rx_err; return put(r, &q, 8);
    case OID_GEN_RCV_NO_BUFFER: case OID_802_3_RCV_ERROR_ALIGNMENT: case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: u = 0; break;
    case OID_802_3_PERMANENT_ADDRESS: return put(r, a->perm_mac, 6);
    case OID_802_3_CURRENT_ADDRESS: return put(r, a->cur_mac, 6);
    case OID_802_3_MAXIMUM_LIST_SIZE: u = 32; break;
    case OID_GEN_STATISTICS: {
        NDIS_STATISTICS_INFO s;
        NdisZeroMemory(&s, sizeof(s));
        s.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        s.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
        s.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
        s.SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV |
            NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT |
            NDIS_STATISTICS_FLAGS_VALID_RCV_ERROR | NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR;
        s.ifHCInUcastPkts = a->rx_ok; s.ifHCInOctets = a->rx_bytes; s.ifInErrors = a->rx_err;
        s.ifHCOutUcastPkts = a->tx_ok; s.ifHCOutOctets = a->tx_bytes; s.ifOutErrors = a->tx_drop;
        return put(r, &s, sizeof(s));
    }
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }
    return put(r, &u, 4);
}
static NDIS_STATUS set(PADAPTER a, NDIS_OID_REQUEST *r)
{
    PVOID buf = r->DATA.SET_INFORMATION.InformationBuffer;
    ULONG len = r->DATA.SET_INFORMATION.InformationBufferLength;
    switch (r->DATA.SET_INFORMATION.Oid) {
    case OID_GEN_CURRENT_PACKET_FILTER:
        if (len < 4) { r->DATA.SET_INFORMATION.BytesNeeded = 4; return NDIS_STATUS_INVALID_LENGTH; }
        a->packet_filter = *(ULONG *)buf;   /* no hardware filter: MAC delivers unicast/broadcast/multicast */
        r->DATA.SET_INFORMATION.BytesRead = 4;
        return NDIS_STATUS_SUCCESS;
    case OID_GEN_CURRENT_LOOKAHEAD:
        if (len < 4) { r->DATA.SET_INFORMATION.BytesNeeded = 4; return NDIS_STATUS_INVALID_LENGTH; }
        a->lookahead = *(ULONG *)buf;
        r->DATA.SET_INFORMATION.BytesRead = 4;
        return NDIS_STATUS_SUCCESS;
    case OID_802_3_MULTICAST_LIST:
        if (len % 6 || len / 6 > 32) return NDIS_STATUS_MULTICAST_FULL;
        r->DATA.SET_INFORMATION.BytesRead = len;
        return NDIS_STATUS_SUCCESS;
    case OID_PNP_SET_POWER:
        r->DATA.SET_INFORMATION.BytesRead = len;
        return NDIS_STATUS_SUCCESS;
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}
static NDIS_STATUS oid_request(NDIS_HANDLE ctx, PNDIS_OID_REQUEST r)
{
    PADAPTER a = ctx;
    mark(43, 0x00a0a0a0);   /* grey: OID traffic */
    switch (r->RequestType) {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        if (r->DATA.QUERY_INFORMATION.Oid == OID_PNP_QUERY_POWER) return NDIS_STATUS_SUCCESS;
        return query(a, r);
    case NdisRequestSetInformation:
        return set(a, r);
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}
static VOID cancel_oid(NDIS_HANDLE ctx, PVOID id) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id); }

/* ---------------------------------------------------------------- lifecycle */
static void free_adapter(PADAPTER a)
{
    for (ULONG i = 0; i < RING_SIZE; i++) {
        if (a->rx_nbl[i]) NdisFreeNetBufferList(a->rx_nbl[i]);
        if (a->rx_mdl[i]) NdisFreeMdl(a->rx_mdl[i]);
    }
    if (a->nbl_pool) NdisFreeNetBufferListPool(a->nbl_pool);
    if (a->tx_ring) NdisMFreeSharedMemory(a->handle, RING_BYTES, FALSE, a->tx_ring, a->tx_ring_pa);
    if (a->rx_ring) NdisMFreeSharedMemory(a->handle, RING_BYTES, FALSE, a->rx_ring, a->rx_ring_pa);
    if (a->tx_scratch) NdisMFreeSharedMemory(a->handle, TX_SCRATCH_SIZE, TRUE, a->tx_scratch, a->tx_scratch_pa);
    if (a->rx_pool) NdisMFreeSharedMemory(a->handle, RX_POOL_SIZE, TRUE, a->rx_pool, a->rx_pool_pa);
    if (a->dma) NdisMDeregisterScatterGatherDma(a->dma);
    if (a->glue) MmUnmapIoSpace((PVOID)a->glue, a->glue_len);
    if (a->glue2) MmUnmapIoSpace((PVOID)a->glue2, a->glue2_len);
    if (a->base) NdisMUnmapIoSpace(a->handle, (PVOID)a->base, a->base_len);
    if (a->timer) NdisFreeTimerObject(a->timer);
    NdisFreeSpinLock(&a->lock);
    NdisFreeMemory(a, 0, 0);
}
static BOOLEAN below_4g(NDIS_PHYSICAL_ADDRESS pa, ULONG len) { return (ULONG64)pa.QuadPart + len <= 0x100000000ULL; }

/* v1.0.2: every failed init step lands in the System event log (source ps5gbe):
 * value 0 = step number, value 1 = NDIS status, value 2 = extra (e.g. LINK reg / chip id) */
static void trace_fail(NDIS_HANDLE h, ULONG step, NDIS_STATUS st, ULONG extra)
{
    NdisWriteErrorLogEntry(h, NDIS_ERROR_CODE_DRIVER_FAILURE, 3, step, (ULONG)st, extra);
}

static NDIS_STATUS initialize(NDIS_HANDLE handle, NDIS_HANDLE drvctx, PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    NDIS_STATUS st;
    ULONG step = 0;
    {
        /* v1.0.10: framebuffer boxes only with DiagMarks=1 (device parameters), and only during initialize */
        NDIS_CONFIGURATION_OBJECT co = { 0 };
        NDIS_HANDLE cfg = NULL;
        co.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
        co.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
        co.Header.Size = NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1;
        co.NdisHandle = handle;
        if (NdisOpenConfigurationEx(&co, &cfg) == NDIS_STATUS_SUCCESS) {
            NDIS_STRING name = NDIS_STRING_CONST("DiagMarks");
            PNDIS_CONFIGURATION_PARAMETER v = NULL;
            NDIS_STATUS cs;
            NdisReadConfiguration(&cs, &v, cfg, &name, NdisParameterInteger);
            if (cs == NDIS_STATUS_SUCCESS && v && v->ParameterData.IntegerData == 1) fb_map();
            NdisCloseConfiguration(cfg);
        }
    }
    OK(0);
    PADAPTER a = NdisAllocateMemoryWithTagPriority(handle, sizeof(ADAPTER), PS5GBE_TAG, NormalPoolPriority);
    UNREFERENCED_PARAMETER(drvctx);
    if (!a) return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(a, sizeof(*a));
    a->handle = handle;
    a->packet_filter = 0;
    a->lookahead = 1500;
    a->paused = 1;
    NdisAllocateSpinLock(&a->lock);

    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES reg;
    NdisZeroMemory(&reg, sizeof(reg));
    reg.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    reg.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.MiniportAdapterContext = a;
    reg.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE | NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER;
    reg.InterfaceType = NdisInterfacePci;
    step = 1; OK(1);
    st = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&reg);
    if (st != NDIS_STATUS_SUCCESS) goto fail;

    step = 2; OK(2);
    /* BAR0 = first memory resource */
    st = NDIS_STATUS_RESOURCE_CONFLICT;
    if (ip->AllocatedResources) {
        for (ULONG i = 0; i < ip->AllocatedResources->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &ip->AllocatedResources->PartialDescriptors[i];
            if (d->Type == CmResourceTypeMemory && !a->base) {
                a->base_len = d->u.Memory.Length;
                st = NdisMMapIoSpace((PVOID *)&a->base, handle, d->u.Memory.Start, a->base_len);
            }
        }
    }
    if (!a->base) goto fail;
    step = 3; OK(3);
    st = map_glue(a);
    if (st != NDIS_STATUS_SUCCESS) goto fail;
    {
        ULONG rev = READ_REGISTER_ULONG((volatile ULONG *)(a->glue + 0x4000));
        a->chip = ((rev & 0xff000000) == 0x11000000 ? 0x110000 : (rev & 0xff000000) == 0x12000000 ? 0x120000 : 0) | (rev & 0xffff);
        for (int k = 0; k < 6; k++) a->perm_mac[k] = READ_REGISTER_UCHAR(a->glue + 0x3000 + k);
        if ((a->perm_mac[0] & 1) || !(a->perm_mac[0] | a->perm_mac[1] | a->perm_mac[2] | a->perm_mac[3] | a->perm_mac[4] | a->perm_mac[5])) {
            step = 4; OK(4);
            st = NDIS_STATUS_ADAPTER_NOT_FOUND;   /* no valid MAC in the glue scratchpad */
            goto fail;
        }
        NdisMoveMemory(a->cur_mac, a->perm_mac, 6);
    }

    NDIS_SG_DMA_DESCRIPTION dd;
    NdisZeroMemory(&dd, sizeof(dd));
    dd.Header.Type = NDIS_OBJECT_TYPE_SG_DMA_DESCRIPTION;
    dd.Header.Revision = NDIS_SG_DMA_DESCRIPTION_REVISION_1;
    dd.Header.Size = NDIS_SIZEOF_SG_DMA_DESCRIPTION_REVISION_1;
    dd.Flags = 0;                       /* 32-bit device */
    dd.MaximumPhysicalMapping = 0x10000;
    step = 5; OK(5);
    st = NdisMRegisterScatterGatherDma(handle, &dd, &a->dma);
    if (st != NDIS_STATUS_SUCCESS) goto fail;

    step = 6; OK(6);
    NdisMAllocateSharedMemory(handle, RING_BYTES, FALSE, (PVOID *)&a->tx_ring, &a->tx_ring_pa);
    NdisMAllocateSharedMemory(handle, RING_BYTES, FALSE, (PVOID *)&a->rx_ring, &a->rx_ring_pa);
    NdisMAllocateSharedMemory(handle, TX_SCRATCH_SIZE, TRUE, (PVOID *)&a->tx_scratch, &a->tx_scratch_pa);
    NdisMAllocateSharedMemory(handle, RX_POOL_SIZE, TRUE, (PVOID *)&a->rx_pool, &a->rx_pool_pa);
    st = NDIS_STATUS_RESOURCES;
    if (!a->tx_ring || !a->rx_ring || !a->tx_scratch || !a->rx_pool) goto fail;
    if (!below_4g(a->tx_ring_pa, RING_BYTES) || !below_4g(a->rx_ring_pa, RING_BYTES) ||
        !below_4g(a->tx_scratch_pa, TX_SCRATCH_SIZE) || !below_4g(a->rx_pool_pa, RX_POOL_SIZE)) goto fail;
    NdisZeroMemory(a->tx_ring, RING_BYTES);
    NdisZeroMemory(a->rx_ring, RING_BYTES);

    NET_BUFFER_LIST_POOL_PARAMETERS pp;
    NdisZeroMemory(&pp, sizeof(pp));
    pp.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pp.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    pp.fAllocateNetBuffer = TRUE;
    pp.PoolTag = PS5GBE_TAG;
    step = 7; OK(7);
    a->nbl_pool = NdisAllocateNetBufferListPool(handle, &pp);
    if (!a->nbl_pool) goto fail;
    for (ULONG i = 0; i < RING_SIZE; i++) {
        a->rx_mdl[i] = NdisAllocateMdl(handle, a->rx_pool + i * DESC_RX_LEN, DESC_RX_LEN);
        if (!a->rx_mdl[i]) goto fail;
        a->rx_nbl[i] = NdisAllocateNetBufferAndNetBufferList(a->nbl_pool, 0, 0, a->rx_mdl[i], 0, DESC_RX_LEN);
        if (!a->rx_nbl[i]) goto fail;
    }

    NDIS_TIMER_CHARACTERISTICS tc;
    NdisZeroMemory(&tc, sizeof(tc));
    tc.Header.Type = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    tc.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    tc.Header.Size = NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    tc.AllocationTag = PS5GBE_TAG;
    tc.TimerFunction = timer_fn;
    tc.FunctionContext = a;
    step = 8; OK(8);
    st = NdisAllocateTimerObject(handle, &tc, &a->timer);
    if (st != NDIS_STATUS_SUCCESS) goto fail;

    init_hw(a);
    OK(24);
    rx_fill(a);
    OK(25);
    init_rings_hw(a);
    OK(26);
    a->link_up = rd(a, GBE_REG_LINK) & 1;

    NDIS_PM_CAPABILITIES pm;
    NdisZeroMemory(&pm, sizeof(pm));
    pm.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pm.Header.Revision = NDIS_PM_CAPABILITIES_REVISION_1;
    pm.Header.Size = NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_1;
    pm.MinMagicPacketWakeUp = NdisDeviceStateUnspecified;
    pm.MinPatternWakeUp = NdisDeviceStateUnspecified;
    pm.MinLinkChangeWakeUp = NdisDeviceStateUnspecified;

    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES ga;
    NdisZeroMemory(&ga, sizeof(ga));
    ga.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    ga.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    ga.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    ga.MediaType = NdisMedium802_3;
    ga.PhysicalMediumType = NdisPhysicalMedium802_3;
    ga.MtuSize = 1500;
    ga.MaxXmitLinkSpeed = ga.MaxRcvLinkSpeed = 1000000000ULL;
    ga.XmitLinkSpeed = ga.RcvLinkSpeed = a->link_up ? 1000000000ULL : NDIS_LINK_SPEED_UNKNOWN;
    ga.MediaConnectState = a->link_up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ga.MediaDuplexState = a->link_up ? MediaDuplexStateFull : MediaDuplexStateUnknown;
    ga.LookaheadSize = 1500;
    ga.PowerManagementCapabilitiesEx = &pm;
    ga.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
    ga.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST |
        NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS;
    ga.MaxMulticastListSize = 32;
    ga.MacAddressLength = 6;
    NdisMoveMemory(ga.PermanentMacAddress, a->perm_mac, 6);
    NdisMoveMemory(ga.CurrentMacAddress, a->cur_mac, 6);
    ga.RecvScaleCapabilities = NULL;
    ga.AccessType = NET_IF_ACCESS_BROADCAST;
    ga.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    ga.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    ga.IfType = IF_TYPE_ETHERNET_CSMACD;
    ga.IfConnectorPresent = TRUE;
    ga.SupportedStatistics = NDIS_STATISTICS_DIRECTED_FRAMES_RCV_SUPPORTED | NDIS_STATISTICS_BYTES_RCV_SUPPORTED |
        NDIS_STATISTICS_DIRECTED_FRAMES_XMIT_SUPPORTED | NDIS_STATISTICS_BYTES_XMIT_SUPPORTED |
        NDIS_STATISTICS_RCV_ERROR_SUPPORTED | NDIS_STATISTICS_XMIT_ERROR_SUPPORTED;
    ga.SupportedOidList = g_oids;
    ga.SupportedOidListLength = sizeof(g_oids);
    step = 9; OK(9);
    st = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&ga);
    if (st != NDIS_STATUS_SUCCESS) { stop_dma(a); goto fail; }

    {
        /* v1.0.7: the 2 ms poll period only takes effect with a 1 ms system timer
         * resolution (default 15.6 ms gave 20-40 ms LAN round trips, v3.21). */
        ExSetTimerResolution(10000, TRUE);
        LARGE_INTEGER due; due.QuadPart = -10000LL * POLL_MS;
        NdisSetTimerObject(a->timer, due, POLL_MS, NULL);
    }
    /* informational entry: step 100 = initialized; extra = LINK register */
    OK(40);
    fb_unmap();                 /* v1.0.10: diagnosis boxes never outlive initialize */
    NdisWriteErrorLogEntry(handle, NDIS_ERROR_CODE_DRIVER_FAILURE, 4, 100, a->chip, rd(a, GBE_REG_LINK), a->tx_ring_pa.LowPart);
    return NDIS_STATUS_SUCCESS;
fail:
    BAD(step);
    fb_unmap();
    trace_fail(handle, step, st, a->glue ? a->chip : 0xffffffff);
    free_adapter(a);
    return st;
}
static VOID halt(NDIS_HANDLE ctx, NDIS_HALT_ACTION action)
{
    PADAPTER a = ctx;
    UNREFERENCED_PARAMETER(action);
    InterlockedExchange(&a->halting, 1);
    if (!NdisCancelTimerObject(a->timer)) KeFlushQueuedDpcs();
    ExSetTimerResolution(0, FALSE);
    KeFlushQueuedDpcs();
    stop_dma(a);
    /* v1.0.8: park the MAC (reset) so no descriptor/buffer DMA can hit memory we free next */
    wr(a, GBE_REG_TX_RING_BASE, 0);
    wr(a, GBE_REG_RX_RING_BASE, 0);
    wr(a, GBE_REG_RESET, 9);
    KeStallExecutionProcessor(1000);
    free_adapter(a);
}
static NDIS_STATUS pause(NDIS_HANDLE ctx, PNDIS_MINIPORT_PAUSE_PARAMETERS p)
{
    PADAPTER a = ctx; UNREFERENCED_PARAMETER(p);
    InterlockedExchange(&a->paused, 1);
    KeFlushQueuedDpcs();        /* no indication may be in flight after Pause returns */
    return NDIS_STATUS_SUCCESS;
}
static NDIS_STATUS restart(NDIS_HANDLE ctx, PNDIS_MINIPORT_RESTART_PARAMETERS p)
{
    PADAPTER a = ctx; UNREFERENCED_PARAMETER(p);
    OK(41);
    InterlockedExchange(&a->paused, 0);
    indicate_link(a);
    OK(42);
    return NDIS_STATUS_SUCCESS;
}
static NDIS_STATUS reset(NDIS_HANDLE ctx, PBOOLEAN addressing)
{
    PADAPTER a = ctx;
    *addressing = FALSE;
    NdisAcquireSpinLock(&a->lock);
    tx_rebuild(a);
    NdisReleaseSpinLock(&a->lock);
    return NDIS_STATUS_SUCCESS;
}
static VOID pnp_event(NDIS_HANDLE ctx, PNET_DEVICE_PNP_EVENT e) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(e); }
static VOID shutdown(NDIS_HANDLE ctx, NDIS_SHUTDOWN_ACTION action)
{
    PADAPTER a = ctx; UNREFERENCED_PARAMETER(action);
    /* may run at high IRQL on bugcheck: only stop the DMA engines */
    wr(a, GBE_REG_IER, 0);
    setb(a, GBE_REG_TX_DMA_CTRL, GBE_DMA_STOP);
    setb(a, GBE_REG_RX_DMA_CTRL, GBE_DMA_STOP);
}
static VOID unload(PDRIVER_OBJECT d) { UNREFERENCED_PARAMETER(d); NdisMDeregisterMiniportDriver(g_driver); }

/* v1.0.1: lld-link drops .reloc when no absolute address exists; the kernel loader then
 * cannot rebase the image (code 39, STATUS_CONFLICTING_ADDRESSES). Keep one absolute pointer. */
DRIVER_INITIALIZE DriverEntry;
__declspec(allocate(".data")) PVOID volatile g_reloc_anchor = (PVOID)&DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    if (!g_reloc_anchor) return STATUS_UNSUCCESSFUL;
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS c;
    NdisZeroMemory(&c, sizeof(c));
    c.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    c.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.MajorNdisVersion = 6;
    c.MinorNdisVersion = 20;
    c.MajorDriverVersion = 1;
    c.MinorDriverVersion = 0;
    c.InitializeHandlerEx = initialize;
    c.HaltHandlerEx = halt;
    c.UnloadHandler = unload;
    c.PauseHandler = pause;
    c.RestartHandler = restart;
    c.OidRequestHandler = oid_request;
    c.SendNetBufferListsHandler = send_nbls;
    c.ReturnNetBufferListsHandler = return_nbls;
    c.CancelSendHandler = cancel_send;
    c.DevicePnPEventNotifyHandler = pnp_event;
    c.ShutdownHandlerEx = shutdown;
    c.ResetHandlerEx = reset;
    c.CancelOidRequestHandler = cancel_oid;
    return NdisMRegisterMiniportDriver(drv, path, NULL, &c, &g_driver);
}
