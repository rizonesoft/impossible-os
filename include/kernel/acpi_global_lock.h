/* ============================================================================
 * acpi_global_lock.h -- ACPI 6.5 section 5.2.10.1 global lock arbitration
 *
 * Split out of the ACPICA OS Services Layer deliberately. The algorithm is
 * pure (it touches one shared word and nothing else) and it is the piece most
 * likely to be silently wrong, so it must stay unit-testable in the DEFAULT
 * build -- where ACPICA itself is compiled out because the test flavor does
 * not fit under the firmware floor. Keeping it here means the test suite
 * covers it whether or not ACPICA is linked.
 *
 * The lock word is shared with SMM firmware and lives in the FACS. Bit 0 is
 * Owned, bit 1 is Pending (ACPI 6.5 table 5.20).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Attempt to acquire. Returns non-zero when this caller now owns the lock,
 * zero when the firmware still holds it and ownership is Pending (the caller
 * must then wait for the firmware's release interrupt rather than proceed).
 *
 * A NULL lock reports acquired: a platform with no FACS global lock has no
 * arbitration to lose, and failing closed there would wedge every AML path
 * that requests it. */
int acpi_global_lock_acquire(volatile uint32_t *lock);

/* Release. Returns non-zero when the Pending bit was set, which obliges the
 * caller to signal the firmware through the SMI command port so the waiting
 * side is woken. Returns zero when nobody was waiting. */
int acpi_global_lock_release(volatile uint32_t *lock);
