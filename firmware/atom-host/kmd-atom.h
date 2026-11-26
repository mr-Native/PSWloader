/* SPDX-License-Identifier: GPL-2.0-only - kmd-atom v1.0: ATOM table execution from a Windows kernel driver */
#ifndef PS5_KMD_ATOM_H
#define PS5_KMD_ATOM_H
#include <ntddk.h>
#define PS5_ATOM_LOG 256
#pragma pack(push, 1)
typedef struct {
    LONG status;                     /* 0 ok, -1 table error, -2 aborted (see fail) */
    ULONG steps, reads, writes;
    ULONG last_read_reg, last_read_value, last_write_reg, last_write_value;
    char fail[48];
    ULONG log_count;
    ULONG log[PS5_ATOM_LOG][2];      /* first writes: reg (dword), value */
} PS5_ATOM_RESULT;
#pragma pack(pop)
NTSTATUS Ps5AtomExecuteKmd(volatile UCHAR *bar5, ULONG bar5_bytes, const UCHAR *rom, ULONG rom_bytes,
                           ULONG table, ULONG *params, ULONG param_dwords, PS5_ATOM_RESULT *res);
#endif
