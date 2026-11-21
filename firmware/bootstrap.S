/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PS5 loader calls physical 0x100200 in long mode, RSI=boot_params.
 * No BIOS entry and no Linux OS are implemented here.
 */
.section .transport_pad,"a"
.fill 512,1,0

.section .entry,"ax"
.code64
.global ps5_start64
ps5_start64:
    cli
    cld
    mov %rsi, %r12
    lea ps5_stack_top(%rip), %rsp
    and $-16, %rsp
    xor %ebp, %ebp
    lgdt ps5_gdt_pointer(%rip)
    pushq $8
    lea 1f(%rip), %rax
    push %rax
    lretq
1:
    mov $16, %eax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    call ps5_bootstrap_idt_setup
    mov %cr4, %rax
    and $~((1 << 20) | (1 << 21) | (1 << 7)), %rax
    or $0x620, %rax
    mov %rax, %cr4
    lea ps5_pml4(%rip), %rax
    mov %rax, %cr3
    /* New page tables have no NDA bits. Clear NDA only after switching CR3.
       This sequence is still an unverified hardware candidate. */
    mov $0xc0000080, %ecx
    rdmsr
    btr $16, %eax
    btr $12, %eax
    wrmsr
    mov %cr0, %rax
    and $~((1 << 2) | (1 << 3)), %rax
    or $0x22, %rax
    mov %rax, %cr0
    mov %r12, %rdi
    call ps5_bootstrap_main
2:
    cli
    hlt
    jmp 2b

.macro ps5_isr number
ps5_fault_\number:
    .if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
        pushq $0
    .endif
    pushq $\number
    jmp ps5_fault_common
.endm
.irp vec,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,255
    ps5_isr \vec
.endr
ps5_fault_common:
    cli
    cld
    mov %rsp, %rdi
    and $-16, %rsp
    call ps5_bootstrap_fault
    jmp 2b

.section .rodata,"a"
.balign 8
ps5_gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
ps5_gdt_end:
ps5_gdt_pointer:
    .word ps5_gdt_end - ps5_gdt - 1
    .quad ps5_gdt
.global ps5_fault_vectors
ps5_fault_vectors:
.irp vec,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,255
    .quad ps5_fault_\vec
.endr

.section .data,"aw"
.balign 4096
.global ps5_pml4
ps5_pml4:
    .quad ps5_pdpt + 3
    .fill 511,8,0
ps5_pdpt:
.set ps5_i,0
.rept 64
    .quad ps5_pds + ps5_i * 4096 + 3
    .set ps5_i,ps5_i+1
.endr
    .fill 448,8,0
ps5_pds:
.set ps5_pa,0
.rept 32768
    /* First 64 GiB identity mapped with 2 MiB leaves. PCI/MMIO 2..4 GiB
       uses PCD; RAM uses the existing MTRR/PAT environment. */
    .if (ps5_pa >= 0x80000000) && (ps5_pa < 0x100000000)
#if defined(PS5_AUDIBLE_CHECKPOINTS) || defined(PS5_POWER_TELEMETRY)
        /* Signal candidate requires inherited PAT entry 3 to be UC. */
        .quad ps5_pa + 0x9b
#else
        .quad ps5_pa + 0x93
#endif
    .else
        .quad ps5_pa + 0x83
    .endif
    .set ps5_pa,ps5_pa+0x200000
.endr

.section .bss,"aw",@nobits
.balign 16
ps5_stack:
    .skip 65536
ps5_stack_top:
.section .note.GNU-stack,"",@progbits
