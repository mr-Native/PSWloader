/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_FRAMEBUFFER_PAGE_MAP_H
#define PS5_FRAMEBUFFER_PAGE_MAP_H
#include <stdint.h>
typedef int (*ps5_page_entry_read)(void *,uint64_t,uint64_t *);
/* Four-level x86-64 only. Callback reads page-table entries, never target
 * framebuffer contents. Verify every page identity-mapped and writable.
 * This is not proof of backing-memory ownership, cache policy or scanout. */
int ps5_framebuffer_identity_writable(uint64_t cr3,uint64_t address,uint64_t bytes,
                                      ps5_page_entry_read read,void *context);
#endif
