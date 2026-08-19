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
 * TPM_BASELINE_BUSY. Small and fixed: BUSY means the transport gate is held by
 * ANOTHER TPM sequence (cross-CPU contention on an SMP-capable caller, or a
 * still-releasing prior operation on the SAME CPU at the current sole caller,
 * boot_phase1, which runs single-CPU before AP bring-up) that finishes inside
 * its own bounded sequence -- so this is contention backoff and never a wait
 * on hardware. Retrying ONLY BUSY is the safety property -- see the
 * TPM_BASELINE_BUSY comment for why BUDGET must not be retried blind. */
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

/* Layer 1 for the two structs above, which ARE the on-NV wire format.
 *
 * Absent until section 29's review, and the gap is exactly the one the record
 * layer next door does not have: tpm_record.h pins every header offset and all
 * three sizes, and those asserts are what prove its structs are padding-free
 * and therefore safe to copy whole. Here the same claim ("the struct IS the
 * wire format") rested on nothing, so a reordered field or a compiler that
 * inserted padding would not fail the BUILD -- it would fail as a fleet-wide
 * TPM_BASELINE_CORRUPT on the next boot, against blobs already written.
 *
 * The offsets are ABSOLUTE NUMBERS, not expressions over the macros that
 * define the struct, and that distinction is the second half of the guard. An
 * assert written as TPM_BASELINE_MAX_PCRS * (4 + TPM_BASELINE_DIGEST) stays
 * true when someone raises MAX_PCRS to measure more PCRs -- a legitimate
 * change -- while the persisted blob silently grows past 412 bytes and every
 * NV index already written becomes unreadable at its declared size. Absolute
 * values make that a BUILD failure, which is the moment to bump
 * TPM_BASELINE_VERSION and write a migration rather than the next boot.
 *
 * EVERY persisted field is pinned, not a selection of them, and the difference
 * is the whole value: the first version of this block pinned only the coarse
 * boundaries, which left equal-width neighbours free to swap. Exchanging
 * secure_boot with secure_boot_valid keeps every size and every pinned offset
 * true while silently reinterpreting blobs already written to NV -- a
 * fleet-wide false integrity failure that the build would have waved through.
 * An assert set that reads as coverage without being it is worse than none.
 *
 * The size asserts stay as independent padding guards, and the crc32 assert as
 * a finality guard: a field appended after the checksum would sit outside the
 * region the checksum covers. */
_Static_assert(sizeof(struct tpm_baseline_pcr) == 36u,
               "tpm_baseline_pcr v1 is 36 bytes; padding or growth would corrupt it");
_Static_assert(__builtin_offsetof(struct tpm_baseline_pcr, index) == 0u,
               "tpm_baseline_pcr.index offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline_pcr, present) == 1u,
               "tpm_baseline_pcr.present offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline_pcr, pad) == 2u,
               "tpm_baseline_pcr.pad offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline_pcr, digest) == 4u,
               "tpm_baseline_pcr.digest offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, magic) == 0u,
               "tpm_baseline.magic must lead the blob");
_Static_assert(__builtin_offsetof(struct tpm_baseline, version) == 4u,
               "tpm_baseline.version offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, size) == 6u,
               "tpm_baseline.size offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, generation) == 8u,
               "tpm_baseline.generation offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, alg) == 12u,
               "tpm_baseline.alg offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, pcr_count) == 14u,
               "tpm_baseline.pcr_count offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, secure_boot) == 15u,
               "tpm_baseline.secure_boot offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, secure_boot_valid) == 16u,
               "tpm_baseline.secure_boot_valid offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, fw_hash_present) == 17u,
               "tpm_baseline.fw_hash_present offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, abi_manifest_present) == 18u,
               "tpm_baseline.abi_manifest_present offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, pad) == 19u,
               "tpm_baseline.pad offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, fw_hash) == 20u,
               "tpm_baseline.fw_hash offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, abi_manifest) == 52u,
               "tpm_baseline.abi_manifest offset pinned");
_Static_assert(__builtin_offsetof(struct tpm_baseline, pcrs) == 84u,
               "tpm_baseline.pcrs offset pinned");
