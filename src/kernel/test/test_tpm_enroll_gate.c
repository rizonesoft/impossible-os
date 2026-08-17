/* ============================================================================
 * test_tpm_enroll_gate.c -- The measured-boot enrollment authority matrix
 *
 * tpm_enroll_gate_evaluate() is a PURE TOTAL predicate over a value struct,
 * which is exactly why the whole refusal matrix is provable here with no
 * TPM, no keyboard, and no boot infrastructure -- the dev host has no
 * swtpm, so a design that hid this decision behind live I/O would have left
 * the security-critical part untested.
 *
 * The assertions below are written as ATTACKS where possible: each one
 * builds the tuple an attacker would need and asserts the refusal, rather
 * than only confirming the happy path.
 *
 * XREF: todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md
 *       "Trusted Enrollment Provenance"
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm_enroll_gate.h"
#include "kernel/tpm_replay.h"
#include "kernel/boot_info.h"
#include "boot/boot_policy.h"
#include "libc/string.h"

/* The tuple of a boot that SHOULD be admitted once confirmed: opt-in set,
 * Secure Boot active, replay clean, a trusted NVRAM-backed recovery
 * request, and a coherent decision record. Every test below starts from
 * this and breaks exactly ONE thing, so a failure names its own cause. */
static void gate_good_inputs(struct tpm_enroll_gate_inputs *in)
{
    in->boot_path               = BOOT_PATH_RECOVERY;
    in->boot_reason             = BOOT_REASON_RECOVERY_TRIGGER;
    in->boot_source_flags       = BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED;
    in->selection_reason        = BOOT_SELECTION_RECOVERY_REQUEST;
    in->audit_degraded          = 0;
    in->sticky_present          = 1;
    in->sticky_recovery_trigger = 1;
    in->secure_boot_enabled     = 1;
    in->whole_chain_verified    = 1;
    in->replay_known            = 1;
    in->replay_verdict          = TPM_REPLAY_VERIFIED;
    in->tpm_enroll              = 1;
    in->confirm                 = TPM_CONFIRM_YES;
}

/* The whole point of the section: a confirmed, loader-signalled, verified
 * boot is the ONLY thing that may write the baseline. */
static void test_enroll_gate_admits_confirmed_recovery(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 1, "confirmed loader-signalled recovery must admit");
    TEST_ASSERT_EQ(r.needs_confirm, 0, "already confirmed: must not ask again");
    TEST_ASSERT_EQ(r.authority, TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN,
                   "admitted enroll must report console-on-trusted-chain authority");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_NONE,
                   "an admitted enroll carries no refusal reason");
}

/* THE ATTACK THE SECTION EXISTS TO STOP. An attacker with ESP write access
 * sets tpm_enroll in boot.conf and, if the loader ever admits recovery-kind
 * entries, makes a recovery entry the store default -- which would set
 * boot_path, boot_reason and the RECOVERY_TRIGGERED source flag, because
 * all three are derived from the selected entry's kind. The NVRAM sticky
 * trigger is the one thing an ESP writer cannot forge, so the gate must
 * refuse on it even when every derived field looks perfect. */
static void test_enroll_gate_refuses_esp_spoofed_recovery(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    /* Everything the entry KIND would set stays "correct"... */
    in.sticky_present          = 0;  /* ...but no NVRAM record backs it */
    in.sticky_recovery_trigger = 0;
    in.selection_reason        = BOOT_SELECTION_STORE_DEFAULT;
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 0, "ESP-spoofed recovery must never admit");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_ABSENT,
                   "spoofed recovery refuses on the absent NVRAM sticky record");
    TEST_ASSERT_EQ(r.authority, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                   "a config-only boot must NEVER be reported as trusted");
}

/* The REVERSE spoof: a tampered handoff (or a loader bug) publishes a full
 * RECOVERY_REQUEST selection with a perfectly coherent decision record, but
 * no NVRAM sticky record backs it. The ladder's reason is published through
 * the same boot_info the rest of the record travels in, so it is only as
 * trustworthy as the handoff; the sticky read is the independent fact. */
