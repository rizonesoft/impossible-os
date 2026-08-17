/* ============================================================================
 * tpm_baseline.h -- measured-boot baseline blob (enroll / verify / rotate)
 *
 * The CONTENT layer of measured-boot attestation: a versioned baseline blob
 * holding the golden per-bank PCR digests + Secure Boot state + a firmware-
 * version hash + the kernel-ABI manifest hash + a monotonic generation. The
 * blob is persisted in an OWNER-auth TPM NV DATA index (tpm_nv.h) so it is
 * READABLE on every boot -- never the PCR-policy-sealed index, which would be
 * circular (you would need the good PCR state to read the values that define
 * it).
 *
 * Phase contract: ENROLLMENT runs only when the recovery-authorized enroll
 * config bit is set (never silent first-boot TOFU). VERIFY runs as a Phase-1
 * step AFTER tpm_transport_init / tpm_pcr_cache_init / Secure Boot reconcile /
 * tpm_replay_verify -- NOT in tpm_integrity_init (Phase 0, before the transport
 * and PCR cache exist). Verify reads the blob, snapshots the current state, and
 * sets boot_integrity_report.overall_status VERIFIED / MISMATCH / NO_BASELINE.
 *
 * The format / marshal / compare / generation logic is pure (MMIO-free, fixture-
 * tested); the snapshot / enroll / verify wrappers drive tpm_pcr_get + the NV
 * wrappers and are not ISR-safe.
 *
 * NOT in scope (tracked follow-ups): real bootloader/kernel IMAGE hashes (the
 * .bootproto sha is the ABI-manifest hash, not the image -- needs bootloader
 * work to compute + carry them), physical-console keypress confirmation, and
 * TPM NV write-lock / monotonic-counter anti-rollback.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define TPM_BASELINE_MAGIC    0x4C534142u  /* "BASL" little-endian */
#define TPM_BASELINE_VERSION  1u
#define TPM_BASELINE_DIGEST   32u           /* SHA-256 -- the measured-boot bank */
/* Golden digests are stored for the measured-boot PCR set {0..7, 11}. */
#define TPM_BASELINE_MAX_PCRS 9u

/* One golden PCR slot (SHA-256 bank). */
struct tpm_baseline_pcr {
    uint8_t index;                       /* PCR index */
    uint8_t present;                     /* 1 = active bank for this PCR at enroll */
    uint8_t pad[2];
    uint8_t digest[TPM_BASELINE_DIGEST];  /* golden digest (valid iff present) */
};

/* The on-NV baseline blob. The struct IS the wire format (fixed layout); crc32
 * covers every byte BEFORE the crc32 field. */
struct tpm_baseline {
    uint32_t magic;                      /* TPM_BASELINE_MAGIC */
    uint16_t version;                    /* TPM_BASELINE_VERSION */
    uint16_t size;                       /* sizeof(struct tpm_baseline) */
    uint32_t generation;                 /* monotonic; rotation must strictly increase */
    uint16_t alg;                        /* TPM_ALG_* bank of the PCR digests */
    uint8_t  pcr_count;                  /* populated slots in pcrs[] */
    uint8_t  secure_boot;                /* golden Secure Boot active state */
    uint8_t  secure_boot_valid;          /* golden SB-readability (0 = was unknown) */
    uint8_t  fw_hash_present;            /* 1 = fw_hash holds a firmware-version hash */
    uint8_t  abi_manifest_present;       /* 1 = abi_manifest holds the manifest hash */
    uint8_t  pad;
    uint8_t  fw_hash[TPM_BASELINE_DIGEST];       /* SHA-256 of SMBIOS bios_version */
    uint8_t  abi_manifest[TPM_BASELINE_DIGEST];  /* kernel-ABI manifest sha256 (identity) */
    struct tpm_baseline_pcr pcrs[TPM_BASELINE_MAX_PCRS];
    uint32_t crc32;                      /* IEEE CRC32 over bytes [0, crc32 offset) */
};

/* Compare verdict (pure golden-vs-current comparison). */
typedef enum {
    TPM_BASELINE_MATCH      = 0,  /* every golden PCR + SB + fw-hash matches current */
    TPM_BASELINE_MISMATCH   = 1,  /* at least one differs */
    TPM_BASELINE_CMP_BADARG = 2,
} tpm_baseline_verdict_t;

