/* SPDX-License-Identifier: GPL-2.0-only
 * NXP/Marvell 88W9097 (IW620, PCI 1B4B:2B56) PCIe ADMA & HostCmd hardware definitions.
 * Register map and command wire formats derived from nxp-imx/mwifiex (lf-6.18.2_1.0.0)
 * and ps5-linux-mwifiex patches. */
#ifndef PS5WIFI_IW620_HW_H
#define PS5WIFI_IW620_HW_H

#include <ndis.h>

#define IW620_VID               0x1B4B
#define IW620_DID               0x2B56

/* BAR2 register offsets (PCIE9097 B0 / IW620) */
#define REG_DEV_ID              0x0000
#define REG_REV_ID              0x0008
#define REG_CPU_INT_EVENT       0x1C20
#define REG_CPU_INT_STATUS      0x1C24
#define REG_HOST_INT_STATUS     0x3C44
#define REG_HOST_INT_MASK       0x3C48
#define REG_HOST_INT_CLR_SEL    0x3C4C
#define REG_HOST_INT_STAT_MASK  0x3C50
#define REG_SCRATCH_0           0x1C60
#define REG_SCRATCH_1           0x1C64
#define REG_SCRATCH_2           0x1C68
#define REG_SCRATCH_3           0x1C6C
#define REG_SCRATCH_6           0x1C78
#define REG_SCRATCH_7           0x1C7C
#define REG_DRV_READY           0x1C90  /* SCRATCH_12 */

#define CPU_INTR_DOOR_BELL      (1u << 1)
#define CPU_INTR_EVENT_DONE     (1u << 5)
#define FIRMWARE_READY_PCIE     0xFEDCBA00u
#define DRV_READY_MAGIC         0x0000A5A5u
#define WLAN_UPLD_SIZE          2312u

/* ADMA queue base offsets in BAR2 */
#define ADMA_CHAN0_Q0           0x10000u /* TX Data  (H2D, dual-desc) */
#define ADMA_CHAN1_Q0           0x10800u /* RX Data  (D2H, dual-desc) */
#define ADMA_CHAN1_Q1           0x10880u /* RX Event (D2H, dual-desc) */
#define ADMA_CHAN2_Q0           0x11000u /* TX Cmd   (H2D, direct)    */
#define ADMA_CHAN2_Q1           0x11080u /* RX CmdRsp(D2H, direct)    */

/* Per-queue ADMA register offsets */
#define ADMA_DMA_CFG            0x0000u
#define ADMA_SRC_LOW            0x0004u
#define ADMA_SRC_HIGH           0x0008u
#define ADMA_DST_LOW            0x000Cu
#define ADMA_DST_HIGH           0x0010u
#define ADMA_SRC_RW_PTR         0x0014u
#define ADMA_DST_RW_PTR         0x0018u
#define ADMA_INT_MAPPING        0x001Cu
#define ADMA_SRC_INT_STAT_MASK  0x0024u
#define ADMA_SRC_INT_MASK       0x0028u
#define ADMA_SRC_INT_STATUS     0x002Cu
#define ADMA_DST_INT_STAT_MASK  0x0030u
#define ADMA_DST_INT_MASK       0x0034u
#define ADMA_DST_INT_STATUS     0x0038u
#define ADMA_DMA_CFG2           0x003Cu
#define ADMA_DMA_CFG3           0x0040u

#define DMA_MODE_DIRECT         0u
#define DMA_MODE_DUAL_DESC      2u
#define DEST_INT_TO_DEVICE      (1u << 0)
#define DEST_INT_TO_HOST        (1u << 1)
#define DEF_ADMA_INT_MASK       (1u << 0)
#define ADMA_Q_PTR_CLR          (1u << 0)

#define ADMA_SRC_ADDR_IS_HOST   (1u << 3)
#define ADMA_DST_ADDR_IS_HOST   (1u << 2)
#define ADMA_SRC_BYPASS_EN      (1u << 16)
#define ADMA_DST_BYPASS_EN      (1u << 17)
#define ADMA_MSI_LEGACY_EN      (1u << 19)
#define ADMA_MSI_SRC_BYPASS_EN  (1u << 21)
#define ADMA_MSI_DST_BYPASS_EN  (1u << 22)

#define ADMA_INT_TX_DONE        (1u << 0)
#define ADMA_INT_RX_RDY         (1u << 16)
#define ADMA_INT_EVT_RDY        (1u << 17)
#define ADMA_INT_CMD_DNLD       (1u << 24)
#define ADMA_INT_CMD_DONE       (1u << 25)
#define ADMA_HOST_INTR_ALL      (ADMA_INT_TX_DONE | ADMA_INT_RX_RDY | ADMA_INT_EVT_RDY | ADMA_INT_CMD_DNLD | ADMA_INT_CMD_DONE)