static void test_enroll_gate_refuses_recovery_selection_without_sticky(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.sticky_present          = 0;
    in.sticky_recovery_trigger = 0;
    /* selection_reason stays RECOVERY_REQUEST and the record stays coherent */
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 0,
                   "a claimed recovery-request selection with no NVRAM record "
                   "must not authorize an enroll");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_ABSENT,
                   "refuses on the absent sticky record, not the selection");

    /* A sticky record that exists but never asked for recovery is the same
     * refusal class, reported distinctly so an operator can tell them apart. */
    gate_good_inputs(&in);
    in.sticky_recovery_trigger = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "a present-but-clear trigger must refuse");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR,
                   "present-but-clear is distinct from absent");
}

/* boot_info.h REQUIRES consumers to treat every sticky_* field as zero when
 * audit_degraded is set. A gate that read the fields anyway would trust
 * bytes the bootloader explicitly said it could not vouch for. */
static void test_enroll_gate_refuses_degraded_sticky_audit(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.audit_degraded = 1;  /* sticky_present/trigger still say 1 */
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 0, "degraded sticky audit must refuse");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_DEGRADED,
                   "degraded audit refuses BEFORE reading the sticky fields");
}

/* Every predicate input is consumed by kernel code that the loader loaded.
 * Without a verified chain an attacker replaces that code, so a "confirmed"
 * enrollment proves nothing. */
static void test_enroll_gate_refuses_untrusted_boot_chain(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.secure_boot_enabled = 0;
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 0, "no Secure Boot means no trusted enrollment");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED,
                   "must refuse naming the untrusted chain");
    TEST_ASSERT_EQ(r.authority, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                   "an unverified chain holds only config authority");
}

/* Enrolling a boot whose event log already disagrees with the hardware PCRs
 * would promote the tampered measurement to golden -- the exact disaster
 * the baseline exists to detect. */
static void test_enroll_gate_refuses_replay_tamper(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.replay_verdict = TPM_REPLAY_TAMPER;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "a TAMPER boot must never become the baseline");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_REPLAY_TAMPER,
                   "TAMPER refusal must be named distinctly");

    /* UNVERIFIABLE means no hardware PCR was readable: there is nothing to
     * enroll and no way to check it, so it refuses too rather than
     * enrolling an unmeasured state. */
    in.replay_verdict = TPM_REPLAY_UNVERIFIABLE;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "an unverifiable replay must not enroll");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE,
                   "UNVERIFIABLE is a distinct refusal from TAMPER");
}

/* REGRESSION: the replay check must be a WHITELIST. An earlier version only
 * looked at the verdict when replay_known was set, so a boot whose replay
 * never ran -- exactly what the caller leaves when tpm_replay_verify()
 * returns a non-OK status -- reached the admitting arm with an otherwise
 * perfect tuple. An out-of-range verdict encoding did the same. */
static void test_enroll_gate_replay_must_be_whitelisted(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.replay_known = 0;  /* replay never ran; verdict field meaningless */
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "no replay verdict must NEVER admit");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE,
                   "an absent replay verdict refuses as unverifiable");

    /* An encoding outside tpm_replay_verdict_t must refuse, not fall
     * through as though it were clean. */
    gate_good_inputs(&in);
    in.replay_verdict = 200;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "an out-of-range replay verdict must not admit");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE,
                   "an unknown verdict encoding refuses as unverifiable");
}

/* REACHABILITY. The NVRAM sticky trigger is the anchor, and it reaches the
 * kernel whether or not the ladder could act on it -- today it never can,
 * because every RECOVERY entry is filtered before the recovery-request
 * branch runs. An earlier version REQUIRED selection_reason ==
 * RECOVERY_REQUEST, which made enrollment impossible on every supported
 * boot path: an insecure capability replaced by a dead one. */