/* Wrapper status (enroll / verify drive the TPM). */
typedef enum {
    TPM_BASELINE_OK         = 0,
    TPM_BASELINE_NO_BASELINE = 1,  /* NV index not defined / never written */
    TPM_BASELINE_NO_TPM     = 2,   /* transport unavailable */
    TPM_BASELINE_CORRUPT    = 3,   /* STORED blob failed magic/version/size/crc */
    TPM_BASELINE_TPMERR     = 4,   /* TPM/NV transaction failure */
    TPM_BASELINE_BADARG     = 5,
    /* THIS KERNEL's own build-time `.bootproto` ABI identity failed validation.
     * Split from TPM_BASELINE_CORRUPT deliberately: the two are operationally
     * OPPOSITE even though both are integrity failures. A corrupt STORED blob
     * invites "offer a re-enroll", which is exactly the wrong response to a
     * corrupt KERNEL descriptor -- it would enroll the corruption as the new
     * golden. Any handler that treats CORRUPT as re-enrollable must not reach
     * this value. */
    TPM_BASELINE_SELF_CORRUPT = 6,
} tpm_baseline_status_t;

/* ---- Pure core (MMIO-free, fixture-tested) ---- */

/* Finalize an assembled baseline for storage: stamps magic / version / size and
 * computes crc32 over the preceding bytes. Returns the blob size (== sizeof),
 * or 0 on a NULL argument. */
uint32_t tpm_baseline_finalize(struct tpm_baseline *b);

/* Validate an UNTRUSTED stored blob into a caller-provided ALIGNED struct (no
 * unaligned cast over the byte buffer -- safe on strict-alignment targets like
 * ARM64). Copies the blob into *out, then checks length, magic, version, size,
 * crc32, AND canonical structure: pcr_count must be the full measured set in
 * the expected index order, present/flags are boolean, and at least one PCR is
 * pinned (a blob that pins NO PCR must not verify). Returns 1 on a valid
 * canonical baseline (with *out populated), 0 otherwise. out must be non-NULL. */
int tpm_baseline_validate(const uint8_t *blob, uint32_t len,
                          struct tpm_baseline *out);

/* Compare a validated golden baseline against a current snapshot (same struct
 * shape, generation/crc ignored): every golden PCR slot with present==1 must
 * equal the current digest for that index+bank, and the SB state must match.
 * Returns MATCH / MISMATCH / BADARG. Pure -- the caller supplies `current`
 * (gathered live by tpm_baseline_snapshot).
 *
 * The two optional digests use DIFFERENT presence rules, on purpose:
 *   fw-hash      -- one-way. Checked only when the GOLDEN carries one, because
 *                   SMBIOS may genuinely be absent, so a golden enrolled
 *                   without a firmware hash is a legitimate state.
 *   abi-manifest -- SYMMETRIC. A presence disagreement in EITHER direction is a
 *                   mismatch. Every correctly built kernel carries this digest
 *                   (tpm_baseline_snapshot fails outright rather than reporting
 *                   it absent), so a golden without one can only be a
 *                   pre-binding baseline; accepting it would let that baseline
 *                   report VERIFIED forever while attesting nothing about the
 *                   ABI. Clearing the mismatch takes a re-enroll. */
tpm_baseline_verdict_t tpm_baseline_compare(const struct tpm_baseline *golden,
                                            const struct tpm_baseline *current);

/* Per-PCR comparison DETAIL, for the boot-integrity report's per-PCR slots.
 *
 * Separate from tpm_baseline_compare on purpose. That function answers "does
 * this boot match the baseline", and to answer it cheaply it returns MISMATCH
 * on the first difference -- including the SCALAR ones (bank, Secure Boot,
 * firmware hash, ABI manifest), which it checks BEFORE looking at any PCR. So a
 * scalar-caused mismatch leaves it with no PCR result to report, and a report
 * publishing "MISMATCH" beside stale per-PCR detail is exactly the
 * self-contradiction the report is supposed to remove.
 *
 * This function therefore always walks the PCRs, whatever the scalars did, and
 * writes one BOOT_INTEGRITY_* status per GOLDEN pcr slot into out_status
 * (positionally matching golden->pcrs[]):
 *   VERIFIED    -- golden slot present and the current digest for that index
 *                  in the same bank is identical
 *   MISMATCH    -- present in the golden but differing, or no longer readable
 *   NO_BASELINE -- the golden pins no digest for that slot, so this boot's
 *                  value is unverified rather than wrong
 * A bank disagreement makes NO digest comparable, so every slot is MISMATCH.
 *
 * Returns the number of entries written (0 on a NULL/oversized argument, which
 * the caller must treat as NOT-EVALUATED rather than as a pass). A cap below
 * the golden count CLAMPS here rather than failing, which is safe only because
 * the return value is the true written extent: a caller pairing this with an
 * overall verdict must not truncate, and tpm_baseline_verify enforces that by
 * refusing an undersized buffer outright. Pure. */
