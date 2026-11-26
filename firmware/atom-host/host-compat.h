#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
typedef uint32_t u32;
static unsigned host_steps,host_depth;static jmp_buf host_abort;
static void host_fail(const char *s){fprintf(stderr,"STOP: %s\n",s);longjmp(host_abort,1);}
#define GFP_KERNEL 0
#define kcalloc(a,b,c) calloc(a,b)
#define kzalloc(a,b) calloc(1,a)
#define kfree free
#define DRM_ERROR(...) do{fprintf(stderr,__VA_ARGS__);host_fail("ATOM reported error");}while(0)
#define printk(...) fprintf(stderr,__VA_ARGS__)
#define pr_info(...) fprintf(stderr,__VA_ARGS__)
#define cpu_to_le32(x) (x)
#define get_unaligned_le32(p) (*(p))
#define lower_32_bits(x) ((uint32_t)(x))
#define upper_32_bits(x) ((uint32_t)((uint64_t)(x)>>32))
#define do_div(a,b) ((a)/=(b))
#define jiffies (++host_steps)
#define jiffies_to_msecs(x) (x)
#define time_after(a,b) ((a)>(b))
#define drm_can_sleep() 1
#define udelay(x) ((void)(x))
#define mdelay(x) ((void)(x))
#define msleep(x) ((void)(x))
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
