/* ============================================================================
 * ab_boot_metadata.h -- A/B dual-slot boot metadata wire ABI (TODO-21 sec 1)
 *
 * The single source of truth for which root slot (A or B) the bootloader
 * selects, how many boot attempts remain per slot, whether a slot has been
 * marked good, and the per-slot anti-rollback version. Written/read by both
 * the pre-ExitBootServices bootloader (the GPT/disk storage adapter lands in
 * sec 2) and the kernel (mark_boot_successful adapter in sec 5), so the
 * struct + validators + CRC live here as shared static-inline code -- the
 * trust gate is byte-identical on both sides (same idiom as
 * boot_health_handoff.h).
 *
 * Storage: the record lives on disk as TWO redundant fixed blocks (sec 7
 * integrity), NOT in UEFI NVRAM -- EFI vars are rejected for the
 * high-frequency `tries` state per the repo BootSticky/boot_history doctrine.
 * The newest valid copy (highest `generation`) wins; if both fail validation
 * the reader loads factory defaults (sec 7).
 *
 * Relationship to existing infra (do NOT duplicate):
 *   - boot_rollback.c owns the security-version anti-rollback FLOOR
 *     (IPOSRequiredSecVersion NVRAM, boot_rollback_mark_steady/raise_if_steady).
 *     `rollback_floor` here is the A/B-slot view that integrates with it (sec 8).
 *   - boot_health_handoff.h owns per-ENTRY boot counting (tries_left/tries_done)
 *     + mark-good. This A/B-SLOT layer coexists above it (sec 3/4/5 wiring).
 *
 * The full v1 layout is pinned NOW (including the sec 7 generation/crc32 and
 * sec 8 rollback_index/floor fields) so the on-disk ABI is stable across the
 * sec 1/7/8 implementations; behavior for the later-section fields ships in
 * those sections, but the bytes never move.
 *
 * Layout discipline: fixed-size, magic + version gated, reserved-zero forward
 * sentinel, CRC-32/IEEE-802.3 over (size - 4) bytes, _Static_assert offset
 * pins -- identical conventions to boot_health_handoff.h.
 * ============================================================================ */

#ifndef AB_BOOT_METADATA_H
#define AB_BOOT_METADATA_H

#define AB_BOOT_META_MAGIC    0x41424D44u   /* 'ABMD' */
#define AB_BOOT_META_VERSION  1u
#define AB_BOOT_META_SIZE     60u

#define AB_BOOT_SLOT_A        0u
#define AB_BOOT_SLOT_B        1u
#define AB_BOOT_SLOT_COUNT    2u

/* Max boot attempts before a slot is considered exhausted (sec 4 rollback). */
#define AB_BOOT_MAX_TRIES     3u

/* Per-slot record. tries/successful drive the sec 3 successful/pending/
 * unbootable state model (pending = !successful && tries < MAX; unbootable =
 * !successful && tries >= MAX; the state is derived, not a separate field).
 * rollback_index is the monotonic OS/security version checked against the
 * floor (sec 8). */
struct ab_boot_slot {
    unsigned int tries;          /* boot attempts consumed, 0..AB_BOOT_MAX_TRIES */
    unsigned int successful;     /* 1 once mark_boot_successful confirmed this slot */
    unsigned int priority;       /* higher = preferred when both slots valid */
    unsigned int rollback_index; /* monotonic security version (sec 8) */
};

_Static_assert(sizeof(unsigned int) == 4u, "ab_boot wire ABI assumes 32-bit unsigned int");
_Static_assert(sizeof(struct ab_boot_slot) == 16u, "ab_boot_slot must be 16 bytes");
_Static_assert(__builtin_offsetof(struct ab_boot_slot, tries) == 0, "");
_Static_assert(__builtin_offsetof(struct ab_boot_slot, successful) == 4, "");
_Static_assert(__builtin_offsetof(struct ab_boot_slot, priority) == 8, "");
_Static_assert(__builtin_offsetof(struct ab_boot_slot, rollback_index) == 12, "");

struct ab_boot_metadata {
    unsigned int magic;                          /* AB_BOOT_META_MAGIC */
    unsigned int version;                        /* AB_BOOT_META_VERSION */
    unsigned int generation;                     /* monotonic; newest valid copy wins (sec 7) */
    unsigned int active_slot;                    /* AB_BOOT_SLOT_A or _B */
    struct ab_boot_slot slot[AB_BOOT_SLOT_COUNT];/* per-slot state */
    unsigned int rollback_floor;                 /* min acceptable rollback_index (sec 8) */
    unsigned int reserved;                       /* must be 0 (forward sentinel) */
    unsigned int crc32;                          /* CRC-32 over the first 56 bytes */
};

