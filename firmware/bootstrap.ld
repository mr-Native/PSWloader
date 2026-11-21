/* SPDX-License-Identifier: BSD-2-Clause-Patent */
ENTRY(ps5_start64)
SECTIONS {
  . = 0x100000;
  .transport_pad : { KEEP(*(.transport_pad)) }
  .entry : { KEEP(*(.entry)) }
  ASSERT(ps5_start64 == 0x100200, "PS5 startup_64 transport entry moved")
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  . = ALIGN(4096);
  ps5_bootstrap_end = .;
  ASSERT(ps5_bootstrap_end < 0x1000000, "Bootstrap exceeds low RAM window")
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
