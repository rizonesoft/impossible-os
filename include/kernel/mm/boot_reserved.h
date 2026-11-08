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
 * (payloads) + 1 (boot stack) + 3 (UKI payload copies) = 120; a
 * _Static_assert in boot_reserved.c holds the sum under this bound. */
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
    BOOT_RESERVED_UKI_PAYLOAD      = 9,  /* signed UKI .initrd/.recovery/.modules copy */
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

/* Verdicts of boot_reserved_range_backing(). */
enum boot_range_backing {
    BOOT_RANGE_BACKED    = 0,  /* every byte is map-described readable RAM */
    BOOT_RANGE_ABOVE_MAP = 1,  /* ends beyond the 4 GiB identity map */
    BOOT_RANGE_NO_MAP    = 2,  /* the handoff carries no memory map */
    BOOT_RANGE_FORBIDDEN = 3,  /* intersects an entry the kernel may not read */
    BOOT_RANGE_GAP       = 4,  /* some byte is described by no entry (or len 0 / wrap) */
};

/* Is every byte of [start, start + len) memory the kernel may read, per the
 * handoff's memory map? `allow_warm` admits BOOT_MMAP_WARM_UPDATE (the sealed
 * warm extent only). On BOOT_RANGE_FORBIDDEN, *out_type (optional) names the
 * offending map type. Pure: reads `info`, keeps no state. */
uint32_t boot_reserved_range_backing(const struct boot_info *info,
                                     uint64_t start, uint64_t len,
                                     int allow_warm, uint32_t *out_type);

/* Freeze a heap copy of the table for boot_reserved_blackbox_dump(). Called by
 * boot_tests_run() BEFORE the suite, whose fixtures wipe the live table, so
 * the dump publishes what this boot really retained. A failed copy makes the
 * dump publish nothing rather than a fixture. Boot CPU only, Phase 3. */
void boot_reserved_freeze_for_dump(void);

/* Put the frozen table back after the boot suite, so a reader after Phase 3
 * (boot_reserved_payload_is_pinned(), boot_reserved_get()) answers from this
 * boot's handoff rather than the last test fixture. No-op when nothing was
 * frozen or the copy failed. Boot CPU only, Phase 3, after the suite. */
void boot_reserved_restore_after_tests(void);

/* Replace the live table with `n` entries of `src` (clamped to
 * BOOT_RESERVED_MAX; NULL loads an empty table) and zero the rest. Returns the
 * count loaded. The restore is built on it; tests drive it with a local array. */
uint32_t boot_reserved_load_table(const struct boot_reserved_region *src,
                                  uint32_t n);

/* boot-reserved.json sizing: every entry line is shorter than
 * BR_DUMP_LINE_MAX (a longer one fails the dump), and the header plus footer
 * fit BR_DUMP_FRAMING_MAX. */
#define BR_DUMP_LINE_MAX     192u
#define BR_DUMP_FRAMING_MAX  256u

/* Copy up to `cap` table entries into `dst`; returns the number copied.
 * Pure over the table; the freeze uses it, and tests can. */
uint32_t boot_reserved_copy_table(struct boot_reserved_region *dst,
                                  uint32_t cap);

/* How much of a region PMM's bitmap tracks, for the dump: pmm_mark_region_used()
 * is void and clamps at the ceiling silently, so the artifact says it instead. */
enum boot_reserved_tracked {
    BOOT_RESERVED_TRACKED_FULL    = 0,
    BOOT_RESERVED_TRACKED_PARTIAL = 1,
    BOOT_RESERVED_TRACKED_NONE    = 2,
};
uint32_t boot_reserved_tracked_class(uint64_t phys_start, uint64_t length,
                                     uint64_t total_frames);

/* Accessors for tests / future consumers. */
/* Did the Phase-0 pass actually PIN the payload descriptor at this index?
 *
 * ASK THIS BEFORE DEREFERENCING ANY PAYLOAD. BOOT_PAYLOAD_FLAG_RESERVED is
 * the PRODUCER's request and the kernel does not rewrite it, so the bit
 * being set is not proof the range was pinned. The pass can decline for
 * reasons a consumer cannot reconstruct from the handoff -- the aggregate
 * reservation budget and the singleton rule both depend on the OTHER
 * descriptors and on walk order. This answers from the reservation table
 * itself, so it cannot disagree with what was pinned.
 *
 * THE PROMISE of a 1: every byte of the range is described by the boot memory
 * map as memory the kernel may read (boot_reserved_range_backing() ==
 * BOOT_RANGE_BACKED) and lies inside the 4 GiB identity map, so it is safe to
 * dereference, and no allocator will hand it out. It does NOT promise the
 * range is inside pmm_get_total_frames(): a frame above that ceiling is
 * outside the bitmap, so no pmm_alloc_* path can return it either; only its
 * table entry marks nothing. */
int boot_reserved_payload_is_pinned(uint32_t payload_index,
                                    uint64_t phys_start, uint64_t length);

/* Did the reservation pass pin the signed UKI payload copy in `slot`
 * (UKI_PAYLOAD_SLOT_* in include/boot/uki_payload_limits.h) at exactly this
 * range? The boot_info v14 uki_*_addr / uki_*_size fields are the loader's
 * CLAIM; they are not rewritten when the pass declines them (boot_info is a
 * measured record, see boot_reserved_payload_is_pinned). A consumer of a UKI
 * payload asks this first and refuses a range that was not pinned, because
 * the PMM treats unpinned EfiLoaderData as free memory. */
int boot_reserved_uki_payload_is_pinned(uint32_t slot,
                                        uint64_t phys_start, uint64_t length);

/* SHA-256 of [data, data + len) as lowercase hex, NUL-terminated. Pure. */
#define BOOT_RESERVED_DIGEST_HEX_LEN 64u
void boot_reserved_digest_hex(const void *data, uint64_t len,
                              char out[BOOT_RESERVED_DIGEST_HEX_LEN + 1u]);

/* Diagnostic, run only when boot.conf sets uki_digest=1 (hashing a full
 * payload costs boot time). Log the SHA-256 of every pinned UKI payload copy
 * as it was received
 * ("boot_reserved: UKI .initrd received sha256=..."), so the bytes the kernel
 * holds can be compared with the signed section they were copied from. Reads
 * the copies through the boot identity map: pmm_init only, right after
 * boot_reserved_apply(), never from a test fixture. */
void boot_reserved_log_uki_digests(void);

uint32_t boot_reserved_count(void);
const struct boot_reserved_region *boot_reserved_get(uint32_t index);

#ifdef KERNEL_TESTS
void boot_reserved_reset_for_test(void);
#endif

#endif /* KERNEL_MM_BOOT_RESERVED_H */