static void test_enroll_gate_admits_when_loader_cannot_honor_trigger(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    /* What today's loader actually publishes: a real NVRAM recovery
     * trigger, but a ladder that fell through to the store default and a
     * decision record describing a normal boot. */
    in.selection_reason  = BOOT_SELECTION_STORE_DEFAULT;
    in.boot_path         = BOOT_PATH_NORMAL;
    in.boot_reason       = BOOT_REASON_NORMAL;
    in.boot_source_flags = 0;
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 1,
                   "a confirmed boot with a real NVRAM trigger must admit "
                   "even when the loader could not act on it");
    TEST_ASSERT_EQ(r.authority, TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN,
                   "the keypress plus the NVRAM trigger carry the authority");

    /* And the spoof STILL fails: the same non-recovery record without the
     * NVRAM trigger is refused, which is what makes relaxing the selection
     * requirement safe rather than a hole. */
    in.sticky_recovery_trigger = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "no NVRAM trigger must still refuse");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR,
                   "the sticky trigger remains the anchor");
}


/* A boot that never opted in is refused first and, critically, is NOT asked
 * to confirm -- that is what keeps every ordinary boot silent. */
static void test_enroll_gate_refuses_without_config_optin(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.tpm_enroll = 0;
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 0, "no opt-in, no enrollment");
    TEST_ASSERT_EQ(r.needs_confirm, 0, "a normal boot must never draw a prompt");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_CONFIG_DISABLED,
                   "must refuse naming the missing opt-in");
    TEST_ASSERT_EQ(r.authority, TPM_ENROLL_AUTH_NONE,
                   "a boot that never asked holds no authority at all");
}

/* needs_confirm is the ONLY signal that should put a prompt on screen, and
 * it must appear only after every non-operator condition already passed. */
static void test_enroll_gate_requests_confirmation_once_eligible(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.confirm = TPM_CONFIRM_PENDING;
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.needs_confirm, 1, "an eligible boot must ask the operator");
    TEST_ASSERT_EQ(r.admit, 0, "pending confirmation must not admit");
    TEST_ASSERT_EQ(r.authority, TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY,
                   "loader signal alone is not console authority");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_NONE,
                   "awaiting an answer is not yet a refusal");

    /* An INELIGIBLE boot must never reach the prompt, however it failed. */
    in.sticky_recovery_trigger = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.needs_confirm, 0,
                   "an ineligible boot must never draw a prompt");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR,
                   "ineligible-at-sticky refuses on the trigger, not on confirm");
}

/* Every non-affirmative confirmation outcome is a distinct, fail-closed
 * refusal. A timeout on a headless machine and an operator pressing the
 * wrong key are different events and must not collapse into one boolean. */
static void test_enroll_gate_confirmation_outcomes_are_distinct(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);

    in.confirm = TPM_CONFIRM_TIMEOUT;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "a timeout must fail closed");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_CONFIRM_TIMEOUT, "timeout is distinct");

    in.confirm = TPM_CONFIRM_WRONG_KEY;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "a decline must fail closed");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_CONFIRM_WRONG_KEY, "decline is distinct");

    in.confirm = TPM_CONFIRM_UNAVAILABLE;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "no console must fail closed, never silently pass");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_CONFIRM_UNAVAILABLE,
                   "unavailable console is distinct from a timeout");

    /* Totality: an out-of-range confirm value must be read as a decline,
     * never as an admit. This is the property that makes the switch's
     * default arm fail closed rather than fall through. */
    in.confirm = (uint8_t)(TPM_CONFIRM_MAX + 7);
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "an unknown confirm value must never admit");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_CONFIRM_WRONG_KEY,
                   "an unknown confirm value is treated as a decline");
}

/* The derived decision-record fields are a coherence check, not authority.
 * Each is asserted separately so a future edit that drops one is caught. */
static void test_enroll_gate_refuses_incoherent_decision_record(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.boot_path = BOOT_PATH_NORMAL;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "a non-recovery boot_path must refuse");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_PATH_MISMATCH, "path mismatch named");

    gate_good_inputs(&in);
    in.boot_reason = BOOT_REASON_NORMAL;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "a non-recovery boot_reason must refuse");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_REASON_MISMATCH, "reason mismatch named");

    gate_good_inputs(&in);
    in.boot_source_flags = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.admit, 0, "a missing RECOVERY_TRIGGERED flag must refuse");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_SOURCE_FLAG_MISSING, "flag miss named");

    /* Other source flags being set must not mask the missing one. */
    gate_good_inputs(&in);
    in.boot_source_flags = BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED << 1;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_SOURCE_FLAG_MISSING,
                   "an unrelated source flag must not satisfy the recovery bit");
}

