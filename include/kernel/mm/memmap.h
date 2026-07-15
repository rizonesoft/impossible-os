/* ============================================================================
 * memmap.h -- Canonical kernel virtual address-space layout
 *
 * SINGLE SOURCE OF TRUTH for the 64-bit virtual memory map: every top-level
 * WINDOW base and extent is defined here and pinned by _Static_assert. No
 * other file may invent a window.
 *
 * Sub-regions carved INSIDE a window may live with their owner (the user-half
 * ELF range and section-view range stay in user_range.h until the low-memory
 * ceiling is retired), but they are not free-floating: this header asserts
 * they are CONTAINED by the window they claim, so the two cannot drift apart
 * silently. A constant that is neither a window here nor a contained
 * sub-region asserted against one is drift.
 *
 * ARCH: x86-64 -- 4-level (and optional 5-level) paging, canonical-form rules,
 * and PDE reserved-bit behavior are Intel SDM Vol. 3A ch. 4 semantics. This
 * header moves under arch/ when the ARM64 port lands (domain 16).
 *
 * Design rationale, transition plan, and the evidence behind each constant:
 *   docs/infrastructure/kernel-address-space.md
 *
 * Layout (4-level paging, 48-bit canonical):
 *
 *   0x0000000000000000  +--------------------------------+
 *                       |  USER (private, per-process)   |  128 TiB
 *   0x00007fffffffffff  +--------------------------------+
 *                       :   non-canonical hole           :
 *   0xffff888000000000  +--------------------------------+
 *                       |  HHDM direct map (RAM, NX/RW)  |   64 TiB
 *   0xffffc87fffffffff  +--------------------------------+
 *   0xffffc90000000000  +--------------------------------+
 *                       |  MMIO / fixmap window (UC/WC)  |   32 TiB
 *   0xffffe8ffffffffff  +--------------------------------+
 *   0xffffea0000000000  +--------------------------------+
 *                       |  per-CPU window                |    1 TiB
 *   0xffffeaffffffffff  +--------------------------------+
 *                       :   reserved / guard             :
 *   0xffffffff80000000  +--------------------------------+
 *                       |  KERNEL IMAGE (-2 GiB window)  |    2 GiB
 *   0xffffffffffffffff  +--------------------------------+
 *
 * TWO DISTINCT physical<->virtual RELATIONS exist. They are NOT
 * interchangeable, and conflating them is the primary bug class this header
 * exists to prevent:
 *
 *   1. HHDM relation   -- for arbitrary RAM reached through the direct map:
 *                         virt = phys + MM_HHDM_BASE
 *   2. IMAGE relation  -- for kernel linker symbols (__text_start, __kernel_end,
 *                         ...) which live in the image window:
 *                         virt = phys + MM_KERNEL_VIRT_BASE
 *
 * A kernel symbol address is NOT an HHDM address. mm_hhdm_to_phys() therefore
 * REJECTS image-window addresses rather than silently returning a wrong
 * physical address.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Size / alignment units ---------------------------------------------- */

#define MM_SIZE_4KIB          0x0000000000001000ULL
#define MM_SIZE_2MIB          0x0000000000200000ULL
#define MM_MASK_2MIB          (MM_SIZE_2MIB - 1ULL)

/* ---- Canonical-form helpers ---------------------------------------------- */

/* 4-level paging sign-extends at bit 47; 5-level (LA57) at bit 56. The kernel
 * image base is canonical under BOTH, so it never moves when LA57 is enabled.
 * The HHDM base is 4-level-specific -- 5-level paging (LA57) support must
 * re-base it, which is why consumers use MM_HHDM_BASE and never a literal. */
#define MM_CANONICAL_SHIFT_4LVL   47
#define MM_CANONICAL_SHIFT_5LVL   56

/* True when bits [63:shift] are all copies of bit [shift] (sign-extended).
 *
 * Must stay a MACRO, not an inline function: it is used inside _Static_assert,
 * which requires an integer constant expression that a function call is not.
 * Consequence: 'a' is EVALUATED TWICE -- pass a constant or a simple lvalue,
 * never an expression with side effects (MM_IS_CANONICAL(*p++, 47) misbehaves). */
