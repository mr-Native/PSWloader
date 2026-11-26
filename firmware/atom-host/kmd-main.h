/* SPDX-License-Identifier: GPL-2.0-only
 * kmd-main v1.0: execute one ATOM command table against GPU BAR5 from a Windows kernel driver.
 * Caller serialises (one execution at a time), passes the validated 64 KiB platform VBIOS (loader copy from
 * 0xC0000) and the mapped BAR5. MC/PLL legacy interfaces abort. */
#include "kmd-atom.h"
static volatile UCHAR *mAtomBar5;
static ULONG mAtomBar5Dw;
static PS5_ATOM_RESULT *mAtomRes;
static uint32_t AtomMmioRead(struct card_info *card, uint32_t reg)
{
    (void)card;
    if (reg >= mAtomBar5Dw) host_fail("BAR5 read range");
    uint32_t v = READ_REGISTER_ULONG((volatile ULONG *)(mAtomBar5 + (SIZE_T)reg * 4));
    mAtomRes->last_read_reg = reg; mAtomRes->last_read_value = v; mAtomRes->reads++;
    return v;
}
static void AtomMmioWrite(struct card_info *card, uint32_t reg, uint32_t v)
{
    (void)card;
    if (reg >= mAtomBar5Dw) host_fail("BAR5 write range");
    WRITE_REGISTER_ULONG((volatile ULONG *)(mAtomBar5 + (SIZE_T)reg * 4), v);
    mAtomRes->last_write_reg = reg; mAtomRes->last_write_value = v; mAtomRes->writes++;
    if (mAtomRes->log_count < PS5_ATOM_LOG) { mAtomRes->log[mAtomRes->log_count][0] = reg; mAtomRes->log[mAtomRes->log_count][1] = v; }
    mAtomRes->log_count++;
}
static uint32_t AtomUnsupportedRead(struct card_info *card, uint32_t reg) { (void)card; (void)reg; host_fail("unsupported PLL/MC read"); return 0; }
static void AtomUnsupportedWrite(struct card_info *card, uint32_t reg, uint32_t v) { (void)card; (void)reg; (void)v; host_fail("unsupported PLL/MC write"); }

NTSTATUS Ps5AtomExecuteKmd(volatile UCHAR *bar5, ULONG bar5_bytes, const UCHAR *rom, ULONG rom_bytes,
                           ULONG table, ULONG *params, ULONG param_dwords, PS5_ATOM_RESULT *res)
{
    static LONG busy;
    if (!bar5 || !rom || rom_bytes != 65536 || table > 80 || !params || !param_dwords || param_dwords > 64 || !res)
        return STATUS_INVALID_PARAMETER;
    if (rom[0] != 0x55 || rom[1] != 0xaa) return STATUS_INVALID_IMAGE_FORMAT;
    if (InterlockedExchange(&busy, 1)) return STATUS_DEVICE_BUSY;
    RtlZeroMemory(res, sizeof(*res));
    mAtomArena = ExAllocatePoolWithTag(NonPagedPoolNx, ATOM_ARENA_BYTES, 'motA');
    if (!mAtomArena) { InterlockedExchange(&busy, 0); return STATUS_INSUFFICIENT_RESOURCES; }
    mAtomArenaUsed = 0; mAtomDelayUsed = 0; mAtomStarted = KeQueryInterruptTime();
    mAtomBar5 = bar5; mAtomBar5Dw = bar5_bytes / 4; mAtomRes = res;
    host_steps = host_depth = 0; mAtomFailReason = NULL;
    res->status = -1;
    if (__builtin_setjmp(mAtomJump) == 0) {
        uint16_t header = get_u16((void *)rom, 0x48);
        if (header > 65536 - 36 || RtlCompareMemory(rom + header + 4, "ATOM", 4) != 4) host_fail("ATOM signature");
        struct card_info card = {0};
        card.reg_read = AtomMmioRead; card.reg_write = AtomMmioWrite;
        card.mc_read = card.pll_read = AtomUnsupportedRead; card.mc_write = card.pll_write = AtomUnsupportedWrite;
        struct atom_context ctx = {0};
        ctx.card = &card; ctx.bios = (void *)rom;
        ctx.cmd_table = get_u16((void *)rom, header + 30); ctx.data_table = get_u16((void *)rom, header + 32);
        uint16_t dir = get_u16((void *)rom, ctx.cmd_table);
        if (dir < 4 + 2 * (table + 1) || ctx.cmd_table + dir > 65536) host_fail("command directory bounds");
        uint16_t cmd = get_u16((void *)rom, ctx.cmd_table + 4 + 2 * table);
        uint16_t cmd_bytes = get_u16((void *)rom, cmd);
        if (cmd < 0x4a || cmd_bytes < 6 || (ULONG)cmd + cmd_bytes > 65536) host_fail("command bounds");
        ctx.scratch = AtomAllocate(512, sizeof(uint32_t)); ctx.scratch_size_bytes = 2048;
        atom_index_iio(&ctx, get_u16((void *)rom, ctx.data_table + 0x32) + 4);
        res->status = amdgpu_atom_execute_table(&ctx, (int)table, (uint32_t *)params, (int)param_dwords);
    }
    res->steps = host_steps;
    if (mAtomFailReason) {
        SIZE_T n = 0; while (mAtomFailReason[n] && n < sizeof(res->fail) - 1) { res->fail[n] = mAtomFailReason[n]; n++; }
        res->status = -2;
    }
    for (int i = 0; i < 32; i++) mAtomAllocations[i] = NULL;
    ExFreePoolWithTag(mAtomArena, 'motA'); mAtomArena = NULL; mAtomArenaUsed = 0;
    mAtomBar5 = NULL; mAtomRes = NULL;
    InterlockedExchange(&busy, 0);
    return STATUS_SUCCESS;
}
