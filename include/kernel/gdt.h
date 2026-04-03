/* ============================================================================
 * gdt.h -- Global Descriptor Table (x86-64 Long Mode)
 *
 * Segments: null, kernel code, kernel data, user code, user data, TSS
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* GDT segment selectors (byte offsets into the GDT)
 * User data MUST be before user code for SYSRET compatibility:
 * SYSRET CS = STAR[63:48]+16, SS = STAR[63:48]+8.
 * With STAR[63:48] = 0x10: SS=0x18 (user data), CS=0x20 (user code). */
#define GDT_NULL_SEG     0x00
#define GDT_KERNEL_CODE  0x08    /* Ring 0 code */
#define GDT_KERNEL_DATA  0x10    /* Ring 0 data */
#define GDT_USER_DATA    0x18    /* Ring 3 data (before code for SYSRET) */
#define GDT_USER_CODE    0x20    /* Ring 3 code */
#define GDT_TSS_SEG      0x28    /* TSS (16 bytes -- two GDT slots) */

/* Number of GDT entries (TSS takes 2 slots in 64-bit mode) */
#define GDT_NUM_ENTRIES  7

/* Task State Segment for x86-64 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          /* Kernel stack pointer (used on ring 3→0 transition) */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;          /* Interrupt Stack Table entries 1-7 */
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   /* I/O Permission Bitmap offset */
} __attribute__((packed));

/* Initialize the GDT with kernel/user segments and TSS */
void gdt_init(void);

/* Set the kernel stack pointer in the TSS (called during task switches) */
void tss_set_kernel_stack(uint64_t stack_top);