#define MM_IS_CANONICAL(a, shift) \
    ((((uint64_t)(a)) >> (shift)) == 0ULL || \
     (((uint64_t)(a)) >> (shift)) == ((1ULL << (64 - (shift))) - 1ULL))

#define MM_IS_CANONICAL_4LVL(a)   MM_IS_CANONICAL(a, MM_CANONICAL_SHIFT_4LVL)

/* ---- User (lower) half --------------------------------------------------- */

#define MM_USER_BASE          0x0000000000000000ULL
#define MM_USER_SIZE          0x0000800000000000ULL   /* 128 TiB */
#define MM_USER_END           (MM_USER_BASE + MM_USER_SIZE)  /* exclusive */

/* ---- HHDM: fixed-offset direct map of physical RAM ----------------------- */

/* Linux-compatible base. Sparse: only validated UEFI RAM types are aliased
 * here (see the alias policy in the design doc). Mapped NX + writable; never
 * executable, never User. MMIO does NOT live here -- vmm_map_mmio_uc() owns
 * device memory in the MMIO window below. */
#define MM_HHDM_BASE          0xffff888000000000ULL
#define MM_HHDM_SIZE          0x0000400000000000ULL   /* 64 TiB of RAM */
#define MM_HHDM_END           (MM_HHDM_BASE + MM_HHDM_SIZE)

/* ---- MMIO / fixmap window ------------------------------------------------ */

#define MM_MMIO_BASE          0xffffc90000000000ULL
#define MM_MMIO_SIZE          0x0000200000000000ULL   /* 32 TiB */
#define MM_MMIO_END           (MM_MMIO_BASE + MM_MMIO_SIZE)

/* ---- Per-CPU window ------------------------------------------------------ */

#define MM_PERCPU_BASE        0xffffea0000000000ULL
#define MM_PERCPU_SIZE        0x0000010000000000ULL   /* 1 TiB */
#define MM_PERCPU_END         (MM_PERCPU_BASE + MM_PERCPU_SIZE)

/* ---- Kernel image window ------------------------------------------------- */

/* The -2 GiB window. This value is NOT a preference: the kernel is compiled
 * with -mcmodel=kernel, which emits R_X86_64_32S (sign-extended 32-bit)
 * relocations for every symbol reference. Those only resolve inside the top
 * 2 GiB. A Windows-style 0xFFFF800000000000 base hard-fails at link time
 * ("relocation R_X86_64_32S out of range") unless -mcmodel=large is adopted,
 * which would turn every symbol reference into a 64-bit absolute. */
#define MM_KERNEL_VIRT_BASE   0xffffffff80000000ULL
#define MM_KERNEL_IMAGE_SIZE  0x0000000080000000ULL   /* 2 GiB, to top of VA */

/* The image window ends at the TOP of the address space, so the usual
 * exclusive bound (BASE + SIZE) WRAPS TO ZERO in uint64_t and silently turns
 * every `v < end` test into a constant false -- rejecting every valid image
 * address. Range checks here use these INCLUSIVE last-address constants
 * instead; never reconstruct an exclusive end for this window. The assert
 * below pins the wrap so the hazard cannot be rediscovered the hard way. */
#define MM_KERNEL_IMAGE_LAST  (MM_KERNEL_VIRT_BASE + (MM_KERNEL_IMAGE_SIZE - 1ULL))
#define MM_KERNEL_PHYS_LAST   (MM_KERNEL_IMAGE_SIZE - 1ULL)

/* TARGET physical load address of the kernel image (linker LMA).
 *
 * NOT YET LIVE: the kernel still links at 1 MiB (`. = 1M` in src/boot/linker.ld)
 * and pmm.c still reserves the image from 0x100000. The linker-split section
 * moves the LMA here; until it lands, this constant states the DESIGN target,
 * not the current tree.
 *
 * WARNING: a linker script cannot #include this header, so src/boot/linker.ld
 * must be updated BY HAND to match, exactly as user/user.ld must track
 * user_range.h. A mismatch means the bootloader copies PT_LOAD segments to one
 * physical base while the kernel's mapping assumes another.
 *
 * 2 MiB-aligned so a PS=1 (2 MiB) PDE may legally map the image window: a huge
 * PDE requires a 2 MiB-aligned physical frame, and a 1 MiB base sets PDE bit
 * 20 -- a reserved bit -- which faults before kernel_main can report it. The
 * bring-up section maps the image with 4 KiB pages for per-section W^X; this
 * alignment keeps the huge-page option legal and removes the reserved-bit
 * hazard class permanently. */
