/* ============================================================================
 * boot_health_handoff.h -- cross-boot ABI for per-entry health-gated mark-good
 *
 * Three UEFI variables compose the bootloader <-> kernel handoff for TODO-07
 * section 14. All under IMPOSSIBLE_OS_VENDOR_GUID:
 *
 *   ImpossibleOS-CurBootCtr   (BS+RT, no NV)
 *     Written by bootloader pre-EBS after policy_counter_decrement. Carries
 *     the selected entry id plus the POST-decrement (tries_left, tries_done)
 *     pair. Kernel reads it during boot_health_check_run to compose the
 *     state-bound mark-good record. Volatile by design: a stale record from
 *     a prior boot is meaningless because the counter state it names no
 *     longer exists. Cleared on every reboot by firmware.
 *
 *   ImpossibleOS-HealthSubset (BS+RT, no NV)
 *     Optional. Written by bootloader pre-EBS iff the selected envelope has
 *     a non-empty `health_check_subset` array. Carries up to
 *     BOOT_HEALTH_SUBSET_MAX_NAMES check-name strings. Kernel reads it at
 *     gate time and restricts the run set to the intersection of (registry,
 *     subset). Empty / absent / invalid -> run all registered checks
 *     (default greenboot semantic).
 *
 *   ImpossibleOS-MarkGood     (NV+BS+RT)
 *     Written by kernel after a successful health gate. Carries
 *     {entry_id, tries_left, tries_done} copied verbatim from the
 *     CurBootCtr record so it is state-bound to the exact counter file
 *     the bootloader wrote this boot. Bootloader consumes ONLY when a
 *     counter file with the matching filename exists in the counters
 *     directory; mismatch -> clear without consuming (stale / replay).
 *     Persists across reboot because a clean shutdown between health-pass
 *     and the next boot is the normal case.
 *
 * Trust invariants enforced by the layout below:
 *   - Stale replay is defeated by state-bound consume: an intervening
 *     failed boot rotates the counter filename so the {entry_id,
 *     tries_left, tries_done} triple in a stale MarkGood record no
 *     longer matches any counter file -- the bootloader clears the var
 *     without consuming.
 *   - Forgery via NtSetSystemEnvironmentValueEx is blocked by a kernel-
 *     side reserved-name guard. The guard refuses user-mode writes for
 *     IMPOSSIBLE_OS_VENDOR_GUID + name in {ImpossibleOS-MarkGood,
 *     ImpossibleOS-CurBootCtr, ImpossibleOS-HealthSubset,
 *     ImpossibleOS-BootSticky}. The state-binding above is the primary
 *     defense; the syscall guard is defense in depth (an attacker would
 *     still have to guess the live counter triple to win).
 *   - Var write failures never block userland entry. Every consumer
 *     treats a missing / corrupt / unwritable record as "no opinion":
 *     kernel falls back to "tries_left already decremented, retry next
 *     boot"; bootloader falls back to "no mark-good available, leave
 *     counter alone".
 *
 * Layout discipline: every cross-boot struct is fixed-size, CRC-32-checked
 * (IEEE 802.3 polynomial, same as the entry-store envelope), magic +
 * version gated, and pinned with _Static_assert to catch ABI drift between
 * the bootloader producer and the kernel consumer.
 *
 * Owner: TODO-07 section 14. Layered ABOVE TODO-21 section 5 (slot-level
 * mark_boot_successful, still unshipped) -- the slot-level hook is a no-op
 * today; the entry-level counter removal is what this header makes work.
 * ============================================================================ */

#ifndef BOOT_HEALTH_HANDOFF_H
#define BOOT_HEALTH_HANDOFF_H

/* Caps. Match boot_entries_parser.h geometry where possible so the
 * bootloader can copy fields without re-marshalling. */
#define BOOT_HEALTH_HANDOFF_ID_LEN          64u
#define BOOT_HEALTH_SUBSET_NAME_LEN         24u   /* 23 chars + NUL */
#define BOOT_HEALTH_SUBSET_MAX_NAMES        8u