_Static_assert(sizeof(struct tpm_baseline) == 412u,
               "tpm_baseline v1 is 412 bytes on NV; growing it needs a VERSION "
               "bump and a migration, not a larger struct");
_Static_assert(__builtin_offsetof(struct tpm_baseline, crc32) == 408u,
               "tpm_baseline.crc32 offset pinned");
/* The formula forms are kept BESIDE the absolute pins, not instead of them:
 * these catch compiler padding (the absolute pins would too, but these say
 * WHICH relation broke), while the absolute pins catch a macro change that
 * grows the blob with every relation still intact. */
_Static_assert(sizeof(struct tpm_baseline_pcr) == 4u + TPM_BASELINE_DIGEST,
               "tpm_baseline_pcr must stay free of padding");
_Static_assert(sizeof(struct tpm_baseline)
               == 20u + (2u * TPM_BASELINE_DIGEST)
                  + (TPM_BASELINE_MAX_PCRS * (4u + TPM_BASELINE_DIGEST)) + 4u,
               "tpm_baseline must stay free of padding");
/* The crc32 field is LAST, and the whole integrity scheme depends on it: the
 * checksum covers every byte before its own offset, so a field added after it
 * would sit outside the digest entirely. Written as a RELATION as well as an
 * absolute offset above, because this one must hold at any future version. */
_Static_assert(__builtin_offsetof(struct tpm_baseline, crc32)
               == sizeof(struct tpm_baseline) - 4u,
               "tpm_baseline.crc32 must be the final field");

/* Compare verdict (pure golden-vs-current comparison). */
typedef enum {
    TPM_BASELINE_MATCH      = 0,  /* every golden PCR + SB + fw-hash matches current */
    TPM_BASELINE_MISMATCH   = 1,  /* at least one differs */
    TPM_BASELINE_CMP_BADARG = 2,
} tpm_baseline_verdict_t;

/* WHICH field produced a MISMATCH verdict.
 *
 * THIS IS A FIRST-MISMATCH DIAGNOSTIC, NOT AN EXHAUSTIVE INVENTORY, and the
 * distinction is load-bearing for anyone reading it. tpm_baseline_compare
 * returns at the first difference it finds, so a boot where the Secure Boot
 * state AND a PCR both moved reports only the Secure Boot state. The
 * comparison order below is the stable precedence, and it is deliberately
 * most-general first: a hash-bank disagreement makes every digest
 * incomparable, so naming a PCR under it would be nonsense.
 *
 *   BANK -> SB_VALIDITY -> SB_STATE -> FW_HASH_ABSENT -> FW_HASH ->
 *   ABI_PRESENCE -> ABI_CONTENT -> NO_PCR_PINNED -> (per golden slot, in
 *   golden order) PCR_ABSENT / PCR_DIGEST
 *
 * The complete per-PCR picture is a SEPARATE surface and always has been:
 * tpm_baseline_compare_pcrs walks every golden slot regardless of what the
 * scalars did. A consumer that needs "everything that differs" reads that; a
 * consumer that needs "what stopped this boot verifying" reads this. */
typedef enum {
    TPM_BASELINE_CAUSE_NONE           = 0,  /* verdict was MATCH; no cause */
    TPM_BASELINE_CAUSE_BANK           = 1,  /* golden and current hash banks differ */
    TPM_BASELINE_CAUSE_SB_VALIDITY    = 2,  /* Secure Boot readability changed */
    TPM_BASELINE_CAUSE_SB_STATE       = 3,  /* Secure Boot on/off changed */
    TPM_BASELINE_CAUSE_FW_HASH_ABSENT = 4,  /* golden pins a fw hash, current has none */
    TPM_BASELINE_CAUSE_FW_HASH        = 5,  /* firmware-version hash differs */
    TPM_BASELINE_CAUSE_ABI_PRESENCE   = 6,  /* ABI-manifest presence disagrees */
    TPM_BASELINE_CAUSE_ABI_CONTENT    = 7,  /* ABI-manifest digest differs */
    TPM_BASELINE_CAUSE_NO_PCR_PINNED  = 8,  /* golden pins no PCR at all */
    TPM_BASELINE_CAUSE_PCR_DIGEST     = 9,  /* a pinned PCR's digest differs */
    TPM_BASELINE_CAUSE_PCR_ABSENT     = 10, /* a pinned PCR is no longer readable */
    TPM_BASELINE_CAUSE_BADARG         = 11, /* verdict was CMP_BADARG */
} tpm_baseline_cause_t;

