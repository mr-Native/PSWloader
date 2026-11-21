/* winload-locate v1.0 - version-independent location of the PS5 native-boot winload patch points by byte
 * signature (instead of fixed RVAs), so a Windows cumulative update does not break the boot. Signatures were
 * derived from winload 26100.9444 and 26100.9711 (identical + unique in both). Portable (host test + UEFI).
 * The caller passes winload's loaded .text section. Offsets returned are RVAs (text_rva + offset). */
#ifndef PS5_WINLOAD_LOCATE_H
#define PS5_WINLOAD_LOCATE_H
#include <stdint.h>
#include <stddef.h>
#define PS5_WL_SITES 9
struct ps5_wl_loc {
    uint32_t sti_rva;            /* RVA of the `fb` byte of Archpx64EnableInterruptsAsm (patch fb->c3) */
    uint32_t site_rva[PS5_WL_SITES];  /* RVA of each EBS call-site `e8` opcode (read rel32 for the target) */
    int sti_ok;                  /* 1 = sti thunk found uniquely */
    int sites_ok;                /* count of sites found uniquely (PS5_WL_SITES on full success) */
};
/* 0 = sti + all sites located uniquely; negative = sti missing (-1) or a site missing/ambiguous (-(2+idx)).
 * Non-fatal partials are still recorded in *out so the caller can apply what was found. */
int ps5_wl_locate(const uint8_t *text, size_t text_len, uint32_t text_rva, struct ps5_wl_loc *out);
#endif
