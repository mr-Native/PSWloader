/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-settings.h v1.3 (17.11.2026, r5.7; v1.1 r5.12 pstates; v1.2 r5.16 refresh; v1.3 r5.20.1 diag; v1.4 r5.20.2 unknown keys kept; v1.5 r5.20.6 diag=full; v1.6 r5.21 bootlist; v1.7 r5.22 testsign) - PSWloader settings file \PS5UEFI.CFG (text, OS independent):
 *   # PSWloader settings v1   (r5.24; older files start with a different comment line - comment lines are ignored)
 *   preset=standard        efficiency|standard|oc
 *   lang=en                en|de
 *   fastboot=0             0|1
 *   bootorder=internal     internal|usb
 *   pstates=0              0|1  (r5.12, v1.1: ACPI CPU P-states SSDT; written only when 1, so r5.7..r5.11 files stay byte-identical)
 *   refresh=native         native|max  (r5.16, v1.2: max = highest refresh the display offers; written only when max)
 *   diag=once              off|once|full  (r5.20.1, v1.3: once = run the system check before the next boot, write \PS5DIAG.LOG,
 *                          v1.5: full = the same plus the ICC fan/temperature reads of the menu check;
 *                          then the firmware writes diag=off; written only when the key was set)
 *   bootlist=AAAAAAAA,...  (r5.21, v1.6: boot order of disks by stable id, up to PS5_BOOTLIST_MAX; written only when set;
 *                          ids of unplugged disks keep their place, see ps5_bootlist_merge)
 *   usbpowercycle=1        0|1  (r5.25.1, v1.8: power-cycle the USB root ports once at start; default 1, written only when 0)
 *   testsign=auto          auto|off  (r5.22, v1.7: auto = the firmware sets testsigning in the BCD of a started Windows ESP
 *                          if it is missing (ps5-bcd.h); written only when off)
 *   crc32=XXXXXXXX         CRC-32 (IEEE, as gBS->CalculateCrc32) over every byte before this line
 * Portable (host-tested); the caller supplies the CRC function. Missing keys keep defaults. v1.4: lines with unknown keys
 * are kept verbatim (up to PS5_SET_EXTRA bytes, whole lines only) and written back before the crc32 line, so keys of
 * newer tools survive a firmware rewrite; known keys with invalid values and comment lines are dropped. */
