/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-pstate-ssdt.h v1.0 (16.11.2026, r5.12, telemetry plan step 2) - portable AML generator for a
 * processor performance SSDT on the PS5 (AMD family 17h, 16 logical CPUs \_SB.C000..C00F, ACPI0007,
 * processor UID n == APIC ID n, SMT siblings 2k/2k+1 share one core).
 *   \_SB.PPCT  Package(2) { FFixedHW, FFixedHW }          AMD: control C0010062 / status C0010063 (OS knows the MSRs)
 *   \_SB.PPSS  Package(n) { {MHz, mW, 0, 0, idx, idx} }   only P-states the hardware defines (PstateEn, idx <= PstateMaxVal)
 *   \_SB.C0xx  Method(_PCT) / Method(_PSS) return the shared packages; Name(_PSD) = {5, 0, core, HW_ALL, 2}
 * Nothing is invented: frequency = FID*200/DID, power = VID voltage * IDD current from the same PStateDef MSR.
 * Integers are always DWordConst (fixed size, easy to check). Host-tested with iasl -d (test_pstate_ssdt.c). */
#ifndef PS5_PSTATE_SSDT_H_
#define PS5_PSTATE_SSDT_H_
#define PS5_PSS_MAX 8
#define PS5_PSS_CPUS 16
struct ps5_pstate { unsigned mhz, mw; };

/* decode PStateDef0..7 + PStateCurLim (C0010061); returns count of contiguous enabled P-states from P0 (0 = none) */
static inline unsigned ps5_pstate_decode(const unsigned long long def[PS5_PSS_MAX], unsigned long long curlim, struct ps5_pstate out[PS5_PSS_MAX])
{
    unsigned max = (unsigned)((curlim >> 4) & 7), n = 0;
    for (unsigned i = 0; i <= max && i < PS5_PSS_MAX; i++) {
        unsigned long long d = def[i];
        unsigned fid = (unsigned)(d & 0xFF), did = (unsigned)((d >> 8) & 0x3F), vid = (unsigned)((d >> 14) & 0xFF);
        unsigned idd = (unsigned)((d >> 22) & 0xFF), div = (unsigned)((d >> 30) & 3);
        if (!(d >> 63) || did < 8 || fid < 16) break;         /* disabled or nonsense (Zen DID >= 8) */
        unsigned mhz = fid * 200 / did;
        if (mhz < 400 || mhz > 5000) break;
        unsigned mv = vid <= 248 ? 1550 - vid * 25 / 4 : 0;   /* SVI2: 1.55 V - 6.25 mV * VID */
        unsigned ma = div == 0 ? idd * 1000 : div == 1 ? idd * 100 : div == 2 ? idd * 10 : 0;
        out[n].mhz = mhz; out[n].mw = (unsigned)((unsigned long long)mv * ma / 1000ull); n++;
    }
    return n;
}

struct ps5_aml { unsigned char *b; unsigned n, cap; int err; };
static inline void ps5_aml_put(struct ps5_aml *a, unsigned char c) { if (a->n < a->cap) a->b[a->n++] = c; else a->err = 1; }
static inline void ps5_aml_mem(struct ps5_aml *a, const char *s, unsigned n) { for (unsigned i = 0; i < n; i++) ps5_aml_put(a, (unsigned char)s[i]); }
static inline void ps5_aml_dword(struct ps5_aml *a, unsigned v) { ps5_aml_put(a, 0x0C); for (int i = 0; i < 4; i++) ps5_aml_put(a, (unsigned char)(v >> (8 * i))); }
/* open a PkgLength-prefixed object: reserve 3 bytes; close moves the body to the minimal encoding */
static inline unsigned ps5_aml_open(struct ps5_aml *a) { unsigned at = a->n; ps5_aml_put(a, 0); ps5_aml_put(a, 0); ps5_aml_put(a, 0); return at; }
static inline void ps5_aml_close(struct ps5_aml *a, unsigned at)
{
    if (a->err) return;
    unsigned body = a->n - at - 3, enc = body + 1 <= 63 ? 1 : body + 2 <= 0xFFF ? 2 : 3, len = body + enc;
    if (enc == 3 && len > 0xFFFFF) { a->err = 1; return; }
    for (unsigned i = 0; i < body; i++) a->b[at + enc + i] = a->b[at + 3 + i];
    if (enc == 1) a->b[at] = (unsigned char)len;
    else if (enc == 2) { a->b[at] = (unsigned char)(0x40 | (len & 0xF)); a->b[at + 1] = (unsigned char)(len >> 4); }
    else { a->b[at] = (unsigned char)(0x80 | (len & 0xF)); a->b[at + 1] = (unsigned char)(len >> 4); a->b[at + 2] = (unsigned char)(len >> 12); }
    a->n = at + enc + body;
}
static inline void ps5_aml_path(struct ps5_aml *a, const char *seg2) { ps5_aml_put(a, '\\'); ps5_aml_put(a, 0x2E); ps5_aml_mem(a, "_SB_", 4); ps5_aml_mem(a, seg2, 4); }
/* ResourceTemplate { Register (FFixedHW, 0, 0, 0) } as Buffer */
static inline void ps5_aml_ffh(struct ps5_aml *a)
{
    static const char gas[17] = { (char)0x82, 0x0C, 0x00, 0x7F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x79, 0x00 };
    ps5_aml_put(a, 0x11); unsigned p = ps5_aml_open(a); ps5_aml_dword(a, 17); ps5_aml_mem(a, gas, 17); ps5_aml_close(a, p);
}
static inline void ps5_aml_return_path(struct ps5_aml *a, const char *name, const char *target)
{   /* Method (name, 0, NotSerialized) { Return (\_SB.target) } */
    ps5_aml_put(a, 0x14); unsigned p = ps5_aml_open(a); ps5_aml_mem(a, name, 4); ps5_aml_put(a, 0x00);
    ps5_aml_put(a, 0xA4); ps5_aml_path(a, target); ps5_aml_close(a, p);
}

