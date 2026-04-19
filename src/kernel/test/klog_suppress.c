/* ============================================================================
 * klog_suppress.c -- TEST_KLOG_SUPPRESS implementation.
 *
 * See include/kernel/test/klog_suppress.h for the API contract.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/klog_suppress.h"

int klog_suppress_begin(const char *subsystem)
{
    if (!subsystem || !subsystem[0])
        return 0;

    struct klog_suppress_record *rec =
        (struct klog_suppress_record *)kmalloc(sizeof(*rec));
    if (!rec)
        return 0;

    rec->subsystem    = subsystem;
    /* Snapshot BOTH the effective level AND whether an explicit
     * override was already active. Without the had_override flag, a
     * test that suppresses a tag with no pre-existing override would
     * leave a permanent override behind at restore (breaking the
     * fallback-to-global semantics). */
    rec->had_override = (uint8_t)klog_has_override(subsystem);
    rec->prev_level   = klog_get_level(subsystem);
    rec->_pad[0] = rec->_pad[1] = rec->_pad[2] = 0;

    /* Register the restore BEFORE changing the level. If test_add_action
     * rejects the registration (action stack full or drain-reentry), we
     * free the record and leave the level unchanged -- the test still
     * runs, just loudly. */
    if (test_add_action(klog_suppress_restore, rec) != 0) {
        kfree(rec);
        return 0;
    }

    klog_set_level(subsystem, LOG_FATAL);
    return 1;
}

void klog_suppress_restore(void *ctx)
{
    if (!ctx)
        return;

    struct klog_suppress_record *rec = (struct klog_suppress_record *)ctx;
    if (rec->had_override) {
        /* Pre-existing override -- restore its value. */
        klog_set_level(rec->subsystem, rec->prev_level);
    } else {
        /* No override existed before; remove the temporary one the
         * suppress created so the tag follows the global default
         * again (matches pre-suppress semantics exactly). */
        klog_remove_override(rec->subsystem);
    }
    kfree(rec);
}

#endif /* KERNEL_TESTS */
