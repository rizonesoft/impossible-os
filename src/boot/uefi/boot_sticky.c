/* ============================================================================
 * boot_sticky.c -- bootloader-side reader for the boot policy NVRAM sticky
 *
 * Reads the `ImpossibleOS-BootSticky` UEFI variable (single 256-byte
 * record) and surfaces the cross-boot trigger bits to the rest of the
 * bootloader. NEVER WRITES. The kernel acks consumed triggers
 * (clears bits) post-publish via uefi_var_set; a reset between observe
 * and ack leaves the trigger pending, which is the correct sticky
 * semantic.
 *
 * Validation: the helper checks attrs (NV+BS+RT), size (==256), magic
 * ('BSST'), version (1), and CRC. Any failure -> treated as absent +
 * audit_degraded=1. The bootloader does NOT delete-then-create on bad
 * data (unlike boot_history.c); leaving a corrupted record in place
 * lets the kernel decide whether to repair after a successful publish.
 * Also avoids the failure mode where a corrupt record carrying
 * recovery_trigger=1 gets silently zeroed before the ladder considers
 * it -- if we cannot trust the data, we surface that fact and let the
 * disk-side audit trail capture the chain of degradation.
 * ============================================================================ */

#include "efi.h"
#include "boot_info_mirror.h"
#include "../../../include/boot/boot_audit_codes.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_GUID g_impossible_os_guid;
extern void serial_early_print(const char *s);

/* CHAR16 var name. Kept file-local to keep the writer (kernel-side) the
 * only producer of this token. */
static CHAR16 g_sticky_var[] = u"ImpossibleOS-BootSticky";

#define BOOT_STICKY_EXPECTED_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                                    EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                                    EFI_VARIABLE_RUNTIME_ACCESS)

/* Surface helper. Sets every sticky_* field on `bi` to a clean
 * "absent + degraded" state and flags audit_degraded. Used both when
 * GetVariable fails AND when the returned record fails validation. */
static void
boot_sticky_mark_degraded(struct boot_info *bi, const char *reason)
{
    bi->audit_degraded = 1;
    bi->sticky_present = 0;
    bi->sticky_recovery_trigger = 0;
    bi->sticky_watchdog_rollback_request = 0;
    bi->sticky_last_outcome = 0;
    bi->sticky_audit_degraded_last_boot = 0;
    bi->sticky_last_event_code = BOOT_AUDIT_EVENT_AUDIT_DEGRADED;
    bi->sticky_last_boot_seq = 0;
    bi->sticky_consumed_trigger_seq = 0;
    serial_early_print("[WARN] boot_sticky: ");
    serial_early_print(reason);
    serial_early_print("\n");
}

/* Read the sticky variable, validate, and populate boot_info v20 audit
 * fields. Bootloader is read-only: never calls SetVariable. Always
 * leaves boot_info in a defined state (either populated from a valid
 * record, or zeroed with audit_degraded=1).
 *
 * Failure modes (all surfaced via audit_degraded=1, never blocking):
 *   - RuntimeServices unavailable
 *   - GetVariable returned EFI_NOT_FOUND  (first boot; this is the
 *                                          expected path -- treated as
 *                                          "no triggers" rather than a
 *                                          true degradation, with
 *                                          last_event_code=FIRST_BOOT
 *                                          so the kernel can distinguish)
 *   - GetVariable returned EFI_BUFFER_TOO_SMALL or any other error
 *   - Returned size != BOOT_STICKY_VAR_SIZE
 *   - Returned attrs != BOOT_STICKY_EXPECTED_ATTRS
 *   - Magic / version / size / CRC fail */
void
boot_sticky_read_into_boot_info(struct boot_info *bi)
{
    if (!bi) return;

    /* Initialize to zero so a missing/failed read leaves clean data. */
    bi->audit_degraded = 0;
    bi->sticky_present = 0;
    bi->sticky_recovery_trigger = 0;
    bi->sticky_watchdog_rollback_request = 0;
    bi->sticky_last_outcome = 0;
    bi->sticky_audit_degraded_last_boot = 0;
    bi->sticky_last_event_code = BOOT_AUDIT_EVENT_UNSET;
    bi->sticky_last_boot_seq = 0;
    bi->sticky_consumed_trigger_seq = 0;
    bi->_audit_pad = 0;

    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->GetVariable) {
        boot_sticky_mark_degraded(bi, "RuntimeServices unavailable");
        return;
    }

    /* Read into a local 256-byte buffer (NOT directly into boot_info)
     * so a malformed record cannot corrupt adjacent boot_info fields
     * via an out-of-bounds firmware write. UEFI 2.10 7.2.1: GetVariable
     * always reports the actual variable size on success and on
     * EFI_BUFFER_TOO_SMALL; we trust nothing else. */
    struct boot_sticky_record rec;
    UINT8 *raw = (UINT8 *)&rec;
    for (UINTN i = 0; i < sizeof(rec); i++) raw[i] = 0;

    UINTN size = sizeof(rec);
    UINT32 attrs = 0;
    EFI_STATUS s = gST->RuntimeServices->GetVariable(
        g_sticky_var, &g_impossible_os_guid, &attrs, &size, &rec);

    if (s == EFI_NOT_FOUND) {
        /* First boot or post-wipe. Not a degradation; flag as
         * FIRST_BOOT so the kernel records it correctly on publish.
         * audit_degraded stays 0 because the clean state IS trusted. */
        bi->sticky_present = 0;
        bi->sticky_last_event_code = BOOT_AUDIT_EVENT_FIRST_BOOT;
        serial_early_print("[BOOT] boot_sticky: absent (first boot)\n");
        return;
    }

    if (EFI_ERROR(s)) {
        boot_sticky_mark_degraded(bi, "GetVariable failed");
        return;
    }

    if (size != BOOT_STICKY_VAR_SIZE) {
        boot_sticky_mark_degraded(bi, "size mismatch");
        return;
    }

    if (attrs != BOOT_STICKY_EXPECTED_ATTRS) {
        boot_sticky_mark_degraded(bi, "attrs mismatch");
        return;
    }

    if (!boot_sticky_record_is_valid(&rec)) {
        boot_sticky_mark_degraded(bi, "magic/version/CRC invalid");
        return;
    }

    /* Valid record. Surface the bits. */
    bi->sticky_present = 1;
    bi->sticky_recovery_trigger = rec.recovery_trigger ? 1 : 0;
    bi->sticky_watchdog_rollback_request = rec.watchdog_rollback_request ? 1 : 0;
    bi->sticky_last_outcome = rec.last_outcome ? 1 : 0;
    bi->sticky_audit_degraded_last_boot = rec.audit_degraded_last_boot ? 1 : 0;
    bi->sticky_last_event_code = rec.last_event_code;
    bi->sticky_last_boot_seq = rec.last_boot_seq;
    bi->sticky_consumed_trigger_seq = rec.consumed_trigger_seq;

    serial_early_print("[BOOT] boot_sticky: read ok");
    if (rec.recovery_trigger)
        serial_early_print(" recovery_trigger=1");
    if (rec.watchdog_rollback_request)
        serial_early_print(" watchdog_rollback=1");
    if (rec.audit_degraded_last_boot)
        serial_early_print(" prev_audit_degraded=1");
    serial_early_print("\n");
}