#define ADMA_BD_FLAG_SOP        (1u << 0)
#define ADMA_BD_FLAG_EOP        (1u << 1)
#define ADMA_BD_FLAG_INT_EN     (1u << 2)
#define ADMA_BD_FLAG_SRC_HOST   (1u << 3)
#define ADMA_BD_FLAG_DST_HOST   (1u << 4)
#define ADMA_MIN_PKT_SIZE       128u

/* PCIe interface header types */
#define MLAN_TYPE_DATA          0x0000u
#define MLAN_TYPE_CMD           0x0001u
#define MLAN_TYPE_EVENT         0x0003u

/* HostCmd IDs */
#define CMD_GET_HW_SPEC         0x0003u
#define CMD_802_11_SCAN         0x0006u
#define CMD_802_11_ASSOCIATE    0x0012u
#define CMD_802_11_DEAUTH       0x0024u
#define CMD_MAC_CONTROL         0x0028u
#define CMD_RSSI_INFO           0x00A4u
#define CMD_TX_RATE_QUERY       0x007Fu
#define CMD_TX_RATE_CFG         0x00D6u
#define CMD_11N_ADDBA_RSP       0x00CFu
#define CMD_802_11_KEY_MATERIAL 0x005Eu
#define CMD_FUNC_INIT           0x00A9u
#define CMD_FUNC_SHUTDOWN       0x00AAu
#define CMD_PCIE_ADMA_INIT      0x0284u
#define CMD_RET_BIT             0x8000u

/* MAC_CONTROL action bits */
#define MAC_ACT_RX_ON           0x0001u
#define MAC_ACT_TX_ON           0x0002u
#define MAC_ACT_WEP_ENABLE      0x0008u
#define MAC_ACT_ETHERNETII_EN   0x0010u
#define MAC_ACT_PROMISCUOUS     0x0080u
#define MAC_ACT_ALL_MULTICAST     0x0100u
#define MAC_ACT_RTS_CTS_EN        0x0200u
#define MAC_ACT_STRICT_PROTECT    0x0400u
#define MAC_ACT_STATIC_DYNAMIC_BW 0x00010000u

/* TLV IDs */
#define TLV_TYPE_SSID           0x0000u
#define TLV_TYPE_RATES          0x0001u
#define TLV_TYPE_PHY_DS         0x0003u
#define TLV_TYPE_CF             0x0004u
#define TLV_TYPE_RSN            0x0030u
#define TLV_TYPE_CHANLIST       0x0101u
#define TLV_TYPE_AUTH_TYPE      0x011Fu
#define TLV_TYPE_KEY_MATERIAL   0x010Eu
#define TLV_TYPE_KEY_PARAM_V2   0x019Cu  /* PROPRIETARY_TLV_BASE_ID + 156 */
#define TLV_TYPE_VHT_CAP        191u
#define TLV_TYPE_OPER_MODE_NTF  199u
#define TLV_TYPE_EXTENSION_ID   255u
#define EXT_ID_HE_CAP           35u
#define PKT_TYPE_AMSDU          0x00E6u

/* Key types & info for CMD_802_11_KEY_MATERIAL (v2) */
#define KEY_TYPE_ID_WEP         0x00u
#define KEY_TYPE_ID_TKIP        0x01u
#define KEY_TYPE_ID_AES         0x02u
#define KEY_INFO_MCAST_KEY      0x0001u
#define KEY_INFO_UCAST_KEY      0x0002u
#define KEY_INFO_ENABLE_KEY     0x0004u
#define KEY_INFO_DEFAULT_KEY    0x0008u
#define KEY_INFO_TX_KEY         0x0010u
#define KEY_INFO_RX_KEY         0x0020u

/* Firmware events */
#define EVENT_DEAUTHENTICATED   0x00000003u
#define EVENT_DISASSOCIATED     0x00000004u
#define EVENT_LINK_LOST         0x00000005u
#define EVENT_ADDBA             0x00000033u

#pragma pack(push, 1)

typedef struct _ADMA_DESC {
    USHORT len;
    USHORT flags;
    USHORT pkt_size;
    USHORT reserved;
    ULONG64 paddr;
} ADMA_DESC;

typedef struct _PCIE_IF_HDR {
    USHORT len;
    USHORT type;
} PCIE_IF_HDR;

typedef struct _HOST_CMD_HDR {
    USHORT command;
    USHORT size;
    USHORT seq_num;
    USHORT result;
} HOST_CMD_HDR;

typedef struct _CMD_ADMA_INIT_BODY {
    USHORT tx_ring_size;
    USHORT rx_ring_size;
    USHORT evt_ring_size;
    UCHAR  int_mode;
    UCHAR  reserved;
} CMD_ADMA_INIT_BODY;

