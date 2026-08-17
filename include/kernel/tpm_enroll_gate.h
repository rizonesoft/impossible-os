/* ============================================================================
 * tpm_enroll_gate.h -- Who is allowed to write the measured-boot baseline
 *
 * The measured-boot baseline is the golden record every later boot is
 * compared against, so whoever can enroll it decides what "untampered"
 * means. Until this gate existed the answer was `boot_mode == 2 &&
 * tpm_enroll`, both parsed verbatim from boot.conf on the ESP -- so ESP
 * write authority alone was enough to make a tampered boot the baseline.
 *
 * This header exposes that decision as ONE PURE TOTAL PREDICATE over a
 * value struct. Nothing here touches a TPM, a keyboard, or an MMIO
 * register, which is what lets the whole refusal matrix be unit-tested
 * without a live TPM (the dev host has no swtpm) and what keeps the
 * security decision separate from the I/O that feeds it.
 *
 * TRUST MODEL -- read before changing an input.
 *
 *   ESP-controlled (an attacker with ESP write access sets these):
 *     boot.conf `tpm_enroll`, and the whole boot-entries store at
 *     \EFI\ImpossibleOS\bootentries.json. Entry KIND drives boot_path /
 *     boot_reason / boot_source_flags through boot_policy_kind_to_path()
 *     (src/boot/uefi/boot_policy.c:268-296), so those three fields are
 *     NOT self-authorizing. A recovery-kind entry cannot be selected in
 *     today's loader (supported_kinds_mask admits SPLIT/SAFE or UKI only,
 *     src/boot/uefi/bootx64.c:6335-6358, filtered at boot_policy.c:230-234),
 *     but that mask is explicitly documented to widen as owner sections
 *     ship, so this gate must not depend on the kind staying unreachable.
 *
 *   NVRAM-controlled (an ESP file writer canNOT set these):
 *     sticky_present / sticky_recovery_trigger, read from the UEFI
 *     variable ImpossibleOS-BootSticky (src/boot/uefi/bootx64.c:6381-6385),
 *     and the ladder's selection_reason, which is
 *     BOOT_SELECTION_RECOVERY_REQUEST only when that NVRAM trigger drove
 *     the choice (src/boot/uefi/boot_policy.c:454-473). audit_degraded == 1
 *     means the bootloader could not trust the sticky read at all, and
 *     boot_info.h requires consumers to treat every sticky_* field as zero
 *     in that case -- this gate does, with a distinct refusal.
 *
 *   Physically present operator:
 *     an affirmative keypress through boot_confirm.h. This is the factor
 *     no remote or file-level attacker holds, which is why it is REQUIRED
 *     rather than advisory.
 *
 * The authenticated-chain condition is why `secure_boot_enabled` AND
 * `whole_chain_verified` are both inputs: every field above is consumed by
 * kernel code, so if the RUNNING KERNEL is not itself verified an attacker
 * replaces the code that asks the question and none of the answers bind.
 * Secure Boot alone does NOT establish that -- on the split path it vouches
 * for BOOTX64.EFI only and kernel.exe is an unsigned ESP file -- which is
 * why a second input exists rather than one flag doing double duty. Console
 * confirmation is therefore reported as LOCAL_CONSOLE_ON_TRUSTED_CHAIN,
 * never as proof of physical presence -- firmware, a hypervisor, or a BMC
 * KVM can inject a keypress. True physical presence needs the TCG
 * Physical Presence Interface, tracked separately.
 *
 * XREF: todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md
 *       "Trusted Enrollment Provenance"
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Where the authority to write a baseline actually came from. Reported
 * alongside every verdict so an operator can tell a console-confirmed
 * enrollment from a config-only one. The first three values NEVER
 * authorize a write; they exist so a refusal can say what the boot did
 * have rather than reporting a bare zero. */