uint8_t tpm_baseline_compare_pcrs(const struct tpm_baseline *golden,
                                  const struct tpm_baseline *current,
                                  uint8_t *out_status, uint8_t cap);

/* Rotation guard: a new baseline may replace an old one only if its generation
 * strictly increases (anti-rollback at the content layer; NV write-lock/counter
 * hardening is a tracked follow-up). Returns 1 if new_gen > old_gen. */
int tpm_baseline_rotation_ok(uint32_t old_gen, uint32_t new_gen);

/* ---- Live wrappers (Phase-1 transport; not ISR-safe) ---- */

/* Gather the CURRENT measured state into a baseline struct: PCR digests for the
 * measured set {0..7,11} in `alg` via tpm_pcr_get, Secure Boot state via
 * tpm_sb_reconcile_report, and the firmware-version hash via SHA-256 over the
 * SMBIOS bios_version, and the kernel-ABI manifest digest via
 * boot_proto_abi_digest. generation/crc are left 0 (the caller stamps them on
 * enroll).
 *
 * Returns OK; NO_TPM when the full measured PCR set is not readable; or CORRUPT
 * when the kernel's own `.bootproto` ABI digest fails validation. CORRUPT is
 * fail-closed by design: that digest is a compile-time constant, so its absence
 * means read-only kernel data is corrupt, and enrolling a baseline with the
 * identity silently dropped would produce a golden that binds nothing. */
tpm_baseline_status_t tpm_baseline_snapshot(uint16_t alg, struct tpm_baseline *out);

/* Enroll: snapshot the current state, stamp a monotonic generation (the existing
 * baseline's generation + 1, or 1 for a first enroll, so a rotation never rolls
 * the counter backward), finalize, and store the blob in the owner-auth NV DATA
 * index `nv_index` (define-if-absent then write). RECOVERY-GATED -- callers must
 * check the recovery + enroll config gate first. */
tpm_baseline_status_t tpm_baseline_enroll(uint32_t nv_index, uint16_t alg);

/* Verify: read the blob from `nv_index`, validate it, snapshot the current
 * state, and compare. When a verdict is produced, *out_overall is set to the
 * BOOT_INTEGRITY_* value (VERIFIED on a full match, MISMATCH on any difference
 * or a corrupt blob, NO_BASELINE when the index is undefined/never written) and
 * the return is OK / CORRUPT / NO_BASELINE. On NO_TPM / TPMERR / BADARG no
 * verdict is produced and *out_overall is left unchanged -- the caller applies
 * *out_overall to the integrity report only on a verdict-producing return.
 * out_overall may be NULL.
 *
 * PER-PCR DETAIL. `out_pcr_status` (capacity `pcr_cap`) receives one
 * BOOT_INTEGRITY_* per golden PCR slot via tpm_baseline_compare_pcrs, and
 * *out_pcr_n the count written, so the caller can publish the overall verdict
 * and the per-PCR detail in ONE report update and they cannot disagree.
 *
 * *out_pcr_n is 0 on every path that never reached the comparison -- an absent
 * baseline, a corrupt stored blob, a corrupt kernel descriptor, a snapshot that
 * failed. That 0 means NOT EVALUATED and the caller must publish it as such;
 * treating it as "no PCR problems" would report a clean per-PCR detail for a
 * boot whose PCRs were never checked. The two out params are INDEPENDENTLY
 * optional: a status buffer with no count pointer is filled normally, and a
 * count pointer with no status buffer reports 0.
 *
 * A non-NULL out_pcr_status with pcr_cap smaller than the golden PCR count is
 * BADARG with NO verdict produced, rather than a truncated detail array. A
 * clamp would hand back VERIFIED beside a partial detail whose tail publishes
 * as UNKNOWN, which is the overall-vs-per-PCR contradiction the per-PCR
 * plumbing exists to remove. Size the buffer to TPM_BASELINE_MAX_PCRS, or pass
 * NULL to decline the detail entirely. */
tpm_baseline_status_t tpm_baseline_verify(uint32_t nv_index, uint16_t alg,
                                          uint8_t *out_overall,
                                          uint8_t *out_pcr_status,
                                          uint8_t pcr_cap,
                                          uint8_t *out_pcr_n);
