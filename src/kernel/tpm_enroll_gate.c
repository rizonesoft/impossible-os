/* ============================================================================
 * tpm_enroll_gate.c -- The measured-boot enrollment authority decision
 *
 * One pure total predicate. No TPM, no keyboard, no MMIO, no globals --
 * see the trust model in include/kernel/tpm_enroll_gate.h before changing
 * an input or reordering a check.
 *
 * The ORDER of the checks below is deliberate and is part of the contract:
 * the most fundamental precondition refuses first, so the refusal an
 * operator sees names the thing they must fix first rather than an
 * incidental later failure. Config opt-in comes first because a boot that
 * never asked to enroll should say exactly that rather than complaining
 * about Secure Boot.
 *
 * XREF: todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md
 *       "Trusted Enrollment Provenance"
 * ============================================================================ */

#include "kernel/tpm_enroll_gate.h"
#include "kernel/boot_info.h"
#include "kernel/tpm_replay.h"
#include "boot/boot_policy.h"

/* Layer 1 of the 5-layer defense. This gate is a SECURITY decision whose
 * correctness depends on raw enum values produced in another translation
 * unit (the bootloader) and compared here. If any of these is renumbered,
 * a gate comparison silently starts admitting or refusing the wrong boot,
 * and nothing else in the tree would catch it -- the values cross the
 * boot_info ABI as plain uint32. Pin them so a reorder is a build error. */
_Static_assert(BOOT_PATH_RECOVERY == 3,
               "tpm_enroll_gate: BOOT_PATH_RECOVERY renumbered -- re-check the gate");
_Static_assert(BOOT_REASON_RECOVERY_TRIGGER == 9,
               "tpm_enroll_gate: BOOT_REASON_RECOVERY_TRIGGER renumbered -- re-check the gate");
_Static_assert(BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED == (1u << 5),
               "tpm_enroll_gate: RECOVERY_TRIGGERED flag bit moved -- re-check the gate");
_Static_assert(BOOT_SELECTION_RECOVERY_REQUEST == 6,
               "tpm_enroll_gate: BOOT_SELECTION_RECOVERY_REQUEST renumbered -- re-check the gate");
_Static_assert(TPM_REPLAY_TAMPER == 1,
               "tpm_enroll_gate: TPM_REPLAY_TAMPER renumbered -- re-check the gate");
_Static_assert(TPM_REPLAY_UNVERIFIABLE == 2,
               "tpm_enroll_gate: TPM_REPLAY_UNVERIFIABLE renumbered -- re-check the gate");

/* Set the whole result in one place so no path can leave a field stale.
 * `admit` is passed explicitly rather than derived, so the single
 * admitting call site is greppable. */
static void gate_result(struct tpm_enroll_gate_result *out,
                        uint8_t admit, uint8_t needs_confirm,
                        uint8_t authority, uint8_t refusal)
{
    out->admit         = admit;
    out->needs_confirm = needs_confirm;
    out->authority     = authority;
    out->refusal       = refusal;
}