#define MM_KERNEL_PHYS_BASE   0x0000000000200000ULL   /* 2 MiB */

/* Virtual address the kernel image is linked at (linker.ld VMA). */
#define MM_KERNEL_IMAGE_BASE  (MM_KERNEL_VIRT_BASE + MM_KERNEL_PHYS_BASE)

/* ---- AP bring-up low envelope -------------------------------------------- */

/* The AP trampoline runs at a fixed low physical address and loads CR3 with a
 * 32-bit `mov cr3, eax` (src/kernel/smp/ap_trampoline.asm), so the kernel PML4
 * frame must stay below 4 GiB for the lifetime of the system, and this
 * envelope must remain identity-mapped in the SAME live CR3 that carries the
 * high half until every AP acknowledges high entry. It is retired after that
 * ack -- the broad bring-up identity map is NEVER retained permanently (see
 * the design doc's teardown rules). */
#define MM_AP_ENVELOPE_BASE   0x0000000000008000ULL
#define MM_AP_ENVELOPE_SIZE   0x0000000000002000ULL   /* trampoline + AP_DATA */
#define MM_AP_ENVELOPE_END    (MM_AP_ENVELOPE_BASE + MM_AP_ENVELOPE_SIZE)

/* Hard ceiling for the kernel PML4 physical frame (32-bit CR3 load on APs). */
#define MM_PML4_PHYS_LIMIT    0x0000000100000000ULL   /* 4 GiB */

/* ---- Compile-time layout gate -------------------------------------------- */

/* Canonicality: every window base must be a legal 4-level canonical address. */
_Static_assert(MM_IS_CANONICAL_4LVL(MM_HHDM_BASE),
    "MM_HHDM_BASE must be canonical under 4-level paging");
_Static_assert(MM_IS_CANONICAL_4LVL(MM_MMIO_BASE),
    "MM_MMIO_BASE must be canonical under 4-level paging");
_Static_assert(MM_IS_CANONICAL_4LVL(MM_PERCPU_BASE),
    "MM_PERCPU_BASE must be canonical under 4-level paging");
_Static_assert(MM_IS_CANONICAL_4LVL(MM_KERNEL_VIRT_BASE),
    "MM_KERNEL_VIRT_BASE must be canonical under 4-level paging");
_Static_assert(MM_IS_CANONICAL_4LVL(MM_USER_END - 1ULL),
    "Top of the user half must be canonical");

/* LA57 invariance: the image base must stay canonical under 5-level paging so
 * enabling LA57 never moves the kernel image. */
_Static_assert(MM_IS_CANONICAL(MM_KERNEL_VIRT_BASE, MM_CANONICAL_SHIFT_5LVL),
    "MM_KERNEL_VIRT_BASE must remain canonical under 5-level paging");

/* -mcmodel=kernel: the whole image window must sit in the top 2 GiB. */
_Static_assert(MM_KERNEL_VIRT_BASE >= 0xffffffff80000000ULL,
    "-mcmodel=kernel requires the image window inside the top 2 GiB");
_Static_assert(MM_KERNEL_IMAGE_SIZE == 0x80000000ULL,
    "Kernel image window is the top 2 GiB");
_Static_assert(MM_KERNEL_IMAGE_LAST == 0xffffffffffffffffULL,
    "Kernel image window must run to the top of the address space");

/* Pin the wrap hazard itself: because the window ends at UINT64_MAX, the
 * exclusive bound IS zero. This assert exists so that anyone who "fixes" the
 * range checks back into `v < BASE + SIZE` form trips a compile error here
 * with the reason attached, rather than shipping an API that silently
 * rejects every address it is asked about. */
_Static_assert(MM_KERNEL_VIRT_BASE + MM_KERNEL_IMAGE_SIZE == 0ULL,
    "exclusive image-window end wraps to 0 -- range checks MUST use "
    "MM_KERNEL_IMAGE_LAST (inclusive), never BASE + SIZE");
_Static_assert(MM_KERNEL_IMAGE_BASE > MM_KERNEL_VIRT_BASE &&
               MM_KERNEL_IMAGE_BASE <= MM_KERNEL_IMAGE_LAST,
    "Kernel image must start inside its window");
