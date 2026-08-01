/* ============================================================================
 * gdt.h -- Global Descriptor Table (x86-64 Long Mode)
 *
 * Segments: null, kernel code, kernel data, user code, user data, TSS
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* =========================================================================
 * WARNING: GDT SEGMENT ORDER IS CRITICAL FOR SYSRET CORRECTNESS
 *
 * SYSRET in 64-bit mode computes user-mode selectors as:
 *   CS = STAR[63:48] + 16    (user code)
 *   SS = STAR[63:48] + 8     (user data)
 *
 * This means GDT_USER_DATA MUST be at a LOWER selector than
 * GDT_USER_CODE, exactly 8 bytes apart. Specifically:
 *   GDT_USER_CODE == GDT_USER_DATA + 8
 *
 * If you swap these, SYSRET will load wrong selectors on every
 * ring-3 return -- triple fault on WHPX, silent corruption on TCG.
 *
 * The _Static_assert below enforces this at compile time.
 * ========================================================================= */
#define GDT_NULL_SEG     0x00
#define GDT_KERNEL_CODE  0x08    /* Ring 0 code */
#define GDT_KERNEL_DATA  0x10    /* Ring 0 data */
#define GDT_USER_DATA    0x18    /* Ring 3 data -- MUST be before code */
#define GDT_USER_CODE    0x20    /* Ring 3 code -- MUST be after data */
#define GDT_TSS_SEG      0x28    /* TSS (16 bytes -- two GDT slots) */

/* Requested Privilege Level, the low 2 bits of any segment selector. A
 * saved CS with RPL 3 is the CPU's own record that the interrupted code was
 * executing at ring 3, which is the only unforgeable proof of user-mode
 * execution available to a handler. */
#define SEL_RPL_MASK     0x3u
#define SEL_RPL_USER     0x3u

/* Compile-time enforcement of SYSRET GDT ordering constraint */
_Static_assert(GDT_USER_CODE == GDT_USER_DATA + 8,
    "SYSRET requires GDT_USER_CODE == GDT_USER_DATA + 8 "
    "(user data selector must be 8 bytes before user code)");
_Static_assert(GDT_KERNEL_CODE == 0x08,
    "GDT_KERNEL_CODE must be 0x08 (first non-null entry)");
_Static_assert(GDT_KERNEL_DATA == 0x10,
    "GDT_KERNEL_DATA must be 0x10 (second entry)");

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
