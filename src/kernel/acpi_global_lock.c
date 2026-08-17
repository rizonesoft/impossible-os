/* ============================================================================
 * acpi_global_lock.c -- ACPI 6.5 section 5.2.10.1 global lock arbitration
 *
 * Arbitrates the FACS global lock word with SMM firmware. ACPICA's own
 * fallback (acenv.h, ACPI_ACQUIRE_GLOBAL_LOCK) is `Acquired = 1`: it claims
 * the lock unconditionally and never even reads the word. That is invisible
 * under emulation, because no emulator contends the lock, and corrupts
 * embedded-controller state on real hardware, which is exactly the class of
 * bug this project's bare-metal-first rule exists to catch.
 *
 * Compiled unconditionally, including in builds where ACPICA is not linked,
 * so the unit suite covers it in the default flavor. See acpi_global_lock.h.
 * ============================================================================ */

#include "kernel/acpi_global_lock.h"

/* ACPI 6.5 table 5.20 global lock bits.
 *
 * Polarity is PENDING = bit 0, OWNED = bit 1 -- the reverse of the intuitive
 * reading, and it was written backwards here on the first pass. ACPICA's own
 * actbl.h is the authority:
 *
 *   #define ACPI_GLOCK_PENDING  (1)     -- 00: Pending global lock ownership
 *   #define ACPI_GLOCK_OWNED    (1<<1)  -- 01: Global lock is owned
 *
 * Swapped, acquiring a free word writes 0x1, which firmware reads as
 * "pending, not owned" -- so OSPM walks into the protected path while SMM
 * still believes the lock is free, and both touch the embedded controller at
 * once. No emulator contends this lock, so only real hardware catches it. */
#define ACPI_GLOCK_PENDING  (1u)
#define ACPI_GLOCK_OWNED    (1u << 1)

int acpi_global_lock_acquire(volatile uint32_t *lock)
{
    uint32_t old, updated;

    /* No FACS global lock on this platform: nothing to arbitrate, and
     * failing closed would wedge every AML path that requests it. */
    if (!lock) {
        return 1;
    }

    do {
        old = __atomic_load_n(lock, __ATOMIC_ACQUIRE);

        /* Claim ownership. If it was already owned, we additionally set
         * Pending so the current owner knows to hand the lock over on
         * release rather than simply dropping it. */
        updated = (old & ~ACPI_GLOCK_OWNED) | ACPI_GLOCK_OWNED;
        if (old & ACPI_GLOCK_OWNED) {
            updated |= ACPI_GLOCK_PENDING;
        }
    } while (!__atomic_compare_exchange_n(lock, &old, updated, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

    /* Pending set means somebody else got there first. */
    return (updated & ACPI_GLOCK_PENDING) == 0u;
}

int acpi_global_lock_release(volatile uint32_t *lock)
{
    uint32_t old, updated;

    if (!lock) {
        return 0;
    }

    do {
        old = __atomic_load_n(lock, __ATOMIC_ACQUIRE);
        updated = old & ~(ACPI_GLOCK_OWNED | ACPI_GLOCK_PENDING);
    } while (!__atomic_compare_exchange_n(lock, &old, updated, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

    /* Caller must raise the SMI when somebody was waiting. */
    return (old & ACPI_GLOCK_PENDING) != 0u;
}