/* First-mismatch attribution, filled by tpm_baseline_compare_detail.
 *
 * FAIL-CLOSED CONTRACT: every entry point that accepts one of these WRITES all
 * four bytes before it can return, including the paths that never compare
 * anything. A caller therefore cannot read a stale cause from a previous call
 * beside a fresh verdict, which is the shape that would make this diagnostic
 * worse than no diagnostic at all. */
struct tpm_baseline_mismatch {
    uint8_t cause;      /* tpm_baseline_cause_t */
    uint8_t pcr_index;  /* PCR register number; meaningful only when pcr_valid */
    uint8_t pcr_valid;  /* 1 when cause is PCR_DIGEST or PCR_ABSENT */
    uint8_t pad;
};

/* One-word name for a tpm_baseline_cause_t, for serial/diagnostic reporting:
 * "none", "hash-bank", "secure-boot-readability", "secure-boot-state",
 * "firmware-hash-absent", "firmware-hash", "abi-manifest-presence",
 * "abi-manifest-content", "no-pcr-pinned", "pcr-digest", "pcr-absent",
 * "bad-argument". Every name describes the FIRST difference only and claims
 * nothing about the rest. An out-of-range value renders "unknown". Pure. */
const char *tpm_baseline_cause_label(uint8_t cause);

/* Operator-facing DIAGNOSIS for a status that names an action, with recovery
 * guidance whose strength varies by status (classified exhaustively below), or
 * NULL for a status with nothing to advise.
 *
 * NON-NULL IS NOT A FAILURE TEST, and this contract said it was until section
 * 29 added the two configuration refusals. Text is returned for the SEVEN
 * integrity failures AND for TPM_BASELINE_NOAUTH and TPM_BASELINE_AUTHREQ,
 * which tpm_baseline_status_is_failure deliberately excludes -- so a caller
 * discriminating on non-NULL would report an expected enrollment refusal as
 * tamper. Ask the PREDICATE what to publish and ask this function what to TELL
 * the operator; they answer different questions on purpose. NULL is returned
 * for TPM_BASELINE_OK, NO_BASELINE, NO_TPM, TPMERR, BUSY, BADARG and any
 * out-of-range value.
 *
 * ONE helper because there are TWO callers and they used to disagree. The
 * Phase-1 verification path named four statuses in a hand-written chain while
 * the ENROLLMENT path published the same MISMATCH verdict with only a numeric
 * `status %d`, so a corrupt kernel identity discovered during enrollment
 * reached the operator as a number. Splitting the text across call sites is
 * what let them drift; there is now one source for it.
 *
 * The guidance is the point, not the label, and two of these are OPPOSITE:
 * a corrupt STORED baseline is somebody else's bad bytes, while a corrupt
 * KERNEL identity means the party doing the measuring cannot be trusted to
 * produce one at all. Current enrollment REFUSES BOTH -- see the SELF_CORRUPT
 * enum contract -- so the difference is about what a future change may safely
 * do, never about today's behavior.
 *
 * WHAT EACH STRING PROMISES IS NOT UNIFORM, and pretending otherwise is how a
 * consumer or a later reader ends up inventing a repair. Every entry is a
 * DIAGNOSIS; the recovery half falls into exactly four kinds, and this list is
 * exhaustive over the seven failure statuses plus the two configuration
 * refusals:
 *
 *   diagnosis only, deliberately -- TORN, RELABELED, IDENTITY. TORN collapses
 *     three directions whose repairs differ, so naming one would be destructive
 *     over-recovery; the other two name what happened and stop because the
 *     authorized replacement they would point at does not exist yet.
 *     RECORD is in this same kind: it says the enrolled index answered with
 *     bytes that are not a record, and stops there, because the authorized
 *     recovery that would replace them does not exist yet either.
 *   diagnosis plus AN ACTION AVAILABLE TODAY -- NOAUTH (install an update
 *     authority) and AUTHREQ (use the authorized enroll path). These two are
 *     not integrity failures at all, which is precisely why non-NULL cannot be
 *     read as a failure test.
 *   diagnosis plus a REQUIRED but currently UNAVAILABLE path -- UNBOUND
 *     (authorized migration, owned by the versioned-baseline-growth work) and
 *     CORRUPT (authorized index replacement). These name the direction so an
 *     operator is not sent somewhere harmful, and the direction is deliberately
 *     not yet an instruction they can carry out.
 *   diagnosis plus an immediately actionable instruction -- SELF_CORRUPT:
 *     do not enroll, reinstall the kernel.
 *
 * No entry states a next step that would deterministically FAIL if attempted;
 * that is the property being held, and it is weaker than "everything named here
 * is reachable today". The CORRUPT text earns its place in the third kind by
 * saying what re-enrolling will actually do rather than implying it repairs
 * anything. Do not "complete" the first kind by inventing an action.
 *
 * Pure -- safe from any context. */
