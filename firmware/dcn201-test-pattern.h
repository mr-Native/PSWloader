/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_DCN201_TEST_PATTERN_H
#define PS5_DCN201_TEST_PATTERN_H
#include <stdint.h>
struct ps5_dcn201_register_write {uint32_t offset,mask,value;};
/* Build six actual DPG0 writes for a solid RGB color. Caller must bring up
 * clocks, timing, OPP routing and output link. No memory framebuffer required.
 * Values are 10-bit RGB; no GOP is represented by this test pattern. */
int ps5_dcn201_solid_pattern(unsigned width,unsigned height,unsigned red,unsigned green,
                            unsigned blue,struct ps5_dcn201_register_write out[6]);
#endif