/* builds the complete SSDT (header + checksum); returns its length or 0 on error / no P-states */
static inline unsigned ps5_pstate_ssdt(const struct ps5_pstate *ps, unsigned count, unsigned char *buf, unsigned cap)
{
    static const char hx[] = "0123456789ABCDEF";
    struct ps5_aml a = { buf, 0, cap, 0 };
    if (count == 0 || count > PS5_PSS_MAX || cap < 36) return 0;
    ps5_aml_mem(&a, "SSDT\0\0\0\0\2\0PSWLDRPS5PSTAT\1\0\0\0PS5 \1\0\0\0", 36);
    ps5_aml_put(&a, 0x10); unsigned sc = ps5_aml_open(&a); ps5_aml_put(&a, '\\'); ps5_aml_mem(&a, "_SB_", 4);   /* Scope (\_SB) */
    ps5_aml_put(&a, 0x08); ps5_aml_mem(&a, "PPCT", 4);                                                         /* Name (PPCT, ...) */
    ps5_aml_put(&a, 0x12); unsigned pk = ps5_aml_open(&a); ps5_aml_put(&a, 2); ps5_aml_ffh(&a); ps5_aml_ffh(&a); ps5_aml_close(&a, pk);
    ps5_aml_put(&a, 0x08); ps5_aml_mem(&a, "PPSS", 4);                                                         /* Name (PPSS, ...) */
    ps5_aml_put(&a, 0x12); pk = ps5_aml_open(&a); ps5_aml_put(&a, (unsigned char)count);
    for (unsigned i = 0; i < count; i++) {
        ps5_aml_put(&a, 0x12); unsigned e = ps5_aml_open(&a); ps5_aml_put(&a, 6);
        ps5_aml_dword(&a, ps[i].mhz); ps5_aml_dword(&a, ps[i].mw); ps5_aml_dword(&a, 0); ps5_aml_dword(&a, 0);
        ps5_aml_dword(&a, i); ps5_aml_dword(&a, i); ps5_aml_close(&a, e);
    }
    ps5_aml_close(&a, pk);
    ps5_aml_close(&a, sc);
    for (unsigned c = 0; c < PS5_PSS_CPUS; c++) {
        char seg[4] = { 'C', '0', '0', hx[c & 15] };
        ps5_aml_put(&a, 0x10); sc = ps5_aml_open(&a); ps5_aml_path(&a, seg);                                   /* Scope (\_SB.C0xx) */
        ps5_aml_return_path(&a, "_PCT", "PPCT");
        ps5_aml_return_path(&a, "_PSS", "PPSS");
        ps5_aml_put(&a, 0x08); ps5_aml_mem(&a, "_PSD", 4);                                                     /* Name (_PSD, ...) */
        ps5_aml_put(&a, 0x12); pk = ps5_aml_open(&a); ps5_aml_put(&a, 1);
        ps5_aml_put(&a, 0x12); unsigned e = ps5_aml_open(&a); ps5_aml_put(&a, 5);
        ps5_aml_dword(&a, 5); ps5_aml_dword(&a, 0); ps5_aml_dword(&a, c / 2); ps5_aml_dword(&a, 0xFE); ps5_aml_dword(&a, 2);
        ps5_aml_close(&a, e); ps5_aml_close(&a, pk);
        ps5_aml_close(&a, sc);
    }
    if (a.err) return 0;
    buf[4] = (unsigned char)a.n; buf[5] = (unsigned char)(a.n >> 8); buf[6] = (unsigned char)(a.n >> 16); buf[7] = (unsigned char)(a.n >> 24);
    unsigned char sum = 0; buf[9] = 0; for (unsigned i = 0; i < a.n; i++) sum = (unsigned char)(sum + buf[i]);
    buf[9] = (unsigned char)(0x100 - sum);
    return a.n;
}
#endif