const char *tpm_baseline_status_repair(uint8_t status);

/* Wrapper status (enroll / verify drive the TPM). */
typedef enum {
    TPM_BASELINE_OK         = 0,
    TPM_BASELINE_NO_BASELINE = 1,  /* NV index not defined / never written */
    TPM_BASELINE_NO_TPM     = 2,   /* could not measure on THIS attempt. Reached
                                    * from three distinct causes: the transport
                                    * is unavailable; the operation outran the
                                    * boot's budget; or a required PCR could not
                                    * be snapshotted (inactive bank, bad
                                    * argument). NV-LAYER contention is no
                                    * longer among them: it reports
                                    * TPM_BASELINE_BUSY, as the retry note below
                                    * says. PCR-layer contention still DOES land
                                    * here, and that is a known wart rather than
                                    * a claim -- tpm_baseline_snapshot collapses
                                    * every unreadable PCR into a partial
                                    * snapshot and returns NO_TPM, so a
                                    * TPM_PCR_BUSY read is reported as "could
                                    * not measure" and is not retried even
                                    * though it would be safe to. All
                                    * of them leave the integrity
                                    * verdict UNPUBLISHED rather than reporting a
                                    * false tamper, which is the property this
                                    * value exists for.
                                    *
                                    * Retry safety DIFFERS by cause, which is why
                                    * NV-layer contention no longer lands here:
                                    * it reports TPM_BASELINE_BUSY. What remains
                                    * is deliberately NOT retryable, with the
                                    * PCR-layer contention above as the one
                                    * known exception (safe to retry, currently
                                    * reported as though it were not) --
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
     * OPPOSITE even though both are integrity failures, and the difference is
     * about WHAT IS TRUSTWORTHY rather than about what today's code happens to
     * do. A corrupt STORED blob is somebody else's bad bytes and says nothing
     * against this kernel; a corrupt kernel descriptor says the measuring party
     * itself cannot be trusted to produce a golden.
     *
     * Enrollment currently REFUSES BOTH, so do NOT document or rely on a
     * behavioral difference here; there is none today. The refusal does NOT come
     * from "the identity check always runs", and a caller must not infer that
     * it did. The boundary is reaching the IDENTITY
     * CHECK, which is not the same as reaching tpm_baseline_snapshot. Three
     * kinds of exit refuse without ever looking: a CONFIGURATION refusal when
     * an authority guard rejects the path before snapshot (AUTHREQ from the
     * unauthenticated entry point on a provisioned machine, NOAUTH from the
     * bound one on an unprovisioned machine), BADARG
     * on a missing or malformed grant before snapshot, and BADARG INSIDE
     * snapshot on a NULL output or an unsupported `alg`, which is rejected
     * ahead of the identity check. Safe in all cases, but only a call that
     * reaches the check DIAGNOSES the corruption, so the ABSENCE of this status
     * carries no assurance. The routing must stay
     * separate anyway, because the safety comes entirely from that validation
     * running: anything that weakened or bypassed it would make enrollment
     * unsafe on exactly this value, while the same weakening on CORRUPT would
     * only write a fresh golden over bad stored bytes. See
     * tpm_baseline_status_repair() for what each one tells the operator. */
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
     * The dangerous operation here is AUTO-BINDING, not re-enrollment, and
     * conflating the two hides what must never happen. Plain enrollment refuses
     * outright on a machine whose authority is provisioned, and an authorized
     * bound enrollment REPLACES the blob with freshly snapshotted live state
     * and binds those new bytes -- neither one authenticates the pre-existing
     * blob. Only an auto-bind, or a migration that trusted what is already
     * stored, would stamp an attacker's well-formed bytes as golden. The repair
     * is the authorized migration owned by the versioned baseline growth and NV
     * index migration work, which is not yet available -- naming the direction
     * is the point, not prescribing a step. */
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
     * The overload it was split out of is now FULLY resolved: TPM_NV_UNAVAIL
     * maps to TPM_BASELINE_NOAUTH, both enroll entry points return NOAUTH or
     * TPM_BASELINE_AUTHREQ, and UNBOUND now means one thing only -- a legacy
     * blob carrying no bind record on the VERIFY path. */
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
    /* The enrolled index answered and passed its identity contract, and the
     * RECORD BYTES it holds do not parse. Published as a MISMATCH exactly as
     * IDENTITY is -- corrupt persisted authentication state is not a clean
     * boot -- but named separately because the two send an operator to
     * different places: IDENTITY says the index answering is not the enrolled
     * one, while this says the enrolled one is answering with rubbish.
     *
     * It was IDENTITY until section 29, and the verdict was right while the
     * diagnosis was not: an operator following the IDENTITY repair went looking
     * for a substituted or redefined index that was never there. */
    TPM_BASELINE_RECORD = 12,
    /* NO UPDATE AUTHORITY IS PROVISIONED, and an operation that requires one
     * was asked for. A CONFIGURATION state, not a verdict about the machine:
     * nothing was measured, nothing failed to match, and the boot must not
     * publish anything for it.
     *
     * Split out of UNBOUND, which was carrying this along with a real verify
     * verdict. The collapse was load-bearing in the wrong direction: because
     * UNBOUND is a published integrity failure, the boot's enroll call site had
     * to special-case it by name to avoid reporting tamper on a healthy machine
     * whose enroll merely refused, and that exception is exactly the kind of
     * hand-maintained exclusion this file removes elsewhere. */
    TPM_BASELINE_NOAUTH = 13,
    /* AN UPDATE AUTHORITY IS PROVISIONED, so the UNAUTHENTICATED enroll entry
     * point refuses: it writes the blob under owner auth and nothing else, so
     * completing it would leave the bind record describing the PREVIOUS blob
     * and the very next verify would report RELABELED -- the bind record is
     * still there and still CURRENT, and it now describes bytes that are gone,
     * which is exactly the relabel shape. NOT UNBOUND: that means no bind
     * record at all. The authorized path is
     * tpm_baseline_enroll_bound.
     *
     * The exact OPPOSITE configuration to NOAUTH, and both returned UNBOUND
     * before section 29 -- one status for two states whose operator actions are
     * "install an authority" and "use the authorized path". */
    TPM_BASELINE_AUTHREQ = 14,
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

/* The DIRECTION-SPECIFIC repair for a pairing that is not CURRENT, or NULL when
 * the pairing names no repair (CURRENT, or a pairing that was never computed).
 *
 * This is the half tpm_baseline_status_repair cannot give: TPM_BASELINE_TORN
 * deliberately collapses three directions whose repairs are opposite, so its
 * line says only the one thing they share. Completing an interrupted commit and
 * entering authorized recovery are different actions, and prescribing recovery
 * for an uncommitted write is destructive over-recovery -- it overwrites an
 * authentic record whose commit increment merely failed. A caller that has the
 * direction reports BOTH lines; a caller that does not still gets the shared
 * one, unchanged. Pure. */
const char *tpm_baseline_pairing_repair(tpm_pairing_t pairing);

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
 * USABLE ON BOTH PATHS, and TPM_BASELINE_UNBOUND is why it once was not: the
 * enroll entry points used to return that same value as an ordinary
 * configuration refusal, so an enroll-side caller had to exclude it by name or
 * publish tamper on a healthy machine. They now return TPM_BASELINE_AUTHREQ and
 * TPM_BASELINE_NOAUTH, neither of which this predicate classes as a failure, so
 * that exclusion was deleted and both paths use the rule unmodified. Adding a
 * configuration state to this predicate would reintroduce the whole problem.
 * Pure. */
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

/* tpm_baseline_compare plus FIRST-MISMATCH attribution. Identical verdict and
 * identical comparison order -- tpm_baseline_compare is a wrapper over this one
 * passing NULL, so the two can never drift.
 *
 * `out_cause` is optional and FAIL-CLOSED when supplied: it is fully written on
 * every return, MATCH and BADARG included (NONE / BADARG respectively), so it
 * never carries a stale value from a previous call. `pcr_valid` is 1 only for
 * the two PCR causes, and then `pcr_index` is the golden slot's register
 * number. Read the tpm_baseline_cause_t comment before rendering it: this names
 * the first difference, never all of them. Pure. */
tpm_baseline_verdict_t tpm_baseline_compare_detail(const struct tpm_baseline *golden,
                                                   const struct tpm_baseline *current,
                                                   struct tpm_baseline_mismatch *out_cause);

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
 * Returns OK; NO_TPM when the full measured PCR set is not readable; or
 * SELF_CORRUPT when the kernel's own `.bootproto` ABI digest fails validation.
 * SELF_CORRUPT and not CORRUPT, which this contract said for a while and which
 * is the one confusion that matters here: CORRUPT is about the STORED blob and
 * routes to stored-blob recovery, while this is about THIS KERNEL and must not.
 * It is fail-closed by design -- that digest is a compile-time constant, so its
 * absence means read-only kernel data is corrupt. A call that reaches the
 * identity check therefore refuses before any baseline is written, rather than
 * producing a golden that binds nothing. The check is NOT the first thing this
 * function does -- a NULL output or an unsupported `alg` is BADARG ahead of it
 * -- and callers can exit earlier still, so the ABSENCE of this status is never
 * evidence that the identity was verified. */
tpm_baseline_status_t tpm_baseline_snapshot(uint16_t alg, struct tpm_baseline *out);

/* Enroll: snapshot the current state, stamp a monotonic generation (the existing
 * baseline's generation + 1, or 1 for a first enroll, so a rotation never rolls
 * the counter backward), finalize, and store the blob in the owner-auth NV DATA
 * index `nv_index` (define-if-absent then write). RECOVERY-GATED -- callers must
 * check the recovery + enroll config gate first.
 *
 * REFUSES with TPM_BASELINE_AUTHREQ while an update authority is provisioned:
 * this path writes the blob under owner auth and nothing else, so completing it
 * would leave the bind record describing the PREVIOUS blob. AUTHREQ is a
 * CONFIGURATION state and not an integrity failure, so a caller must not
 * publish a verdict for it -- it means "use tpm_baseline_enroll_bound", and on
 * a healthy authority-provisioned machine it is the expected answer. */
tpm_baseline_status_t tpm_baseline_enroll(uint32_t nv_index, uint16_t alg);

/* Enroll or rotate a baseline AND its authenticated bind record, as one
 * operation. This is the only correct path once an update authority is
 * provisioned: tpm_baseline_enroll writes the blob under owner auth and nothing
 * else, so using it after a bind exists would leave the bind describing the
 * PREVIOUS blob, so the next verify would report TPM_BASELINE_RELABELED: the
 * record is present and CURRENT and describes content that no longer matches.
 * (Not UNBOUND, which means no bind record exists at all.) That is
 * why the plain enroll now REFUSES with TPM_BASELINE_AUTHREQ while an
 * authority is installed, rather than quietly breaking the machine it was
 * asked to update.
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
 * Returns TPM_BASELINE_OK, TPM_BASELINE_BADARG on a NULL or malformed
 * transition, TPM_BASELINE_NOAUTH when no authority is provisioned, or
 * whatever the enroll or the authorized bind reports. NOAUTH is the mirror of
 * the plain enroll's AUTHREQ and is equally a configuration state rather than
 * a verdict: the two say "install an authority" and "use the authorized path",
 * and both returned the same overloaded UNBOUND before the split. */
struct tpm_authz_transition;
tpm_baseline_status_t tpm_baseline_enroll_bound(uint32_t nv_index, uint16_t alg,
                                                const struct tpm_authz_transition *tr);

/* Verify: read the blob from `nv_index`, validate it, snapshot the current
 * state, and compare.
 *
 * VERDICT-PRODUCING returns set *out_overall to a BOOT_INTEGRITY_* value and a
 * caller MUST publish it: TPM_BASELINE_OK (VERIFIED on a full match),
 * TPM_BASELINE_NO_BASELINE (the index is undefined or never written),
 * TPM_BASELINE_CORRUPT and TPM_BASELINE_SELF_CORRUPT, and the five authenticity
 * failures TPM_BASELINE_UNBOUND, TPM_BASELINE_TORN, TPM_BASELINE_RELABELED,
 * TPM_BASELINE_IDENTITY and TPM_BASELINE_RECORD (all MISMATCH).
 *
 * NON-VERDICT returns leave *out_overall untouched and MUST NOT be published:
 * TPM_BASELINE_NO_TPM (absent or too slow -- a machine that could not measure
 * has not failed to match), TPM_BASELINE_BUSY (transport contention -- the
 * ONLY one of these that is safe to retry blind, since nothing was
 * submitted), TPM_BASELINE_TPMERR (a device fault that reached no conclusion) and
 * TPM_BASELINE_BADARG.
 *
 * TPM_BASELINE_NOAUTH is deliberately in NEITHER list, because this verifier
 * cannot return it. Every producer of the underlying TPM_NV_UNAVAIL sits behind
 * a `!tpm_authz_provisioned()` guard, and this function consults the bind path
 * only when that same predicate is TRUE, on a set-once authority -- so the one
 * status meaning "no authority installed" is unreachable from here. It is a
 * real return of the ENROLL entry points; listing it here was a stale claim
 * added when the status was, not a path any caller can reach.
 *
 * The distinction is the whole point: publishing a non-verdict reports a false
 * tamper, and DROPPING a verdict leaves a detected attack invisible under
 * whatever the previous phase published. An earlier version of this comment
 * listed only OK, CORRUPT and NO_BASELINE, and every caller written against it
 * would silently discard the authenticity verdicts. It went stale a SECOND
 * time when TPM_BASELINE_RECORD and TPM_BASELINE_NOAUTH were added, which is
 * why tpm_baseline_status_is_failure() is the authority and these lists are a
 * reader's summary of it: a caller that switches on the list rather than
 * calling the predicate will drift again. */
tpm_baseline_status_t tpm_baseline_verify(uint32_t nv_index, uint16_t alg,
                                          uint8_t *out_overall,
                                          uint8_t *out_pcr_status,
                                          uint8_t pcr_cap,
                                          uint8_t *out_pcr_n);

/* tpm_baseline_verify plus the FIRST-MISMATCH attribution from the comparison
 * it performed. Identical behavior in every other respect -- tpm_baseline_verify
 * is a wrapper over this one passing NULL for `out_cause`.
 *
 * `out_cause` is optional and FAIL-CLOSED when supplied: it is fully written
 * before ANY return, so the many paths that never reach a comparison (no
 * transport, no baseline, a corrupt or unauthenticated blob, a device fault)
 * leave NONE rather than a stale cause from a previous boot's call. Only
 * TPM_BASELINE_OK can report a cause other than NONE, because it is the only
 * status reached by actually comparing two baselines: the authenticity
 * failures are MISMATCH verdicts whose reason is the STATUS, not a field.
 *
 * `out_pairing` is optional and FAIL-CLOSED on the same terms: it is set to
 * TPM_PAIRING_BADARG -- "no pairing claim was made" -- before any return, and
 * carries a real direction only where one was computed. It exists because
 * TPM_BASELINE_TORN collapses UNCOMMITTED, TORN and IMPOSSIBLE, whose repairs
 * are opposite; the verifier already computes the direction and used to discard
 * it, leaving a consumer able to log only the one sentence every direction
 * shares. Pass it to tpm_baseline_pairing_repair for the direction-specific
 * line. A NULL here changes nothing else about the call. */
tpm_baseline_status_t tpm_baseline_verify_detail(uint32_t nv_index, uint16_t alg,
                                                 uint8_t *out_overall,
                                                 uint8_t *out_pcr_status,
                                                 uint8_t pcr_cap,
                                                 uint8_t *out_pcr_n,
                                                 struct tpm_baseline_mismatch *out_cause,
                                                 tpm_pairing_t *out_pairing);
