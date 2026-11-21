/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "atom-native.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
typedef UINT8 uint8_t;typedef UINT16 uint16_t;typedef UINT32 uint32_t;typedef UINT64 uint64_t;
typedef UINT32 u32;typedef BOOLEAN bool;
#define true TRUE
#define false FALSE
#define EINVAL 22
#define GFP_KERNEL 0
static unsigned host_steps,host_depth;
static UINTN mAtomDelayUsed;
static UINT64 mAtomStarted;
static BASE_LIBRARY_JUMP_BUFFER mAtomJump;
static VOID *mAtomAllocations[32];
static UINT8 *mAtomArena;
static UINTN mAtomArenaUsed;
#define ATOM_ARENA_BYTES 131072U
static EFI_STATUS mAtomStatus;
static VOID host_fail(const char *Reason) {
 DEBUG((DEBUG_ERROR,"PS5 ATOM aborted: %a\n",Reason));
 mAtomStatus=EFI_ABORTED;LongJump(&mAtomJump,1);
}
static VOID *AtomAllocate(UINTN Count,UINTN Size) {
 if(!Count || Size>MAX_UINTN/Count)host_fail("allocation bounds");
 for(UINTN I=0;I<32;I++)if(!mAtomAllocations[I]) {
  UINTN Bytes=Count*Size;
  if(Bytes>ATOM_ARENA_BYTES-15)host_fail("arena allocation size");
  Bytes=(Bytes+15)&~15ULL;
  if(!mAtomArena || Bytes>ATOM_ARENA_BYTES-mAtomArenaUsed)host_fail("arena exhausted");
  VOID *P=mAtomArena+mAtomArenaUsed;mAtomArenaUsed+=Bytes;
  mAtomAllocations[I]=P;return P;
 }
 host_fail("allocation count");return NULL;
}
static VOID AtomFree(VOID *P) {
 if(!P)return;
 for(UINTN I=0;I<32;I++)if(mAtomAllocations[I]==P){mAtomAllocations[I]=NULL;return;}
 host_fail("unknown allocation");
}
static VOID AtomDelay(UINTN Microseconds) {
 if(Microseconds>255000 || host_steps>200000 || Microseconds>2000000-mAtomDelayUsed ||
    AsmReadTsc()-mAtomStarted>1596300000ULL*5)host_fail("delay budget");
 mAtomDelayUsed+=Microseconds;
 UINT64 Start=AsmReadTsc(),Ticks=((UINT64)Microseconds*1596300000ULL)/1000000;
 while(AsmReadTsc()-Start<Ticks)CpuPause();
}
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
/* The AMD loop timeout is wall time, not interpreted instruction count.
 * Host mock timing must never shorten native PHY lock waits. One unit=1ms. */
#define jiffies ((UINT64)(AsmReadTsc()/1596300ULL))
#define jiffies_to_msecs(x) (x)
#define time_after(a,b) ((a)>(b))
#define drm_can_sleep() 1
#define udelay(x) AtomDelay(x)
#define mdelay(x) AtomDelay((UINTN)(x)*1000)
#define msleep(x) AtomDelay((UINTN)(x)*1000)
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