_Static_assert(sizeof(struct ab_boot_metadata) == AB_BOOT_META_SIZE,
    "ab_boot_metadata size must match AB_BOOT_META_SIZE");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, magic) == 0, "");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, version) == 4, "");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, generation) == 8, "");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, active_slot) == 12, "");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, slot) == 16, "");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, rollback_floor) == 48, "");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, reserved) == 52, "");
_Static_assert(__builtin_offsetof(struct ab_boot_metadata, crc32) == 56, "");

/* CRC-32/IEEE-802.3 over the first (total_len - 4) bytes of buf (excludes the
 * trailing crc32 field). Same polynomial as boot_health_handoff /
 * boot_entries envelope so producers and consumers agree byte-for-byte.
 * Inline so the freestanding bootloader and the kernel share one definition
 * without a separate object file. */
static inline unsigned int
ab_boot_meta_compute_crc(const void *buf, unsigned int total_len)
{
    static const unsigned int poly = 0xEDB88320u;
    const unsigned char *p = (const unsigned char *)buf;
    unsigned int crc = 0xFFFFFFFFu;
    unsigned int n;
    if (total_len < 4u) return 0u;
    n = total_len - 4u;
    for (unsigned int i = 0; i < n; i++) {
        crc ^= (unsigned int)p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (poly & -(int)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Validate a record: magic, version, reserved-zero, active_slot in range, and
 * CRC over the first (AB_BOOT_META_SIZE - 4) bytes. Returns 1 (valid) or 0.
 * Shared so the trust gate is identical on bootloader and kernel sides. */
static inline int
ab_boot_meta_is_valid(const struct ab_boot_metadata *r)
{
    if (!r) return 0;
    if (r->magic != AB_BOOT_META_MAGIC) return 0;
    if (r->version != AB_BOOT_META_VERSION) return 0;
    if (r->reserved != 0u) return 0;
    if (r->active_slot >= AB_BOOT_SLOT_COUNT) return 0;
    /* Per-slot field domains: a CRC-valid but crafted/corrupt record must not
     * smuggle out-of-domain tries/successful into the rollback state machine. */
    for (unsigned int s = 0u; s < AB_BOOT_SLOT_COUNT; s++) {
        if (r->slot[s].tries > AB_BOOT_MAX_TRIES) return 0;
        if (r->slot[s].successful > 1u) return 0;
    }
    if (ab_boot_meta_compute_crc(r, AB_BOOT_META_SIZE) != r->crc32) return 0;
    return 1;
}

/* Populate `out` with factory defaults: boot slot A, no tries consumed, not
 * yet successful, slot A preferred, zero rollback state. Used when both stored
 * copies fail validation (sec 7 corrupt-metadata recovery). CRC is finalized. */
static inline void
ab_boot_meta_default(struct ab_boot_metadata *out)
{
    unsigned int i;
    if (!out) return;
    for (i = 0; i < AB_BOOT_META_SIZE; i++)
        ((unsigned char *)out)[i] = 0u;
    out->magic = AB_BOOT_META_MAGIC;
    out->version = AB_BOOT_META_VERSION;
    out->generation = 0u;
    out->active_slot = AB_BOOT_SLOT_A;
    out->slot[AB_BOOT_SLOT_A].priority = 1u;  /* A preferred over a blank B */
    out->crc32 = ab_boot_meta_compute_crc(out, AB_BOOT_META_SIZE);
}

/* Stamp magic/version, clear reserved, and finalize the CRC. Call before
 * serializing a record to storage so ab_boot_meta_is_valid accepts it. */
static inline void
ab_boot_meta_finalize(struct ab_boot_metadata *out)
{
    if (!out) return;
    out->magic = AB_BOOT_META_MAGIC;
    out->version = AB_BOOT_META_VERSION;
    out->reserved = 0u;
    out->crc32 = ab_boot_meta_compute_crc(out, AB_BOOT_META_SIZE);
}

/* Given the two redundant stored copies, return the valid one with the higher
 * `generation` (sec 7 newest-wins), or 0 if neither validates (caller loads
 * factory defaults). On a tie in generation, prefers `a`. *winner receives the
 * chosen pointer when the return is 1. */
static inline int
ab_boot_meta_select_newest(const struct ab_boot_metadata *a,
                           const struct ab_boot_metadata *b,
                           const struct ab_boot_metadata **winner)
{
    int va = ab_boot_meta_is_valid(a);
    int vb = ab_boot_meta_is_valid(b);
    if (!va && !vb) return 0;
    if (va && !vb) { if (winner) *winner = a; return 1; }
    if (!va && vb) { if (winner) *winner = b; return 1; }
    if (winner) *winner = (b->generation > a->generation) ? b : a;
    return 1;
}

#endif /* AB_BOOT_METADATA_H */