/* UEFI variable names (UCS-2 wide form is u"..."; ASCII mirror used for
 * the kernel-side reserved-name guard and bootcfg). */
#define BOOT_HEALTH_VAR_MARK_GOOD_ASCII     "ImpossibleOS-MarkGood"
#define BOOT_HEALTH_VAR_CUR_BOOT_CTR_ASCII  "ImpossibleOS-CurBootCtr"
#define BOOT_HEALTH_VAR_HEALTH_SUBSET_ASCII "ImpossibleOS-HealthSubset"

/* Magics. Distinct from boot_sticky's 'BSST' so a misread or cross-wired
 * variable surfaces as REJECT_BAD_MAGIC rather than silently parsing as a
 * different record type. */
#define BOOT_HEALTH_MARK_GOOD_MAGIC    0x4D4B4744u   /* 'MKGD' */
#define BOOT_HEALTH_CUR_BOOT_CTR_MAGIC 0x43425443u   /* 'CBTC' */
#define BOOT_HEALTH_SUBSET_MAGIC       0x48535542u   /* 'HSUB' */
#define BOOT_HEALTH_VAR_VERSION        1u

/* Size constants. Fixed per-variable so firmware quota accounting and the
 * static_asserts below stay in sync. */
#define BOOT_HEALTH_MARK_GOOD_SIZE     88u
#define BOOT_HEALTH_CUR_BOOT_CTR_SIZE  88u
#define BOOT_HEALTH_SUBSET_SIZE        \
    (16u + BOOT_HEALTH_SUBSET_MAX_NAMES * BOOT_HEALTH_SUBSET_NAME_LEN + 4u)

/* MarkGood: kernel-written, bootloader-consumed. Persists across reboot.
 *
 * entry_id is NUL-terminated within BOOT_HEALTH_HANDOFF_ID_LEN bytes.
 * tries_left + tries_done are copied verbatim from the CurBootCtr record
 * the kernel read this boot -- they encode the EXACT counter file the
 * bootloader wrote post-decrement, so consume is state-bound.
 *
 * reserved is zeroed by the writer; readers must reject non-zero (forward
 * compat sentinel).
 */
struct boot_health_mark_good_record {
    unsigned int   magic;                              /* BOOT_HEALTH_MARK_GOOD_MAGIC */
    unsigned int   version;                            /* BOOT_HEALTH_VAR_VERSION */
    char           entry_id[BOOT_HEALTH_HANDOFF_ID_LEN]; /* NUL-terminated */
    unsigned int   tries_left;                         /* post-decrement state */
    unsigned int   tries_done;                         /* post-decrement state */
    unsigned int   reserved;                           /* must be 0 */
    unsigned int   crc32;                              /* CRC-32 over above 84 bytes */
};

_Static_assert(sizeof(struct boot_health_mark_good_record) == BOOT_HEALTH_MARK_GOOD_SIZE,
    "boot_health_mark_good_record size must match BOOT_HEALTH_MARK_GOOD_SIZE");
_Static_assert(__builtin_offsetof(struct boot_health_mark_good_record, magic) == 0, "");
_Static_assert(__builtin_offsetof(struct boot_health_mark_good_record, version) == 4, "");
_Static_assert(__builtin_offsetof(struct boot_health_mark_good_record, entry_id) == 8, "");
_Static_assert(__builtin_offsetof(struct boot_health_mark_good_record, tries_left) == 72, "");
_Static_assert(__builtin_offsetof(struct boot_health_mark_good_record, tries_done) == 76, "");
_Static_assert(__builtin_offsetof(struct boot_health_mark_good_record, reserved) == 80, "");
_Static_assert(__builtin_offsetof(struct boot_health_mark_good_record, crc32) == 84, "");

/* CurBootCtr: bootloader-written, kernel-consumed. Per-boot only.
 *
 * Identical layout to mark-good record so the kernel can copy fields
 * one-to-one. Different magic so a misread surfaces cleanly.
 */