typedef enum {
    TPM_ENROLL_AUTH_NONE                = 0,  /* nothing asserted enrollment */
    TPM_ENROLL_AUTH_ESP_CONFIG_ONLY     = 1,  /* boot.conf tpm_enroll and nothing more */
    TPM_ENROLL_AUTH_LOADER_SIGNAL_ONLY  = 2,  /* NVRAM-backed recovery, no keypress yet */
    TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN = 3,  /* the only authorizing value */
} tpm_enroll_authority_t;

#define TPM_ENROLL_AUTH_MAX TPM_ENROLL_AUTH_LOCAL_CONSOLE_ON_TRUSTED_CHAIN

/* Exactly why a write was refused. Distinct values, never one boolean:
 * an operator who is refused needs to know whether to set a config flag,
 * assert the NVRAM recovery trigger, or press a different key. */
typedef enum {
    TPM_ENROLL_REFUSE_NONE                 = 0,   /* admitted; not a refusal */
    TPM_ENROLL_REFUSE_CONFIG_DISABLED      = 1,   /* boot.conf tpm_enroll clear */
    TPM_ENROLL_REFUSE_BOOT_CHAIN_UNTRUSTED = 2,   /* Secure Boot inactive */
    TPM_ENROLL_REFUSE_REPLAY_TAMPER        = 3,   /* event log already disagrees with the PCRs */
    TPM_ENROLL_REFUSE_REPLAY_UNVERIFIABLE  = 4,   /* no hardware PCR readable */
    TPM_ENROLL_REFUSE_STICKY_DEGRADED      = 5,   /* audit_degraded: sticky read untrusted */
    TPM_ENROLL_REFUSE_STICKY_ABSENT        = 6,   /* no sticky record at all */
    TPM_ENROLL_REFUSE_STICKY_TRIGGER_CLEAR = 7,   /* sticky present, recovery not requested */
    /* 8-10: incoherent decision record, checked ONLY when the ladder
     * actually published a RECOVERY_REQUEST selection. There is deliberately
     * no "selection is not RECOVERY_REQUEST" refusal: the ladder cannot
     * publish that reason at all today, so requiring it would refuse every
     * boot. See the step-5 comment in tpm_enroll_gate.c. */
    TPM_ENROLL_REFUSE_PATH_MISMATCH        = 8,   /* boot_path is not RECOVERY */
    TPM_ENROLL_REFUSE_REASON_MISMATCH      = 9,   /* boot_reason is not RECOVERY_TRIGGER */
    TPM_ENROLL_REFUSE_SOURCE_FLAG_MISSING  = 10,  /* RECOVERY_TRIGGERED source flag clear */
    TPM_ENROLL_REFUSE_CONFIRM_TIMEOUT      = 11,  /* operator never answered */
    TPM_ENROLL_REFUSE_CONFIRM_WRONG_KEY    = 12,  /* operator declined */
    TPM_ENROLL_REFUSE_CONFIRM_UNAVAILABLE  = 13,  /* no usable console to ask on */
    /* Secure Boot IS active, but it did not cover the RUNNING KERNEL. A
     * separate value from BOOT_CHAIN_UNTRUSTED because the operator's remedy
     * differs: that one says "enable Secure Boot", this one says "boot the
     * UKI". See whole_chain_verified for why the distinction is real. */
    TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED    = 14,
} tpm_enroll_refusal_t;

#define TPM_ENROLL_REFUSE_MAX TPM_ENROLL_REFUSE_KERNEL_UNVERIFIED

/* Outcome of the console confirmation. An ENUM, not a bool: a timeout on
 * a headless machine and an operator pressing the wrong key are different
 * events and an operator reading the log must be able to tell them apart. */
