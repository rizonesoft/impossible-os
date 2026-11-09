/* ============================================================================
 * gdt.h -- Global Descriptor Table (x86-64 Long Mode)
 *
 * Segments: null, kernel code, kernel data, user code, user data, TSS
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/acpi.h"    /* MAX_CPUS: one TSS descriptor per logical CPU */

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

/* Per-CPU TSS descriptors (TODO-09-boot S10). One shared GDT carries a 16-byte
 * TSS descriptor per logical CPU, CPU c at entries 5+2c and 6+2c, so its
 * selector is GDT_TSS_SEG + 16*c and the BSP (c = 0) keeps 0x28. The segment
 * entries 0..4 are untouched, so the SYSRET layout above does not move. Each
 * descriptor is written once before its CPU starts and loaded once by that
 * CPU's `ltr`, which marks it busy; nothing rewrites a descriptor afterwards.
 * ap_trampoline.asm hardcodes the 0x28 base and 16 stride. */
#define GDT_TSS_FIRST_INDEX  5u
#define GDT_TSS_SEL_STRIDE   16u
#define GDT_TSS_SEL_FOR(c)   ((uint16_t)(GDT_TSS_SEG + GDT_TSS_SEL_STRIDE * (uint32_t)(c)))

/* Number of GDT entries: 5 segments + 2 slots per CPU's TSS */
#define GDT_NUM_ENTRIES  (GDT_TSS_FIRST_INDEX + 2u * MAX_CPUS)

_Static_assert(GDT_TSS_SEG == GDT_TSS_FIRST_INDEX * 8u,
    "the BSP TSS selector must be the first TSS slot");
_Static_assert(GDT_TSS_SEL_FOR(0) == 0x28 && GDT_TSS_SEL_STRIDE == 16u,
    "ap_trampoline.asm derives ltr selectors as 0x28 + 16*cpu");
_Static_assert(GDT_NUM_ENTRIES * 8u <= 0x10000u, "GDT limit is 16 bits");

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

/* Set the CALLING CPU's TSS.rsp0 (called during task switches). Each CPU has
 * its own TSS, so this never touches another CPU's ring-0 entry stack. */
void tss_set_kernel_stack(uint64_t stack_top);

/* Selector of CPU `cpu`'s TSS descriptor, or 0 for a CPU index past MAX_CPUS.
 * Pure. */
uint16_t gdt_tss_selector(uint32_t cpu);

/* Prepare the TSS of AP `cpu` (1..MAX_CPUS-1) before that AP is started: rsp0
 * = `rsp0` (its kernel stack top) and guarded IST stacks for #DF, NMI and MCE,
 * then publish its descriptor as an AVAILABLE 64-bit TSS. Returns the selector
 * the AP must `ltr`, or 0 when the IST stacks cannot be allocated or guarded,
 * in which case nothing is published and the caller must not start the AP: an
 * AP without its own IST would take an NMI on whatever TR its INIT left (base
 * 0, so the stack pointer comes from the real-mode IVT). BSP only, before the
 * AP's SIPI; never called twice for one CPU. */
uint16_t gdt_install_ap_tss(uint32_t cpu, uint64_t rsp0);

/* Undo gdt_install_ap_tss for an AP that provably never loaded its TSS (the
 * BSP won its bringup-record revoke): unpublish the lookup, clear the
 * descriptor, and return the IST runs to the PMM (or quarantine them, as
 * ist_try_alloc does). BSP only. A no-op for CPU 0, an out-of-range CPU, or a
 * CPU with no published TSS. Never call it for a CPU that may have run ltr. */
void gdt_release_ap_tss(uint32_t cpu);

/* IST stack top `index` (1-based, as gdt_get_ist) of CPU `cpu`'s TSS, or 0 for
 * a CPU with no TSS or an out-of-range index. Read-only, lock-free. */
uint64_t gdt_get_cpu_ist(uint32_t cpu, unsigned index);

/* --- BSP ring-0 entry stack (bare-metal hardening: BSP entry-stack guard) -------------------------
 *
 * The stack the CPU loads into RSP on a ring-3 -> ring-0 transition while
 * TSS.rsp0 still holds the boot value. Allocated from the PMM by gdt_init()
 * with a guard page beneath it, replacing the unguarded BSS array that used to
 * publish a `stack_top` label from boot_hw.c.
 *
 * It is NOT the Phase-0/1 execution stack: the UEFI bootloader calls the
 * kernel entry on its own stack without switching RSP, so early boot never
 * runs here. This stack is live from the first ring-3 entry until the
 * scheduler writes an incoming thread's kernel_rsp over TSS.rsp0.
 *
 * All five are read-only queries over state written once on the BSP in Phase 1
 * before any AP starts, so none takes a lock and all are callable from a fault
 * path. 16 KiB matches AP_STACK_SIZE. */
#define BSP_ENTRY_STACK_SIZE 16384

uintptr_t bsp_entry_stack_guard(void);   /* base of the guard page itself */
uintptr_t bsp_entry_stack_base(void);    /* lowest usable byte */
uint64_t  bsp_entry_stack_top(void);     /* stack top: base + BSP_ENTRY_STACK_SIZE */
int       bsp_entry_stack_guarded(void); /* 0 = guard install degraded */

/* Deepest byte ever disturbed, in bytes below the top -- the measured margin
 * against BSP_ENTRY_STACK_SIZE rather than an assumed one. 0 when the run was
 * never poisoned (guard install degraded). A lower bound in principle: a frame
 * that leaves a hole, or writes the poison value itself, reads as untouched. */
uint32_t  bsp_entry_stack_peak_used(void);

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
