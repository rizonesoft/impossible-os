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
#include "kernel/tpm_record.h"   /* tpm_pairing_t for the pairing-status map */
#include "kernel/tpm_nv.h"       /* tpm_nv_status_t for the NV-status map */

#define TPM_BASELINE_MAGIC    0x4C534142u  /* "BASL" little-endian */
#define TPM_BASELINE_VERSION  1u
#define TPM_BASELINE_DIGEST   32u           /* SHA-256 -- the measured-boot bank */
/* Golden digests are stored for the measured-boot PCR set {0..7, 11}. */
#define TPM_BASELINE_MAX_PCRS 9u

/* Attempts the boot makes at tpm_baseline_verify while it reports
 * TPM_BASELINE_BUSY. Small and fixed: BUSY is another CPU's in-flight TPM
 * transaction, which finishes inside its own bounded sequence, so this is
 * contention backoff and never a wait on hardware. Retrying ONLY BUSY is the
 * safety property -- see the TPM_BASELINE_BUSY comment for why BUDGET must not
 * be retried blind. */
#define TPM_BASELINE_VERIFY_RETRIES 3u

/* PAUSE-instruction spins between verify retries (boot_interrupts.c). Not a
 * calibrated real-time delay -- just enough cycles to give a competing CPU's
 * in-flight bounded sequence room to progress before the next attempt, which a
 * zero-gap retry loop cannot do. Kept small and bounded (no subsystem
 * dependency: works before mono_clock or the scheduler is up) so the total
 * cost across TPM_BASELINE_VERIFY_RETRIES attempts stays a bounded spin, not a
 * wait for hardware. */
#define TPM_BASELINE_VERIFY_BACKOFF_SPINS 20000u

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
    TPM_BASELINE_NO_TPM     = 2,   /* could not measure on THIS attempt. Reached
                                    * from four distinct causes: the transport is
                                    * unavailable; it was BUSY with another
                                    * transaction; the operation outran the
                                    * boot's budget; or a required PCR could not
                                    * be snapshotted (inactive bank, bad
                                    * argument). All of them leave the integrity
                                    * verdict UNPUBLISHED rather than reporting a
                                    * false tamper, which is the property this
                                    * value exists for.
                                    *
                                    * Retry safety DIFFERS by cause, which is why
                                    * the retry-SAFE cause no longer lands here:
                                    * transport contention now reports
                                    * TPM_BASELINE_BUSY. What remains under
                                    * NO_TPM is deliberately NOT retryable --
                                    * BUDGET abandoned the command in flight with
                                    * its completion UNKNOWN, so a blind retry of
                                    * state-changing work can apply it twice (the
                                    * same reason tpm_nv_increment refuses to
                                    * retry), and an unavailable transport or an
                                    * unsnapshottable PCR will not change on the
                                    * next call either. A caller wanting to retry
                                    * past a BUDGET must reconcile at the
                                    * operation level or use an idempotent one. */
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
    /* The stored blob is well-formed but is NOT bound to an authenticated
     * record, on a kernel whose update authority IS provisioned.
     *
     * Distinct from CORRUPT because the failure is authenticity, not integrity:
     * the blob's CRC is perfect, which is exactly the attack. An owner-auth
     * attacker can take an old vulnerable baseline, stamp the current
     * generation on it and recompute the CRC, and every structural check
     * passes. Only the bind record catches it, so a baseline with no bind
     * record is refused rather than accepted as golden.
     *
     * Also distinct from CORRUPT in what it invites: a corrupt blob invites a
     * re-enroll, and re-enrolling here would silently authenticate whatever an
     * attacker last wrote. The repair is the authorized migration.
     * The repair is the authorized migration owned by the versioned baseline
     * growth and NV index migration work. */
    TPM_BASELINE_UNBOUND = 7,
    /* The bind record and its commit counter DISAGREE: the counter has moved
     * past any record behind it, or a record sits ahead of a commit that never
     * landed. Distinct from UNBOUND because the operator action is opposite.
     * UNBOUND says no authenticated record was ever written and the repair is
     * an authorized migration of a legacy blob. This says an authorized record
     * WAS written and the pairing is broken.
     *
     * ONE THING IS COMMON TO EVERY PAIRING THAT LANDS HERE, and it is the only
     * thing this status asserts: the repair is AUTHORIZED, and it is never a
     * fresh enrollment -- that would overwrite the evidence of a rollback with
     * a new golden and launder it.
     *
     * The repair itself is NOT common, so this status does not name one. A
     * counter ahead of the record (TPM_PAIRING_TORN) has lost the committed
     * bytes and needs authorized recovery. A record one ahead of the counter
     * (TPM_PAIRING_UNCOMMITTED) is an authentic record whose commit increment
     * never landed, which a crash produces and so does an ordinary write whose
     * later readback, policy or increment step failed; completing that commit
     * is a legitimate repair where recovery would be an over-reaction. Read
     * tpm_ab_floor_read_view() / tpm_baseline_bind_view() for the direction
     * (tpm_pairing_t) and choose on it -- the direction is exactly what those
     * views exist to carry, and this collapsed status must never be the sole
     * input to a repair decision. */
    TPM_BASELINE_TORN = 8,
    /* An authenticated bind record is CURRENT -- its pairing with the commit
     * counter is intact -- and it says the stored blob is not the one that was
     * enrolled. That is the relabel attack the bind record exists to catch: old
     * vulnerable content, the current generation, a correctly recomputed CRC,
     * every structural check passing.
     *
     * Split out of UNBOUND because the two operator actions are opposite, and
     * collapsing them invited exactly the wrong one: a machine under active
     * tamper would be told to migrate its blob, authenticating the attacker's
     * content.
     *
     * NOTE, a known wart rather than a claim: UNBOUND is still OVERLOADED
     * elsewhere. It is returned for a legacy blob with no bind record, for
     * TPM_NV_UNAVAIL, and by both enroll entry points when no authority is
     * provisioned. Splitting those belongs to the enroll path and is owned by
     * section 29; the split here covers the VERIFY path only, which is the one
     * that decides a boot's integrity verdict. */
    TPM_BASELINE_RELABELED = 9,
    /* The NV index behind the bind record failed its ENROLLED CONTRACT before
     * any pairing could be computed: a wrong Name, a redefined index, a counter
     * whose public area does not match the compiled manifest. That is detected
     * tamper, and a publishable authenticity failure.
     *
     * Distinct from TPMERR, which is reserved for a failure that produced NO
     * integrity conclusion at all. Routing an identity failure to TPMERR left
     * an attacker-supplied index definition detected and then omitted from the
     * boot verdict -- caught, and silently. */
    TPM_BASELINE_IDENTITY = 10,
    /* Transport CONTENTION -- another transaction held the TPM -- and the ONLY
     * value in this enum that is safe to retry blind.
     *
     * Split out of NO_TPM because the two are operationally opposite even though
     * both leave the verdict unpublished on the attempt that produced them.
     * NO_TPM says "this machine could not measure", which an operator reads as a
     * missing or broken TPM; BUSY says "nothing was submitted, ask again".
     * Collapsing contention into NO_TPM made a momentarily busy TPM
     * indistinguishable from an absent one, so the boot abandoned its integrity
     * verdict for the WHOLE boot over a condition that clears in milliseconds.
     *
     * BUDGET deliberately does NOT map here. It looks equally transient and is
     * not: the command was abandoned IN FLIGHT and may have executed, so a blind
     * retry is exactly what must not happen. Only "the TPM never received it" is
     * retry-safe, and BUSY is the sole status that guarantees it. */
    TPM_BASELINE_BUSY = 11,
} tpm_baseline_status_t;

