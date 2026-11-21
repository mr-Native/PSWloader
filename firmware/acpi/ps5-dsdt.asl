/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Development candidate: measured PCI windows, no invented sleep or INTx
 * routing. Pair with hardware-reduced FADT; not installed by current builder.
 */
DefinitionBlock ("", "DSDT", 2, "PSWLDR", "PS5WIN11", 1)
{
    Name (_S0, Package (4) { Zero, Zero, Zero, Zero })
    Scope (_SB)
    {
        Device (C000)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x00)
        }
        Device (C001)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x01)
        }
        Device (C002)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x02)
        }
        Device (C003)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x03)
        }
        Device (C004)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x04)
        }
        Device (C005)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x05)
        }
        Device (C006)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x06)
        }
        Device (C007)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x07)
        }
        Device (C008)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x08)
        }
        Device (C009)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x09)
        }
        Device (C00A)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0A)
        }
        Device (C00B)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0B)
        }
        Device (C00C)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0C)
        }
        Device (C00D)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0D)
        }
        Device (C00E)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0E)
        }
        Device (C00F)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x0F)
        }
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))
            Name (_UID, Zero)
            Name (_SEG, Zero)
            Name (_BBN, Zero)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0, 0, 0x7F, 0, 0x80)
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0, 0x1000, 0x2FFF, 0, 0x2000)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite, 0, 0x80000000, 0xC43FFFFF, 0, 0x44400000)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite, 0, 0xD0000000, 0xE06FFFFF, 0, 0x10700000)
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    Prefetchable, ReadWrite, 0, 0x980000000, 0x99FFFFFFF, 0, 0x20000000)
            })
        }
        // ECAM belongs to the host controller, not to endpoint BAR allocation.
        Device (ECAM)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0xF0000000, 0x08000000)
            })
        }
    }
}
