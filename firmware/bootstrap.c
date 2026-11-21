/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "handoff.h"
#include "pci-handoff.h"
#include "acpi-handoff.h"
#ifdef PS5_EMBED_ACPI
#include "acpi-image.h"
#endif
#ifdef PS5_PCI_CONFIG_TEST
#include "pci-config-test.h"
#endif

#ifdef PS5_AUDIBLE_CHECKPOINTS
void ps5_boot_signal(unsigned milestone);
void ps5_boot_error(unsigned code);
#else
static void ps5_boot_signal(unsigned milestone) { (void)milestone; }
static void ps5_boot_error(unsigned code) { (void)code; }
#endif

#ifdef PS5_POWER_TELEMETRY
int ps5_boot_reboot(void);
void ps5_power_calibrate(void);
void ps5_power_report(unsigned code);
void ps5_power_report_pci_byte(unsigned bridge,unsigned offset,uint8_t actual);
#else
static void ps5_power_calibrate(void) { }
static void ps5_power_report(unsigned code) { (void)code; }
#endif

static uint8_t prepared_hobs[PS5_HOB_CAPACITY];
static uint8_t pci_hobs[PS5_HOB_CAPACITY];
static uint8_t bootstrap_idt[256][16] __attribute__((aligned(16)));
extern uintptr_t ps5_fault_vectors[33];