/* A NULL input struct is a caller that cannot describe the boot. It must
 * fail closed rather than fault, because this runs in Phase 1 where a
 * fault is a silent halt. */
static void test_enroll_gate_null_inputs_fail_closed(void)
{
    struct tpm_enroll_gate_result r;

    r.admit = 1;  /* poison: prove the callee overwrote it */
    r.needs_confirm = 1;
    tpm_enroll_gate_evaluate((const struct tpm_enroll_gate_inputs *)0, &r);

    TEST_ASSERT_EQ(r.admit, 0, "NULL inputs must fail closed, not admit");
    TEST_ASSERT_EQ(r.needs_confirm, 0, "NULL inputs must not request a prompt");
    TEST_ASSERT_EQ(r.authority, TPM_ENROLL_AUTH_NONE, "NULL inputs hold no authority");

    /* A NULL result pointer must simply return rather than fault. */
    tpm_enroll_gate_evaluate((const struct tpm_enroll_gate_inputs *)0,
                             (struct tpm_enroll_gate_result *)0);
}

/* Labels back the operator-facing log line, so an unknown value must not
 * return NULL into a %s. */
static void test_enroll_gate_labels_are_total(void)
{
    uint32_t i;

    for (i = 0; i <= TPM_ENROLL_REFUSE_MAX + 3u; i++)
        TEST_ASSERT(tpm_enroll_refusal_label((uint8_t)i) != (const char *)0,
                    "every refusal label must be non-NULL");
    for (i = 0; i <= TPM_ENROLL_AUTH_MAX + 3u; i++)
        TEST_ASSERT(tpm_enroll_authority_label((uint8_t)i) != (const char *)0,
                    "every authority label must be non-NULL");

    /* The out-of-range answer is specifically "unknown", not a stale
     * neighbour's label -- a silent misattribution in a security log is
     * worse than an explicit unknown. */
    TEST_ASSERT_EQ(strcmp(tpm_enroll_refusal_label((uint8_t)(TPM_ENROLL_REFUSE_MAX + 1)),
                          "unknown"), 0,
                   "out-of-range refusal label must be 'unknown'");
    TEST_ASSERT_EQ(strcmp(tpm_enroll_authority_label((uint8_t)(TPM_ENROLL_AUTH_MAX + 1)),
                          "unknown"), 0,
                   "out-of-range authority label must be 'unknown'");

    /* And a VALID value must NOT be "unknown" -- without this control the
     * assertions above would still pass if every label returned "unknown". */
    TEST_ASSERT_NEQ(strcmp(tpm_enroll_refusal_label(TPM_ENROLL_REFUSE_REPLAY_TAMPER),
                           "unknown"), 0,
                    "a valid refusal must not report as unknown");
    TEST_ASSERT_NEQ(strcmp(tpm_enroll_authority_label(
                               TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN),
                           "unknown"), 0,
                    "a valid authority must not report as unknown");
}

/* CRITICAL REGRESSION. Secure Boot being ON does not mean the RUNNING KERNEL
 * was covered by it. On the legacy split path the firmware verifies
 * BOOTX64.EFI and the loader then reads an UNSIGNED kernel.exe off the ESP
 * (src/boot/uefi/bootx64.c:7894-7896, 12281-12283). The first version of
 * this gate checked only secure_boot_enabled and would have stamped such a
 * boot "local-console-on-trusted-chain" -- the exact lie the authority value
 * exists to prevent. */