void tpm_enroll_gate_evaluate(const struct tpm_enroll_gate_inputs *in,
                              struct tpm_enroll_gate_result *out)
{
    if (!out) return;
    /* Fail closed on a missing input struct rather than faulting. A caller
     * that cannot describe the boot has not earned an enrollment. */
    if (!in) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_NONE,
                    TPM_ENROLL_REFUSE_CONFIG_DISABLED);
        return;
    }

    /* 1. Did anyone ask to enroll at all? This is ESP-controlled and
     *    therefore never sufficient, but its ABSENCE is decisive: no
     *    opt-in, no enrollment, and no reason to prompt an operator. */
    if (!in->tpm_enroll) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_NONE,
                    TPM_ENROLL_REFUSE_CONFIG_DISABLED);
        return;
    }

    /* From here on the boot has at least asserted the config opt-in, so
     * every remaining refusal reports ESP_CONFIG_ONLY: that is genuinely
     * all the authority it holds, and naming it is what stops a
     * config-only boot from ever being read as trusted. */

    /* 2. Is the code asking the question itself verified? Every field
     *    below is consumed by kernel code loaded by the loader. Without an
     *    authenticated chain an attacker replaces that code, so no answer
     *    binds and a "confirmed" enrollment would be theatre. */
    if (!in->secure_boot_enabled) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED);
        return;
    }
    /* Secure Boot being ON is NOT the same as the running kernel having been
     * verified, and treating it as such was a critical defect in the first
     * version of this gate. On the legacy split path the firmware verifies
     * BOOTX64.EFI and the loader then reads an UNSIGNED kernel.exe off the
     * ESP (src/boot/uefi/bootx64.c:7894-7896, 12281-12283). An ESP attacker
     * who swaps that file gets a kernel that satisfies every other check
     * here -- and, being the kernel, could simply call the enroll path
     * directly. Reporting such a boot as running on a trusted chain is the
     * specific lie this check exists to prevent. */
    if (!in->whole_chain_verified) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED);
        return;
    }

    /* 3. Never enroll a state that is not KNOWN to agree with its own event
     *    log. Enrolling a TAMPER boot would promote the tampered
     *    measurement to golden, the precise disaster the baseline exists to
     *    detect.
     *
     *    This is a WHITELIST and must stay one. An earlier version wrote
     *    `if (replay_known) { refuse TAMPER; refuse UNVERIFIABLE; }`, which
     *    failed OPEN twice over: a boot whose replay never ran at all
     *    (replay_known == 0, exactly what the caller leaves when
     *    tpm_replay_verify() returns a non-OK status) sailed through to the
     *    admitting arm, and so did any out-of-range verdict encoding. Only
     *    an explicit VERIFIED proceeds. */
    if (!in->replay_known) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE);
        return;
    }
    if (in->replay_verdict == TPM_REPLAY_TAMPER) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_REPLAY_TAMPER);
        return;
    }
    if (in->replay_verdict != TPM_REPLAY_VERIFIED) {
        /* UNVERIFIABLE, or any encoding outside the enum. */
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE);
        return;
    }

    /* 4. The NVRAM-backed recovery assertion. boot_info.h requires that a
     *    degraded audit read be treated as all-zero sticky state, so check
     *    it BEFORE the sticky fields -- reading them first would trust
     *    bytes the bootloader already said it could not vouch for. */
    if (in->audit_degraded) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_STICKY_DEGRADED);
        return;
    }
    if (!in->sticky_present) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_STICKY_ABSENT);
        return;
    }
    if (!in->sticky_recovery_trigger) {
        gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                    TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR);
        return;
    }

    /* 5. Coherence of the ladder's published decision record -- CONDITIONAL,
     *    and the condition is the whole point.
     *
     *    The obvious design is to REQUIRE selection_reason ==
     *    RECOVERY_REQUEST. That was the first version and it was wrong in a
     *    way no test caught: BOOT_SELECTION_RECOVERY_REQUEST is assigned at
     *    exactly one place (src/boot/uefi/boot_policy.c:470), inside the
     *    ladder branch that scans for a RECOVERY-kind CANDIDATE -- and the
     *    kind filter (boot_policy.c:230-234) has already rejected every
     *    RECOVERY entry, because supported_kinds_mask admits only SPLIT/SAFE
     *    or UKI (bootx64.c:6335-6358). So the ladder can NEVER publish that
     *    reason today, and requiring it would have made baseline enrollment
     *    impossible on every supported boot path: an insecure capability
     *    replaced by a dead one.
     *
     *    The NVRAM sticky trigger above is already the unforgeable anchor,
     *    and it reaches the kernel independently of what the ladder managed
     *    to do with it (boot_sticky.c:143 writes it straight into boot_info).
     *    So the rule is: the trigger authorizes, and the derived record must
     *    be COHERENT WITH ITSELF where the loader actually produced one.
     *
     *    This gives up nothing against the ESP spoof. A recovery-kind entry
     *    chosen as the store default sets boot_path / boot_reason / the
     *    source flag from the entry KIND, but it cannot set the sticky
     *    trigger -- so it is already refused above, before reaching here. */
    if (in->selection_reason == (uint32_t)BOOT_SELECTION_RECOVERY_REQUEST) {
        /* The loader DID honour the trigger, so its record must line up.
         * An incoherent record here means a loader bug or a tampered
         * handoff, and either way it is not something to enroll under. */
        if (in->boot_path != (uint32_t)BOOT_PATH_RECOVERY) {
            gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                        TPM_ENROLL_REFUSE_PATH_MISMATCH);
            return;
        }
        if (in->boot_reason != (uint32_t)BOOT_REASON_RECOVERY_TRIGGER) {
            gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                        TPM_ENROLL_REFUSE_REASON_MISMATCH);
            return;
        }
        if (!(in->boot_source_flags & BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED)) {
            gate_result(out, 0, 0, TPM_ENROLL_AUTH_ESP_CONFIG_ONLY,
                        TPM_ENROLL_REFUSE_SOURCE_FLAG_MISSING);
            return;
        }
    }
    /* Otherwise the loader could not act on the trigger (today: always).
     * The trigger plus a verified chain plus the operator's keypress still
     * carry the authority, so this is not a refusal -- but note that the
     * reported authority below stays LOADER_SIGNAL_ONLY until that keypress
     * lands, exactly as in the coherent case. */

    /* Every non-operator condition now holds. The boot has a real
     * NVRAM-backed recovery assertion under a verified chain -- which is
     * LOADER_SIGNAL_ONLY, still not enough to write. */
    switch (in->confirm) {
        case TPM_CONFIRM_PENDING:
            /* Caller has not prompted yet. This is the ONLY result that
             * should make a caller put a prompt on the screen, which is
             * what keeps a normal boot silent. */
            gate_result(out, 0, 1, TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY,
                        TPM_ENROLL_REFUSE_NONE);
            return;
        case TPM_CONFIRM_YES:
            /* The single admitting path in this function. */
            gate_result(out, 1, 0,
                        TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN,
                        TPM_ENROLL_REFUSE_NONE);
            return;
        case TPM_CONFIRM_TIMEOUT:
            gate_result(out, 0, 0, TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY,
                        TPM_ENROLL_REFUSE_CONFIRM_TIMEOUT);
            return;
        case TPM_CONFIRM_UNAVAILABLE:
            gate_result(out, 0, 0, TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY,
                        TPM_ENROLL_REFUSE_CONFIRM_UNAVAILABLE);
            return;
        case TPM_CONFIRM_WRONG_KEY:
        default:
            /* An unrecognized confirm value is treated as a decline, not
             * as an error: totality with no admitting default is the
             * property that makes this gate fail closed. */
            gate_result(out, 0, 0, TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY,
                        TPM_ENROLL_REFUSE_CONFIRM_WRONG_KEY);
            return;
    }
}

