/* SPDX-License-Identifier: GPL-2.0-only
 * ps5wifi v1.0.5 - NDIS 6.20 Native 802.11 miniport for the PS5 Wi-Fi 6 adapter
 * (NXP/Marvell 88W9097 "IW620", PCI 1B4B:2B56) on native Windows 11.
 *
 * Hardware programming model follows nxp-imx/mwifiex (lf-6.18.2_1.0.0) and the
 * ps5-linux-mwifiex patches (ADMA dual-descriptor rings, 128-byte minimum TX DMA
 * padding, fresh-FW FLR reset, connected-state scan caching).
 */
#include <ndis.h>
#include <windot11.h>
#include "iw620_hw.h"

#define PS5WIFI_TAG         'ifw5'
#define PS5WIFI_VERSION     0x00010000u

#define TX_RING_SIZE        32u
#define TX_RING_BITS        5u
#define RX_RING_SIZE        32u
#define RX_RING_BITS        5u
#define EVT_RING_SIZE       8u
#define EVT_RING_BITS       3u

#define TX_BUF_STRIDE       2048u
#define RX_BUF_STRIDE       2048u
#define EVT_BUF_STRIDE      1024u
#define CMD_BUF_SIZE        2048u
#define POLL_MS             2u

#define MAX_BSS_ENTRIES     64u
#define MAX_BSS_IE_BYTES    1024u

/* Reloc anchor so lld-link always emits .reloc (avoids Code 39) */
static void * volatile g_reloc_anchor = (void *)&g_reloc_anchor;

/* UEFI runtime variable GUID for Ps5WifiFw */
static const GUID g_wifi_fw_guid = {
    0x7c8e51b2, 0x4a19, 0x4e7c, { 0x9b, 0x31, 0x50, 0x53, 0x35, 0x57, 0x46, 0x57 }
};

typedef struct _BSS_ENTRY {
    UCHAR   bssid[6];
    UCHAR   ssid[32];
    ULONG   ssid_len;
    USHORT  cap_info;
    USHORT  beacon_period;
    ULONG64 timestamp;
    LONG    rssi;
    ULONG   channel;
    ULONG   freq_khz;
    ULONG   phy_id;
    ULONG   ie_len;
    UCHAR   ie[MAX_BSS_IE_BYTES];
    BOOLEAN valid;
} BSS_ENTRY;

typedef struct _WIFI_ADAPTER {
    NDIS_HANDLE handle;
    NDIS_HANDLE dma;
    NDIS_HANDLE nbl_pool;
    NDIS_HANDLE timer;
    NDIS_SPIN_LOCK lock;

    volatile UCHAR *bar0;
    ULONG bar0_len;
    volatile UCHAR *bar2;
    ULONG bar2_len;

    /* Shared DMA memory */
    ADMA_DESC *tx_ring;     NDIS_PHYSICAL_ADDRESS tx_ring_pa;
    ADMA_DESC *rx_ring;     NDIS_PHYSICAL_ADDRESS rx_ring_pa;
    ADMA_DESC *evt_ring;    NDIS_PHYSICAL_ADDRESS evt_ring_pa;

    UCHAR *tx_pool;         NDIS_PHYSICAL_ADDRESS tx_pool_pa;
    UCHAR *rx_pool;         NDIS_PHYSICAL_ADDRESS rx_pool_pa;
    UCHAR *evt_pool;        NDIS_PHYSICAL_ADDRESS evt_pool_pa;
    UCHAR *cmd_buf;         NDIS_PHYSICAL_ADDRESS cmd_buf_pa;
    UCHAR *cmdrsp_buf;      NDIS_PHYSICAL_ADDRESS cmdrsp_buf_pa;

    PMDL rx_mdl[RX_RING_SIZE];
    PNET_BUFFER_LIST rx_nbl[RX_RING_SIZE];
    UCHAR rx_dot11_buf[RX_RING_SIZE][2048];

    ULONG tx_wr, tx_rd;
    ULONG rx_rd, rx_wr;
    ULONG evt_rd, evt_wr;
    USHORT cmd_seq;
    KSPIN_LOCK tx_lock;

    /* 802.11 state */
    UCHAR perm_mac[6];
    UCHAR cur_mac[6];
    UCHAR assoc_bssid[6];
    UCHAR desired_ssid[32];
    ULONG desired_ssid_len;
    ULONG op_mode;
    ULONG cur_phy_id;
    ULONG cur_channel;
    ULONG packet_filter;
    ULONG lookahead;
    BOOLEAN radio_on;
    BOOLEAN exclude_unencrypted;
    BOOLEAN assoc_primed;
    ULONG auto_config;
    DOT11_BSS_TYPE desired_bss_type;
    DOT11_AUTH_ALGORITHM auth_algo;
    DOT11_CIPHER_ALGORITHM ucast_cipher;
    DOT11_CIPHER_ALGORITHM mcast_cipher;
    ULONG default_key_id;
    BOOLEAN connected;
    BOOLEAN gtk_set;
    BOOLEAN assoc_in_progress;
    LONG paused;
    LONG halting;

    /* BSS scan cache */
    BSS_ENTRY bss[MAX_BSS_ENTRIES];
    ULONG bss_count;

    /* Counters & diagnostics */
    ULONG hw_ready;
    ULONG fw_loaded;
    ULONG fw_size;
    ULONG hw_rev;
    ULONG scan_count;
    ULONG last_err;
    ULONG64 tx_ok, rx_ok, tx_bytes, rx_bytes, tx_drop, rx_err;
} WIFI_ADAPTER, *PWIFI_ADAPTER;

static NDIS_HANDLE g_driver;

/* ---- Low-level MMIO & utility helpers ---------------------------------------- */
static __forceinline ULONG r32(PWIFI_ADAPTER a, ULONG off) {
    return READ_REGISTER_ULONG((volatile ULONG *)(a->bar2 + off));
}
static __forceinline VOID w32(PWIFI_ADAPTER a, ULONG off, ULONG val) {
    WRITE_REGISTER_ULONG((volatile ULONG *)(a->bar2 + off), val);
}

static VOID delay_us(ULONG us) {
    NdisStallExecution(us);
}