static void test_enroll_gate_requires_verified_kernel(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    gate_good_inputs(&in);
    in.secure_boot_enabled  = 1;  /* firmware verified the LOADER... */
    in.whole_chain_verified = 0;  /* ...but not the kernel it then loaded */
    tpm_enroll_gate_evaluate(&in, &r);

    TEST_ASSERT_EQ(r.admit, 0,
                   "Secure Boot alone must not authorize an enroll when the "
                   "running kernel was never verified");
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED,
                   "an unverified kernel refuses distinctly from Secure Boot off");
    TEST_ASSERT_NEQ(r.authority, TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN,
                    "an unverified kernel must NEVER report a trusted chain");

    /* Secure Boot off is still its own, differently-remedied refusal. */
    gate_good_inputs(&in);
    in.secure_boot_enabled = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED,
                   "Secure Boot off stays distinct from kernel-unverified");
}

/* PRECEDENCE. The gate documents its check ORDER as part of the contract:
 * the first failing precondition is the one reported, so an operator is told
 * the thing they must fix FIRST rather than an incidental later failure.
 *
 * Single-fault fixtures cannot pin that. Break only one condition and
 * swapping any two adjacent checks still yields the same answer everywhere,
 * so the documented diagnostic contract could regress silently. Each row
 * below fails TWO conditions at once and asserts the EARLIER refusal wins. */
static void test_enroll_gate_refusal_precedence_is_pinned(void)
{
    struct tpm_enroll_gate_inputs in;
    struct tpm_enroll_gate_result r;

    /* config before secure boot */
    gate_good_inputs(&in);
    in.tpm_enroll = 0; in.secure_boot_enabled = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_CONFIG_DISABLED,
                   "config opt-in is reported before Secure Boot");

    /* secure boot before whole-chain */
    gate_good_inputs(&in);
    in.secure_boot_enabled = 0; in.whole_chain_verified = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED,
                   "Secure Boot is reported before whole-chain coverage");

    /* whole-chain before replay */
    gate_good_inputs(&in);
    in.whole_chain_verified = 0; in.replay_known = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED,
                   "kernel verification is reported before the replay verdict");

    /* replay before audit-degraded */
    gate_good_inputs(&in);
    in.replay_verdict = TPM_REPLAY_TAMPER; in.audit_degraded = 1;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_REPLAY_TAMPER,
                   "replay tamper is reported before a degraded sticky audit");

    /* audit-degraded before sticky-present */
    gate_good_inputs(&in);
    in.audit_degraded = 1; in.sticky_present = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_DEGRADED,
                   "a degraded audit is reported before an absent sticky record");

    /* sticky-present before sticky-trigger */
    gate_good_inputs(&in);
    in.sticky_present = 0; in.sticky_recovery_trigger = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_ABSENT,
                   "an absent record is reported before a clear trigger");

    /* sticky-trigger before coherence */
    gate_good_inputs(&in);
    in.sticky_recovery_trigger = 0; in.boot_path = BOOT_PATH_NORMAL;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR,
                   "the sticky trigger is reported before record coherence");

    /* eligibility before confirmation: a refused precondition must never be
     * reported as a confirmation problem, however the operator answered. */
    gate_good_inputs(&in);
    in.sticky_recovery_trigger = 0; in.confirm = TPM_CONFIRM_TIMEOUT;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR,
                   "eligibility is reported before the confirmation outcome");

    /* coherence checks are themselves ordered: path, then reason, then flag */
    gate_good_inputs(&in);
    in.boot_path = BOOT_PATH_NORMAL; in.boot_reason = BOOT_REASON_NORMAL;
    in.boot_source_flags = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_PATH_MISMATCH,
                   "path mismatch is reported before reason and source flag");

    gate_good_inputs(&in);
    in.boot_reason = BOOT_REASON_NORMAL; in.boot_source_flags = 0;
    tpm_enroll_gate_evaluate(&in, &r);
    TEST_ASSERT_EQ(r.refusal, TPM_ENROLL_REFUSE_REASON_MISMATCH,
                   "reason mismatch is reported before the source flag");
}

/* EXHAUSTIVE label mapping. The previous label test only asserted non-NULL
 * plus one valid value not equal to "unknown", which an implementation
 * returning ONE arbitrary string for every valid value would pass -- and
 * that implementation would make every security-log refusal indistinguishable
 * while the suite stayed green. Distinct refusals are the whole point, so
 * each value is pinned to its exact text. */