/* ---- Pure core (MMIO-free, fixture-tested) ---- */

/* Map a bind-record pairing verdict onto the baseline status the boot reports.
 *
 * PURE, and extracted rather than left inline because this mapping is the whole
 * recovery-routing decision: it is what decides whether an operator is told to
 * migrate a legacy blob, complete an interrupted commit, or enter authorized
 * recovery. Inline inside the verifier it was reachable only through a full
 * fake-TIS fixture, so most of its enum values had no coverage at all and a
 * regression could have routed a destroyed anchor to re-enrollment -- which
 * launders the rollback evidence -- with every test still green.
 *
 * CURRENT maps to TPM_BASELINE_RELABELED, NOT to OK: this function is only
 * consulted once the digest comparison has already failed, so an intact pairing
 * means the record is committed and says this blob is not the enrolled one.
 * BADARG maps to TPM_BASELINE_IDENTITY, not to TPMERR. This function is only
 * consulted on a TPM_NV_MISMATCH, and the only MISMATCH that arrives before a
 * pairing could be computed is an index that failed its enrolled contract --
 * detected tamper, which the boot must PUBLISH. TPMERR is reserved for a
 * failure that reached no conclusion at all, and the boot drops it. */
tpm_baseline_status_t tpm_baseline_pairing_status(tpm_pairing_t pairing);

/* Is this status a CONCLUSIVE INTEGRITY FAILURE on the VERIFY path?
 *
 * ONE rule, used both by tpm_baseline_verify to decide whether to write
 * *out_overall and by the boot to decide whether to publish it, because the gap
 * kept reappearing one value at a time: each new status had to be remembered in
 * an allowlist, and a forgotten one meant a DETECTED attack left the previous
 * verdict standing while every test stayed green. Failing to publish is silent
 * by construction, which is why the rule lives here rather than in two
 * hand-maintained lists.
 *
 * NOT included: NO_TPM and TPMERR reached no conclusion (an absent, contended
 * or too-slow device has not failed to match), and BADARG is a caller error
 * rather than a statement about the machine.
 *
 * VERIFY-PATH ONLY, and TPM_BASELINE_UNBOUND is why: on the verify path it
 * means a blob with no authenticated bind record, but the ENROLL entry points
 * return the same value as an ordinary configuration refusal on a healthy
 * authority-provisioned machine. An enroll-side caller must exclude it. Pure. */
