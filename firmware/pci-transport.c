/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Matches the routing in PS5 Linux arch/x86/pci/common.c (sfc_read/write).
 * This is a firmware transport primitive, not a Windows PCI bus driver.
 */
#include "pci-transport.h"

int ps5_pci_access(const struct ps5_pci_io *io, uint32_t address, unsigned width,
                   int ari_forwarding, int write, uint32_t *value) {
    if (!io || !io->enter || !io->leave || !io->read || !io->write || !value
            || (width != 1 && width != 2 && width != 4)
            || address >= 0x08000000U || (address & (width - 1U))
            || (ari_forwarding != 0 && ari_forwarding != 1)
            || (write != 0 && write != 1)) return -1;
    int status = io->enter(io->context);
    if (status) return status;
    uint32_t target = address;
    if (ari_forwarding) {
        uint32_t rid;
        status = io->read(io->context, (address & ~0xfffU) | 0x150U, 2, &rid);
        if (status) goto done;
        rid &= 0xffff;
        if (rid != 0 && rid != 0xffff) {
            uint32_t bus = rid >> 8;
            uint32_t slot = (rid & 255) >> 3;
            uint32_t function = rid & 7;
            uint32_t raw_bus_base = address & 0x07f00000U;
            uint32_t selector, control, aperture, type;
            if (bus == 0 || bus == 32) {
                bus = 32;
                selector = 0xd80; control = 0xd90; aperture = 0xf80; type = 4;
            } else if (bus < 32) {
                selector = 0xd00; control = 0xd10; aperture = 0xf00;
                type = bus == 1 ? 4 : 5;
            } else {
                status = -2; // Unsupported RID: no selector/data side effects.
                goto done;
            }
            status = io->write(io->context, raw_bus_base | control, 4, type);
            if (status) goto done;
            uint32_t window = bus << 24 | slot << 19 | function << 16 | (address & 0xf80);
            status = io->write(io->context, raw_bus_base | selector, 4, window);
            if (status) goto done;
            target = raw_bus_base | aperture | (address & 0x7f);
        }
    }
    if (write) status = io->write(io->context, target, width, *value);
    else status = io->read(io->context, target, width, value);
done:
    io->leave(io->context);
    return status;
}