_Static_assert(MM_KERNEL_PHYS_BASE <= MM_KERNEL_PHYS_LAST,
    "Kernel image physical range must be non-empty");

/* The image LMA must be 2 MiB-aligned so a PS=1 PDE can legally map it. */
_Static_assert((MM_KERNEL_PHYS_BASE & MM_MASK_2MIB) == 0ULL,
    "MM_KERNEL_PHYS_BASE must be 2 MiB-aligned (PS=1 PDE reserved-bit rule)");
_Static_assert(MM_KERNEL_PHYS_BASE < MM_PML4_PHYS_LIMIT,
    "Kernel image must load below 4 GiB");

/* The image must not collide with the AP low envelope. */
_Static_assert(MM_KERNEL_PHYS_BASE >= MM_AP_ENVELOPE_END,
    "Kernel image LMA must not overlap the AP bring-up envelope");

/* Window ordering + non-overlap across the whole higher half. */
_Static_assert(MM_USER_END <= MM_HHDM_BASE,
    "User half must end before the HHDM begins");
_Static_assert(MM_HHDM_END <= MM_MMIO_BASE,
    "HHDM must not overlap the MMIO window");
_Static_assert(MM_MMIO_END <= MM_PERCPU_BASE,
    "MMIO window must not overlap the per-CPU window");
_Static_assert(MM_PERCPU_END <= MM_KERNEL_VIRT_BASE,
    "Per-CPU window must not overlap the kernel image window");

/* The HHDM must be able to alias every physical address the PMM can hand out
 * on a 48-bit-canonical machine. 64 TiB of direct map covers any plausible
 * physical memory while leaving the rest of the higher half for the other
 * windows. */
_Static_assert(MM_HHDM_SIZE >= 0x0000100000000000ULL,
    "HHDM must alias at least 16 TiB of physical RAM");

/* ---- Translation helpers -------------------------------------------------
 *
 * These are the ONLY sanctioned physical<->virtual conversions. Page-table
 * entries and CR3 always hold PHYSICAL addresses; any dereference of one must
 * go through mm_phys_to_hhdm() once the direct map is live.
 * -------------------------------------------------------------------------- */

/* True when 'p' can be aliased through the direct map.
 *
 * Physical 0 is deliberately EXCLUDED. Both relations here report failure by
 * returning 0/NULL, so admitting phys 0 would make mm_hhdm_to_phys(MM_HHDM_BASE)
 * return the same value it uses to mean "not a direct-map address" -- a caller
 * could not tell a valid translation from a rejection. Excluding it is not a
 * lie about the window: pmm_init() reserves the entire low 1 MiB
 * (pmm_mark_region_used(0, 0x100000)) and pmm_alloc_frame() already returns 0
 * to mean "no frame", so physical 0 is not an allocatable frame anywhere in
 * this kernel. This also makes the HHDM relation symmetric with the image
 * relation, whose range starts at MM_KERNEL_PHYS_BASE and so can never produce
 * 0 either. Net: for BOTH relations, 0 means failure and nothing else. */
static inline int mm_phys_in_hhdm(uint64_t phys)
{
    return phys != 0 && phys < MM_HHDM_SIZE;
}

/* True when 'v' is a kernel-image (linker-symbol) virtual address. Uses the
 * INCLUSIVE last address: an exclusive bound wraps to 0 here (see
 * MM_KERNEL_IMAGE_LAST) and would make this a constant false. */
static inline int mm_virt_in_image(uint64_t virt)
{
    return virt >= MM_KERNEL_IMAGE_BASE && virt <= MM_KERNEL_IMAGE_LAST;
}

/* True when 'p' is a physical address the image window can express. The lower
 * bound is the image LMA, which keeps every derived physical address non-zero
 * and so keeps 0 usable as an unambiguous failure sentinel. */
static inline int mm_phys_in_image(uint64_t phys)
{
    return phys >= MM_KERNEL_PHYS_BASE && phys <= MM_KERNEL_PHYS_LAST;
}