const char *tpm_enroll_refusal_label(uint8_t refusal)
{
    switch (refusal) {
        case TPM_ENROLL_REFUSE_NONE:                 return "none";
        case TPM_ENROLL_REFUSE_CONFIG_DISABLED:      return "config-disabled";
        case TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED: return "boot-chain-untrusted";
        case TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED:    return "kernel-unverified";
        case TPM_ENROLL_REFUSE_REPLAY_TAMPER:        return "replay-tamper";
        case TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE:  return "replay-unverifiable";
        case TPM_ENROLL_REFUSE_STICKY_DEGRADED:      return "sticky-degraded";
        case TPM_ENROLL_REFUSE_STICKY_ABSENT:        return "sticky-absent";
        case TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR: return "sticky-trigger-clear";
        case TPM_ENROLL_REFUSE_PATH_MISMATCH:        return "path-mismatch";
        case TPM_ENROLL_REFUSE_REASON_MISMATCH:      return "reason-mismatch";
        case TPM_ENROLL_REFUSE_SOURCE_FLAG_MISSING:  return "source-flag-missing";
        case TPM_ENROLL_REFUSE_CONFIRM_TIMEOUT:      return "confirm-timeout";
        case TPM_ENROLL_REFUSE_CONFIRM_WRONG_KEY:    return "confirm-wrong-key";
        case TPM_ENROLL_REFUSE_CONFIRM_UNAVAILABLE:  return "confirm-unavailable";
        default:                                     return "unknown";
    }
}

const char *tpm_enroll_authority_label(uint8_t authority)
{
    switch (authority) {
        case TPM_ENROLL_AUTH_NONE:               return "none";
        case TPM_ENROLL_AUTH_ESP_CONFIG_ONLY:    return "esp-config-only";
        case TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY: return "loader-signal-only";
        case TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN:
                                                 return "local-console-on-trusted-chain";
        default:                                 return "unknown";
    }
}