struct boot_health_cur_boot_ctr_record {
    unsigned int   magic;                              /* BOOT_HEALTH_CUR_BOOT_CTR_MAGIC */
    unsigned int   version;                            /* BOOT_HEALTH_VAR_VERSION */
    char           entry_id[BOOT_HEALTH_HANDOFF_ID_LEN]; /* NUL-terminated */
    unsigned int   tries_left;                         /* post-decrement */
    unsigned int   tries_done;                         /* post-decrement */
    unsigned int   reserved;                           /* must be 0 */
    unsigned int   crc32;                              /* CRC-32 over above 84 bytes */
};

_Static_assert(sizeof(struct boot_health_cur_boot_ctr_record) == BOOT_HEALTH_CUR_BOOT_CTR_SIZE,
    "boot_health_cur_boot_ctr_record size must match BOOT_HEALTH_CUR_BOOT_CTR_SIZE");
_Static_assert(__builtin_offsetof(struct boot_health_cur_boot_ctr_record, magic) == 0, "");
_Static_assert(__builtin_offsetof(struct boot_health_cur_boot_ctr_record, version) == 4, "");
_Static_assert(__builtin_offsetof(struct boot_health_cur_boot_ctr_record, entry_id) == 8, "");
_Static_assert(__builtin_offsetof(struct boot_health_cur_boot_ctr_record, tries_left) == 72, "");
_Static_assert(__builtin_offsetof(struct boot_health_cur_boot_ctr_record, tries_done) == 76, "");
_Static_assert(__builtin_offsetof(struct boot_health_cur_boot_ctr_record, reserved) == 80, "");
_Static_assert(__builtin_offsetof(struct boot_health_cur_boot_ctr_record, crc32) == 84, "");

/* HealthSubset: bootloader-written, kernel-consumed. Per-boot only.
 *
 * count is the number of populated entries in names; remainder is
 * zero-padded. Each name is a NUL-terminated ASCII string within
 * BOOT_HEALTH_SUBSET_NAME_LEN bytes. Names are matched against the kernel
 * registry by exact string comparison; unknown names are ignored with a
 * warning (forward-compat for future check additions).
 */
struct boot_health_subset_record {
    unsigned int   magic;                              /* BOOT_HEALTH_SUBSET_MAGIC */
    unsigned int   version;                            /* BOOT_HEALTH_VAR_VERSION */
    unsigned int   count;                              /* 0..BOOT_HEALTH_SUBSET_MAX_NAMES */
    unsigned int   reserved;                           /* must be 0 */
    char           names[BOOT_HEALTH_SUBSET_MAX_NAMES][BOOT_HEALTH_SUBSET_NAME_LEN];
    unsigned int   crc32;                              /* CRC-32 over above */
};

_Static_assert(sizeof(struct boot_health_subset_record) == BOOT_HEALTH_SUBSET_SIZE,
    "boot_health_subset_record size must match BOOT_HEALTH_SUBSET_SIZE");
_Static_assert(__builtin_offsetof(struct boot_health_subset_record, magic) == 0, "");
_Static_assert(__builtin_offsetof(struct boot_health_subset_record, version) == 4, "");
_Static_assert(__builtin_offsetof(struct boot_health_subset_record, count) == 8, "");
_Static_assert(__builtin_offsetof(struct boot_health_subset_record, reserved) == 12, "");
_Static_assert(__builtin_offsetof(struct boot_health_subset_record, names) == 16, "");
_Static_assert(__builtin_offsetof(struct boot_health_subset_record, crc32) ==
    16 + BOOT_HEALTH_SUBSET_MAX_NAMES * BOOT_HEALTH_SUBSET_NAME_LEN, "");

/* CRC-32/IEEE-802.3 over the first (total_len - 4) bytes of buf. Same
 * polynomial used by boot_entries envelope and boot_sticky so producer
 * and consumer agree byte-for-byte. Inline so both the freestanding
 * bootloader (no kernel headers) and the kernel can use it without
 * pulling a separate object file. */