/* True when 'v' is a direct-map virtual address.
 *
 * STRICT lower bound. MM_HHDM_BASE is the alias of physical 0, which the
 * forward relation rejects (see mm_phys_in_hhdm), so admitting it here would
 * make the two domains disagree: mm_hhdm_to_phys(MM_HHDM_BASE) would return 0
 * -- the failure sentinel -- for an address this predicate had just called
 * valid. The forward and inverse domains must exclude exactly the same page. */
static inline int mm_virt_in_hhdm(uint64_t virt)
{
    return virt > MM_HHDM_BASE && virt < MM_HHDM_END;
}

/* HHDM relation: physical RAM -> direct-map virtual address. Returns NULL for
 * a physical address the direct map cannot alias.
 *
 * CALLERS MUST CHECK THE RETURN. Do NOT assume a missed check "just faults":
 * the bring-up identity map marks the whole low 4 GiB Present+Writable+User
 * (bootx64.c setup_page_tables), so while it is live -- which is exactly the
 * window in which these helpers become the page-table walkers' hot path -- a
 * NULL dereference silently reads/writes physical page 0 (IVT/BDA) instead of
 * faulting. The same "NULL is not a fault here" trap already cost this repo
 * once; see the GS_BASE entry in docs/infrastructure/bare-metal-gotchas.md. */
static inline void *mm_phys_to_hhdm(uint64_t phys)
{
    if (!mm_phys_in_hhdm(phys))
        return (void *)0;
    return (void *)(uintptr_t)(phys + MM_HHDM_BASE);
}

/* HHDM relation, inverse. Returns 0 for an address that is NOT a direct-map
 * address -- in particular for a kernel-image symbol, whose physical address
 * must be obtained through mm_image_virt_to_phys() instead. Silently applying
 * the HHDM offset to an image symbol would yield a plausible-looking but wrong
 * physical address; rejecting is what keeps the two relations from blending.
 * Physical 0 is never a legal PMM frame, so 0 is an unambiguous sentinel. */
static inline uint64_t mm_hhdm_to_phys(const void *virt)
{
    uint64_t v = (uint64_t)(uintptr_t)virt;
    if (!mm_virt_in_hhdm(v))
        return 0;
    return v - MM_HHDM_BASE;
}

/* IMAGE relation: kernel linker symbol -> its physical load address. Returns 0
 * when 'virt' is not an image-window address. */
static inline uint64_t mm_image_virt_to_phys(const void *virt)
{
    uint64_t v = (uint64_t)(uintptr_t)virt;
    if (!mm_virt_in_image(v))
        return 0;
    return v - MM_KERNEL_VIRT_BASE;
}

/* IMAGE relation, inverse. Returns NULL for a physical address the image
 * window cannot express -- an unchecked add would wrap an out-of-range input
 * into an unrelated virtual address. */
static inline void *mm_image_phys_to_virt(uint64_t phys)
{
    if (!mm_phys_in_image(phys))
        return (void *)0;
    return (void *)(uintptr_t)(phys + MM_KERNEL_VIRT_BASE);
}

/* Reconstruct a CANONICAL virtual address from page-table indices. Every
 * page-table walker MUST use this: a naive (pml4i << 39) | ... yields a
 * positive, non-canonical value for pml4i >= 256, so comparisons against high
 * kernel symbols silently never match and the walk skips the high half
 * entirely -- while still reporting success. */
static inline uint64_t mm_canonical_from_indices(uint64_t pml4i, uint64_t pdpti,
                                                 uint64_t pdi, uint64_t pti)
{
    /* Mask each index to its 9 bits FIRST. Without this the canonical
     * guarantee is only conditional: an out-of-range pml4i (say 512) sets bit
     * 48 while leaving bit 47 clear, so the sign-extension below does not fire
     * and the function returns a NON-canonical value while its name promises
     * otherwise -- and an over-range lower index would bleed into the field
     * above it. Masking makes the guarantee hold for any input. */
    uint64_t v = ((pml4i & 0x1ffULL) << 39) | ((pdpti & 0x1ffULL) << 30) |
                 ((pdi   & 0x1ffULL) << 21) | ((pti   & 0x1ffULL) << 12);
    /* Sign-extend bit 47 across [63:48]. */
    if (v & (1ULL << MM_CANONICAL_SHIFT_4LVL))
        v |= ~((1ULL << (MM_CANONICAL_SHIFT_4LVL + 1)) - 1ULL);
    return v;
}