static ULONG crc32_calc(const UCHAR *data, ULONG len) {
    ULONG c = 0xFFFFFFFFu;
    for (ULONG i = 0; i < len; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static ULONG chan_to_freq_khz(ULONG ch) {
    if (ch >= 1 && ch <= 13) return (2407u + ch * 5u) * 1000u;
    if (ch == 14) return 2484000u;
    if (ch >= 36 && ch <= 177) return (5000u + ch * 5u) * 1000u;
    return 2412000u;
}

static VOID diag_write_dword(PCWSTR name, ULONG val) {
    UNICODE_STRING path, vname;
    OBJECT_ATTRIBUTES oa;
    HANDLE hk = NULL;
    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\ps5wifi\\Parameters");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateKey(&hk, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        RtlInitUnicodeString(&vname, name);
        ZwSetValueKey(hk, &vname, 0, REG_DWORD, &val, sizeof(val));
        ZwClose(hk);
    }
}

/* ---- Firmware acquisition (UEFI Ps5WifiFw variable -> file fallback) ---------- */
static NTSTATUS load_firmware_image(NDIS_HANDLE handle, UCHAR **out_buf, ULONG *out_len) {
    *out_buf = NULL;
    *out_len = 0;

    /* 1. Try UEFI runtime variable Ps5WifiFw (populated by PSWloader.elf + UEFI) */
    {
        UNICODE_STRING vname;
        PS5_WIFI_FW_VAR var;
        ULONG sz = sizeof(var);
        ULONG attr = 0;
        RtlInitUnicodeString(&vname, L"Ps5WifiFw");
        NTSTATUS st = ExGetFirmwareEnvironmentVariable(&vname, (LPGUID)&g_wifi_fw_guid, &var, &sz, &attr);
        if (NT_SUCCESS(st) && sz == sizeof(var) && var.PhysAddr && var.Size >= 0x1000 && var.Size <= 0x100000) {
            PHYSICAL_ADDRESS pa;
            pa.QuadPart = (LONGLONG)var.PhysAddr;
            PVOID mapped = MmMapIoSpace(pa, var.Size, MmNonCached);
            if (mapped) {
                UCHAR *buf = (UCHAR *)NdisAllocateMemoryWithTagPriority(handle, var.Size, PS5WIFI_TAG, NormalPoolPriority);
                if (buf) {
                    RtlCopyMemory(buf, mapped, var.Size);
                    MmUnmapIoSpace(mapped, var.Size);
                    if (crc32_calc(buf, var.Size) == var.Crc32) {
                        *out_buf = buf;
                        *out_len = var.Size;
                        diag_write_dword(L"FwSource", 1);
                        return STATUS_SUCCESS;
                    }
                    NdisFreeMemory(buf, var.Size, 0);
                } else {
                    MmUnmapIoSpace(mapped, var.Size);
                }
            }
        }
    }

    /* 2. Fallback: \SystemRoot\System32\drivers\ps5wifi_fw.bin */
    {
        UNICODE_STRING fname;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        HANDLE hf = NULL;
        RtlInitUnicodeString(&fname, L"\\SystemRoot\\System32\\drivers\\ps5wifi_fw.bin");
        InitializeObjectAttributes(&oa, &fname, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        NTSTATUS st = ZwCreateFile(&hf, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                                   FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
        if (!NT_SUCCESS(st)) return st;

        FILE_STANDARD_INFORMATION fsi;
        RtlZeroMemory(&fsi, sizeof(fsi));
        st = ZwQueryInformationFile(hf, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
        if (!NT_SUCCESS(st) || fsi.EndOfFile.QuadPart < 0x1000 || fsi.EndOfFile.QuadPart > 0x100000) {
            ZwClose(hf);
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        ULONG len = (ULONG)fsi.EndOfFile.QuadPart;
        UCHAR *buf = (UCHAR *)NdisAllocateMemoryWithTagPriority(handle, len, PS5WIFI_TAG, NormalPoolPriority);
        if (!buf) {
            ZwClose(hf);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        LARGE_INTEGER off;
        off.QuadPart = 0;
        st = ZwReadFile(hf, NULL, NULL, NULL, &iosb, buf, len, &off, NULL);
        ZwClose(hf);
        if (!NT_SUCCESS(st) || iosb.Information != len) {
            NdisFreeMemory(buf, len, 0);
            return STATUS_UNSUCCESSFUL;
        }
        *out_buf = buf;
        *out_len = len;
        diag_write_dword(L"FwSource", 2);
        return STATUS_SUCCESS;
    }
}

/* ---- IW620 Firmware Download (wlan_pcie_prog_fw_w_helper) -------------------- */
static BOOLEAN iw620_download_fw(PWIFI_ADAPTER a) {
    ULONG st3 = r32(a, REG_SCRATCH_3);
    diag_write_dword(L"InitialScratch3", st3);
    if (st3 == FIRMWARE_READY_PCIE) {
        a->fw_loaded = 1;
        return TRUE;
    }

    UCHAR *fw = NULL;
    ULONG fw_len = 0;
    if (!NT_SUCCESS(load_firmware_image(a->handle, &fw, &fw_len))) {
        a->last_err = 0x1001;
        return FALSE;
    }
    a->fw_size = fw_len;
    diag_write_dword(L"FwSize", fw_len);

    /* Disable host interrupts during FW download */
    w32(a, REG_HOST_INT_MASK, 0);
    w32(a, REG_HOST_INT_STAT_MASK, 0);

    /* Reuse cmd_buf (2048B) or tx_pool (64KB) as physically contiguous DMA staging */
    UCHAR *dma_va = a->tx_pool;
    ULONG64 dma_pa = (ULONG64)a->tx_pool_pa.QuadPart;

    ULONG offset = 0;
    ULONG retries = 0;
    BOOLEAN ok = FALSE;

    while (offset < fw_len && retries < 20) {
        ULONG len = 0;
        for (ULONG t = 0; t < 50000; t++) {
            len = r32(a, REG_SCRATCH_2);
            if (len || offset) break;
            delay_us(10);
        }
        if (len == 0) {
            ok = TRUE;
            break;
        }
        if (len > WLAN_UPLD_SIZE) {
            a->last_err = 0x1002;
            break;
        }
        if (len & 1u) {
            ULONG fstat = r32(a, REG_SCRATCH_7);
            ULONG foff = r32(a, REG_SCRATCH_6);
            retries++;
            if (fstat & 0xC0u) {
                offset = 0;
            } else if (foff < fw_len) {
                offset = foff;
            }
            len &= ~1u;
            if (!len) continue;
        }
        ULONG txlen = len;
        if (offset + txlen > fw_len) txlen = fw_len - offset;
        RtlCopyMemory(dma_va, fw + offset, txlen);
        if (txlen < len) RtlZeroMemory(dma_va + txlen, len - txlen);

        w32(a, REG_SCRATCH_0, (ULONG)(dma_pa & 0xFFFFFFFFULL));
        w32(a, REG_SCRATCH_1, (ULONG)(dma_pa >> 32));
        w32(a, REG_SCRATCH_2, len);
        w32(a, REG_CPU_INT_EVENT, CPU_INTR_DOOR_BELL);

        BOOLEAN ack = FALSE;
        for (ULONG t = 0; t < 100000; t++) {
            if ((r32(a, REG_CPU_INT_STATUS) & CPU_INTR_DOOR_BELL) == 0) {
                ack = TRUE;
                break;
            }
            delay_us(10);
        }
        if (!ack) {
            a->last_err = 0x1003;
            break;
        }
        offset += txlen;
    }

    NdisFreeMemory(fw, fw_len, 0);
    if (!ok && offset < fw_len) return FALSE;

    /* Wait for FIRMWARE_READY_PCIE in SCRATCH_3 (up to 5s) */
    for (ULONG t = 0; t < 5000; t++) {
        if (r32(a, REG_SCRATCH_3) == FIRMWARE_READY_PCIE) {
            a->fw_loaded = 1;
            return TRUE;
        }
        delay_us(1000);
    }
    a->last_err = 0x1004;
    return FALSE;
}

/* ---- ADMA Queue Initialization (wlan_init_adma) ------------------------------ */
static VOID iw620_init_adma_q(PWIFI_ADAPTER a, ULONG q_addr, BOOLEAN h2d,
                              BOOLEAN dual_desc, ULONG64 pbase, USHORT size, BOOLEAN init)
{
    if (init) {
        ULONG imap = 0;
        if (dual_desc) imap = h2d ? DEST_INT_TO_DEVICE : DEST_INT_TO_HOST;
        w32(a, q_addr + ADMA_INT_MAPPING, imap);
    }

    if (h2d) {
        w32(a, q_addr + ADMA_SRC_LOW, (ULONG)(pbase & 0xFFFFFFFFULL));
        w32(a, q_addr + ADMA_SRC_HIGH, (ULONG)(pbase >> 32));
        if (init) {
            w32(a, q_addr + ADMA_SRC_INT_STAT_MASK, DEF_ADMA_INT_MASK);
            w32(a, q_addr + ADMA_SRC_INT_MASK, DEF_ADMA_INT_MASK);
        }
    } else {
        w32(a, q_addr + ADMA_DST_LOW, (ULONG)(pbase & 0xFFFFFFFFULL));
        w32(a, q_addr + ADMA_DST_HIGH, (ULONG)(pbase >> 32));
        if (init && dual_desc) {
            w32(a, q_addr + ADMA_DST_INT_STAT_MASK, DEF_ADMA_INT_MASK);
            w32(a, q_addr + ADMA_DST_INT_MASK, DEF_ADMA_INT_MASK);
        }
    }

    if (init) {
        ULONG cfg2 = r32(a, q_addr + ADMA_DMA_CFG2);
        cfg2 &= ~(ADMA_SRC_BYPASS_EN | ADMA_DST_BYPASS_EN |
                  ADMA_MSI_SRC_BYPASS_EN | ADMA_MSI_DST_BYPASS_EN |
                  ADMA_MSI_LEGACY_EN | (1u << 18) | (1u << 20));
        if (dual_desc) {
            if (h2d) {
                cfg2 |= ADMA_SRC_BYPASS_EN | ADMA_MSI_SRC_BYPASS_EN;
            } else {
                ULONG cfg3 = r32(a, q_addr + ADMA_DMA_CFG3);
                w32(a, q_addr + ADMA_DMA_CFG3, cfg3 | ADMA_Q_PTR_CLR);
                cfg2 |= ADMA_DST_BYPASS_EN | ADMA_MSI_DST_BYPASS_EN;
            }
        } else {
            cfg2 |= h2d ? ADMA_SRC_ADDR_IS_HOST : ADMA_DST_ADDR_IS_HOST;
        }
        cfg2 |= ADMA_MSI_LEGACY_EN;
        w32(a, q_addr + ADMA_DMA_CFG2, cfg2);
    }

    ULONG cfg = r32(a, q_addr + ADMA_DMA_CFG);
    if (h2d) {
        if (dual_desc) {
            cfg &= ~(0x0004u | 0x0003u | 0x0F00u | 0xFFFF0000u);
            cfg |= ((ULONG)size << 8);
        } else {
            cfg &= ~(0x0004u | 0x0003u | 0x0F00u | 0xF000u | 0xFFFF0000u);
            cfg |= ((ULONG)size << 16);
        }
    } else {
        if (dual_desc) {
            cfg &= ~(0x0004u | 0x0003u | 0xF000u | 0xFFFF0000u);
            cfg |= ((ULONG)size << 12);
        } else {
            cfg &= ~(0x0004u | 0x0003u | 0x0F00u | 0xF000u);
        }
    }
    cfg |= (dual_desc ? DMA_MODE_DUAL_DESC : DMA_MODE_DIRECT);
    w32(a, q_addr + ADMA_DMA_CFG, cfg);

    if (q_addr == ADMA_CHAN2_Q0 && !init) {
        w32(a, q_addr + ADMA_SRC_RW_PTR, 1);
    }
}

static VOID iw620_setup_rings(PWIFI_ADAPTER a) {
    RtlZeroMemory(a->tx_ring, TX_RING_SIZE * sizeof(ADMA_DESC));
    RtlZeroMemory(a->rx_ring, RX_RING_SIZE * sizeof(ADMA_DESC));
    RtlZeroMemory(a->evt_ring, EVT_RING_SIZE * sizeof(ADMA_DESC));

    for (ULONG i = 0; i < RX_RING_SIZE; i++) {
        a->rx_ring[i].paddr = (ULONG64)a->rx_pool_pa.QuadPart + (ULONG64)i * RX_BUF_STRIDE;
        a->rx_ring[i].len = (USHORT)RX_BUF_STRIDE;
        a->rx_ring[i].pkt_size = 0;
        a->rx_ring[i].flags = ADMA_BD_FLAG_INT_EN | ADMA_BD_FLAG_DST_HOST;
    }
    for (ULONG i = 0; i < EVT_RING_SIZE; i++) {
        a->evt_ring[i].paddr = (ULONG64)a->evt_pool_pa.QuadPart + (ULONG64)i * EVT_BUF_STRIDE;
        a->evt_ring[i].len = (USHORT)EVT_BUF_STRIDE;
        a->evt_ring[i].pkt_size = 0;
        a->evt_ring[i].flags = ADMA_BD_FLAG_INT_EN | ADMA_BD_FLAG_DST_HOST;
    }

    a->tx_wr = 0; a->tx_rd = 0;
    a->rx_rd = 0; a->rx_wr = RX_RING_SIZE;
    a->evt_rd = 0; a->evt_wr = EVT_RING_SIZE;

    iw620_init_adma_q(a, ADMA_CHAN0_Q0, TRUE,  TRUE,  (ULONG64)a->tx_ring_pa.QuadPart,  TX_RING_BITS,  TRUE);
    iw620_init_adma_q(a, ADMA_CHAN1_Q0, FALSE, TRUE,  (ULONG64)a->rx_ring_pa.QuadPart,  RX_RING_BITS,  TRUE);
    iw620_init_adma_q(a, ADMA_CHAN1_Q1, FALSE, TRUE,  (ULONG64)a->evt_ring_pa.QuadPart, EVT_RING_BITS, TRUE);
    iw620_init_adma_q(a, ADMA_CHAN2_Q0, TRUE,  FALSE, (ULONG64)a->cmd_buf_pa.QuadPart, CMD_BUF_SIZE,  TRUE);
    iw620_init_adma_q(a, ADMA_CHAN2_Q1, FALSE, FALSE, (ULONG64)a->cmdrsp_buf_pa.QuadPart, CMD_BUF_SIZE, TRUE);

    /* Arm RX and Event rings (full ring posted: wrptr = ring_size, 13-bit wrap counter) */
    w32(a, ADMA_CHAN1_Q0 + ADMA_DST_RW_PTR, a->rx_wr);
    w32(a, ADMA_CHAN1_Q1 + ADMA_DST_RW_PTR, a->evt_wr);

    /* Enable status mask for polling (keep host IRQ mask masked since we poll) */
    w32(a, REG_HOST_INT_CLR_SEL, 0);
    w32(a, REG_HOST_INT_STAT_MASK, ADMA_HOST_INTR_ALL);
    w32(a, REG_HOST_INT_MASK, 0);
    w32(a, REG_DRV_READY, DRV_READY_MAGIC);
}

/* ---- Synchronous Host Command execution -------------------------------------- */
static BOOLEAN iw620_exec_cmd(PWIFI_ADAPTER a, USHORT cmd_id, const void *body, USHORT body_len,
                              void *rsp_body, USHORT *rsp_body_len, ULONG timeout_ms)
{
    USHORT cmd_size = (USHORT)(sizeof(HOST_CMD_HDR) + body_len);
    USHORT total_len = (USHORT)(sizeof(PCIE_IF_HDR) + cmd_size);
    if (total_len > CMD_BUF_SIZE) return FALSE;

    RtlZeroMemory(a->cmd_buf, total_len);
    RtlZeroMemory(a->cmdrsp_buf, CMD_BUF_SIZE);

    PCIE_IF_HDR *ifh = (PCIE_IF_HDR *)a->cmd_buf;
    ifh->len = total_len;
    ifh->type = MLAN_TYPE_CMD;

    HOST_CMD_HDR *hdr = (HOST_CMD_HDR *)(a->cmd_buf + sizeof(PCIE_IF_HDR));
    hdr->command = cmd_id;
    hdr->size = cmd_size;
    hdr->seq_num = ++a->cmd_seq;
    hdr->result = 0;
    if (body && body_len) {
        RtlCopyMemory(a->cmd_buf + sizeof(PCIE_IF_HDR) + sizeof(HOST_CMD_HDR), body, body_len);
    }

    /* Clear any stale CMD_DONE status (REG_HOST_INT_STATUS is write-0-to-clear on IW620) */
    w32(a, REG_HOST_INT_STATUS, ~(ADMA_INT_CMD_DONE | ADMA_INT_CMD_DNLD));
    w32(a, ADMA_CHAN2_Q1 + ADMA_DST_INT_STATUS, DEF_ADMA_INT_MASK);

    /* Arm response buffer and trigger direct ADMA command */
    iw620_init_adma_q(a, ADMA_CHAN2_Q1, FALSE, FALSE, (ULONG64)a->cmdrsp_buf_pa.QuadPart, CMD_BUF_SIZE, FALSE);
    iw620_init_adma_q(a, ADMA_CHAN2_Q0, TRUE,  FALSE, (ULONG64)a->cmd_buf_pa.QuadPart, total_len, FALSE);

    /* Poll for CMD_DONE */
    ULONG polls = timeout_ms * 10;
    BOOLEAN got = FALSE;
    for (ULONG i = 0; i < polls; i++) {
        ULONG st = r32(a, REG_HOST_INT_STATUS);
        if (st & ADMA_INT_CMD_DONE) {
            w32(a, REG_HOST_INT_STATUS, ~st);
            w32(a, ADMA_CHAN2_Q1 + ADMA_DST_INT_STATUS, DEF_ADMA_INT_MASK);
            got = TRUE;
            break;
        }
        delay_us(100);
    }
    if (!got) {
        a->last_err = 0x2000u | cmd_id;
        return FALSE;
    }

    HOST_CMD_HDR *rhdr = (HOST_CMD_HDR *)(a->cmdrsp_buf + sizeof(PCIE_IF_HDR));
    if ((rhdr->command & 0x7FFFu) != cmd_id || rhdr->result != 0) {
        a->last_err = 0x3000u | (rhdr->result & 0xFFu);
        return FALSE;
    }

    if (rsp_body && rsp_body_len) {
        USHORT avail = (rhdr->size > sizeof(HOST_CMD_HDR)) ? (USHORT)(rhdr->size - sizeof(HOST_CMD_HDR)) : 0;
        USHORT copy = (avail < *rsp_body_len) ? avail : *rsp_body_len;
        if (copy) RtlCopyMemory(rsp_body, a->cmdrsp_buf + sizeof(PCIE_IF_HDR) + sizeof(HOST_CMD_HDR), copy);
        *rsp_body_len = copy;
    }
    return TRUE;
}

static BOOLEAN iw620_hw_init(PWIFI_ADAPTER a) {
    a->hw_rev = r32(a, REG_REV_ID);
    diag_write_dword(L"HwRev", a->hw_rev);

    if (!iw620_download_fw(a)) {
        diag_write_dword(L"Stage", 1);
        diag_write_dword(L"LastErr", a->last_err);
        return FALSE;
    }
    diag_write_dword(L"Stage", 2);

    iw620_setup_rings(a);
    diag_write_dword(L"Stage", 3);

    /* 1. CMD_FUNC_INIT (IW620 initializes ADMA rings via MMIO directly, no CMD_PCIE_ADMA_INIT) */
    if (!iw620_exec_cmd(a, CMD_FUNC_INIT, NULL, 0, NULL, NULL, 1000)) {
        diag_write_dword(L"Stage", 4);
        diag_write_dword(L"LastErr", a->last_err);
        /* Continue anyway: some FW revisions return 0 or already-initialized */
    }
    /* Sync TX ptr & re-arm RX/Event write pointers after CMD_FUNC_INIT */
    {
        ULONG hw_tx_rd = (r32(a, ADMA_CHAN0_Q0 + ADMA_SRC_RW_PTR) >> 16) & 0x1FFFu;
        a->tx_wr = hw_tx_rd;
        a->tx_rd = hw_tx_rd;
        w32(a, ADMA_CHAN0_Q0 + ADMA_SRC_RW_PTR, a->tx_wr);
    }
    w32(a, ADMA_CHAN1_Q0 + ADMA_DST_RW_PTR, a->rx_wr);
    w32(a, ADMA_CHAN1_Q1 + ADMA_DST_RW_PTR, a->evt_wr);

    /* 2. CMD_GET_HW_SPEC -> real MAC address (retry up to 3x for clean hot-reload) */
    CMD_GET_HW_SPEC_BODY spec;
    USHORT spec_len = sizeof(spec);
    BOOLEAN got_spec = FALSE;
    for (int tries = 0; tries < 3; tries++) {
        spec_len = sizeof(spec);
        RtlZeroMemory(&spec, sizeof(spec));
        if (iw620_exec_cmd(a, CMD_GET_HW_SPEC, &spec, sizeof(spec), &spec, &spec_len, 1000)) {
            got_spec = TRUE;
            break;
        }
        w32(a, REG_HOST_INT_STATUS, 0);
        delay_us(20000);
    }
    if (!got_spec) {
        diag_write_dword(L"Stage", 5);
        diag_write_dword(L"LastErr", a->last_err);
        return FALSE;
    }
    RtlCopyMemory(a->perm_mac, spec.permanent_addr, 6);
    RtlCopyMemory(a->cur_mac, spec.permanent_addr, 6);
    diag_write_dword(L"MacLo", (ULONG)a->perm_mac[2] | ((ULONG)a->perm_mac[3] << 8) |
                               ((ULONG)a->perm_mac[4] << 16) | ((ULONG)a->perm_mac[5] << 24));
    diag_write_dword(L"MacHi", (ULONG)a->perm_mac[0] | ((ULONG)a->perm_mac[1] << 8));

    /* 3. CMD_MAC_CONTROL (RX + TX + Ethernet-II + All-Multicast) */
    CMD_MAC_CONTROL_BODY mc;
    mc.action = MAC_ACT_RX_ON | MAC_ACT_TX_ON | MAC_ACT_ETHERNETII_EN | MAC_ACT_ALL_MULTICAST;
    (VOID)iw620_exec_cmd(a, CMD_MAC_CONTROL, &mc, sizeof(mc), NULL, NULL, 1000);

    a->hw_ready = 1;
    diag_write_dword(L"Stage", 10);
    diag_write_dword(L"LastErr", 0);
    return TRUE;
}

/* ---- Scan & BSS Parsing ------------------------------------------------------ */
static VOID parse_scan_bss_list(PWIFI_ADAPTER a, const UCHAR *rsp, USHORT rsp_len) {
    if (rsp_len < sizeof(CMD_SCAN_RSP_HDR)) return;
    const CMD_SCAN_RSP_HDR *sh = (const CMD_SCAN_RSP_HDR *)rsp;
    const UCHAR *p = rsp + sizeof(CMD_SCAN_RSP_HDR);
    const UCHAR *end = rsp + sizeof(CMD_SCAN_RSP_HDR) + sh->bss_descript_size;
    if (end > rsp + rsp_len) end = rsp + rsp_len;

    for (ULONG s = 0; s < sh->number_of_sets && p + 2 <= end; s++) {
        USHORT bss_sz = *(const USHORT *)p;
        p += 2;
        if (bss_sz < 19 || p + bss_sz > end) break;
        const UCHAR *entry_end = p + bss_sz;

        UCHAR bssid[6];
        RtlCopyMemory(bssid, p, 6);
        LONG rssi = -(LONG)p[6];
        ULONG64 ts = *(const ULONG64 *)(p + 7);
        USHORT bcn = *(const USHORT *)(p + 15);
        USHORT cap = *(const USHORT *)(p + 17);
        const UCHAR *ies = p + 19;
        ULONG ie_len = (ULONG)(entry_end - ies);
        p = entry_end;

        /* Find existing or free slot */
        ULONG idx = MAX_BSS_ENTRIES;
        for (ULONG i = 0; i < a->bss_count; i++) {
            if (RtlEqualMemory(a->bss[i].bssid, bssid, 6)) { idx = i; break; }
        }
        if (idx == MAX_BSS_ENTRIES) {
            if (a->bss_count >= MAX_BSS_ENTRIES) continue;
            idx = a->bss_count++;
        }

        BSS_ENTRY *b = &a->bss[idx];
        RtlZeroMemory(b, sizeof(*b));
        RtlCopyMemory(b->bssid, bssid, 6);
        b->rssi = rssi;
        b->timestamp = ts;
        b->beacon_period = bcn ? bcn : 100;
        b->cap_info = cap;
        b->channel = 1;
        b->ie_len = (ie_len <= MAX_BSS_IE_BYTES) ? ie_len : MAX_BSS_IE_BYTES;
        if (b->ie_len) RtlCopyMemory(b->ie, ies, b->ie_len);

        /* Parse SSID (IE 0) and DS Channel (IE 3) */
        const UCHAR *ie = ies;
        while (ie + 2 <= ies + ie_len) {
            UCHAR id = ie[0], ilen = ie[1];
            if (ie + 2 + ilen > ies + ie_len) break;
            if (id == 0 && ilen <= 32) {
                b->ssid_len = ilen;
                if (ilen) RtlCopyMemory(b->ssid, ie + 2, ilen);
            } else if (id == 3 && ilen >= 1) {
                b->channel = ie[2];
            }
            ie += 2 + ilen;
        }
        b->freq_khz = chan_to_freq_khz(b->channel);
        b->phy_id = (b->channel > 14) ? 1 : 0;
        b->valid = TRUE;
    }
}

static VOID iw620_do_scan(PWIFI_ADAPTER a) {
    static const UCHAR ch24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    static const UCHAR ch5[]  = { 36, 40, 44, 48, 149, 153, 157, 161 };
    UCHAR req[256];
    UCHAR rsp[1900];

    a->scan_count++;
    if (!a->hw_ready || a->connected) {
        /* Connected-state scan caching (matches ps5-iw620.patch stability workaround) */
        goto confirm;
    }

    a->bss_count = 0;
    /* Scan 2.4 GHz in batches of 4 channels, then 5 GHz */
    for (int band = 0; band < 2; band++) {
        const UCHAR *chlist = band == 0 ? ch24 : ch5;
        ULONG nch = band == 0 ? sizeof(ch24) : sizeof(ch5);
        for (ULONG off = 0; off < nch; off += 4) {
            ULONG batch = (nch - off > 4) ? 4 : (nch - off);
            RtlZeroMemory(req, sizeof(req));
            CMD_SCAN_REQ_HDR *sh = (CMD_SCAN_REQ_HDR *)req;
            sh->bss_mode = 3; /* any */
            RtlFillMemory(sh->bssid, 6, 0xFF);
            USHORT pos = sizeof(CMD_SCAN_REQ_HDR);

            TLV_HDR *tlv = (TLV_HDR *)(req + pos);
            tlv->type = TLV_TYPE_CHANLIST;
            tlv->len = (USHORT)(batch * sizeof(CHAN_SCAN_PARAM));
            pos += sizeof(TLV_HDR);

            for (ULONG i = 0; i < batch; i++) {
                CHAN_SCAN_PARAM *cp = (CHAN_SCAN_PARAM *)(req + pos);
                cp->radio_type = (UCHAR)band;
                cp->chan_number = chlist[off + i];
                cp->scan_type = 0; /* active */
                cp->min_scan_time = 20;
                cp->max_scan_time = 80;
                pos += sizeof(CHAN_SCAN_PARAM);
            }

            USHORT rlen = sizeof(rsp);
            if (iw620_exec_cmd(a, CMD_802_11_SCAN, req, pos, rsp, &rlen, 1500)) {
                parse_scan_bss_list(a, rsp, rlen);
            }
        }
    }
    diag_write_dword(L"ScanCount", a->scan_count);
    diag_write_dword(L"BssCount", a->bss_count);

confirm:
    {
        NDIS_STATUS_INDICATION ind;
        NDIS_STATUS status = NDIS_STATUS_SUCCESS;
        RtlZeroMemory(&ind, sizeof(ind));
        ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
        ind.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
        ind.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
        ind.SourceHandle = a->handle;
        ind.StatusCode = NDIS_STATUS_DOT11_SCAN_CONFIRM;
        ind.StatusBuffer = &status;
        ind.StatusBufferSize = sizeof(status);
        NdisMIndicateStatusEx(a->handle, &ind);
    }
}

/* ---- Link & Association indications ------------------------------------------ */
static VOID indicate_link_state(PWIFI_ADAPTER a, BOOLEAN up) {
    NDIS_LINK_STATE ls;
    NDIS_STATUS_INDICATION ind;
    RtlZeroMemory(&ls, sizeof(ls));
    ls.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    ls.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    ls.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    ls.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ls.MediaDuplexState = MediaDuplexStateHalf;
    ls.XmitLinkSpeed = up ? 300000000ULL : NDIS_LINK_SPEED_UNKNOWN;
    ls.RcvLinkSpeed  = up ? 300000000ULL : NDIS_LINK_SPEED_UNKNOWN;

    RtlZeroMemory(&ind, sizeof(ind));
    ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    ind.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    ind.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    ind.SourceHandle = a->handle;
    ind.StatusCode = NDIS_STATUS_LINK_STATE;
    ind.StatusBuffer = &ls;
    ind.StatusBufferSize = sizeof(ls);
    NdisMIndicateStatusEx(a->handle, &ind);
}

static VOID iw620_poll_events(PWIFI_ADAPTER a);

/* Own RSN IE for the association: the AP's group cipher, exactly one pairwise
 * cipher (CCMP) and one AKM, no capabilities. Windows' supplicant repeats the
 * IE from the association request in 4-way message 2, so the same bytes go to
 * the firmware and into the association completion. Returns 0 if the AP has no
 * RSN IE with CCMP. */
static ULONG build_own_rsn_ie(const BSS_ENTRY *t, DOT11_AUTH_ALGORITHM algo, UCHAR *out) {
    static const UCHAR oui[3] = { 0x00, 0x0F, 0xAC };
    const UCHAR *ie = t->ie, *end = t->ie + t->ie_len;
    if (algo != DOT11_AUTH_ALGO_RSNA_PSK && algo != DOT11_AUTH_ALGO_RSNA) return 0;
    while (ie + 2 <= end) {
        UCHAR id = ie[0], len = ie[1];
        if (ie + 2 + len > end) return 0;
        if (id == 0x30) {
            const UCHAR *b = ie + 2, *e = b + len;
            if (len < 8 || b[0] != 1 || b[1] != 0) return 0;
            const UCHAR *group = b + 2;
            const UCHAR *q = b + 6;
            if (q + 2 > e) return 0;
            USHORT n = (USHORT)(q[0] | (q[1] << 8)); q += 2;
            BOOLEAN ccmp = FALSE;
            for (USHORT i = 0; i < n; i++, q += 4) {
                if (q + 4 > e) return 0;
                if (RtlEqualMemory(q, oui, 3) && q[3] == 4) ccmp = TRUE;
            }
            if (!ccmp) return 0;
            out[0] = 0x30; out[1] = 20;
            out[2] = 1; out[3] = 0;
            RtlCopyMemory(out + 4, group, 4);
            out[8] = 1; out[9] = 0; RtlCopyMemory(out + 10, oui, 3); out[13] = 4;
            out[14] = 1; out[15] = 0; RtlCopyMemory(out + 16, oui, 3);
            out[19] = (algo == DOT11_AUTH_ALGO_RSNA_PSK) ? 2 : 1;
            out[20] = 0; out[21] = 0;
            return 22;
        }
        ie += 2 + len;
    }
    return 0;
}

/* Length of the leading run of complete IEs (a cut beacon may end mid-IE). */
static ULONG whole_ies_len(const UCHAR *ie, ULONG len) {
    ULONG pos = 0;
    while (pos + 2 <= len && pos + 2u + ie[pos + 1] <= len) pos += 2u + ie[pos + 1];
    return pos;
}

static VOID iw620_do_connect(PWIFI_ADAPTER a) {
    /* Locate matching BSS from scan cache */
    BSS_ENTRY *target = NULL;
    for (ULONG i = 0; i < a->bss_count; i++) {
        if (a->bss[i].valid && a->bss[i].ssid_len == a->desired_ssid_len &&
            RtlEqualMemory(a->bss[i].ssid, a->desired_ssid, a->desired_ssid_len)) {
            if (!target || a->bss[i].rssi > target->rssi) target = &a->bss[i];
        }
    }

    /* 1. CONNECTION_START */
    {
        DOT11_CONNECTION_START_PARAMETERS cs;
        NDIS_STATUS_INDICATION ind;
        RtlZeroMemory(&cs, sizeof(cs));
        cs.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        cs.Header.Revision = DOT11_CONNECTION_START_PARAMETERS_REVISION_1;
        cs.Header.Size = sizeof(cs);
        cs.BSSType = dot11_BSS_type_infrastructure;
        RtlZeroMemory(&ind, sizeof(ind));
        ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
        ind.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
        ind.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
        ind.SourceHandle = a->handle;
        ind.StatusCode = NDIS_STATUS_DOT11_CONNECTION_START;
        ind.StatusBuffer = &cs;
        ind.StatusBufferSize = sizeof(cs);
        NdisMIndicateStatusEx(a->handle, &ind);
    }

    BOOLEAN ok = FALSE;
    UCHAR rsn_ie[22];
    ULONG rsn_len = 0;
    if (target && a->hw_ready) {
        /* 2. ASSOCIATION_START */
        {
            DOT11_ASSOCIATION_START_PARAMETERS as;
            NDIS_STATUS_INDICATION ind;
            RtlZeroMemory(&as, sizeof(as));
            as.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            as.Header.Revision = DOT11_ASSOCIATION_START_PARAMETERS_REVISION_1;
            as.Header.Size = sizeof(as);
            as.SSID.uSSIDLength = target->ssid_len;
            RtlCopyMemory(as.SSID.ucSSID, target->ssid, target->ssid_len);
            RtlCopyMemory(as.MacAddr, target->bssid, 6);
            RtlZeroMemory(&ind, sizeof(ind));
            ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
            ind.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
            ind.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
            ind.SourceHandle = a->handle;
            ind.StatusCode = NDIS_STATUS_DOT11_ASSOCIATION_START;
            ind.StatusBuffer = &as;
            ind.StatusBufferSize = sizeof(as);
            NdisMIndicateStatusEx(a->handle, &ind);
        }

        /* Build CMD_802_11_ASSOCIATE */
        UCHAR req[512];
        RtlZeroMemory(req, sizeof(req));
        CMD_ASSOC_REQ_HDR *ah = (CMD_ASSOC_REQ_HDR *)req;
        RtlCopyMemory(ah->peer_sta_addr, target->bssid, 6);
        ah->cap_info = target->cap_info;
        ah->listen_interval = 10;
        ah->beacon_period = target->beacon_period;
        ah->dtim_period = 1;
        USHORT pos = sizeof(CMD_ASSOC_REQ_HDR);

        /* SSID TLV */
        TLV_HDR *t = (TLV_HDR *)(req + pos);
        t->type = TLV_TYPE_SSID; t->len = (USHORT)target->ssid_len;
        pos += sizeof(TLV_HDR);
        RtlCopyMemory(req + pos, target->ssid, target->ssid_len);
        pos += (USHORT)target->ssid_len;

        /* PHY DS TLV */
        t = (TLV_HDR *)(req + pos);
        t->type = TLV_TYPE_PHY_DS; t->len = 1;
        pos += sizeof(TLV_HDR);
        req[pos++] = (UCHAR)target->channel;

        /* CF Param Set TLV (required by IW620 associate) */
        t = (TLV_HDR *)(req + pos);
        t->type = 0x0004u; t->len = 6;
        pos += sizeof(TLV_HDR);
        RtlZeroMemory(req + pos, 6);
        pos += 6;

        /* Supported Rates TLV (5 GHz OFDM vs 2.4 GHz CCK+OFDM) */
        static const UCHAR rates2g[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
        static const UCHAR rates5g[] = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
        const UCHAR *rbuf = (target->channel > 14) ? rates5g : rates2g;
        USHORT rsz = (target->channel > 14) ? sizeof(rates5g) : sizeof(rates2g);
        t = (TLV_HDR *)(req + pos);
        t->type = TLV_TYPE_RATES; t->len = rsz;
        pos += sizeof(TLV_HDR);
        RtlCopyMemory(req + pos, rbuf, rsz);
        pos += rsz;

        /* Auth Type TLV (0 = Open) */
        t = (TLV_HDR *)(req + pos);
        t->type = TLV_TYPE_AUTH_TYPE; t->len = 2;
        pos += sizeof(TLV_HDR);
        *(USHORT *)(req + pos) = 0;
        pos += 2;

        /* Channel List TLV (explicit band + channel for 2.4/5 GHz) */
        t = (TLV_HDR *)(req + pos);
        t->type = TLV_TYPE_CHANLIST; t->len = sizeof(CHAN_SCAN_PARAM);
        pos += sizeof(TLV_HDR);
        CHAN_SCAN_PARAM *cp = (CHAN_SCAN_PARAM *)(req + pos);
        RtlZeroMemory(cp, sizeof(*cp));
        cp->radio_type = (target->channel > 14) ? 1 : 0;
        cp->chan_number = (UCHAR)target->channel;
        pos += sizeof(CHAN_SCAN_PARAM);

        /* Own RSN IE (one pairwise cipher, one AKM); a copy of the beacon's
         * IE is invalid when the AP offers several AKMs or ciphers. */
        rsn_len = build_own_rsn_ie(target, a->auth_algo, rsn_ie);
        if (rsn_len && pos + sizeof(TLV_HDR) + rsn_len - 2 < sizeof(req)) {
            t = (TLV_HDR *)(req + pos);
            t->type = TLV_TYPE_RSN; t->len = (USHORT)(rsn_len - 2);
            pos += sizeof(TLV_HDR);
            RtlCopyMemory(req + pos, rsn_ie + 2, rsn_len - 2);
            pos += (USHORT)(rsn_len - 2);
        }

        /* Clear any stale firmware STA/TX-queue state before associating */
        {
            CMD_DEAUTH_BODY d;
            RtlCopyMemory(d.mac_addr, target->bssid, 6);
            d.reason_code = 3;
            (VOID)iw620_exec_cmd(a, CMD_802_11_DEAUTH, &d, sizeof(d), NULL, NULL, 500);
        }

        CMD_ASSOC_RSP_HDR arsp;
        USHORT rlen = sizeof(arsp);
        RtlZeroMemory(&arsp, sizeof(arsp));
        BOOLEAN assoc_cmd_ok = iw620_exec_cmd(a, CMD_802_11_ASSOCIATE, req, pos, &arsp, &rlen, 3000);
        if (assoc_cmd_ok && arsp.status_code == 0 && (arsp.aid & 0x3FFFu) != 0 && arsp.aid != 0xFFFFu && !a->assoc_primed) {
            /* Prime IW620 data path once after fresh init (fixes first-assoc TX stall at idx 5) */
            a->assoc_primed = TRUE;
            delay_us(20000);
            iw620_poll_events(a);
            {
                CMD_DEAUTH_BODY d;
                RtlCopyMemory(d.mac_addr, target->bssid, 6);
                d.reason_code = 3;
                (VOID)iw620_exec_cmd(a, CMD_802_11_DEAUTH, &d, sizeof(d), NULL, NULL, 500);
            }
            delay_us(20000);
            iw620_poll_events(a);
            rlen = sizeof(arsp);
            RtlZeroMemory(&arsp, sizeof(arsp));
            assoc_cmd_ok = iw620_exec_cmd(a, CMD_802_11_ASSOCIATE, req, pos, &arsp, &rlen, 3000);
        }
        if (assoc_cmd_ok && arsp.status_code == 0 && (arsp.aid & 0x3FFFu) != 0 && arsp.aid != 0xFFFFu) {
            /* Let IW620 finish post-assoc port release & re-apply MAC_CONTROL */
            delay_us(50000);
            iw620_poll_events(a);
            {
                CMD_MAC_CONTROL_BODY mc;
                mc.action = MAC_ACT_RX_ON | MAC_ACT_TX_ON | MAC_ACT_ETHERNETII_EN | MAC_ACT_ALL_MULTICAST;
                (VOID)iw620_exec_cmd(a, CMD_MAC_CONTROL, &mc, sizeof(mc), NULL, NULL, 1000);
            }
            ok = TRUE;
            a->connected = TRUE;
            a->gtk_set = FALSE;
            a->cur_channel = target->channel;
            RtlCopyMemory(a->assoc_bssid, target->bssid, 6);
            a->cur_phy_id = target->phy_id;
            diag_write_dword(L"AssocState", 1);
        }

        /* 3. ASSOCIATION_COMPLETION, followed by the association request body,
         * the beacon body and the active PHY list. Windows' WPA2 supplicant
         * needs the RSN IEs of both frames for the 4-way handshake. */
        {
            static UCHAR abuf[sizeof(DOT11_ASSOCIATION_COMPLETION_PARAMETERS) + 128 + 12 + MAX_BSS_IE_BYTES + 8];
            DOT11_ASSOCIATION_COMPLETION_PARAMETERS ac;
            NDIS_STATUS_INDICATION ind;
            ULONG used = sizeof(ac);
            RtlZeroMemory(&ac, sizeof(ac));
            RtlZeroMemory(abuf, sizeof(abuf));
            ac.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            ac.Header.Revision = DOT11_ASSOCIATION_COMPLETION_PARAMETERS_REVISION_1;
            ac.Header.Size = sizeof(ac);
            RtlCopyMemory(ac.MacAddr, target->bssid, 6);
            ac.uStatus = ok ? DOT11_ASSOC_STATUS_SUCCESS : DOT11_ASSOC_STATUS_UNREACHABLE;
            if (ok) {
                UCHAR *f = abuf + used;
                ULONG n = 0;
                /* Association request body: capability, listen interval, SSID, rates, RSN */
                f[n++] = (UCHAR)target->cap_info; f[n++] = (UCHAR)(target->cap_info >> 8);
                f[n++] = 10; f[n++] = 0;
                f[n++] = 0; f[n++] = (UCHAR)target->ssid_len;
                RtlCopyMemory(f + n, target->ssid, target->ssid_len); n += target->ssid_len;
                f[n++] = 1; f[n++] = (UCHAR)rsz;
                RtlCopyMemory(f + n, rbuf, rsz); n += rsz;
                if (rsn_len) { RtlCopyMemory(f + n, rsn_ie, rsn_len); n += rsn_len; }
                ac.uAssocReqOffset = used; ac.uAssocReqSize = n; used += n;
                /* Beacon/probe response body: timestamp, interval, capability, IEs */
                f = abuf + used; n = 0;
                RtlCopyMemory(f, &target->timestamp, 8); n = 8;
                f[n++] = (UCHAR)target->beacon_period; f[n++] = (UCHAR)(target->beacon_period >> 8);
                f[n++] = (UCHAR)target->cap_info; f[n++] = (UCHAR)(target->cap_info >> 8);
                ULONG il = whole_ies_len(target->ie, target->ie_len);
                RtlCopyMemory(f + n, target->ie, il); n += il;
                ac.uBeaconOffset = used; ac.uBeaconSize = n; used += n;
                used = (used + 3u) & ~3u;
                *(ULONG *)(abuf + used) = target->phy_id;
                ac.uActivePhyListOffset = used; ac.uActivePhyListSize = sizeof(ULONG); used += sizeof(ULONG);
                ac.AuthAlgo = a->auth_algo;
                ac.UnicastCipher = a->ucast_cipher;
                ac.MulticastCipher = a->mcast_cipher;
                ac.bPortAuthorized = (a->auth_algo == DOT11_AUTH_ALGO_80211_OPEN) ? TRUE : FALSE;
                ac.DSInfo = DOT11_DS_CHANGED;
            }
            RtlCopyMemory(abuf, &ac, sizeof(ac));
            RtlZeroMemory(&ind, sizeof(ind));
            ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
            ind.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
            ind.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
            ind.SourceHandle = a->handle;
            ind.StatusCode = NDIS_STATUS_DOT11_ASSOCIATION_COMPLETION;
            ind.StatusBuffer = abuf;
            ind.StatusBufferSize = used;
            NdisMIndicateStatusEx(a->handle, &ind);
        }
    }

    /* 4. CONNECTION_COMPLETION */
    {
        DOT11_CONNECTION_COMPLETION_PARAMETERS cc;
        NDIS_STATUS_INDICATION ind;
        RtlZeroMemory(&cc, sizeof(cc));
        cc.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        cc.Header.Revision = DOT11_CONNECTION_COMPLETION_PARAMETERS_REVISION_1;
        cc.Header.Size = sizeof(cc);
        cc.uStatus = ok ? DOT11_CONNECTION_STATUS_SUCCESS : DOT11_CONNECTION_STATUS_CANDIDATE_LIST_EXHAUSTED;
        RtlZeroMemory(&ind, sizeof(ind));
        ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
        ind.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
        ind.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
        ind.SourceHandle = a->handle;
        ind.StatusCode = NDIS_STATUS_DOT11_CONNECTION_COMPLETION;
        ind.StatusBuffer = &cc;
        ind.StatusBufferSize = sizeof(cc);
        NdisMIndicateStatusEx(a->handle, &ind);
    }

    if (ok) indicate_link_state(a, TRUE);
}

static VOID iw620_do_disconnect(PWIFI_ADAPTER a) {
    if (a->connected && a->hw_ready) {
        CMD_DEAUTH_BODY d;
        RtlCopyMemory(d.mac_addr, a->assoc_bssid, 6);
        d.reason_code = 3; /* deauth leaving */
        (VOID)iw620_exec_cmd(a, CMD_802_11_DEAUTH, &d, sizeof(d), NULL, NULL, 1000);
    }
    if (a->connected) {
        a->connected = FALSE;
        diag_write_dword(L"AssocState", 0);
        indicate_link_state(a, FALSE);
        DOT11_DISASSOCIATION_PARAMETERS dp;
        NDIS_STATUS_INDICATION ind;
        RtlZeroMemory(&dp, sizeof(dp));
        dp.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        dp.Header.Revision = DOT11_DISASSOCIATION_PARAMETERS_REVISION_1;
        dp.Header.Size = sizeof(dp);
        RtlCopyMemory(dp.MacAddr, a->assoc_bssid, 6);
        dp.uReason = DOT11_DISASSOC_REASON_OS;
        RtlZeroMemory(&ind, sizeof(ind));
        ind.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
        ind.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
        ind.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
        ind.SourceHandle = a->handle;
        ind.StatusCode = NDIS_STATUS_DOT11_DISASSOCIATION;
        ind.StatusBuffer = &dp;
        ind.StatusBufferSize = sizeof(dp);
        NdisMIndicateStatusEx(a->handle, &ind);
    }
}

/* Windows hands CCMP keys over as DOT11_KEY_ALGO_CCMP (IV48 counter, length,
 * key: 28 bytes for a 128-bit key); accept a raw 16-byte key as well. */
static const UCHAR *ccmp_key(const UCHAR *k, USHORT len, const UCHAR **pn) {
    *pn = NULL;
    if (len == 16) return k;
    if (len >= FIELD_OFFSET(DOT11_KEY_ALGO_CCMP, ucCCMPKey) + 16) {
        const DOT11_KEY_ALGO_CCMP *c = (const DOT11_KEY_ALGO_CCMP *)k;
        if (c->ulCCMPKeyLength == 16) { *pn = c->ucIV48Counter; return c->ucCCMPKey; }
    }
    return NULL;
}

static VOID iw620_set_key(PWIFI_ADAPTER a, const UCHAR *mac, UCHAR idx, BOOLEAN is_ucast,
                          UCHAR key_type, const UCHAR *key, USHORT key_len, const UCHAR *pn)
{
    /* Same layout and flags as mwifiex_cmd_802_11_key_material_v2 (station, WPA2). */
    if (!a->hw_ready || key_len > 32) return;
    UCHAR buf[sizeof(USHORT) + sizeof(TLV_HDR) + sizeof(KEY_PARAM_V2)];
    RtlZeroMemory(buf, sizeof(buf));
    *(USHORT *)buf = 1; /* HostCmd_ACT_GEN_SET */
    TLV_HDR *t = (TLV_HDR *)(buf + 2);
    t->type = TLV_TYPE_KEY_PARAM_V2;
    t->len = (USHORT)sizeof(KEY_PARAM_V2);
    KEY_PARAM_V2 *kp = (KEY_PARAM_V2 *)(buf + 2 + sizeof(TLV_HDR));
    if (mac && is_ucast) RtlCopyMemory(kp->mac_addr, mac, 6);
    else RtlFillMemory(kp->mac_addr, 6, 0xFF);
    kp->key_idx = (UCHAR)(idx & 0x0F);
    kp->key_type = key_type;
    kp->key_info = KEY_INFO_ENABLE_KEY |
                   (is_ucast ? (KEY_INFO_UCAST_KEY | KEY_INFO_TX_KEY | KEY_INFO_RX_KEY)
                             : (KEY_INFO_MCAST_KEY | KEY_INFO_RX_KEY));
    if (!a->gtk_set) kp->key_info |= KEY_INFO_DEFAULT_KEY;
    if (pn) RtlCopyMemory(kp->pn, pn, 6);
    kp->key_len = key_len;
    if (key_len) RtlCopyMemory(kp->key, key, key_len);
    BOOLEAN ok = iw620_exec_cmd(a, CMD_802_11_KEY_MATERIAL, buf, (USHORT)sizeof(buf), NULL, NULL, 1000);
    if (ok && !is_ucast) a->gtk_set = TRUE;
    diag_write_dword(is_ucast ? L"PtkSet" : L"GtkSet", ok ? 1u : 0x80000000u);
}

/* ---- TX & RX Data Path (802.11 <-> 802.3 + IW620 TxPD/RxPD) ------------------ */
static VOID iw620_poll_rx(PWIFI_ADAPTER a) {
    if (!a->hw_ready || a->paused || a->halting) return;

    ULONG ptr = r32(a, ADMA_CHAN1_Q0 + ADMA_DST_RW_PTR);
    ULONG hw_rd = (ptr >> 16) & 0x1FFFu;

    while (a->rx_rd != hw_rd) {
        ULONG slot = a->rx_rd & (RX_RING_SIZE - 1);
        UCHAR *raw = a->rx_pool + slot * RX_BUF_STRIDE;
        PCIE_IF_HDR *ifh = (PCIE_IF_HDR *)raw;
        if (ifh->type == MLAN_TYPE_DATA && ifh->len >= sizeof(PCIE_IF_HDR) + sizeof(IW620_RXPD)) {
            IW620_RXPD *rxpd = (IW620_RXPD *)(raw + sizeof(PCIE_IF_HDR));
            ULONG eth_off = sizeof(PCIE_IF_HDR) + rxpd->rx_pkt_offset;
            ULONG eth_len = rxpd->rx_pkt_length;
            if (eth_len >= 14 && eth_off + eth_len <= RX_BUF_STRIDE && eth_len <= 1600) {
                const UCHAR *eth = raw + eth_off;
                /* Convert 802.3 (14B) -> 802.11 FromDS data (24B) + LLC/SNAP (8B) */
                UCHAR *d11 = a->rx_dot11_buf[slot];
                RtlZeroMemory(d11, 32);
                d11[0] = 0x08; /* Data */
                d11[1] = 0x02; /* FromDS */
                RtlCopyMemory(d11 + 4,  eth, 6);            /* Addr1 = DA */
                RtlCopyMemory(d11 + 10, a->assoc_bssid, 6); /* Addr2 = BSSID */
                RtlCopyMemory(d11 + 16, eth + 6, 6);        /* Addr3 = SA */
                /* LLC/SNAP header */
                d11[24] = 0xAA; d11[25] = 0xAA; d11[26] = 0x03;
                d11[27] = 0x00; d11[28] = 0x00; d11[29] = 0x00;
                d11[30] = eth[12]; d11[31] = eth[13];       /* EtherType */
                ULONG pay_len = eth_len - 14;
                if (pay_len) RtlCopyMemory(d11 + 32, eth + 14, pay_len);
                ULONG d11_len = 32 + pay_len;

                PNET_BUFFER_LIST nbl = a->rx_nbl[slot];
                PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl);
                MmInitializeMdl(a->rx_mdl[slot], d11, d11_len);
                MmBuildMdlForNonPagedPool(a->rx_mdl[slot]);
                NET_BUFFER_FIRST_MDL(nb) = a->rx_mdl[slot];
                NET_BUFFER_CURRENT_MDL(nb) = a->rx_mdl[slot];
                NET_BUFFER_CURRENT_MDL_OFFSET(nb) = 0;
                NET_BUFFER_DATA_OFFSET(nb) = 0;
                NET_BUFFER_DATA_LENGTH(nb) = d11_len;

                DOT11_EXTSTA_RECV_CONTEXT rctx;
                RtlZeroMemory(&rctx, sizeof(rctx));
                rctx.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
                rctx.Header.Revision = DOT11_EXTSTA_RECV_CONTEXT_REVISION_1;
                rctx.Header.Size = sizeof(rctx);
                rctx.uReceiveFlags = 0;
                rctx.uPhyId = a->cur_phy_id;
                rctx.uChCenterFrequency = chan_to_freq_khz(a->cur_channel ? a->cur_channel : 1) / 1000u;
                rctx.lRSSI = -50;
                rctx.ucDataRate = 108;
                NET_BUFFER_LIST_INFO(nbl, MediaSpecificInformation) = &rctx;

                NdisMIndicateReceiveNetBufferLists(a->handle, nbl, 0, 1, NDIS_RECEIVE_FLAGS_RESOURCES);
                a->rx_ok++;
                a->rx_bytes += d11_len;
            }
        }
        /* Re-arm RX descriptor */
        a->rx_ring[slot].len = (USHORT)RX_BUF_STRIDE;
        a->rx_ring[slot].pkt_size = 0;
        a->rx_ring[slot].flags = ADMA_BD_FLAG_INT_EN | ADMA_BD_FLAG_DST_HOST;
        a->rx_rd = (a->rx_rd + 1) & 0x1FFFu;
        a->rx_wr = (a->rx_wr + 1) & 0x1FFFu;
        w32(a, ADMA_CHAN1_Q0 + ADMA_DST_RW_PTR, a->rx_wr);
    }
}

static VOID iw620_poll_events(PWIFI_ADAPTER a) {
    if (!a->hw_ready || a->paused || a->halting) return;

    ULONG st = r32(a, REG_HOST_INT_STATUS);
    ULONG clr = st & (ADMA_INT_TX_DONE | ADMA_INT_RX_RDY | ADMA_INT_EVT_RDY);
    if (clr) w32(a, REG_HOST_INT_STATUS, ~clr);

    ULONG ptr = r32(a, ADMA_CHAN1_Q1 + ADMA_DST_RW_PTR);
    ULONG hw_rd = (ptr >> 16) & 0x1FFFu;
    BOOLEAN got_evt = FALSE;

    while (a->evt_rd != hw_rd) {
        ULONG slot = a->evt_rd & (EVT_RING_SIZE - 1);
        a->evt_ring[slot].len = (USHORT)EVT_BUF_STRIDE;
        a->evt_ring[slot].pkt_size = 0;
        a->evt_ring[slot].flags = ADMA_BD_FLAG_INT_EN | ADMA_BD_FLAG_DST_HOST;
        a->evt_rd = (a->evt_rd + 1) & 0x1FFFu;
        a->evt_wr = (a->evt_wr + 1) & 0x1FFFu;
        w32(a, ADMA_CHAN1_Q1 + ADMA_DST_RW_PTR, a->evt_wr);
        got_evt = TRUE;
    }
    if (got_evt) {
        w32(a, REG_CPU_INT_EVENT, CPU_INTR_EVENT_DONE);
    }
}

static VOID PollTimer(PVOID sys1, PVOID ctx, PVOID sys2, PVOID sys3) {
    UNREFERENCED_PARAMETER(sys1); UNREFERENCED_PARAMETER(sys2); UNREFERENCED_PARAMETER(sys3);
    PWIFI_ADAPTER a = (PWIFI_ADAPTER)ctx;
    if (a->halting) return;
    iw620_poll_events(a);
    iw620_poll_rx(a);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)POLL_MS * 10000LL;
    NdisSetTimerObject(a->timer, due, 0, NULL);
}

static VOID MiniportSendNbls(NDIS_HANDLE ctx, PNET_BUFFER_LIST nbl_list, NDIS_PORT_NUMBER port, ULONG flags) {
    UNREFERENCED_PARAMETER(port);
    PWIFI_ADAPTER a = (PWIFI_ADAPTER)ctx;
    ULONG cflags = (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;

    for (PNET_BUFFER_LIST nbl = nbl_list; nbl; ) {
        PNET_BUFFER_LIST next = NET_BUFFER_LIST_NEXT_NBL(nbl);
        NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
        NDIS_STATUS st = NDIS_STATUS_SUCCESS;

        if (!a->hw_ready || !a->connected || a->paused || a->halting) {
            st = NDIS_STATUS_FAILURE;
            a->tx_drop++;
        } else {
            for (PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
                ULONG d11_len = NET_BUFFER_DATA_LENGTH(nb);
                if (d11_len < 32 || d11_len > 1600) { a->tx_drop++; continue; }

                UCHAR tmp[1600];
                ULONG copied = 0, skip = NET_BUFFER_CURRENT_MDL_OFFSET(nb);
                for (PMDL m = NET_BUFFER_CURRENT_MDL(nb); m && copied < d11_len; m = m->Next) {
                    UCHAR *va = (UCHAR *)MmGetSystemAddressForMdlSafe(m, LowPagePriority);
                    if (!va) break;
                    ULONG avail = MmGetMdlByteCount(m) - skip;
                    ULONG take = (d11_len - copied < avail) ? (d11_len - copied) : avail;
                    RtlCopyMemory(tmp + copied, va + skip, take);
                    copied += take;
                    skip = 0;
                }
                if (copied < 32) { a->tx_drop++; continue; }

                /* Convert 802.11 (24B + 8B LLC/SNAP) -> 802.3 (14B + payload) */
                ULONG hdr80211 = (tmp[0] & 0x80) ? 26u : 24u;
                if (copied < hdr80211 + 8) { a->tx_drop++; continue; }

                KIRQL old_irql;
                KeAcquireSpinLock(&a->tx_lock, &old_irql);

                ULONG hw_rd = (r32(a, ADMA_CHAN0_Q0 + ADMA_SRC_RW_PTR) >> 16) & 0x1FFFu;
                if (((a->tx_wr - hw_rd) & 0x1FFFu) >= (TX_RING_SIZE - 1)) {
                    a->tx_drop++;
                    KeReleaseSpinLock(&a->tx_lock, old_irql);
                    continue;
                }

                ULONG slot = a->tx_wr & (TX_RING_SIZE - 1);
                UCHAR *dst = a->tx_pool + slot * TX_BUF_STRIDE;
                RtlZeroMemory(dst, ADMA_MIN_PKT_SIZE);

                ULONG pay_len = copied - (hdr80211 + 8);
                ULONG eth_len = 14 + pay_len;
                ULONG pkt_total = sizeof(PCIE_IF_HDR) + sizeof(IW620_TXPD) + eth_len;
                ULONG dma_len = (pkt_total < ADMA_MIN_PKT_SIZE) ? ADMA_MIN_PKT_SIZE : ((pkt_total + 7u) & ~7u);
                if (dma_len > TX_BUF_STRIDE) {
                    a->tx_drop++;
                    KeReleaseSpinLock(&a->tx_lock, old_irql);
                    continue;
                }

                PCIE_IF_HDR *ifh = (PCIE_IF_HDR *)dst;
                ifh->len = (USHORT)dma_len;
                ifh->type = MLAN_TYPE_DATA;

                IW620_TXPD *txpd = (IW620_TXPD *)(dst + sizeof(PCIE_IF_HDR));
                RtlZeroMemory(txpd, sizeof(*txpd));
                txpd->tx_pkt_length = (USHORT)eth_len;
                txpd->tx_pkt_offset = sizeof(IW620_TXPD);
                txpd->priority = 0; /* BE (avoids IW620 TID 1/3/5 stalls) */
                RtlCopyMemory(txpd->ra_mac, tmp + 4, 6); /* BSSID */

                UCHAR *eth = dst + sizeof(PCIE_IF_HDR) + sizeof(IW620_TXPD);
                RtlCopyMemory(eth,     tmp + 16, 6); /* DA = Addr3 (ToDS) */
                RtlCopyMemory(eth + 6, tmp + 10, 6); /* SA = Addr2 */
                eth[12] = tmp[hdr80211 + 6];
                eth[13] = tmp[hdr80211 + 7];
                if (pay_len) RtlCopyMemory(eth + 14, tmp + hdr80211 + 8, pay_len);
                if (dma_len > pkt_total) RtlZeroMemory(dst + pkt_total, dma_len - pkt_total);

                a->tx_ring[slot].paddr = (ULONG64)a->tx_pool_pa.QuadPart + (ULONG64)slot * TX_BUF_STRIDE;
                a->tx_ring[slot].len = (USHORT)dma_len; /* >= 128 bytes (ps5-iw620-tx-min128) */
                a->tx_ring[slot].pkt_size = (USHORT)dma_len;
                a->tx_ring[slot].flags = ADMA_BD_FLAG_SOP | ADMA_BD_FLAG_EOP | ADMA_BD_FLAG_INT_EN | ADMA_BD_FLAG_SRC_HOST;
                KeMemoryBarrier();

                a->tx_wr = (a->tx_wr + 1) & 0x1FFFu;
                w32(a, ADMA_CHAN0_Q0 + ADMA_SRC_RW_PTR, a->tx_wr);
                a->tx_ok++;
                a->tx_bytes += eth_len;
                KeReleaseSpinLock(&a->tx_lock, old_irql);
            }
        }
        NET_BUFFER_LIST_STATUS(nbl) = st;
        NdisMSendNetBufferListsComplete(a->handle, nbl, cflags);
        nbl = next;
    }
}

/* ---- OID Handler (Native 802.11 + General) ----------------------------------- */
static NDIS_STATUS handle_enum_bss_list(PWIFI_ADAPTER a, PVOID buf, ULONG buflen, PULONG written, PULONG needed) {
    ULONG arr_hdr = FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer);
    ULONG ent_hdr = FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer);
    ULONG total = arr_hdr;
    for (ULONG i = 0; i < a->bss_count; i++) {
        if (!a->bss[i].valid) continue;
        total += ent_hdr + a->bss[i].ie_len;
    }
    *needed = total;
    if (buflen < total) return NDIS_STATUS_BUFFER_OVERFLOW;

    PDOT11_BYTE_ARRAY arr = (PDOT11_BYTE_ARRAY)buf;
    RtlZeroMemory(arr, total);
    arr->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    arr->Header.Revision = DOT11_BSS_ENTRY_BYTE_ARRAY_REVISION_1;
    arr->Header.Size = sizeof(DOT11_BYTE_ARRAY);
    arr->uNumOfBytes = total - arr_hdr;
    arr->uTotalNumOfBytes = arr->uNumOfBytes;

    UCHAR *p = arr->ucBuffer;
    for (ULONG i = 0; i < a->bss_count; i++) {
        if (!a->bss[i].valid) continue;
        BSS_ENTRY *s = &a->bss[i];
        DOT11_BSS_ENTRY d;
        RtlZeroMemory(&d, sizeof(d));
        d.uPhyId = s->phy_id;
        d.PhySpecificInfo.uChCenterFrequency = (s->freq_khz >= 2400000u) ? (s->freq_khz / 1000u) : 2437u;
        RtlCopyMemory(d.dot11BSSID, s->bssid, 6);
        d.dot11BSSType = dot11_BSS_type_infrastructure;
        d.lRSSI = s->rssi;
        d.uLinkQuality = (s->rssi > -50) ? 100 : (s->rssi < -90 ? 10 : (ULONG)(100 + (s->rssi + 50) * 2));
        d.bInRegDomain = TRUE;
        d.usBeaconPeriod = s->beacon_period ? s->beacon_period : 100;
        d.ullTimestamp = s->timestamp;
        d.ullHostTimestamp = s->timestamp;
        d.usCapabilityInformation = s->cap_info;
        d.uBufferLength = s->ie_len;
        RtlCopyMemory(p, &d, ent_hdr);
        if (s->ie_len) RtlCopyMemory(p + ent_hdr, s->ie, s->ie_len);
        p += ent_hdr + s->ie_len;
    }
    *written = total;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS MiniportOidRequest(NDIS_HANDLE ctx, PNDIS_OID_REQUEST req) {
    PWIFI_ADAPTER a = (PWIFI_ADAPTER)ctx;
    NDIS_STATUS st = NDIS_STATUS_SUCCESS;

    if (req->RequestType == NdisRequestQueryInformation || req->RequestType == NdisRequestQueryStatistics) {
        NDIS_OID oid = req->DATA.QUERY_INFORMATION.Oid;
        PVOID buf = req->DATA.QUERY_INFORMATION.InformationBuffer;
        ULONG len = req->DATA.QUERY_INFORMATION.InformationBufferLength;
        ULONG out = 0, need = 0;
        ULONG v32 = 0;

        switch (oid) {
        case OID_GEN_HARDWARE_STATUS:
            v32 = NdisHardwareStatusReady; out = 4; break;
        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            v32 = NdisMediumNative802_11; out = 4; break;
        case OID_GEN_MAXIMUM_LOOKAHEAD:
        case OID_GEN_CURRENT_LOOKAHEAD:
            v32 = 1500; out = 4; break;
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            v32 = 1500; out = 4; break;
        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            v32 = 1514; out = 4; break;
        case OID_GEN_LINK_SPEED:
            v32 = 3000000; out = 4; break; /* 300 Mbps in 100 bps units */
        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            v32 = 65536; out = 4; break;
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            v32 = 1; out = 4; break;
        case OID_GEN_VENDOR_ID:
            v32 = 0x001B4Bu; out = 4; break;
        case OID_GEN_VENDOR_DRIVER_VERSION:
            v32 = PS5WIFI_VERSION; out = 4; break;
        case OID_GEN_DRIVER_VERSION:
            v32 = 0x0614; out = 2; break;
        case OID_GEN_CURRENT_PACKET_FILTER:
            v32 = a->packet_filter; out = 4; break;
        case OID_GEN_MAC_OPTIONS:
            v32 = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
            out = 4; break;
        case OID_GEN_MEDIA_CONNECT_STATUS:
            v32 = a->connected ? NdisMediaStateConnected : NdisMediaStateDisconnected; out = 4; break;
        case OID_GEN_MAXIMUM_SEND_PACKETS:
            v32 = 16; out = 4; break;
        case OID_GEN_XMIT_OK:
            v32 = (ULONG)a->tx_ok; out = 4; break;
        case OID_GEN_RCV_OK:
            v32 = (ULONG)a->rx_ok; out = 4; break;
        case OID_GEN_XMIT_ERROR:
            v32 = (ULONG)a->tx_drop; out = 4; break;
        case OID_GEN_RCV_ERROR:
            v32 = (ULONG)a->rx_err; out = 4; break;
        case OID_GEN_VENDOR_DESCRIPTION: {
            static const char desc[] = "PS5 Wi-Fi 6 Adapter (ps5wifi)";
            need = sizeof(desc);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            RtlCopyMemory(buf, desc, need); out = need; break;
        }
        case OID_802_3_PERMANENT_ADDRESS:
        case OID_DOT11_MAC_ADDRESS:
            need = 6; if (len < 6) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            RtlCopyMemory(buf, a->perm_mac, 6); out = 6; break;
        case OID_802_3_CURRENT_ADDRESS:
        case OID_DOT11_CURRENT_ADDRESS:
            need = 6; if (len < 6) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            RtlCopyMemory(buf, a->cur_mac, 6); out = 6; break;
        case OID_DOT11_OPERATION_MODE_CAPABILITY: {
            DOT11_OPERATION_MODE_CAPABILITY cap;
            RtlZeroMemory(&cap, sizeof(cap));
            cap.uOpModeCapability = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
            need = sizeof(cap);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            RtlCopyMemory(buf, &cap, need); out = need; break;
        }
        case OID_DOT11_CURRENT_OPERATION_MODE: {
            DOT11_CURRENT_OPERATION_MODE m;
            RtlZeroMemory(&m, sizeof(m));
            m.uCurrentOpMode = a->op_mode;
            need = sizeof(m);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            RtlCopyMemory(buf, &m, need); out = need; break;
        }
        case OID_DOT11_NIC_POWER_STATE: {
            BOOLEAN on = a->radio_on;
            need = sizeof(BOOLEAN);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            *(BOOLEAN *)buf = on; out = need; break;
        }
        case OID_DOT11_HARDWARE_PHY_STATE: {
            need = sizeof(BOOLEAN);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            *(BOOLEAN *)buf = TRUE; out = need; break;
        }
        case OID_DOT11_UNICAST_USE_GROUP_ENABLED: {
            need = sizeof(BOOLEAN);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            *(BOOLEAN *)buf = TRUE; out = need; break;
        }
        case OID_DOT11_DESIRED_PHY_LIST: {
            need = sizeof(DOT11_PHY_ID_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_PHY_ID_LIST pl = (PDOT11_PHY_ID_LIST)buf;
            RtlZeroMemory(pl, need);
            pl->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            pl->Header.Revision = DOT11_PHY_ID_LIST_REVISION_1;
            pl->Header.Size = sizeof(DOT11_PHY_ID_LIST);
            pl->uNumOfEntries = 1;
            pl->uTotalNumOfEntries = 1;
            pl->dot11PhyId[0] = DOT11_PHY_ID_ANY;
            out = need; break;
        }
        case OID_DOT11_CURRENT_CHANNEL:
            v32 = 6; out = 4; break;
        case OID_DOT11_MPDU_MAX_LENGTH:
            v32 = 2346; out = 4; break;
        case OID_DOT11_CURRENT_TX_POWER_LEVEL:
            v32 = 1; out = 4; break;
        case OID_DOT11_SUPPORTED_POWER_LEVELS: {
            need = sizeof(DOT11_SUPPORTED_POWER_LEVELS);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_SUPPORTED_POWER_LEVELS pl = (PDOT11_SUPPORTED_POWER_LEVELS)buf;
            RtlZeroMemory(pl, need);
            pl->uNumOfSupportedPowerLevels = 1;
            pl->uTxPowerLevelValues[0] = 100;
            out = need; break;
        }
        case OID_DOT11_RECV_SENSITIVITY_LIST: {
            need = sizeof(DOT11_RECV_SENSITIVITY_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_RECV_SENSITIVITY_LIST rsl = (PDOT11_RECV_SENSITIVITY_LIST)buf;
            rsl->uNumOfEntries = 1;
            rsl->uTotalNumOfEntries = 1;
            rsl->dot11RecvSensitivity[0].ucDataRate = 108;
            rsl->dot11RecvSensitivity[0].lRSSIMin = -95;
            rsl->dot11RecvSensitivity[0].lRSSIMax = -20;
            out = need; break;
        }
        case OID_DOT11_WME_IMPLEMENTED: {
            need = sizeof(BOOLEAN);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            *(BOOLEAN *)buf = FALSE; out = need; break;
        }
        case OID_DOT11_OPERATIONAL_RATE_SET: {
            need = sizeof(DOT11_RATE_SET);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_RATE_SET rs = (PDOT11_RATE_SET)buf;
            RtlZeroMemory(rs, need);
            rs->uRateSetLength = 5;
            rs->ucRateSet[0] = 0x82; rs->ucRateSet[1] = 0x84;
            rs->ucRateSet[2] = 0x8B; rs->ucRateSet[3] = 0x96; rs->ucRateSet[4] = 0x6C;
            out = need; break;
        }
        case OID_GEN_SUPPORTED_LIST: {
            static const NDIS_OID oids[] = {
                OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
                OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BLOCK_SIZE,
                OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION,
                OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_MAXIMUM_TOTAL_SIZE,
                OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_STATISTICS,
                OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
                OID_802_3_MAXIMUM_LIST_SIZE, OID_DOT11_MAC_ADDRESS, OID_DOT11_CURRENT_ADDRESS,
                OID_DOT11_CURRENT_OPERATION_MODE, OID_DOT11_NIC_POWER_STATE,
                OID_DOT11_HARDWARE_PHY_STATE, OID_DOT11_CURRENT_PHY_ID, OID_DOT11_SUPPORTED_PHY_TYPES,
                OID_DOT11_DESIRED_PHY_LIST, OID_DOT11_ENUM_BSS_LIST, OID_DOT11_FLUSH_BSS_LIST,
                OID_DOT11_SCAN_REQUEST, OID_DOT11_CONNECT_REQUEST, OID_DOT11_DISCONNECT_REQUEST,
                OID_DOT11_RESET_REQUEST, OID_DOT11_AUTO_CONFIG_ENABLED, OID_DOT11_EXCLUDE_UNENCRYPTED,
                OID_DOT11_DESIRED_BSS_TYPE, OID_DOT11_DESIRED_SSID_LIST, OID_DOT11_DESIRED_BSSID_LIST,
                OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM, OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM,
                OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM, OID_DOT11_CIPHER_DEFAULT_KEY_ID,
                OID_DOT11_CIPHER_DEFAULT_KEY, OID_DOT11_CIPHER_KEY_MAPPING_KEY, OID_DOT11_STATISTICS,
                OID_DOT11_UNICAST_USE_GROUP_ENABLED, OID_DOT11_CURRENT_CHANNEL,
                OID_DOT11_MPDU_MAX_LENGTH, OID_DOT11_SUPPORTED_POWER_LEVELS,
                OID_DOT11_CURRENT_TX_POWER_LEVEL, OID_DOT11_OPERATIONAL_RATE_SET
            };
            need = sizeof(oids);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            RtlCopyMemory(buf, oids, need); out = need; break;
        }
        case OID_DOT11_CURRENT_PHY_ID:
            v32 = a->cur_phy_id; out = 4; break;
        case OID_DOT11_SUPPORTED_PHY_TYPES: {
            need = sizeof(DOT11_SUPPORTED_PHY_TYPES) + sizeof(DOT11_PHY_TYPE);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_SUPPORTED_PHY_TYPES p = (PDOT11_SUPPORTED_PHY_TYPES)buf;
            p->uNumOfEntries = 2; p->uTotalNumOfEntries = 2;
            p->dot11PHYType[0] = dot11_phy_type_erp;
            p->dot11PHYType[1] = dot11_phy_type_ofdm;
            out = need; break;
        }
        case OID_DOT11_ENUM_BSS_LIST:
            st = handle_enum_bss_list(a, buf, len, &out, &need);
            break;
        case OID_DOT11_AUTO_CONFIG_ENABLED:
            v32 = a->auto_config; out = 4; break;
        case OID_DOT11_EXCLUDE_UNENCRYPTED:
            need = sizeof(BOOLEAN);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            *(BOOLEAN *)buf = a->exclude_unencrypted; out = need; break;
        case OID_DOT11_DESIRED_BSS_TYPE:
            v32 = (ULONG)a->desired_bss_type; out = 4; break;
        case OID_DOT11_POWER_MGMT_REQUEST:
            v32 = DOT11_POWER_SAVING_NO_POWER_SAVING; out = 4; break;
        case OID_DOT11_CIPHER_DEFAULT_KEY_ID:
            v32 = a->default_key_id; out = 4; break;
        case OID_DOT11_UNREACHABLE_DETECTION_THRESHOLD:
            v32 = 2000; out = 4; break;
        case OID_DOT11_HIDDEN_NETWORK_ENABLED:
        case OID_DOT11_SAFE_MODE_ENABLED:
        case OID_DOT11_MEDIA_STREAMING_ENABLED: {
            need = sizeof(BOOLEAN);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            *(BOOLEAN *)buf = FALSE; out = need; break;
        }
        case OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM: {
            need = sizeof(DOT11_AUTH_ALGORITHM_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_AUTH_ALGORITHM_LIST al = (PDOT11_AUTH_ALGORITHM_LIST)buf;
            RtlZeroMemory(al, need);
            al->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            al->Header.Revision = DOT11_AUTH_ALGORITHM_LIST_REVISION_1;
            al->Header.Size = sizeof(DOT11_AUTH_ALGORITHM_LIST);
            al->uNumOfEntries = 1;
            al->uTotalNumOfEntries = 1;
            al->AlgorithmIds[0] = a->auth_algo;
            out = need; break;
        }
        case OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM:
        case OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM: {
            need = sizeof(DOT11_CIPHER_ALGORITHM_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_CIPHER_ALGORITHM_LIST cl = (PDOT11_CIPHER_ALGORITHM_LIST)buf;
            RtlZeroMemory(cl, need);
            cl->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            cl->Header.Revision = DOT11_CIPHER_ALGORITHM_LIST_REVISION_1;
            cl->Header.Size = sizeof(DOT11_CIPHER_ALGORITHM_LIST);
            cl->uNumOfEntries = 1;
            cl->uTotalNumOfEntries = 1;
            cl->AlgorithmIds[0] = (oid == OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM) ? a->ucast_cipher : a->mcast_cipher;
            out = need; break;
        }
        case OID_DOT11_SUPPORTED_UNICAST_ALGORITHM_PAIR:
        case OID_DOT11_SUPPORTED_MULTICAST_ALGORITHM_PAIR: {
            need = sizeof(DOT11_AUTH_CIPHER_PAIR_LIST) + 2 * sizeof(DOT11_AUTH_CIPHER_PAIR);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_AUTH_CIPHER_PAIR_LIST pl = (PDOT11_AUTH_CIPHER_PAIR_LIST)buf;
            RtlZeroMemory(pl, need);
            pl->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            pl->Header.Revision = DOT11_AUTH_CIPHER_PAIR_LIST_REVISION_1;
            pl->Header.Size = sizeof(DOT11_AUTH_CIPHER_PAIR_LIST);
            pl->uNumOfEntries = 3;
            pl->uTotalNumOfEntries = 3;
            pl->AuthCipherPairs[0].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN; pl->AuthCipherPairs[0].CipherAlgoId = DOT11_CIPHER_ALGO_NONE;
            pl->AuthCipherPairs[1].AuthAlgoId = DOT11_AUTH_ALGO_RSNA_PSK;   pl->AuthCipherPairs[1].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;
            pl->AuthCipherPairs[2].AuthAlgoId = DOT11_AUTH_ALGO_WPA_PSK;    pl->AuthCipherPairs[2].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;
            out = need; break;
        }
        case OID_DOT11_DESIRED_SSID_LIST: {
            need = sizeof(DOT11_SSID_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_SSID_LIST sl = (PDOT11_SSID_LIST)buf;
            RtlZeroMemory(sl, need);
            sl->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            sl->Header.Revision = DOT11_SSID_LIST_REVISION_1;
            sl->Header.Size = sizeof(DOT11_SSID_LIST);
            sl->uNumOfEntries = 1;
            sl->uTotalNumOfEntries = 1;
            sl->SSIDs[0].uSSIDLength = a->desired_ssid_len;
            if (a->desired_ssid_len) RtlCopyMemory(sl->SSIDs[0].ucSSID, a->desired_ssid, a->desired_ssid_len);
            out = need; break;
        }
        case OID_DOT11_DESIRED_BSSID_LIST: {
            need = sizeof(DOT11_BSSID_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_BSSID_LIST bl = (PDOT11_BSSID_LIST)buf;
            RtlZeroMemory(bl, need);
            bl->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            bl->Header.Revision = DOT11_BSSID_LIST_REVISION_1;
            bl->Header.Size = sizeof(DOT11_BSSID_LIST);
            bl->uNumOfEntries = 1;
            bl->uTotalNumOfEntries = 1;
            RtlFillMemory(bl->BSSIDs[0], 6, 0xFF);
            out = need; break;
        }
        case OID_DOT11_EXCLUDED_MAC_ADDRESS_LIST: {
            need = sizeof(DOT11_MAC_ADDRESS_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_MAC_ADDRESS_LIST ml = (PDOT11_MAC_ADDRESS_LIST)buf;
            RtlZeroMemory(ml, need);
            ml->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            ml->Header.Revision = DOT11_MAC_ADDRESS_LIST_REVISION_1;
            ml->Header.Size = sizeof(DOT11_MAC_ADDRESS_LIST);
            out = need; break;
        }
        case OID_DOT11_ACTIVE_PHY_LIST: {
            need = sizeof(DOT11_PHY_ID_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_PHY_ID_LIST pl = (PDOT11_PHY_ID_LIST)buf;
            RtlZeroMemory(pl, need);
            pl->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            pl->Header.Revision = DOT11_PHY_ID_LIST_REVISION_1;
            pl->Header.Size = sizeof(DOT11_PHY_ID_LIST);
            pl->uNumOfEntries = 1;
            pl->uTotalNumOfEntries = 1;
            pl->dot11PhyId[0] = 0;
            out = need; break;
        }
        case OID_DOT11_EXTSTA_CAPABILITY: {
            need = sizeof(DOT11_EXTSTA_CAPABILITY);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_EXTSTA_CAPABILITY cap = (PDOT11_EXTSTA_CAPABILITY)buf;
            RtlZeroMemory(cap, need);
            cap->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            cap->Header.Revision = DOT11_EXTSTA_CAPABILITY_REVISION_1;
            cap->Header.Size = sizeof(DOT11_EXTSTA_CAPABILITY);
            cap->uScanSSIDListSize = 4;
            cap->uDesiredBSSIDListSize = 8;
            cap->uDesiredSSIDListSize = 1;
            cap->uExcludedMacAddressListSize = 4;
            cap->uPrivacyExemptionListSize = 32;
            cap->uKeyMappingTableSize = 4;
            cap->uDefaultKeyTableSize = 4;
            cap->uWEPKeyValueMaxLength = 13;
            cap->uPMKIDCacheSize = 3;
            cap->uMaxNumPerSTADefaultKeyTables = 0;
            out = need; break;
        }
        case OID_DOT11_ENUM_ASSOCIATION_INFO: {
            need = sizeof(DOT11_ASSOCIATION_INFO_LIST);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_ASSOCIATION_INFO_LIST ail = (PDOT11_ASSOCIATION_INFO_LIST)buf;
            RtlZeroMemory(ail, need);
            ail->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            ail->Header.Revision = DOT11_ASSOCIATION_INFO_LIST_REVISION_1;
            ail->Header.Size = sizeof(DOT11_ASSOCIATION_INFO_LIST);
            out = need; break;
        }
        case OID_DOT11_STATISTICS: {
            need = sizeof(DOT11_STATISTICS);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PDOT11_STATISTICS ds = (PDOT11_STATISTICS)buf;
            RtlZeroMemory(ds, need);
            ds->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            ds->Header.Revision = DOT11_STATISTICS_REVISION_1;
            ds->Header.Size = sizeof(DOT11_STATISTICS);
            ds->ullFourWayHandshakeFailures = 0;
            out = need; break;
        }
        case OID_GEN_STATISTICS: {
            need = sizeof(NDIS_STATISTICS_INFO);
            if (len < need) { st = NDIS_STATUS_BUFFER_TOO_SHORT; break; }
            PNDIS_STATISTICS_INFO si = (PNDIS_STATISTICS_INFO)buf;
            RtlZeroMemory(si, need);
            si->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            si->Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
            si->Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
            si->SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV |
                                      NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT;
            si->ifHCInUcastPkts = a->rx_ok;
            si->ifHCOutUcastPkts = a->tx_ok;
            out = need; break;
        }
        default:
            st = NDIS_STATUS_NOT_SUPPORTED;
            break;
        }

        if (st == NDIS_STATUS_SUCCESS && out > 0 && need == 0) {
            if (len < out) { need = out; st = NDIS_STATUS_BUFFER_TOO_SHORT; }
            else RtlCopyMemory(buf, &v32, out);
        }
        req->DATA.QUERY_INFORMATION.BytesWritten = (st == NDIS_STATUS_SUCCESS) ? out : 0;
        req->DATA.QUERY_INFORMATION.BytesNeeded = need;
        return st;
    }

    if (req->RequestType == NdisRequestSetInformation) {
        NDIS_OID oid = req->DATA.SET_INFORMATION.Oid;
        PVOID buf = req->DATA.SET_INFORMATION.InformationBuffer;
        ULONG len = req->DATA.SET_INFORMATION.InformationBufferLength;
        req->DATA.SET_INFORMATION.BytesRead = len;

        switch (oid) {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (len >= 4) a->packet_filter = *(ULONG *)buf;
            break;
        case OID_GEN_CURRENT_LOOKAHEAD:
            if (len >= 4) a->lookahead = *(ULONG *)buf;
            break;
        case OID_DOT11_CURRENT_OPERATION_MODE:
            if (len >= sizeof(DOT11_CURRENT_OPERATION_MODE))
                a->op_mode = ((PDOT11_CURRENT_OPERATION_MODE)buf)->uCurrentOpMode;
            break;
        case OID_DOT11_NIC_POWER_STATE:
            if (len >= sizeof(BOOLEAN)) a->radio_on = *(BOOLEAN *)buf;
            break;
        case OID_DOT11_CURRENT_PHY_ID:
            if (len >= 4) a->cur_phy_id = *(ULONG *)buf;
            break;
        case OID_DOT11_AUTO_CONFIG_ENABLED:
            if (len >= 4) a->auto_config = *(ULONG *)buf;
            break;
        case OID_DOT11_EXCLUDE_UNENCRYPTED:
            if (len >= sizeof(BOOLEAN)) a->exclude_unencrypted = *(BOOLEAN *)buf;
            break;
        case OID_DOT11_DESIRED_BSS_TYPE:
            if (len >= 4) a->desired_bss_type = *(DOT11_BSS_TYPE *)buf;
            break;
        case OID_DOT11_DESIRED_SSID_LIST:
            if (len >= sizeof(DOT11_SSID_LIST)) {
                PDOT11_SSID_LIST sl = (PDOT11_SSID_LIST)buf;
                if (sl->uNumOfEntries > 0 && sl->SSIDs[0].uSSIDLength <= 32) {
                    a->desired_ssid_len = sl->SSIDs[0].uSSIDLength;
                    RtlCopyMemory(a->desired_ssid, sl->SSIDs[0].ucSSID, a->desired_ssid_len);
                }
            }
            break;
        case OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM:
            if (len >= sizeof(DOT11_AUTH_ALGORITHM_LIST)) {
                PDOT11_AUTH_ALGORITHM_LIST al = (PDOT11_AUTH_ALGORITHM_LIST)buf;
                if (al->uNumOfEntries > 0) a->auth_algo = al->AlgorithmIds[0];
            }
            break;
        case OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM:
            if (len >= sizeof(DOT11_CIPHER_ALGORITHM_LIST)) {
                PDOT11_CIPHER_ALGORITHM_LIST cl = (PDOT11_CIPHER_ALGORITHM_LIST)buf;
                if (cl->uNumOfEntries > 0) a->ucast_cipher = cl->AlgorithmIds[0];
            }
            break;
        case OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM:
            if (len >= sizeof(DOT11_CIPHER_ALGORITHM_LIST)) {
                PDOT11_CIPHER_ALGORITHM_LIST cl = (PDOT11_CIPHER_ALGORITHM_LIST)buf;
                if (cl->uNumOfEntries > 0) a->mcast_cipher = cl->AlgorithmIds[0];
            }
            break;
        case OID_DOT11_CIPHER_DEFAULT_KEY_ID:
            if (len >= 4) a->default_key_id = *(ULONG *)buf;
            break;
        case OID_DOT11_CIPHER_DEFAULT_KEY:
            if (len >= sizeof(DOT11_CIPHER_DEFAULT_KEY_VALUE)) {
                PDOT11_CIPHER_DEFAULT_KEY_VALUE kv = (PDOT11_CIPHER_DEFAULT_KEY_VALUE)buf;
                const UCHAR *pn, *k = ccmp_key(kv->ucKey, kv->usKeyLength, &pn);
                if (!kv->bDelete && kv->AlgorithmId == DOT11_CIPHER_ALGO_CCMP && k
                        && len >= (ULONG)(FIELD_OFFSET(DOT11_CIPHER_DEFAULT_KEY_VALUE, ucKey) + kv->usKeyLength)) {
                    iw620_set_key(a, NULL, (UCHAR)kv->uKeyIndex, FALSE, KEY_TYPE_ID_AES, k, 16, pn);
                }
            }
            break;
        case OID_DOT11_CIPHER_KEY_MAPPING_KEY:
            if (len >= sizeof(DOT11_BYTE_ARRAY) + sizeof(DOT11_CIPHER_KEY_MAPPING_KEY_VALUE)) {
                PDOT11_BYTE_ARRAY ba = (PDOT11_BYTE_ARRAY)buf;
                PDOT11_CIPHER_KEY_MAPPING_KEY_VALUE kv = (PDOT11_CIPHER_KEY_MAPPING_KEY_VALUE)ba->ucBuffer;
                const UCHAR *pn, *k = ccmp_key(kv->ucKey, kv->usKeyLength, &pn);
                if (!kv->bDelete && kv->AlgorithmId == DOT11_CIPHER_ALGO_CCMP && k
                        && len >= (ULONG)(FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer)
                                          + FIELD_OFFSET(DOT11_CIPHER_KEY_MAPPING_KEY_VALUE, ucKey) + kv->usKeyLength)) {
                    iw620_set_key(a, kv->PeerMacAddr, 0, TRUE, KEY_TYPE_ID_AES, k, 16, pn);
                }
            }
            break;
        case OID_DOT11_FLUSH_BSS_LIST:
            a->bss_count = 0;
            break;
        case OID_DOT11_SCAN_REQUEST:
            iw620_do_scan(a);
            break;
        case OID_DOT11_CONNECT_REQUEST:
            iw620_do_connect(a);
            break;
        case OID_DOT11_DISCONNECT_REQUEST:
            iw620_do_disconnect(a);
            break;
        case OID_DOT11_DESIRED_BSSID_LIST:
        case OID_DOT11_PMKID_LIST:
        case OID_DOT11_PRIVACY_EXEMPTION_LIST:
        case OID_DOT11_MEDIA_STREAMING_ENABLED:
        case OID_DOT11_SAFE_MODE_ENABLED:
        case OID_DOT11_POWER_MGMT_REQUEST:
        case OID_DOT11_HIDDEN_NETWORK_ENABLED:
        case OID_DOT11_UNREACHABLE_DETECTION_THRESHOLD:
        case OID_DOT11_DESIRED_PHY_LIST:
        case OID_802_3_MULTICAST_LIST:
            break;
        default:
            st = NDIS_STATUS_NOT_SUPPORTED;
            break;
        }
        return st;
    }

    if (req->RequestType == NdisRequestMethod) {
        NDIS_OID moid = req->DATA.METHOD_INFORMATION.Oid;
        if (moid == OID_DOT11_RESET_REQUEST) {
            iw620_do_disconnect(a);
            req->DATA.METHOD_INFORMATION.BytesRead = req->DATA.METHOD_INFORMATION.InputBufferLength;
            if (req->DATA.METHOD_INFORMATION.OutputBufferLength >= sizeof(DOT11_STATUS_INDICATION)) {
                PDOT11_STATUS_INDICATION si = (PDOT11_STATUS_INDICATION)req->DATA.METHOD_INFORMATION.InformationBuffer;
                si->uStatusType = DOT11_STATUS_RESET_CONFIRM;
                si->ndisStatus = NDIS_STATUS_SUCCESS;
                req->DATA.METHOD_INFORMATION.BytesWritten = sizeof(DOT11_STATUS_INDICATION);
            }
            return NDIS_STATUS_SUCCESS;
        }
        if (moid == OID_DOT11_RECV_SENSITIVITY_LIST) {
            ULONG need = sizeof(DOT11_RECV_SENSITIVITY_LIST);
            req->DATA.METHOD_INFORMATION.BytesRead = req->DATA.METHOD_INFORMATION.InputBufferLength;
            if (req->DATA.METHOD_INFORMATION.OutputBufferLength < need) {
                req->DATA.METHOD_INFORMATION.BytesNeeded = need;
                return NDIS_STATUS_BUFFER_TOO_SHORT;
            }
            PDOT11_RECV_SENSITIVITY_LIST rsl = (PDOT11_RECV_SENSITIVITY_LIST)req->DATA.METHOD_INFORMATION.InformationBuffer;
            rsl->uNumOfEntries = 1;
            rsl->uTotalNumOfEntries = 1;
            rsl->dot11RecvSensitivity[0].ucDataRate = 108;
            rsl->dot11RecvSensitivity[0].lRSSIMin = -95;
            rsl->dot11RecvSensitivity[0].lRSSIMax = -20;
            req->DATA.METHOD_INFORMATION.BytesWritten = need;
            return NDIS_STATUS_SUCCESS;
        }
        if (moid == OID_DOT11_ENUM_BSS_LIST) {
            ULONG out = 0, need = 0;
            req->DATA.METHOD_INFORMATION.BytesRead = req->DATA.METHOD_INFORMATION.InputBufferLength;
            NDIS_STATUS mst = handle_enum_bss_list(a,
                req->DATA.METHOD_INFORMATION.InformationBuffer,
                req->DATA.METHOD_INFORMATION.OutputBufferLength,
                &out, &need);
            req->DATA.METHOD_INFORMATION.BytesWritten = (mst == NDIS_STATUS_SUCCESS) ? out : 0;
            req->DATA.METHOD_INFORMATION.BytesNeeded = need;
            return mst;
        }
        return NDIS_STATUS_NOT_SUPPORTED;
    }

    return NDIS_STATUS_NOT_SUPPORTED;
}

/* ---- Miniport Lifecycle ------------------------------------------------------ */
static VOID free_adapter(PWIFI_ADAPTER a) {
    if (!a) return;
    if (a->timer) NdisFreeTimerObject(a->timer);
    for (ULONG i = 0; i < RX_RING_SIZE; i++) {
        if (a->rx_nbl[i]) NdisFreeNetBufferList(a->rx_nbl[i]);
        if (a->rx_mdl[i]) IoFreeMdl(a->rx_mdl[i]);
    }
    if (a->nbl_pool) NdisFreeNetBufferListPool(a->nbl_pool);
    if (a->cmdrsp_buf) NdisMFreeSharedMemory(a->handle, CMD_BUF_SIZE, FALSE, a->cmdrsp_buf, a->cmdrsp_buf_pa);
    if (a->cmd_buf)    NdisMFreeSharedMemory(a->handle, CMD_BUF_SIZE, FALSE, a->cmd_buf, a->cmd_buf_pa);
    if (a->evt_pool)   NdisMFreeSharedMemory(a->handle, EVT_RING_SIZE * EVT_BUF_STRIDE, FALSE, a->evt_pool, a->evt_pool_pa);
    if (a->rx_pool)    NdisMFreeSharedMemory(a->handle, RX_RING_SIZE * RX_BUF_STRIDE, FALSE, a->rx_pool, a->rx_pool_pa);
    if (a->tx_pool)    NdisMFreeSharedMemory(a->handle, TX_RING_SIZE * TX_BUF_STRIDE, FALSE, a->tx_pool, a->tx_pool_pa);
    if (a->evt_ring)   NdisMFreeSharedMemory(a->handle, EVT_RING_SIZE * sizeof(ADMA_DESC), FALSE, a->evt_ring, a->evt_ring_pa);
    if (a->rx_ring)    NdisMFreeSharedMemory(a->handle, RX_RING_SIZE * sizeof(ADMA_DESC), FALSE, a->rx_ring, a->rx_ring_pa);
    if (a->tx_ring)    NdisMFreeSharedMemory(a->handle, TX_RING_SIZE * sizeof(ADMA_DESC), FALSE, a->tx_ring, a->tx_ring_pa);
    if (a->dma)        NdisMDeregisterScatterGatherDma(a->dma);
    if (a->bar2)       NdisMUnmapIoSpace(a->handle, (PVOID)a->bar2, a->bar2_len);
    if (a->bar0)       NdisMUnmapIoSpace(a->handle, (PVOID)a->bar0, a->bar0_len);
    NdisFreeSpinLock(&a->lock);
    NdisFreeMemory(a, sizeof(*a), 0);
}

static const NDIS_OID g_supported_oids[] = {
    OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_STATISTICS,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_DOT11_MAC_ADDRESS, OID_DOT11_CURRENT_ADDRESS,
    OID_DOT11_CURRENT_OPERATION_MODE, OID_DOT11_NIC_POWER_STATE,
    OID_DOT11_HARDWARE_PHY_STATE, OID_DOT11_CURRENT_PHY_ID, OID_DOT11_SUPPORTED_PHY_TYPES,
    OID_DOT11_DESIRED_PHY_LIST, OID_DOT11_ENUM_BSS_LIST, OID_DOT11_FLUSH_BSS_LIST,
    OID_DOT11_SCAN_REQUEST, OID_DOT11_CONNECT_REQUEST, OID_DOT11_DISCONNECT_REQUEST,
    OID_DOT11_RESET_REQUEST, OID_DOT11_AUTO_CONFIG_ENABLED, OID_DOT11_EXCLUDE_UNENCRYPTED,
    OID_DOT11_DESIRED_BSS_TYPE, OID_DOT11_DESIRED_SSID_LIST, OID_DOT11_DESIRED_BSSID_LIST,
    OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM, OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM,
    OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM, OID_DOT11_CIPHER_DEFAULT_KEY_ID,
    OID_DOT11_CIPHER_DEFAULT_KEY, OID_DOT11_CIPHER_KEY_MAPPING_KEY, OID_DOT11_STATISTICS,
    OID_DOT11_UNICAST_USE_GROUP_ENABLED, OID_DOT11_CURRENT_CHANNEL,
    OID_DOT11_MPDU_MAX_LENGTH, OID_DOT11_SUPPORTED_POWER_LEVELS,
    OID_DOT11_CURRENT_TX_POWER_LEVEL, OID_DOT11_OPERATIONAL_RATE_SET,
    OID_DOT11_RECV_SENSITIVITY_LIST, OID_DOT11_WME_IMPLEMENTED
};

static NDIS_STATUS MiniportInitializeEx(NDIS_HANDLE handle, NDIS_HANDLE drv_ctx, PNDIS_MINIPORT_INIT_PARAMETERS params) {
    UNREFERENCED_PARAMETER(drv_ctx);
    PWIFI_ADAPTER a = (PWIFI_ADAPTER)NdisAllocateMemoryWithTagPriority(handle, sizeof(*a), PS5WIFI_TAG, NormalPoolPriority);
    if (!a) return NDIS_STATUS_RESOURCES;
    RtlZeroMemory(a, sizeof(*a));
    a->handle = handle;
    a->radio_on = TRUE;
    a->op_mode = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
    a->desired_bss_type = dot11_BSS_type_infrastructure;
    a->auth_algo = DOT11_AUTH_ALGO_RSNA_PSK;
    a->ucast_cipher = DOT11_CIPHER_ALGO_CCMP;
    a->mcast_cipher = DOT11_CIPHER_ALGO_CCMP;
    a->auto_config = DOT11_PHY_AUTO_CONFIG_ENABLED_FLAG | DOT11_MAC_AUTO_CONFIG_ENABLED_FLAG;
    /* Fallback locally administered MAC until GET_HW_SPEC overwrites it */
    a->perm_mac[0] = 0x02; a->perm_mac[1] = 0x50; a->perm_mac[2] = 0x53;
    a->perm_mac[3] = 0x35; a->perm_mac[4] = 0x57; a->perm_mac[5] = 0x01;
    RtlCopyMemory(a->cur_mac, a->perm_mac, 6);
    NdisAllocateSpinLock(&a->lock);

    /* Register miniport adapter registration attributes first */
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES reg;
    RtlZeroMemory(&reg, sizeof(reg));
    reg.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    reg.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.MiniportAdapterContext = a;
    reg.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE | NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER;
    reg.CheckForHangTimeInSeconds = 4;
    reg.InterfaceType = NdisInterfacePci;
    NDIS_STATUS st = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&reg);
    if (st != NDIS_STATUS_SUCCESS) { free_adapter(a); return st; }

    /* Map PCIe BAR0 and BAR2 */
    if (params->AllocatedResources) {
        ULONG mem_idx = 0;
        for (ULONG i = 0; i < params->AllocatedResources->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &params->AllocatedResources->PartialDescriptors[i];
            if (d->Type == CmResourceTypeMemory) {
                PVOID va = NULL;
                st = NdisMMapIoSpace(&va, handle, d->u.Memory.Start, d->u.Memory.Length);
                if (st != NDIS_STATUS_SUCCESS) { free_adapter(a); return st; }
                if (mem_idx == 0) { a->bar0 = (volatile UCHAR *)va; a->bar0_len = d->u.Memory.Length; }
                else if (mem_idx == 1) { a->bar2 = (volatile UCHAR *)va; a->bar2_len = d->u.Memory.Length; }
                mem_idx++;
            }
        }
    }
    if (!a->bar2 && a->bar0) { a->bar2 = a->bar0; a->bar2_len = a->bar0_len; }
    if (!a->bar2) { free_adapter(a); return NDIS_STATUS_RESOURCE_CONFLICT; }

    /* DMA registration */
    NDIS_SG_DMA_DESCRIPTION dma;
    RtlZeroMemory(&dma, sizeof(dma));
    dma.Header.Type = NDIS_OBJECT_TYPE_SG_DMA_DESCRIPTION;
    dma.Header.Revision = NDIS_SG_DMA_DESCRIPTION_REVISION_1;
    dma.Header.Size = NDIS_SIZEOF_SG_DMA_DESCRIPTION_REVISION_1;
    dma.Flags = NDIS_SG_DMA_64_BIT_ADDRESS;
    dma.MaximumPhysicalMapping = 4096;
    dma.ProcessSGListHandler = NULL;
    dma.SharedMemAllocateCompleteHandler = NULL;
    (VOID)NdisMRegisterScatterGatherDma(handle, &dma, &a->dma);

    NdisMAllocateSharedMemory(handle, TX_RING_SIZE * sizeof(ADMA_DESC), FALSE, (PVOID *)&a->tx_ring, &a->tx_ring_pa);
    NdisMAllocateSharedMemory(handle, RX_RING_SIZE * sizeof(ADMA_DESC), FALSE, (PVOID *)&a->rx_ring, &a->rx_ring_pa);
    NdisMAllocateSharedMemory(handle, EVT_RING_SIZE * sizeof(ADMA_DESC), FALSE, (PVOID *)&a->evt_ring, &a->evt_ring_pa);
    NdisMAllocateSharedMemory(handle, TX_RING_SIZE * TX_BUF_STRIDE, FALSE, (PVOID *)&a->tx_pool, &a->tx_pool_pa);
    NdisMAllocateSharedMemory(handle, RX_RING_SIZE * RX_BUF_STRIDE, FALSE, (PVOID *)&a->rx_pool, &a->rx_pool_pa);
    NdisMAllocateSharedMemory(handle, EVT_RING_SIZE * EVT_BUF_STRIDE, FALSE, (PVOID *)&a->evt_pool, &a->evt_pool_pa);
    NdisMAllocateSharedMemory(handle, CMD_BUF_SIZE, FALSE, (PVOID *)&a->cmd_buf, &a->cmd_buf_pa);
    NdisMAllocateSharedMemory(handle, CMD_BUF_SIZE, FALSE, (PVOID *)&a->cmdrsp_buf, &a->cmdrsp_buf_pa);

    if (!a->tx_ring || !a->rx_ring || !a->evt_ring || !a->tx_pool || !a->rx_pool || !a->evt_pool || !a->cmd_buf || !a->cmdrsp_buf) {
        free_adapter(a);
        return NDIS_STATUS_RESOURCES;
    }

    /* RX NBL pool */
    NET_BUFFER_LIST_POOL_PARAMETERS pp;
    RtlZeroMemory(&pp, sizeof(pp));
    pp.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pp.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    pp.fAllocateNetBuffer = TRUE;
    pp.PoolTag = PS5WIFI_TAG;
    a->nbl_pool = NdisAllocateNetBufferListPool(handle, &pp);
    if (!a->nbl_pool) { free_adapter(a); return NDIS_STATUS_RESOURCES; }

    for (ULONG i = 0; i < RX_RING_SIZE; i++) {
        a->rx_mdl[i] = IoAllocateMdl(a->rx_dot11_buf[i], 2048, FALSE, FALSE, NULL);
        if (!a->rx_mdl[i]) { free_adapter(a); return NDIS_STATUS_RESOURCES; }
        MmBuildMdlForNonPagedPool(a->rx_mdl[i]);
        a->rx_nbl[i] = NdisAllocateNetBufferAndNetBufferList(a->nbl_pool, 0, 0, a->rx_mdl[i], 0, 0);
        if (!a->rx_nbl[i]) { free_adapter(a); return NDIS_STATUS_RESOURCES; }
    }

    KeInitializeSpinLock(&a->tx_lock);

    /* Bring up IW620 hardware (non-fatal if FW missing so diagnostics stay readable) */
    (VOID)iw620_hw_init(a);

    /* General attributes */
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES gen;
    RtlZeroMemory(&gen, sizeof(gen));
    gen.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    gen.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    gen.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    gen.MediaType = NdisMediumNative802_11;
    gen.PhysicalMediumType = NdisPhysicalMediumNative802_11;
    gen.MtuSize = 1500;
    gen.MaxXmitLinkSpeed = 300000000ULL;
    gen.MaxRcvLinkSpeed  = 300000000ULL;
    gen.XmitLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    gen.RcvLinkSpeed  = NDIS_LINK_SPEED_UNKNOWN;
    gen.MediaConnectState = MediaConnectStateDisconnected;
    gen.MediaDuplexState = MediaDuplexStateHalf;
    gen.LookaheadSize = 1500;
    gen.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
    gen.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                                 NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST |
                                 NDIS_PACKET_TYPE_802_11_DIRECTED_DATA | NDIS_PACKET_TYPE_802_11_BROADCAST_DATA |
                                 NDIS_PACKET_TYPE_802_11_MULTICAST_DATA | NDIS_PACKET_TYPE_802_11_ALL_MULTICAST_DATA;
    gen.MaxMulticastListSize = 32;
    gen.MacAddressLength = 6;
    RtlCopyMemory(gen.PermanentMacAddress, a->perm_mac, 6);
    RtlCopyMemory(gen.CurrentMacAddress, a->cur_mac, 6);
    gen.AccessType = NET_IF_ACCESS_BROADCAST;
    gen.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    gen.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    gen.IfType = IF_TYPE_IEEE80211;
    gen.IfConnectorPresent = TRUE;
    gen.SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV |
                              NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT;
    gen.SupportedOidList = (PNDIS_OID)g_supported_oids;
    gen.SupportedOidListLength = sizeof(g_supported_oids);
    st = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&gen);
    if (st != NDIS_STATUS_SUCCESS) { free_adapter(a); return st; }

    /* Native 802.11 ExtSTA attributes */
    DOT11_EXTSTA_ATTRIBUTES ext;
    DOT11_AUTH_CIPHER_PAIR pairs[8];
    DOT11_AUTH_CIPHER_PAIR adhoc_pairs[1];
    DOT11_SUPPORTED_PHY_TYPES phy_types;
    DOT11_PHY_ATTRIBUTES phys[2];
    NDIS_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES n11;

    RtlZeroMemory(&ext, sizeof(ext));
    ext.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    ext.Header.Revision = DOT11_EXTSTA_ATTRIBUTES_REVISION_2;
    ext.Header.Size = RTL_SIZEOF_THROUGH_FIELD(DOT11_EXTSTA_ATTRIBUTES, pAdhocSupportedMcastAlgoPairs);
    ext.uScanSSIDListSize = 4;
    ext.uDesiredBSSIDListSize = 8;
    ext.uDesiredSSIDListSize = 1;
    ext.uExcludedMacAddressListSize = 4;
    ext.uPrivacyExemptionListSize = 32;
    ext.uKeyMappingTableSize = 4;
    ext.uDefaultKeyTableSize = 4;
    ext.uWEPKeyValueMaxLength = 13;
    ext.uPMKIDCacheSize = 3;
    ext.uMaxNumPerSTADefaultKeyTables = 0;
    ext.bStrictlyOrderedServiceClassImplemented = FALSE;
    ext.ucSupportedQoSProtocolFlags = 0;
    ext.bSafeModeImplemented = FALSE;

    pairs[0].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN; pairs[0].CipherAlgoId = DOT11_CIPHER_ALGO_NONE;
    pairs[1].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN; pairs[1].CipherAlgoId = DOT11_CIPHER_ALGO_WEP40;
    pairs[2].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN; pairs[2].CipherAlgoId = DOT11_CIPHER_ALGO_WEP104;
    pairs[3].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN; pairs[3].CipherAlgoId = DOT11_CIPHER_ALGO_WEP;
    pairs[4].AuthAlgoId = DOT11_AUTH_ALGO_RSNA;       pairs[4].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;
    pairs[5].AuthAlgoId = DOT11_AUTH_ALGO_RSNA_PSK;   pairs[5].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;
    pairs[6].AuthAlgoId = DOT11_AUTH_ALGO_WPA;        pairs[6].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;
    pairs[7].AuthAlgoId = DOT11_AUTH_ALGO_WPA_PSK;    pairs[7].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;
    ext.uInfraNumSupportedUcastAlgoPairs = 8;
    ext.pInfraSupportedUcastAlgoPairs = pairs;
    ext.uInfraNumSupportedMcastAlgoPairs = 8;
    ext.pInfraSupportedMcastAlgoPairs = pairs;
    adhoc_pairs[0].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN; adhoc_pairs[0].CipherAlgoId = DOT11_CIPHER_ALGO_NONE;
    ext.uAdhocNumSupportedUcastAlgoPairs = 1;
    ext.pAdhocSupportedUcastAlgoPairs = adhoc_pairs;
    ext.uAdhocNumSupportedMcastAlgoPairs = 1;
    ext.pAdhocSupportedMcastAlgoPairs = adhoc_pairs;

    RtlZeroMemory(&phy_types, sizeof(phy_types));
    phy_types.uNumOfEntries = 1;
    phy_types.uTotalNumOfEntries = 1;
    phy_types.dot11PHYType[0] = dot11_phy_type_erp;

    RtlZeroMemory(phys, sizeof(phys));
    phys[0].Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    phys[0].Header.Revision = DOT11_PHY_ATTRIBUTES_REVISION_1;
    phys[0].Header.Size = sizeof(DOT11_PHY_ATTRIBUTES);
    phys[0].PhyType = dot11_phy_type_erp;
    phys[0].bHardwarePhyState = TRUE;
    phys[0].bSoftwarePhyState = TRUE;
    phys[0].bCFPollable = FALSE;
    phys[0].uMPDUMaxLength = 2346;
    phys[0].TempType = dot11_temp_type_1;
    phys[0].DiversitySupport = dot11_diversity_support_dynamic;
    phys[0].uNumberSupportedPowerLevels = 1;
    phys[0].TxPowerLevels[0] = 100;
    phys[0].uNumDataRateMappingEntries = 5;
    phys[0].DataRateMappingEntries[0].ucDataRateIndex = 2;   phys[0].DataRateMappingEntries[0].usDataRateValue = 2;
    phys[0].DataRateMappingEntries[1].ucDataRateIndex = 4;   phys[0].DataRateMappingEntries[1].usDataRateValue = 4;
    phys[0].DataRateMappingEntries[2].ucDataRateIndex = 11;  phys[0].DataRateMappingEntries[2].usDataRateValue = 11;
    phys[0].DataRateMappingEntries[3].ucDataRateIndex = 22;  phys[0].DataRateMappingEntries[3].usDataRateValue = 22;
    phys[0].DataRateMappingEntries[4].ucDataRateIndex = 108; phys[0].DataRateMappingEntries[4].usDataRateValue = 108;
    phys[0].SupportedDataRatesValue.ucSupportedTxDataRatesValue[0] = 2;
    phys[0].SupportedDataRatesValue.ucSupportedTxDataRatesValue[1] = 4;
    phys[0].SupportedDataRatesValue.ucSupportedTxDataRatesValue[2] = 11;
    phys[0].SupportedDataRatesValue.ucSupportedTxDataRatesValue[3] = 22;
    phys[0].SupportedDataRatesValue.ucSupportedTxDataRatesValue[4] = 108;
    RtlCopyMemory(phys[0].SupportedDataRatesValue.ucSupportedRxDataRatesValue,
                  phys[0].SupportedDataRatesValue.ucSupportedTxDataRatesValue, 5);

    phys[1] = phys[0];
    phys[1].PhyType = dot11_phy_type_ofdm;

    RtlZeroMemory(&n11, sizeof(n11));
    n11.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES;
    n11.Header.Revision = NDIS_MINIPORT_ADAPTER_802_11_ATTRIBUTES_REVISION_2;
    n11.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES_REVISION_2;
    n11.OpModeCapability = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
    n11.NumOfTXBuffers = TX_RING_SIZE;
    n11.NumOfRXBuffers = RX_RING_SIZE;
    n11.MultiDomainCapabilityImplemented = FALSE;
    n11.NumSupportedPhys = 2;
    n11.SupportedPhyAttributes = phys;
    n11.ExtSTAAttributes = &ext;
    st = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&n11);
    if (st != NDIS_STATUS_SUCCESS) {
        diag_write_dword(L"N11AttrErr", (ULONG)st);
        free_adapter(a);
        return st;
    }

    /* Start 2ms RX polling timer */
    NDIS_TIMER_CHARACTERISTICS tc;
    RtlZeroMemory(&tc, sizeof(tc));
    tc.Header.Type = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    tc.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    tc.Header.Size = NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    tc.AllocationTag = PS5WIFI_TAG;
    tc.TimerFunction = PollTimer;
    tc.FunctionContext = a;
    st = NdisAllocateTimerObject(handle, &tc, &a->timer);
    if (st != NDIS_STATUS_SUCCESS) { free_adapter(a); return st; }

    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)POLL_MS * 10000LL;
    NdisSetTimerObject(a->timer, due, 0, NULL);

    return NDIS_STATUS_SUCCESS;
}

static VOID MiniportHaltEx(NDIS_HANDLE ctx, NDIS_HALT_ACTION action) {
    UNREFERENCED_PARAMETER(action);
    PWIFI_ADAPTER a = (PWIFI_ADAPTER)ctx;
    InterlockedExchange(&a->halting, 1);
    if (a->timer) NdisCancelTimerObject(a->timer);
    if (a->hw_ready) (VOID)iw620_exec_cmd(a, CMD_FUNC_SHUTDOWN, NULL, 0, NULL, NULL, 500);
    free_adapter(a);
}

static NDIS_STATUS MiniportPause(NDIS_HANDLE ctx, PNDIS_MINIPORT_PAUSE_PARAMETERS p) {
    UNREFERENCED_PARAMETER(p);
    InterlockedExchange(&((PWIFI_ADAPTER)ctx)->paused, 1);
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS MiniportRestart(NDIS_HANDLE ctx, PNDIS_MINIPORT_RESTART_PARAMETERS p) {
    UNREFERENCED_PARAMETER(p);
    InterlockedExchange(&((PWIFI_ADAPTER)ctx)->paused, 0);
    return NDIS_STATUS_SUCCESS;
}

static VOID MiniportReturnNbls(NDIS_HANDLE ctx, PNET_BUFFER_LIST nbl, ULONG flags) {
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(nbl); UNREFERENCED_PARAMETER(flags);
}

static VOID MiniportCancelSend(NDIS_HANDLE ctx, PVOID id) {
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id);
}

static VOID MiniportCancelOid(NDIS_HANDLE ctx, PVOID id) {
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id);
}

/* v1.0.5: at shutdown/restart the IW620 must stop writing into host memory. The restart goes through the
 * PSWloader firmware (warm re-entry) without a power cycle, so a running RX/event DMA ring would keep writing
 * into memory that now belongs to the loader and the firmware. Like mwifiex: leave the BSS, FUNC_SHUTDOWN,
 * then bus mastering off. On a bugcheck (high IRQL) only the flag is set. */
static VOID MiniportShutdownEx(NDIS_HANDLE ctx, NDIS_SHUTDOWN_ACTION action) {
    PWIFI_ADAPTER a = (PWIFI_ADAPTER)ctx;
    InterlockedExchange(&a->halting, 1);
    if (action != NdisShutdownPowerOff || KeGetCurrentIrql() > DISPATCH_LEVEL) return;
    if (a->timer) NdisCancelTimerObject(a->timer);
    if (a->hw_ready) {
        if (a->connected) {
            CMD_DEAUTH_BODY d;
            RtlCopyMemory(d.mac_addr, a->assoc_bssid, 6);
            d.reason_code = 3;
            (VOID)iw620_exec_cmd(a, CMD_802_11_DEAUTH, &d, sizeof(d), NULL, NULL, 500);
        }
        (VOID)iw620_exec_cmd(a, CMD_FUNC_SHUTDOWN, NULL, 0, NULL, NULL, 500);
        a->hw_ready = FALSE;
    }
    USHORT cmd = 0;
    if (NdisMGetBusData(a->handle, PCI_WHICHSPACE_CONFIG, 4, &cmd, sizeof(cmd)) == sizeof(cmd)) {
        cmd &= (USHORT)~0x0004u;   /* PCI command: bus master off */
        (VOID)NdisMSetBusData(a->handle, PCI_WHICHSPACE_CONFIG, 4, &cmd, sizeof(cmd));
    }
}

static VOID MiniportDevicePnPEvent(NDIS_HANDLE ctx, PNET_DEVICE_PNP_EVENT ev) {
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(ev);
}

static BOOLEAN MiniportCheckForHang(NDIS_HANDLE ctx) {
    UNREFERENCED_PARAMETER(ctx);
    return FALSE;
}

static NDIS_STATUS MiniportResetEx(NDIS_HANDLE ctx, PBOOLEAN addr_reset) {
    UNREFERENCED_PARAMETER(ctx);
    *addr_reset = FALSE;
    return NDIS_STATUS_SUCCESS;
}

static VOID DriverUnload(PDRIVER_OBJECT drv) {
    UNREFERENCED_PARAMETER(drv);
    if (g_driver) NdisMDeregisterMiniportDriver(g_driver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg) {
    if (!g_reloc_anchor) return STATUS_UNSUCCESSFUL;
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS ch;
    RtlZeroMemory(&ch, sizeof(ch));
    ch.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    ch.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    ch.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    ch.MajorNdisVersion = 6;
    ch.MinorNdisVersion = 20;
    ch.MajorDriverVersion = 1;
    ch.MinorDriverVersion = 0;
    ch.InitializeHandlerEx = MiniportInitializeEx;
    ch.HaltHandlerEx = MiniportHaltEx;
    ch.UnloadHandler = DriverUnload;
    ch.PauseHandler = MiniportPause;
    ch.RestartHandler = MiniportRestart;
    ch.OidRequestHandler = MiniportOidRequest;
    ch.SendNetBufferListsHandler = MiniportSendNbls;
    ch.ReturnNetBufferListsHandler = MiniportReturnNbls;
    ch.CancelSendHandler = MiniportCancelSend;
    ch.CheckForHangHandlerEx = MiniportCheckForHang;
    ch.ResetHandlerEx = MiniportResetEx;
    ch.DevicePnPEventNotifyHandler = MiniportDevicePnPEvent;
    ch.ShutdownHandlerEx = MiniportShutdownEx;
    ch.CancelOidRequestHandler = MiniportCancelOid;
    return NdisMRegisterMiniportDriver(drv, reg, NULL, &ch, &g_driver);
}
