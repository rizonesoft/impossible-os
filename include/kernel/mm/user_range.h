/* ============================================================================
 * user_range.h -- User-mode ELF load range constants
 *
 * Single source of truth for the user-mode address range. Three files depend
 * on these values staying in sync:
 *   1. user/user.ld        -- linker base address (`. = USER_ELF_BASE`)
 *   2. src/kernel/mm/vmm.c -- page table U/S policy for this range
 *   3. src/kernel/mm/pmm.c -- pmm_mark_region_used() reservation
 *
 * The linker script cannot #include this header, so user.ld must be updated
 * manually if these values change. The static asserts below and in vmm.c/pmm.c
 * catch all C-side drift at compile time.
 *
 * WARNING: Changing these values requires updating user/user.ld to match.
 * A mismatch means user binaries load at wrong addresses or overwrite kernel
 * memory. The unit test in test_nt_types.c verifies all three agree.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/mm/memmap.h"   /* canonical windows these ranges live inside */

/* User ELF load range: 0x800000 .. 0x900000 (8 MiB .. 9 MiB)
 * Must be above kernel BSS end (~0x4C0000) to avoid collision.
 * Must be within a single 2 MiB PD entry for the split-PT optimization. */
#define USER_ELF_BASE   0x800000UL   /* first byte of user region */
#define USER_ELF_SIZE   0x100000UL   /* 1 MiB total region size */
#define USER_ELF_END    (USER_ELF_BASE + USER_ELF_SIZE)  /* 0x900000 */

/* PD index for the 2 MiB region containing user ELF (0x800000 >> 21 = 4) */
#define USER_PD_INDEX   (USER_ELF_BASE >> 21)

/* Full extent of the 2 MiB huge page that contains the user ELF range:
 * 0x800000 .. 0xA00000. vmm_create_user_pml4() replaces this entire PD
 * entry with a per-process 4 KiB PT (User bits on ELF pages, Present bit
 * CLEARED on the 0x900000 guard page). Kernel allocations must therefore
 * never land anywhere inside this window: a kernel structure at, e.g.,
 * 0x900000 would vanish whenever a user process's CR3 is active. PMM
 * reserves the full window, not just USER_ELF_SIZE. */
#define USER_PT_WINDOW_BASE  (USER_PD_INDEX << 21)            /* 0x800000 */
#define USER_PT_WINDOW_SIZE  0x200000UL                       /* one PD entry */
#define USER_PT_WINDOW_END   (USER_PT_WINDOW_BASE + USER_PT_WINDOW_SIZE)

/* Shmem / section view user-VA range (distinct from USER_ELF range).
 * Each user task has a private 256 MiB bump region for MapViewOfSection
 * allocations. Shmem sections get mapped here with User+Writable PTEs
 * so the user binary can read/write them without the USER_ELF range's
 * image-only invariant. task_exec resets the per-task bump pointer to
 * the base so a fresh process image starts fresh. */
#define SECTION_VIEW_BASE   0x10000000UL   /* 256 MiB */
#define SECTION_VIEW_LIMIT  0x20000000UL   /* 512 MiB -- 256 MiB range */

/* Compile-time sanity checks */
_Static_assert(USER_ELF_BASE == 0x800000, "USER_ELF_BASE must be 0x800000");
_Static_assert(USER_ELF_SIZE == 0x100000, "USER_ELF_SIZE must be 1 MiB");
_Static_assert(USER_ELF_END  == 0x900000, "USER_ELF_END must be 0x900000");
_Static_assert(USER_PD_INDEX == 4, "USER_PD_INDEX must be 4 (0x800000 >> 21)");
/* Verify range fits within a single 2 MiB PD entry */
_Static_assert((USER_ELF_BASE >> 21) == ((USER_ELF_END - 1) >> 21),
    "User ELF range must fit within a single 2 MiB PD entry");
_Static_assert(USER_PT_WINDOW_BASE == 0x800000 &&
               USER_PT_WINDOW_END  == 0xA00000,
    "User PT window must span the full 2 MiB PD entry (0x800000-0xA00000)");
_Static_assert(USER_ELF_END < USER_PT_WINDOW_END,
    "0x900000 guard page must lie inside the reserved PT window");

/* Containment against the canonical map (memmap.h). These ranges are
 * sub-regions carved inside the user window, not independent windows, so they
 * must provably sit inside it. Without these the two headers -- included on
 * adjacent lines by vmm.c -- could drift apart with nothing to catch it, and
 * memmap.h's single-source-of-truth claim would be aspirational rather than
 * enforced. The higher-half relocation retires these constants; until then,
 * containment is what keeps them honest. */
_Static_assert(USER_ELF_BASE >= MM_USER_BASE && USER_ELF_END <= MM_USER_END,
    "User ELF range must lie inside the canonical user window (memmap.h)");
_Static_assert(USER_PT_WINDOW_END <= MM_USER_END,
    "User PT window must lie inside the canonical user window (memmap.h)");
_Static_assert(SECTION_VIEW_BASE >= MM_USER_BASE &&
               SECTION_VIEW_LIMIT <= MM_USER_END,
    "Section-view range must lie inside the canonical user window (memmap.h)");
_Static_assert(USER_ELF_END <= SECTION_VIEW_BASE ||
               SECTION_VIEW_LIMIT <= USER_ELF_BASE,
    "User ELF range and section-view range must not overlap");
