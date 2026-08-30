/* ============================================================================
 * boot_reserved.h -- single authoritative table of boot_info-derived
 * physical regions that PMM must NOT reclaim.
 *
 * Scope: boot_info-driven regions only (struct boot_info itself, USB
 * DMA pages + scratchpad, TPM event log, framebuffer, UEFI runtime
 * memory map, typed payloads with BOOT_PAYLOAD_FLAG_RESERVED, the
 * loader-owned kernel boot stack).
 *
 * OUT of scope (handled directly by pmm_init's existing calls): first
 * 1 MiB legacy area, kernel image, bitmap itself, user-mode ELF
 * range. Those are PMM-internal -- their addresses and sizes are
 * determined by the kernel's own layout, not by a bootloader
 * handoff field, so they are reserved by `pmm_mark_region_used()`
 * calls inside `pmm_init` and do not need the table.
 *
 * Failure modes:
 *   - overlap between any two entries in the table is fatal (producer
 *     bug: two retained regions disagree on ownership).
 *   - phys_start + length overflow is fatal (malformed bootloader).
 *   - count >= BOOT_RESERVED_MAX is fatal (producer bug; we refuse to
 *     silently truncate).
 *
 * ============================================================================ */

#ifndef KERNEL_MM_BOOT_RESERVED_H
#define KERNEL_MM_BOOT_RESERVED_H

#include "kernel/types.h"
#include "kernel/boot_init.h"

struct boot_info;  /* forward decl -- full definition in kernel/boot_info.h */

/* Fixed upper bound. Worst-case current boot: 1 (boot_info) + 16 (USB
 * DMA) + 1 (scratchpad) + 1 (TPM) + 1 (FB) + 64 (rt_mmap_max) + 32
 * (payloads) = 116. 128 leaves headroom for a future descriptor class. */
#define BOOT_RESERVED_MAX 128

enum boot_reserved_kind {
    BOOT_RESERVED_NONE             = 0,
    BOOT_RESERVED_BOOT_INFO        = 1,  /* struct boot_info at 0x10000 */
    BOOT_RESERVED_USB_DMA_PAGE     = 2,  /* xHCI DMA page (DCBAA/rings/ERST) */
    BOOT_RESERVED_USB_SCRATCHPAD   = 3,  /* xHCI scratchpad contiguous pages */
    BOOT_RESERVED_TPM_EVENT_LOG    = 4,  /* copied TCG event log */
    BOOT_RESERVED_FRAMEBUFFER      = 5,  /* linear GOP framebuffer */
    BOOT_RESERVED_RT_MMAP          = 6,  /* UEFI runtime services region */
    BOOT_RESERVED_PAYLOAD = 7, /* typed payload with FLAG_RESERVED */
    BOOT_RESERVED_BOOT_STACK       = 8,  /* loader-owned kernel boot stack */
};

struct boot_reserved_region {
    uint64_t phys_start;           /* inclusive base physical address */
    uint64_t length;               /* size in bytes */
    uint32_t kind;                 /* enum boot_reserved_kind */
    uint32_t source_index;         /* 0 for singletons; array index for USB/rt_mmap/payload */
};

enum boot_reserved_error {
    BOOT_RESERVED_ERR_OK              = 0,
    BOOT_RESERVED_ERR_COUNT_OOR       = 1,  /* population would exceed BOOT_RESERVED_MAX */
    BOOT_RESERVED_ERR_RANGE_WRAP      = 2,  /* phys_start + length overflows uint64_t */
    BOOT_RESERVED_ERR_OVERLAP         = 3,  /* two retained regions disagree */
    BOOT_RESERVED_ERR_NULL_INFO       = 4,  /* NULL boot_info */
    BOOT_RESERVED_ERR_ZERO_LENGTH     = 5,  /* zero-length reservation refused */
};

/* Walk boot_info and populate the table. Runs in pmm_init BEFORE
 * `pmm_mark_region_used()` is called on anything table-driven. On
 * BOOT_FATAL the specific failure class is written to *out_err. */
boot_result_t boot_reserved_populate_from_info(const struct boot_info *info,
                                               enum boot_reserved_error *out_err);

/* Apply every table entry to the PMM via pmm_mark_region_used(). Idempotent:
 * safe to call multiple times; pmm_mark_region_used() itself is idempotent. */
void boot_reserved_apply(void);

/* Scan every BOOT_RESERVED_PAYLOAD entry for overlap with the given
 * range. Used by pmm_init to check bootloader-loaded payloads
 * against the PMM-internal reservations (first 1 MiB, kernel image,
 * bitmap, USER_ELF range) that boot_payload_validate() cannot see
 * (it runs before pmm_init has computed the bitmap layout).
 *
 * Returns the 1-based index (idx + 1) of the first colliding payload
 * entry, or 0 if all payload entries are disjoint from [start,
 * start + len). `label` is embedded in the LOG_ERROR on hit for
 * diagnosis (e.g. "pmm_bitmap", "user_elf"). */
uint32_t boot_reserved_check_payloads_disjoint(uint64_t start,
                                               uint64_t len,
                                               const char *label);

/* LOG_INFO one line per entry (phys, length, kind name, source index). */
void boot_reserved_log(void);

/* Emit the table as JSON at X:\Diag\boot-reserved.json. Idempotent. */
void boot_reserved_blackbox_dump(void);

/* Accessors for tests / future consumers. */
/* Did the Phase-0 pass actually PIN the payload descriptor at this index?
 *
 * ASK THIS BEFORE DEREFERENCING ANY PAYLOAD. BOOT_PAYLOAD_FLAG_RESERVED is
 * the PRODUCER's request and the kernel does not rewrite it, so the bit
 * being set is not proof the range was pinned. The pass can decline for
 * reasons a consumer cannot reconstruct from the handoff -- the aggregate
 * reservation budget and the singleton rule both depend on the OTHER
 * descriptors and on walk order. This answers from the reservation table
 * itself, so it cannot disagree with what was pinned. */
int boot_reserved_payload_is_pinned(uint32_t payload_index,
                                    uint64_t phys_start, uint64_t length);

uint32_t boot_reserved_count(void);
const struct boot_reserved_region *boot_reserved_get(uint32_t index);

#ifdef KERNEL_TESTS
void boot_reserved_reset_for_test(void);
#endif

#endif /* KERNEL_MM_BOOT_RESERVED_H */