typedef enum {
    TPM_CONFIRM_PENDING     = 0,  /* not asked yet -- caller should prompt */
    TPM_CONFIRM_YES         = 1,  /* affirmative keypress */
    TPM_CONFIRM_WRONG_KEY   = 2,  /* a decline, or any other recognized key */
    TPM_CONFIRM_TIMEOUT     = 3,  /* deadline expired with no answer */
    TPM_CONFIRM_UNAVAILABLE = 4,  /* no console to prompt on */
} tpm_confirm_outcome_t;

#define TPM_CONFIRM_MAX TPM_CONFIRM_UNAVAILABLE

/* Every input the decision depends on, by value. Taking a struct rather
 * than reading globals is what makes the predicate testable: a test
 * builds the exact (spoofed / degraded / confirmed) tuple it wants. */
struct tpm_enroll_gate_inputs {
    uint32_t boot_path;               /* enum boot_path_type */
    uint32_t boot_reason;             /* enum boot_reason_code */
    uint32_t boot_source_flags;       /* BOOT_SOURCE_FLAG_* bitmask */
    uint32_t selection_reason;        /* enum boot_selection_reason */
    uint8_t  audit_degraded;          /* 1 = sticky_* fields must be read as zero */
    uint8_t  sticky_present;          /* 1 = a trusted sticky record was read */
    uint8_t  sticky_recovery_trigger; /* 1 = NVRAM asked for recovery */
    uint8_t  secure_boot_enabled;     /* 1 = firmware reports Secure Boot active */
    /* 1 = Secure Boot actually covered THE RUNNING KERNEL, not merely the
     * loader. These are two different facts and conflating them was a
     * critical defect in the first version of this gate.
     *
     * The bootloader is explicit about the difference. On the UKI path the
     * kernel is the signed PE's own `.linux` section, and reading
     * `\kernel.exe` from the ESP instead "would give us an unsigned copy"
     * (src/boot/uefi/bootx64.c:7894-7896). On the legacy SPLIT path "the
     * trust anchor ... is the Secure Boot signature on BOOTX64.EFI itself"
     * (bootx64.c:12281-12283) -- so kernel.exe is an unverified ESP file
     * even with Secure Boot fully enabled.
     *
     * Callers set this from BOOT_FLAG_INVOKED_VIA_UKI (boot_info.h:372).
     * Without it, an ESP attacker who swaps kernel.exe gets a boot that
     * passes every other check here and would be reported as running on a
     * trusted chain. */
    uint8_t  whole_chain_verified;
    uint8_t  replay_known;            /* 1 = a replay verdict was produced */
    uint8_t  replay_verdict;          /* tpm_replay_verdict_t, valid iff replay_known */
    uint8_t  tpm_enroll;              /* boot.conf opt-in (ESP-controlled) */
    uint8_t  confirm;                 /* tpm_confirm_outcome_t */
};

/* The decision. `admit` is the only field a caller may use to authorize
 * an NV write; `authority` and `refusal` are for reporting. */
struct tpm_enroll_gate_result {
    uint8_t admit;         /* 1 = the caller may write the baseline */
    uint8_t needs_confirm; /* 1 = all else passed; prompt, then re-evaluate */
    uint8_t authority;     /* tpm_enroll_authority_t */
    uint8_t refusal;       /* tpm_enroll_refusal_t */
};

/* Evaluate the gate. PURE and TOTAL: no globals, no I/O, every input
 * combination yields exactly one verdict, and a NULL `in` is handled as a
 * fail-closed refusal rather than a fault.
 *
 * Fail-closed by construction: `admit` is set on exactly one path, the one
 * where every condition held AND the operator confirmed. */
void tpm_enroll_gate_evaluate(const struct tpm_enroll_gate_inputs *in,
                              struct tpm_enroll_gate_result *out);

/* Stable label for a refusal value, for logs and the integrity report.
 * Never NULL -- an out-of-range value returns "unknown". */
const char *tpm_enroll_refusal_label(uint8_t refusal);

/* Stable label for an authority value. Never NULL. */
const char *tpm_enroll_authority_label(uint8_t authority);