int tpm_baseline_status_is_failure(tpm_baseline_status_t bs);

/* Map an NV-layer status onto the baseline status the boot reports.
 *
 * PURE, and exported for the same reason as the pairing map above: this switch
 * decides which failures the boot PUBLISHES and which it drops as
 * inconclusive, and getting one arm wrong means a detected attack is caught and
 * then silently discarded. Kept static, it was reachable only through full
 * fixtures and its most security-relevant arms had no coverage.
 *
 * The arm that matters most: RECREATED and CONTRACT are DETECTED TAMPER with no
 * benign producer in any context, so they map to TPM_BASELINE_IDENTITY
 * (published), never to TPMERR (dropped) and never to NO_BASELINE (which this
 * file treats as a genuine first enroll, and reaching it is the laundering the
 * identity gate exists to stop).
 *
 * MISMATCH is deliberately NOT in that class here, and the two maps disagree
 * about it on purpose. The WRITE path produces MISMATCH for an ordinary commit-
 * counter race between the in-sequence read and the write, so classifying it
 * globally as tamper would make bound enrollment report an attack on a healthy
 * contended machine. The VERIFY path never reaches this map with a MISMATCH: it
 * is intercepted and routed through tpm_baseline_pairing_status above, whose
 * BADARG arm is the verification-specific identity mapping. */
tpm_baseline_status_t tpm_baseline_nv_status(tpm_nv_status_t st);

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

/* Enroll or rotate a baseline AND its authenticated bind record, as one
 * operation. This is the only correct path once an update authority is
 * provisioned: tpm_baseline_enroll writes the blob under owner auth and nothing
 * else, so using it after a bind exists would leave the bind describing the
 * PREVIOUS blob and the next verify would report TPM_BASELINE_UNBOUND. That is
 * why the plain enroll now REFUSES with UNBOUND while an authority is
 * installed, rather than quietly breaking the machine it was asked to update.
 *
 * Order is blob then bind, deliberately: the bind digest covers the blob, so a
 * bind that lands describes bytes already on the device, and a bind that fails
 * leaves the machine fail-closed and re-bindable rather than authenticating a
 * blob that may never have been written.
 *
 * The bind is computed over the blob READ BACK from the index, never over a
 * freshly derived one. Re-deriving looks equivalent and is not: the stored
 * blob carries the generation the enroll assigned, so a second snapshot would
 * bind bytes that can never match and every rotation would verify as a
 * mismatch.
 *
 * Returns TPM_BASELINE_OK, TPM_BASELINE_BADARG on a NULL transition,
 * TPM_BASELINE_UNBOUND when no authority is provisioned, or whatever the
 * enroll or the authorized bind reports. */
struct tpm_authz_transition;
tpm_baseline_status_t tpm_baseline_enroll_bound(uint32_t nv_index, uint16_t alg,
                                                const struct tpm_authz_transition *tr);

/* Verify: read the blob from `nv_index`, validate it, snapshot the current
 * state, and compare.
 *
 * VERDICT-PRODUCING returns set *out_overall to a BOOT_INTEGRITY_* value and a
 * caller MUST publish it: TPM_BASELINE_OK (VERIFIED on a full match),
 * TPM_BASELINE_NO_BASELINE (the index is undefined or never written),
 * TPM_BASELINE_CORRUPT and TPM_BASELINE_SELF_CORRUPT, and the four authenticity
 * failures TPM_BASELINE_UNBOUND, TPM_BASELINE_TORN, TPM_BASELINE_RELABELED and
 * TPM_BASELINE_IDENTITY (all MISMATCH).
 *
 * NON-VERDICT returns leave *out_overall untouched and MUST NOT be published:
 * TPM_BASELINE_NO_TPM (absent, contended, or too slow -- a machine that could
 * not measure has not failed to match), TPM_BASELINE_TPMERR (a device fault
 * that reached no conclusion) and TPM_BASELINE_BADARG.
 *
 * The distinction is the whole point: publishing a non-verdict reports a false
 * tamper, and DROPPING a verdict leaves a detected attack invisible under
 * whatever the previous phase published. An earlier version of this comment
 * listed only OK, CORRUPT and NO_BASELINE, and every caller written against it
 * would silently discard the authenticity verdicts. */
tpm_baseline_status_t tpm_baseline_verify(uint32_t nv_index, uint16_t alg,
                                          uint8_t *out_overall,
                                          uint8_t *out_pcr_status,
                                          uint8_t pcr_cap,
                                          uint8_t *out_pcr_n);