void ps5_bootstrap_idt_setup(void) {
    for (unsigned i = 0; i < 256; i++) {
        uintptr_t handler = ps5_fault_vectors[i < 32 ? i : 32];
        uint8_t *g = bootstrap_idt[i];
        g[0] = (uint8_t)handler; g[1] = (uint8_t)(handler >> 8);
        g[2] = 8; g[3] = 0; g[4] = 0; g[5] = 0x8e;
        g[6] = (uint8_t)(handler >> 16); g[7] = (uint8_t)(handler >> 24);
        for (unsigned j = 8; j < 12; j++) g[j] = (uint8_t)(handler >> (32 + (j - 8) * 8));
        for (unsigned j = 12; j < 16; j++) g[j] = 0;
    }
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) descriptor = {
        sizeof(bootstrap_idt) - 1, (uintptr_t)bootstrap_idt
    };
    __asm__ volatile("lidt %0" :: "m"(descriptor) : "memory");
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void message(const char *p) {
    volatile uint32_t *status = (volatile uint32_t *)(uintptr_t)0xc101010c;
    volatile uint32_t *tx = (volatile uint32_t *)(uintptr_t)0xc1010104;
    while (*p) {
        unsigned budget = 100000;
        while (budget && !(*status & 0x20)) budget--;
        if (!budget) return;
        *tx = (uint8_t)*p++;
    }
}
static void checkpoint(uint32_t code) {
    // Best-effort retained-RAM breadcrumb inside the loader's reserved arena.
    // A future cold boot may overwrite this; it is not persistent evidence.
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)UINT64_C(0x111000000);
    p[0] = UINT64_C(0x31544f4f42575350); // PSWBOOT1
    p[1] = code;
    __asm__ volatile("sfence" ::: "memory");
}
static __attribute__((noreturn)) void stop(uint32_t code) {
    checkpoint(code);
    ps5_boot_error(code);
#ifdef PS5_DEFER_POWER_CALIBRATION
    ps5_power_calibrate();
#endif
    ps5_power_report(code);
#ifdef PS5_POWER_TELEMETRY
    if((code>>8)==1 || (code>>8)==2 || (code>>8)==5 || (code>>8)==6)
        (void)ps5_boot_reboot();
#endif
    // Fault reports must not touch UART MMIO again and recursively fault.
    if ((code >> 8) != 4) message("PS5 UEFI bootstrap stopped\r\n");
    for (;;) __asm__ volatile("cli; hlt");
}
__attribute__((noreturn)) void ps5_bootstrap_fault(const uint64_t *frame) {
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)UINT64_C(0x111000000);
    p[2] = frame[1]; // Architectural exception error code, or explicit zero.
    p[3] = frame[2]; // Faulting RIP.
    uint64_t cr2;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    p[4] = cr2;
    stop(0x400U + (uint32_t)frame[0]);
}
static void physical_copy(uint64_t to, const uint8_t *from, size_t bytes) {
    uint8_t *p = (uint8_t *)(uintptr_t)to;
    while (bytes--) *p++ = *from++;
}
#ifdef PS5_IOAPIC_SCRATCH
static void physical_zero(uint64_t to, size_t bytes) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)to;
    while (bytes--) *p++ = 0;
}
#endif
void ps5_bootstrap_main(const uint8_t *bp) {
    uintptr_t bp_address = (uintptr_t)bp;
    // This binary is PS5-loader-only. It must not be run through generic kexec.
    if (bp_address != 0x10000 || read32(bp + 0x23c) != 5) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    // Shared across separately linked UEFI PCI library instances.
    *(volatile uint32_t *)(uintptr_t)UINT64_C(0x111000100) = 0;
#ifndef PS5_DEFER_POWER_CALIBRATION
    ps5_power_calibrate();
#endif
    checkpoint(1);
    ps5_boot_signal(1);
    message("PS5 UEFI bootstrap entered\r\n");
    uint64_t source = (uint64_t)read32(bp + 0x218) | (uint64_t)read32(bp + 0xc0) << 32;
    uint64_t size = (uint64_t)read32(bp + 0x21c) | (uint64_t)read32(bp + 0xc4) << 32;
    if (source < UINT64_C(0x100000000) || source >= UINT64_C(0x111000000)
            || size < 4096 || size > UINT64_C(0x111000000) - source) stop(0x100);
    uint32_t a = 0x80000000, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (a < 0x80000008) stop(0x101);
    a = 0x80000008;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    struct ps5_handoff_result result;
    int status = ps5_prepare_handoff(bp, 4096, (const uint8_t *)(uintptr_t)source,
                                    (size_t)size, prepared_hobs, sizeof(prepared_hobs),
                                    a & 255, &result);
    if (status) stop(0x200U + (uint32_t)(-status));
    uint8_t sony[64], amd[64];
#ifdef PS5_PCI_CONFIG_TEST
    for (unsigned i = 0; i < 64; i++) { sony[i] = test_sony[i]; amd[i] = test_amd[i]; }
#else
    // Read the actual root ports; never use a stale captured profile on PS5.
    for (unsigned i = 0; i < 64; i += 4) {
        uint32_t s = *(volatile uint32_t *)(uintptr_t)(0xf0009000U + i);
        uint32_t a = *(volatile uint32_t *)(uintptr_t)(0xf0041000U + i);
        for (unsigned j = 0; j < 4; j++) {
            sony[i + j] = (uint8_t)(s >> (j * 8));
            amd[i + j] = (uint8_t)(a >> (j * 8));
        }
    }
#endif
    status = ps5_add_pci_handoff(prepared_hobs, (size_t)result.hob_bytes,
                                  sony, amd, sizeof(sony), pci_hobs, sizeof(pci_hobs), &result);
#ifdef PS5_POWER_TELEMETRY
    if(status==-2) {
        unsigned bridge,offset;uint8_t actual,expected,mask;
        if(ps5_pci_profile_mismatch(sony,amd,sizeof(sony),&bridge,&offset,&actual,&expected,&mask)==1) {
            checkpoint(0x502);
            ps5_power_report_pci_byte(bridge,offset,actual);
            (void)ps5_boot_reboot();
            for(;;) __asm__ volatile("cli; hlt");
        }
    }
#endif
    if (status) stop(0x500U + (uint32_t)(-status));
    const uint8_t *final_hobs = pci_hobs;
#ifdef PS5_EMBED_ACPI
    status = ps5_add_acpi_handoff(pci_hobs, (size_t)result.hob_bytes,
                                   acpi_image, sizeof(acpi_image), prepared_hobs,
                                   sizeof(prepared_hobs), &result);
    if (status) stop(0x600U + (uint32_t)(-status));
    physical_copy(PS5_ACPI_BASE, acpi_image, sizeof(acpi_image));
#ifdef PS5_IOAPIC_SCRATCH
    /* Clear the dedicated EfiReserved IO-APIC scratch page that acpi-handoff carved out right after the
     * arena. Defensive: the HAL writes 0 to IOWIN before it reads, so the version read is 0 regardless,
     * but a reserved page should not carry stale GameOS data. Requires a 1-page arena (enforced in handoff). */
    physical_zero(PS5_ACPI_BASE + sizeof(acpi_image), (size_t)PS5_IOAPIC_SCRATCH_SIZE);
#endif
    final_hobs = prepared_hobs;
#endif
    checkpoint(2);
    ps5_boot_signal(2);
    physical_copy(PS5_IMAGE_BASE, (const uint8_t *)(uintptr_t)(source + 4096), (size_t)result.image_bytes);
    physical_copy(PS5_HOB_BASE, final_hobs, (size_t)result.hob_bytes);
    __asm__ volatile("mfence" ::: "memory");
    checkpoint(3);
#ifndef PS5_DEFER_SUCCESS_REPORT
    ps5_power_report(3);
#endif
    ps5_boot_signal(3);
    message("PS5 UEFI handoff\r\n");
    typedef void (__attribute__((ms_abi)) *uefi_entry)(void *);
    ((uefi_entry)(uintptr_t)result.entry)((void *)(uintptr_t)PS5_HOB_BASE);
    stop(0x300);
}