typedef struct _CMD_GET_HW_SPEC_BODY {
    USHORT hw_if_version;
    USHORT version;
    USHORT fw_country_code;
    USHORT num_of_mcast_adr;
    UCHAR  permanent_addr[6];
    USHORT region_code;
    USHORT number_of_antenna;
    ULONG  fw_release_number;
    ULONG  hw_dev_cap;
    ULONG  reserved_2;
    ULONG  reserved_3;
    ULONG  fw_cap_info;
    ULONG  dot_11n_dev_cap;
    UCHAR  dev_mcs_support;
    USHORT mp_end_port;
    USHORT mgmt_buf_count;
    ULONG  reserved_8;
    ULONG  reserved_9;
    ULONG  dot_11ac_dev_cap;
    ULONG  dot_11ac_mcs_support;
} CMD_GET_HW_SPEC_BODY;

typedef struct _CMD_MAC_CONTROL_BODY {
    ULONG action;
} CMD_MAC_CONTROL_BODY;

typedef struct _CMD_SCAN_REQ_HDR {
    UCHAR bss_mode;
    UCHAR bssid[6];
    /* Followed by TLVs */
} CMD_SCAN_REQ_HDR;

typedef struct _CMD_SCAN_RSP_HDR {
    USHORT bss_descript_size;
    UCHAR  number_of_sets;
    /* Followed by BSS descriptors */
} CMD_SCAN_RSP_HDR;

typedef struct _CMD_ASSOC_REQ_HDR {
    UCHAR  peer_sta_addr[6];
    USHORT cap_info;
    USHORT listen_interval;
    USHORT beacon_period;
    UCHAR  dtim_period;
    /* Followed by TLVs */
} CMD_ASSOC_REQ_HDR;

typedef struct _CMD_ASSOC_RSP_HDR {
    USHORT cap_info;
    USHORT status_code;
    USHORT aid;
} CMD_ASSOC_RSP_HDR;

typedef struct _CMD_DEAUTH_BODY {
    UCHAR  mac_addr[6];
    USHORT reason_code;
} CMD_DEAUTH_BODY;

typedef struct _TLV_HDR {
    USHORT type;
    USHORT len;
} TLV_HDR;

typedef struct _CHAN_SCAN_PARAM {
    UCHAR  radio_type;   /* 0 = 2.4 GHz, 1 = 5 GHz */
    UCHAR  chan_number;
    UCHAR  scan_type;    /* 0 = active, 1 = passive */
    USHORT min_scan_time;
    USHORT max_scan_time;
} CHAN_SCAN_PARAM;

typedef struct _IW620_TXPD {
    UCHAR  bss_type;
    UCHAR  bss_num;
    USHORT tx_pkt_length;
    USHORT tx_pkt_offset;
    USHORT tx_pkt_type;
    ULONG  tx_control;
    UCHAR  priority;
    UCHAR  flags;
    UCHAR  pkt_delay_2ms;
    UCHAR  reserved;
    ULONG  tx_control_1;
    UCHAR  ra_mac[6];
    UCHAR  reserved3[2];
} IW620_TXPD;

typedef struct _IW620_RXPD {
    UCHAR   bss_type;
    UCHAR   bss_num;
    USHORT  rx_pkt_length;
    USHORT  rx_pkt_offset;
    USHORT  rx_pkt_type;
    USHORT  seq_num;
    UCHAR   priority;
    UCHAR   rx_rate;
    CHAR    snr;
    CHAR    nf;
    UCHAR   rate_info;
    UCHAR   reserved[3];
    UCHAR   flags;
    UCHAR   antenna;
    ULONG64 toa_tod_tstamps;
    ULONG   rx_info;
    UCHAR   reserved3[7];
    UCHAR   reserved4;
} IW620_RXPD;

/* mwifiex_ie_type_key_param_set_v2 (after the TLV header) with the AES
 * parameter block: 10 fixed bytes + PN, key length and key. */
#define KEY_PARAMS_FIXED_LEN    10u
typedef struct _KEY_PARAM_V2 {
    UCHAR  mac_addr[6];
    UCHAR  key_idx;
    UCHAR  key_type;
    USHORT key_info;
    UCHAR  pn[8];
    USHORT key_len;
    UCHAR  key[32];
} KEY_PARAM_V2;

#pragma pack(pop)

/* UEFI Runtime Variable "Ps5WifiFw" published by PSWloader UEFI */
typedef struct _PS5_WIFI_FW_VAR {
    ULONG64 PhysAddr;
    ULONG   Size;
    ULONG   Crc32;
} PS5_WIFI_FW_VAR;

#endif /* PS5WIFI_IW620_HW_H */