static inline unsigned int
boot_health_handoff_compute_crc(const void *buf, unsigned int total_len)
{
    static const unsigned int poly = 0xEDB88320u;
    const unsigned char *p = (const unsigned char *)buf;
    unsigned int crc = 0xFFFFFFFFu;
    unsigned int n;
    if (total_len < 4u) return 0u;
    n = total_len - 4u;  /* exclude trailing crc32 field */
    for (unsigned int i = 0; i < n; i++) {
        crc ^= (unsigned int)p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (poly & -(int)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Validators. Each returns 1 (valid) or 0 (rejected). Validators are
 * shared between bootloader and kernel so the trust gate is identical on
 * both sides. */
static inline int
boot_health_mark_good_is_valid(const struct boot_health_mark_good_record *r)
{
    unsigned int i;
    if (!r) return 0;
    if (r->magic != BOOT_HEALTH_MARK_GOOD_MAGIC) return 0;
    if (r->version != BOOT_HEALTH_VAR_VERSION) return 0;
    if (r->reserved != 0u) return 0;
    /* entry_id must be NUL-terminated within the buffer. Empty id ("")
     * is rejected: a kernel-written mark-good record with an empty entry
     * id is a producer bug; clear and ignore. */
    for (i = 0; i < BOOT_HEALTH_HANDOFF_ID_LEN; i++)
        if (r->entry_id[i] == '\0') break;
    if (i == 0u || i == BOOT_HEALTH_HANDOFF_ID_LEN) return 0;
    if (r->crc32 != boot_health_handoff_compute_crc(r,
            (unsigned int)sizeof(*r))) return 0;
    return 1;
}

static inline int
boot_health_cur_boot_ctr_is_valid(const struct boot_health_cur_boot_ctr_record *r)
{
    unsigned int i;
    if (!r) return 0;
    if (r->magic != BOOT_HEALTH_CUR_BOOT_CTR_MAGIC) return 0;
    if (r->version != BOOT_HEALTH_VAR_VERSION) return 0;
    if (r->reserved != 0u) return 0;
    for (i = 0; i < BOOT_HEALTH_HANDOFF_ID_LEN; i++)
        if (r->entry_id[i] == '\0') break;
    if (i == 0u || i == BOOT_HEALTH_HANDOFF_ID_LEN) return 0;
    if (r->crc32 != boot_health_handoff_compute_crc(r,
            (unsigned int)sizeof(*r))) return 0;
    return 1;
}

static inline int
boot_health_subset_is_valid(const struct boot_health_subset_record *r)
{
    unsigned int i, j;
    if (!r) return 0;
    if (r->magic != BOOT_HEALTH_SUBSET_MAGIC) return 0;
    if (r->version != BOOT_HEALTH_VAR_VERSION) return 0;
    if (r->reserved != 0u) return 0;
    if (r->count > BOOT_HEALTH_SUBSET_MAX_NAMES) return 0;
    /* Each populated name must be NUL-terminated AND printable ASCII
     * (matching the host validator + parser grammar). Names that
     * survive this validator are safe to pass to the registry name
     * comparison without further sanitisation. */
    for (i = 0; i < r->count; i++) {
        int saw_nul = 0;
        for (j = 0; j < BOOT_HEALTH_SUBSET_NAME_LEN; j++) {
            unsigned char c = (unsigned char)r->names[i][j];
            if (c == 0) { saw_nul = 1; break; }
            if (c < 0x20u || c > 0x7Eu || c == '\\' || c == '"') return 0;
        }
        if (j == 0u || !saw_nul) return 0;
    }
    /* Enforce zero-fill on unused slots so a producer bug or version-
     * skewed bootloader cannot leave stale bytes in names[count..MAX)
     * yet still produce a CRC-valid record. */
    for (i = r->count; i < BOOT_HEALTH_SUBSET_MAX_NAMES; i++) {
        for (j = 0; j < BOOT_HEALTH_SUBSET_NAME_LEN; j++) {
            if (r->names[i][j] != 0) return 0;
        }
    }
    if (r->crc32 != boot_health_handoff_compute_crc(r,
            (unsigned int)sizeof(*r))) return 0;
    return 1;
}

#endif /* BOOT_HEALTH_HANDOFF_H */
