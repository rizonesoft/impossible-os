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

/* Number of Interrupt Stack Table slots the x86-64 TSS defines (ist1..ist7).
 * The IDT encodes the slot as a 1-based index, so 0 means "no IST". */
#define TSS_IST_COUNT  7

/* gdt_get_ist() dispatches one switch case per slot, so a struct that grew an
 * ist8 would silently read as "no such slot". The IST fields are contiguous
 * 8-byte entries, so their span pins the count against the layout itself. */
_Static_assert(__builtin_offsetof(struct tss, ist7)
                   - __builtin_offsetof(struct tss, ist1)
               == (TSS_IST_COUNT - 1) * 8,
               "TSS_IST_COUNT does not match the ist1..ist7 span in struct tss");

/* Set the kernel stack pointer in the TSS (called during task switches) */
void tss_set_kernel_stack(uint64_t stack_top);

/* Read-only accessor for a TSS Interrupt Stack Table entry: the stack TOP the
 * CPU loads when it delivers a vector whose IDT descriptor names IST slot
 * `index`. `index` is that 1-based IDT encoding, so 1..TSS_IST_COUNT are the
 * only valid values and anything else returns 0. Returns 0 for a slot
 * gdt_init() has not populated.
 *
 * The TSS instance stays file-local to gdt.c. The only legitimate outside need
 * is a read-only query -- diagnostics, and the bare-metal test that proves
 * #DF/NMI/MCE actually received guarded stacks instead of triple-faulting on
 * delivery -- and handing out a writable TSS pointer for that would let any
 * caller repoint a critical-exception stack.
 *
 * SMP: the IST fields are written once by gdt_init() on the BSP in Phase 1,
 * before any AP is started, and are never rewritten (tss_set_kernel_stack
 * touches rsp0 only). The read is a single naturally-aligned 8-byte load, so
 * no lock is required and none is taken -- which also keeps this callable from
 * a panic or fault path, where taking a lock is forbidden. */
uint64_t gdt_get_ist(unsigned index);

/* Pure field-selection half of gdt_get_ist(): maps the 1-based IST index onto
 * the matching ist1..ist7 field of an explicitly-supplied TSS. Returns 0 for
 * index 0, for anything past TSS_IST_COUNT, and for a NULL tss.
 *
 * Split out from the accessor so the index-to-field mapping can be pinned
 * against a TSS carrying a distinct sentinel per slot. Against the LIVE TSS
 * that mapping is unfalsifiable: this kernel assigns only ist1..ist3 and the
 * remaining four read as 0, which is the same answer an out-of-range index
 * gives, so a mapping that dropped or misrouted a case would look identical to
 * a correct one. */
uint64_t tss_get_ist(const struct tss *tss, unsigned index);