#ifndef PS5_SETTINGS_H_
#define PS5_SETTINGS_H_
#define PS5_SET_EXTRA 192
#define PS5_BOOTLIST_MAX 16
struct ps5_settings { unsigned bootlist_n, bootlist[PS5_BOOTLIST_MAX]; unsigned preset, lang, fastboot, bootorder, pstates, refresh, diag, testsign, usbcycle, extra_len;   /* testsign 0 auto 1 off */   /* diag 0 absent 1 once 2 off 3 full */ char extra[PS5_SET_EXTRA]; };   /* diag 0 absent 1 once 2 off */   /* refresh 0 native 1 max */   /* preset 0/1/2, lang 0 en 1 de, bootorder 0 internal 1 usb */
typedef unsigned (*ps5_crc32_fn)(const char *data, unsigned len);
static const char *const ps5_set_preset[3] = { "efficiency", "standard", "oc" };
static const char *const ps5_set_lang[2] = { "en", "de" };
static const char *const ps5_set_boot[2] = { "internal", "usb" };
static const char *const ps5_set_refresh[2] = { "native", "max" };
static const char *const ps5_set_diag[4] = { "", "once", "off", "full" };
static inline void ps5_settings_default(struct ps5_settings *s) { s->preset = 1; s->lang = 0; s->fastboot = 0; s->bootorder = 0; s->pstates = 0; s->refresh = 0; s->diag = 0; s->testsign = 0; s->usbcycle = 1; s->extra_len = 0; s->extra[0] = 0; s->bootlist_n = 0; }
static inline unsigned ps5_set_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static inline int ps5_set_eq(const char *a, unsigned n, const char *b) { unsigned m = ps5_set_len(b); if (n != m) return 0; for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static inline int ps5_set_pick(const char *v, unsigned n, const char *const *names, unsigned count) { for (unsigned i = 0; i < count; i++) if (ps5_set_eq(v, n, names[i])) return (int)i; return -1; }
static inline int ps5_set_hex(const char *v, unsigned n, unsigned *out) { unsigned x = 0; if (n != 8) return 0; for (unsigned i = 0; i < 8; i++) { char c = v[i]; unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16u; if (d > 15) return 0; x = x << 4 | d; } *out = x; return 1; }
/* returns 1 = valid file with matching crc32 line (settings filled), 0 = invalid (settings = defaults) */
static inline int ps5_settings_parse(const char *t, unsigned size, ps5_crc32_fn crc, struct ps5_settings *s)
{
    struct ps5_settings r; int crc_ok = 0;
    ps5_settings_default(&r);
    for (unsigned i = 0; i < size; ) {
        unsigned start = i, end; while (i < size && t[i] != '\n') i++;
        end = i; if (i < size) i++;
        if (end > start && t[end - 1] == '\r') end--;
        unsigned eq = start; while (eq < end && t[eq] != '=') eq++;
        if (eq >= end || t[start] == '#') continue;
        const char *k = t + start, *v = t + eq + 1; unsigned kn = eq - start, vn = end - eq - 1;
        while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
        int x;
        if (ps5_set_eq(k, kn, "crc32")) { unsigned want; crc_ok = ps5_set_hex(v, vn, &want) && crc(t, start) == want; break; }
        if (ps5_set_eq(k, kn, "preset") && (x = ps5_set_pick(v, vn, ps5_set_preset, 3)) >= 0) r.preset = (unsigned)x;
        else if (ps5_set_eq(k, kn, "lang") && (x = ps5_set_pick(v, vn, ps5_set_lang, 2)) >= 0) r.lang = (unsigned)x;
        else if (ps5_set_eq(k, kn, "fastboot") && vn == 1 && (v[0] == '0' || v[0] == '1')) r.fastboot = (unsigned)(v[0] - '0');
        else if (ps5_set_eq(k, kn, "usbpowercycle") && vn == 1 && (v[0] == '0' || v[0] == '1')) r.usbcycle = (unsigned)(v[0] - '0');   /* v1.8 */
        else if (ps5_set_eq(k, kn, "pstates") && vn == 1 && (v[0] == '0' || v[0] == '1')) r.pstates = (unsigned)(v[0] - '0');
        else if (ps5_set_eq(k, kn, "bootorder") && (x = ps5_set_pick(v, vn, ps5_set_boot, 2)) >= 0) r.bootorder = (unsigned)x;
        else if (ps5_set_eq(k, kn, "refresh") && (x = ps5_set_pick(v, vn, ps5_set_refresh, 2)) >= 0) r.refresh = (unsigned)x;
        else if (ps5_set_eq(k, kn, "diag") && vn && (x = ps5_set_pick(v, vn, ps5_set_diag, 4)) > 0) r.diag = (unsigned)x;
        else if (ps5_set_eq(k, kn, "testsign") && (ps5_set_eq(v, vn, "auto") || ps5_set_eq(v, vn, "off"))) r.testsign = ps5_set_eq(v, vn, "off");   /* v1.7 */
        else if (ps5_set_eq(k, kn, "bootlist")) {   /* v1.6: 8-hex ids separated by commas; a bad entry ends the list */
            unsigned at = 0, id; r.bootlist_n = 0;
            while (at + 8 <= vn && r.bootlist_n < PS5_BOOTLIST_MAX && ps5_set_hex(v + at, 8, &id)) {
                unsigned d = 0; for (unsigned j = 0; j < r.bootlist_n; j++) if (r.bootlist[j] == id) d = 1;
                if (!d && id) r.bootlist[r.bootlist_n++] = id;
                at += 8; if (at < vn && v[at] == ',') at++; else break;
            }
        }
        else if (!ps5_set_eq(k, kn, "preset") && !ps5_set_eq(k, kn, "lang") && !ps5_set_eq(k, kn, "fastboot") && !ps5_set_eq(k, kn, "usbpowercycle") && !ps5_set_eq(k, kn, "pstates") &&
                 !ps5_set_eq(k, kn, "bootorder") && !ps5_set_eq(k, kn, "refresh") && !ps5_set_eq(k, kn, "diag") && !ps5_set_eq(k, kn, "bootlist") && !ps5_set_eq(k, kn, "testsign") &&
                 r.extra_len + (end - start) + 3 <= PS5_SET_EXTRA) {   /* v1.4: unknown key, kept with CRLF */
            for (unsigned j = start; j < end; j++) r.extra[r.extra_len++] = t[j];
            r.extra[r.extra_len++] = '\r'; r.extra[r.extra_len++] = '\n'; r.extra[r.extra_len] = 0;
        }
    }
    if (!crc_ok) { ps5_settings_default(s); return 0; }
    *s = r; return 1;
}
/* legacy \PS5POWER.CFG ("preset=..." without crc): returns preset 0..2 or -1 */
static inline int ps5_settings_legacy_preset(const char *t, unsigned size)
{
    for (unsigned i = 0; i + 7 <= size; i++) {
        if (!ps5_set_eq(t + i, 7, "preset=")) continue;
        unsigned j = i + 7, n = 0; while (j + n < size && t[j + n] != '\r' && t[j + n] != '\n' && t[j + n] != ' ') n++;
        return ps5_set_pick(t + j, n, ps5_set_preset, 3);
    }
    return -1;
}
static inline unsigned ps5_set_put(char *o, unsigned at, unsigned cap, const char *s) { while (*s && at + 1 < cap) o[at++] = *s++; o[at] = 0; return at; }
/* formats the full file incl. crc32 line into out (cap >= 160 + PS5_SET_EXTRA); returns length */
static inline unsigned ps5_settings_format(const struct ps5_settings *s, ps5_crc32_fn crc, char *out, unsigned cap)
{
    static const char hx[] = "0123456789ABCDEF";
    unsigned n = 0;
    n = ps5_set_put(out, n, cap, "# PSWloader settings v1\r\npreset="); n = ps5_set_put(out, n, cap, ps5_set_preset[s->preset > 2 ? 1 : s->preset]);
    n = ps5_set_put(out, n, cap, "\r\nlang="); n = ps5_set_put(out, n, cap, ps5_set_lang[s->lang > 1 ? 0 : s->lang]);
    n = ps5_set_put(out, n, cap, s->fastboot ? "\r\nfastboot=1\r\nbootorder=" : "\r\nfastboot=0\r\nbootorder=");
    n = ps5_set_put(out, n, cap, ps5_set_boot[s->bootorder > 1 ? 0 : s->bootorder]);
    n = ps5_set_put(out, n, cap, s->pstates ? "\r\npstates=1\r\n" : "\r\n");
    if (s->refresh == 1) n = ps5_set_put(out, n, cap, "refresh=max\r\n");
    if (s->usbcycle == 0) n = ps5_set_put(out, n, cap, "usbpowercycle=0\r\n");   /* v1.8: default 1 is not written */
    if (s->diag >= 1 && s->diag <= 3) { n = ps5_set_put(out, n, cap, "diag="); n = ps5_set_put(out, n, cap, ps5_set_diag[s->diag]); n = ps5_set_put(out, n, cap, "\r\n"); }
    if (s->testsign == 1) n = ps5_set_put(out, n, cap, "testsign=off\r\n");   /* v1.7 */
    if (s->bootlist_n && s->bootlist_n <= PS5_BOOTLIST_MAX) {   /* v1.6 */
        n = ps5_set_put(out, n, cap, "bootlist=");
        for (unsigned i = 0; i < s->bootlist_n; i++) {
            char h[10]; for (int j = 0; j < 8; j++) h[j] = hx[(s->bootlist[i] >> (28 - 4 * j)) & 15];
            h[8] = i + 1 < s->bootlist_n ? ',' : 0; h[9] = 0; n = ps5_set_put(out, n, cap, h);
        }
        n = ps5_set_put(out, n, cap, "\r\n");
    }
    if (s->extra_len && s->extra_len < PS5_SET_EXTRA) n = ps5_set_put(out, n, cap, s->extra);   /* v1.4 */
    unsigned c = crc(out, n); char line[] = "crc32=00000000\r\n";
    for (int i = 0; i < 8; i++) line[6 + i] = hx[(c >> (28 - 4 * i)) & 15];
    return ps5_set_put(out, n, cap, line);
}
/* v1.6 r5.21: display/boot order of the disks present now. present[] = ids in discovery order (unique). Disks in the
 * stored list come first in stored order, the others follow in discovery order; with no stored list and
 * usb_first (legacy bootorder=usb), USB disks (is_usb[i]) come before the rest. out[] = indices into present[]. */
static inline unsigned ps5_bootlist_order(const unsigned *stored, unsigned sn, const unsigned *present, const unsigned char *is_usb,
                                          unsigned pn, int usb_first, unsigned *out)
{
    unsigned n = 0, used[PS5_BOOTLIST_MAX] = { 0 };
    if (pn > PS5_BOOTLIST_MAX) pn = PS5_BOOTLIST_MAX;
    for (unsigned i = 0; i < sn; i++) for (unsigned j = 0; j < pn; j++) if (!used[j] && present[j] == stored[i]) { used[j] = 1; out[n++] = j; }
    for (int pass = 0; pass < 2; pass++)
        for (unsigned j = 0; j < pn; j++) {
            if (used[j]) continue;
            if (!sn && usb_first && pass == 0 && !is_usb[j]) continue;
            used[j] = 1; out[n++] = j;
        }
    return n;
}
/* v1.6 r5.21: new stored list after the user reordered the present disks (neworder = their ids, new order). Places of
 * present disks in the old list are refilled in the new order, unplugged disks keep their place, present disks not
 * in the old list are appended. Returns the new length (<= PS5_BOOTLIST_MAX). */
static inline unsigned ps5_bootlist_merge(const unsigned *old, unsigned on, const unsigned *neworder, unsigned nn, unsigned *out)
{
    unsigned n = 0, k = 0;
    for (unsigned i = 0; i < on && n < PS5_BOOTLIST_MAX; i++) {
        int present = 0; for (unsigned j = 0; j < nn; j++) if (neworder[j] == old[i]) present = 1;
        if (!present) out[n++] = old[i];
        else if (k < nn) out[n++] = neworder[k++];
    }
    while (k < nn && n < PS5_BOOTLIST_MAX) {
        int dup = 0; for (unsigned j = 0; j < n; j++) if (out[j] == neworder[k]) dup = 1;
        if (!dup) out[n++] = neworder[k];
        k++;
    }
    return n;
}
#endif
