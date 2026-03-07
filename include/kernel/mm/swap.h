/* ============================================================================
 * swap.h — Swap / Page File Support
 *
 * Virtual memory swap subsystem. When physical memory is low, pages can be
 * "swapped out" to a backing store and retrieved on demand via page faults.
 *
 * Current backing store: RAM-based (simulated swap area). When a real disk
 * filesystem is available, the I/O functions can be replaced with
 * ATA/disk writes to C:\Impossible\System\pagefile.sys.
 *
 * Page replacement uses a Clock (second-chance) algorithm scanning the
 * Accessed bit in page table entries.
 *
 * Swap encoding in PTEs:
 *   When a page is swapped out, its PTE has Present=0 and bits 9-62
 *   encode the swap slot ID. Bit 1 (our custom SWAP flag) is set to 1
 *   to distinguish swapped pages from truly unmapped pages.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Swap slot size = one page (4 KiB) */
#define SWAP_SLOT_SIZE   4096

/* Maximum number of swap slots (= max pages in swap).
 * Default: 256 slots = 1 MiB of swap space.
 * Can be increased for larger swap files. */
#define SWAP_MAX_SLOTS   256

/* PTE bits used for swap encoding */
#define SWAP_PTE_FLAG    (1ULL << 1)   /* bit 1: swap marker (Present=0, Swap=1) */
#define SWAP_PTE_SHIFT   9            /* swap slot ID stored in bits 9-62 */
#define SWAP_PTE_MASK    0x7FFFFFFFFFE00ULL  /* mask for swap slot ID bits */

/* Encode a swap slot ID into a PTE value (Present=0, Swap=1) */
#define SWAP_ENCODE_PTE(slot) (SWAP_PTE_FLAG | ((uint64_t)(slot) << SWAP_PTE_SHIFT))

/* Decode a swap slot ID from a PTE value */
#define SWAP_DECODE_PTE(pte) ((uint32_t)(((pte) & SWAP_PTE_MASK) >> SWAP_PTE_SHIFT))

/* Check if a PTE represents a swapped page */
#define SWAP_IS_SWAPPED(pte) (((pte) & 1) == 0 && ((pte) & SWAP_PTE_FLAG) != 0)

/* --- Swap slot tracking --- */
typedef struct swap_slot {
    uint8_t  in_use;         /* 1 = slot contains a page */
    uintptr_t virt_addr;     /* virtual address that was swapped (for reverse lookup) */
} swap_slot_t;

/* --- Clock page replacement tracking --- */
#define CLOCK_MAX_PAGES  512  /* max tracked pages for replacement */

typedef struct clock_entry {
    uintptr_t virt_addr;     /* virtual address */
    uint8_t   in_use;        /* 1 = entry is valid */
} clock_entry_t;

/* --- Globals (defined in swap.c) --- */
extern uint8_t     *swap_store;
extern swap_slot_t  swap_slots[];
extern uint32_t     swap_used;

/* --- API --- */

/* Initialize the swap subsystem.
 * num_slots: number of swap slots to allocate (each = 4 KiB).
 * If 0, uses SWAP_MAX_SLOTS. */
void swap_init(uint32_t num_slots);

/* Swap out a page: write its contents to a swap slot, free the physical frame,
 * and update the PTE to encode the swap slot ID.
 * Returns the swap slot ID, or -1 on failure. */
int swap_out(uintptr_t virt_addr);

/* Swap in a page: allocate a physical frame, read the swap slot contents,
 * map the page back, and free the swap slot.
 * Returns 0 on success, -1 on failure. */
int swap_in(uint32_t swap_id, uintptr_t virt_addr);

/* Handle a page fault — check if the faulting address is swapped.
 * Returns 1 if handled (page was swapped in), 0 if not a swap fault. */
int swap_handle_fault(uintptr_t fault_addr, uint64_t error_code);

/* Register a page for clock replacement tracking. */
void swap_clock_register(uintptr_t virt_addr);

/* Select a victim page to swap out using the Clock algorithm.
 * Returns the virtual address of the victim, or 0 if none found. */
uintptr_t swap_clock_victim(void);

/* Get swap statistics */
uint32_t swap_get_total_slots(void);
uint32_t swap_get_used_slots(void);
uint32_t swap_get_free_slots(void);