static void test_enroll_gate_labels_map_exactly(void)
{
    static const struct { uint8_t v; const char *s; } refusals[] = {
        { TPM_ENROLL_REFUSE_NONE,                 "none" },
        { TPM_ENROLL_REFUSE_CONFIG_DISABLED,      "config-disabled" },
        { TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED, "boot-chain-untrusted" },
        { TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED,    "kernel-unverified" },
        { TPM_ENROLL_REFUSE_REPLAY_TAMPER,        "replay-tamper" },
        { TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE,  "replay-unverifiable" },
        { TPM_ENROLL_REFUSE_STICKY_DEGRADED,      "sticky-degraded" },
        { TPM_ENROLL_REFUSE_STICKY_ABSENT,        "sticky-absent" },
        { TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR, "sticky-trigger-clear" },
        { TPM_ENROLL_REFUSE_PATH_MISMATCH,        "path-mismatch" },
        { TPM_ENROLL_REFUSE_REASON_MISMATCH,      "reason-mismatch" },
        { TPM_ENROLL_REFUSE_SOURCE_FLAG_MISSING,  "source-flag-missing" },
        { TPM_ENROLL_REFUSE_CONFIRM_TIMEOUT,      "confirm-timeout" },
        { TPM_ENROLL_REFUSE_CONFIRM_WRONG_KEY,    "confirm-wrong-key" },
        { TPM_ENROLL_REFUSE_CONFIRM_UNAVAILABLE,  "confirm-unavailable" },
    };
    static const struct { uint8_t v; const char *s; } auths[] = {
        { TPM_ENROLL_AUTH_NONE,               "none" },
        { TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,    "esp-config-only" },
        { TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY, "loader-signal-only" },
        { TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN,
          "local-console-on-trusted-chain" },
    };
    uint32_t i;

    for (i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++)
        TEST_ASSERT_EQ(strcmp(tpm_enroll_refusal_label(refusals[i].v),
                              refusals[i].s), 0,
                       "each refusal maps to its own exact label");
    for (i = 0; i < sizeof(auths) / sizeof(auths[0]); i++)
        TEST_ASSERT_EQ(strcmp(tpm_enroll_authority_label(auths[i].v),
                              auths[i].s), 0,
                       "each authority maps to its own exact label");
}

void test_register_tpm_enroll_gate(void)
{
    test_suite_register_cat("tpm: enroll gate admits confirmed recovery",
        test_enroll_gate_admits_confirmed_recovery, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refuses ESP-spoofed recovery",
        test_enroll_gate_refuses_esp_spoofed_recovery, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refuses recovery selection without sticky",
        test_enroll_gate_refuses_recovery_selection_without_sticky, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate replay must be whitelisted",
        test_enroll_gate_replay_must_be_whitelisted, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate admits when loader cannot honor trigger",
        test_enroll_gate_admits_when_loader_cannot_honor_trigger, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refuses degraded sticky audit",
        test_enroll_gate_refuses_degraded_sticky_audit, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate requires verified kernel",
        test_enroll_gate_requires_verified_kernel, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refuses untrusted boot chain",
        test_enroll_gate_refuses_untrusted_boot_chain, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refuses replay tamper",
        test_enroll_gate_refuses_replay_tamper, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refuses without config opt-in",
        test_enroll_gate_refuses_without_config_optin, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate requests confirmation once eligible",
        test_enroll_gate_requests_confirmation_once_eligible, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate confirmation outcomes distinct",
        test_enroll_gate_confirmation_outcomes_are_distinct, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refuses incoherent decision record",
        test_enroll_gate_refuses_incoherent_decision_record, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate NULL inputs fail closed",
        test_enroll_gate_null_inputs_fail_closed, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate refusal precedence is pinned",
        test_enroll_gate_refusal_precedence_is_pinned, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate labels map exactly",
        test_enroll_gate_labels_map_exactly, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: enroll gate labels are total",
        test_enroll_gate_labels_are_total, TEST_CAT_SECURITY);
}
