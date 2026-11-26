/* SPDX-License-Identifier: GPL-2.0-only
 * kmd-compat v1.0 (07.11.2026): Windows kernel adapter for the AMD ATOM interpreter (atom-host.c), used by
 * ps5gpulab IOCTL_PS5LAB_POST to run the VBIOS ASIC_Init like amdgpu's "GPU posting now". Same limits as the
 * UEFI adapter: bounded arena allocations, 2 s delay budget, instruction/recursion limits, MC/PLL abort. */
#include <ntddk.h>
#include <stdint.h>
typedef uint32_t u32;
typedef BOOLEAN bool;
#define true TRUE
#define false FALSE
#define EINVAL 22
#define GFP_KERNEL 0
static unsigned host_steps, host_depth;
static ULONG mAtomDelayUsed;
static ULONG64 mAtomStarted;               /* KeQueryInterruptTime, 100 ns */
static void *mAtomJump[5];                 /* __builtin_setjmp buffer */
static void *mAtomAllocations[32];
static UCHAR *mAtomArena;
static SIZE_T mAtomArenaUsed;
#define ATOM_ARENA_BYTES 131072U
static const char *mAtomFailReason;
static void host_fail(const char *reason) { mAtomFailReason = reason; __builtin_longjmp(mAtomJump, 1); }
static void *AtomAllocate(SIZE_T count, SIZE_T size)
{
    if (!count || size > ((SIZE_T)-1) / count) host_fail("allocation bounds");
    for (int i = 0; i < 32; i++) if (!mAtomAllocations[i]) {
        SIZE_T bytes = count * size;
        if (bytes > ATOM_ARENA_BYTES - 15) host_fail("arena allocation size");
        bytes = (bytes + 15) & ~(SIZE_T)15;
        if (!mAtomArena || bytes > ATOM_ARENA_BYTES - mAtomArenaUsed) host_fail("arena exhausted");
        void *p = mAtomArena + mAtomArenaUsed; mAtomArenaUsed += bytes;
        RtlZeroMemory(p, bytes);
        mAtomAllocations[i] = p; return p;
    }
    host_fail("allocation count"); return NULL;
}
static void AtomFree(void *p)
{
    if (!p) return;
    for (int i = 0; i < 32; i++) if (mAtomAllocations[i] == p) { mAtomAllocations[i] = NULL; return; }
    host_fail("unknown allocation");
}
static void AtomDelay(ULONG us)
{
    if (us > 255000 || host_steps > 200000 || us > 2000000 - mAtomDelayUsed ||
        KeQueryInterruptTime() - mAtomStarted > 5ULL * 10000000) host_fail("delay budget");
    mAtomDelayUsed += us;
    while (us) { ULONG c = us > 50 ? 50 : us; KeStallExecutionProcessor(c); us -= c; }
}
static uint32_t ReadUnaligned32(const void *p) { uint32_t v; RtlCopyMemory(&v, p, 4); return v; }
#define kcalloc(a,b,c) AtomAllocate(a,b)
#define kzalloc(a,b) AtomAllocate(1,a)
#define kfree AtomFree
#define DRM_ERROR(Format,...) host_fail(Format)
#define printk(...) ((void)0)
#define pr_info(...) host_fail("unsupported operation or operand")
#define cpu_to_le32(x) (x)
#define get_unaligned_le32(p) ReadUnaligned32(p)
#define lower_32_bits(x) ((uint32_t)(x))
#define upper_32_bits(x) ((uint32_t)((uint64_t)(x)>>32))
#define do_div(a,b) ((a)/=(b))
#define jiffies ((ULONG64)(KeQueryInterruptTime() / 10000))      /* 1 unit = 1 ms */
#define jiffies_to_msecs(x) (x)
#define time_after(a,b) ((a)>(b))
#define drm_can_sleep() 1
#define udelay(x) AtomDelay((ULONG)(x))
#define mdelay(x) AtomDelay((ULONG)(x)*1000)
#define msleep(x) AtomDelay((ULONG)(x)*1000)
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
