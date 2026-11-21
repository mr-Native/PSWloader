/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_DCN201_MEMORY_H
#define PS5_DCN201_MEMORY_H
#include <stdint.h>
struct ps5_dcn201_memory {uint64_t fb_base,fb_end,uma_base,uma_end;};
/* MC register fields are 24-bit units of 16MiB; TOP is inclusive.
 * Pure decode; successful decoding does not establish CPU accessibility.
 * Outputs unchanged on failure. All ranges below use exclusive ends. */
int ps5_dcn201_memory_decode(uint32_t base,uint32_t top,uint32_t offset,struct ps5_dcn201_memory *out);
int ps5_dcn201_uma_contains(const struct ps5_dcn201_memory *,uint64_t address,uint64_t bytes);
/* Convert original GPU allocation address, NOT an already-programmed HUBP
 * surface by assumption. GPU range has precedence as in Linux DCN201. */
int ps5_dcn201_gpu_to_uma(const struct ps5_dcn201_memory *,uint64_t address,uint64_t bytes,uint64_t *out);
/* Pick a 64KiB-aligned framebuffer within the intersection of a caller-
 * verified RAM interval and UMA window. Does not reserve or touch memory. */
int ps5_dcn201_framebuffer_candidate(const struct ps5_dcn201_memory *,uint64_t ram_base,
                                    uint64_t ram_bytes,uint64_t bytes,uint64_t *out);
struct ps5_dcn201_aperture_plan {uint64_t cpu_address,surface_address,bytes;};
/* HDP non-surface base maps BAR offsets into original GPU FB space.
 * Build an aligned plan within both BAR and FB windows. This proves only
 * address arithmetic: CPU accessibility and ownership remain caller checks. */
int ps5_dcn201_aperture_framebuffer(const struct ps5_dcn201_memory *,uint64_t cpu_base,
                                   uint64_t aperture_bytes,uint32_t hdp_low,uint32_t hdp_high,
                                   uint64_t bytes,struct ps5_dcn201_aperture_plan *out);
#endif
