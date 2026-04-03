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

/* User ELF load range: 0x800000 .. 0x900000 (8 MiB .. 9 MiB)
 * Must be above kernel BSS end (~0x4C0000) to avoid collision.
 * Must be within a single 2 MiB PD entry for the split-PT optimization. */
#define USER_ELF_BASE   0x800000UL   /* first byte of user region */
#define USER_ELF_SIZE   0x100000UL   /* 1 MiB total region size */
#define USER_ELF_END    (USER_ELF_BASE + USER_ELF_SIZE)  /* 0x900000 */

/* PD index for the 2 MiB region containing user ELF (0x800000 >> 21 = 4) */
#define USER_PD_INDEX   (USER_ELF_BASE >> 21)

/* Compile-time sanity checks */
_Static_assert(USER_ELF_BASE == 0x800000, "USER_ELF_BASE must be 0x800000");
_Static_assert(USER_ELF_SIZE == 0x100000, "USER_ELF_SIZE must be 1 MiB");
_Static_assert(USER_ELF_END  == 0x900000, "USER_ELF_END must be 0x900000");
_Static_assert(USER_PD_INDEX == 4, "USER_PD_INDEX must be 4 (0x800000 >> 21)");
/* Verify range fits within a single 2 MiB PD entry */
_Static_assert((USER_ELF_BASE >> 21) == ((USER_ELF_END - 1) >> 21),
    "User ELF range must fit within a single 2 MiB PD entry");
