/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_ACPI_VALIDATE_H
#define PS5_ACPI_VALIDATE_H
#include <stddef.h>
#include <stdint.h>
/* Validate bounded RSDP/XSDT/FADT/DSDT/APIC/MCFG candidate without dereferencing
 * any physical pointer. Semantic compatibility with Windows is not implied. */
int ps5_validate_acpi(const uint8_t *arena, size_t size, uint64_t base);
#endif
