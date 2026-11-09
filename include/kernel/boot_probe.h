/* ============================================================================
 * boot_probe.h -- Shared outcome convention for boot-time driver probes
 *
 * Every boot-time `*_init()` probe answers three different questions with one
 * integer, and until now it answered only two of them: a bare `-1` meant BOTH
 * "no such device on this machine" and "the device is here and its init
 * failed". The boot load-status log therefore could not
 * tell SKIPPED from FAILED, and a machine with a dead controller logged the
 * same line as a machine with no controller at all.
 *
 * The values are chosen so no existing caller changes behavior:
 *   - success stays 0, so `rc != 0` and `rc == 0` are unchanged;
 *   - ABSENT keeps the historical -1, so every `rc < 0` / `rc == -1` site that
 *     means "device not present" keeps its exact meaning;
 *   - FAILED is a NEW value, so a caller that never learned about it still
 *     treats it as the failure it already treated -1 as.
 *
 * A probe that cannot tell the two apart must return ABSENT rather than
 * guessing: an over-reported FAILED puts a spurious "degraded" line in front
 * of an operator on every clean boot, which is the failure mode that made the
 * aggregate record useless in the first place.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

enum boot_probe_result {
    BOOT_PROBE_OK     =  0,   /* device present and initialized             */
    BOOT_PROBE_ABSENT = -1,   /* no such device on this machine             */
    BOOT_PROBE_FAILED = -2,   /* device present, initialization failed      */
};

/* Map a probe result onto a `boot_load_state`:
 *   OK      -> BOOT_LOAD_LOADED
 *   ABSENT  -> BOOT_LOAD_SKIPPED
 *   anything else (FAILED, and any unrecognised negative) -> BOOT_LOAD_FAILED
 * A positive value is NOT success: no probe in this tree returns one, and
 * reading an unexpected positive as LOADED would report a driver up on the
 * strength of a value nobody defined. Pure; exposed for tests. */
uint8_t boot_probe_load_state(int rc);

/* Map an attempted/usable pair onto a `boot_load_state`, for probes that
 * enumerate several instances (NVMe controllers, USB HID interfaces):
 *   attempted == 0                  -> BOOT_LOAD_SKIPPED  (no hardware)
 *   usable    == 0                  -> BOOT_LOAD_FAILED   (all of them failed)
 *   usable    <  attempted          -> BOOT_LOAD_DEGRADED (partial failure)
 *   usable    >= attempted          -> BOOT_LOAD_LOADED
 * Pure; exposed for tests. */
uint8_t boot_probe_count_state(uint32_t attempted, uint32_t usable);

/* Map a probe result onto a load-status err code: OK 0, ABSENT 1, and 2 for
 * FAILED or any value nobody defined. Never negates its input, so INT_MIN and
 * positive garbage are safe. Pure; exposed for tests. */
uint16_t boot_probe_err_code(int rc);

/* Load state for a multi-device driver that returns a probe result AND keeps
 * device counts: an OK result is refined by the counts (some usable of more
 * attempted -> DEGRADED), any other result stands as boot_probe_load_state()
 * maps it, because a controller-level failure has no devices to count. Pure;
 * exposed for tests. */
uint8_t boot_probe_combined_state(int rc, uint32_t attempted, uint32_t usable);

/* One driver's probe outcome, written by the CPU that ran the probe and read by
 * the BSP after the async barrier. `done` is release-stored LAST, so a reader
 * that acquires done == 1 sees the other three fields. */
struct boot_probe_slot {
    uint32_t done;          /* release-stored LAST by the probing CPU */
    uint8_t  state;         /* boot_load_state */
    uint16_t err;           /* boot_probe_err_code() of the result */
    uint32_t duration_ms;   /* measured on the probing CPU, not the BSP */
};

/* Publish an outcome into `slot`. `t0` and `now` are the probing CPU's own
 * start and end stamps; a clock that went backwards records 0, never a
 * wrapped duration. Pure apart from the release store; exposed for tests. */
void boot_probe_slot_publish(struct boot_probe_slot *slot, uint8_t state,
                             uint16_t err, uint32_t t0, uint32_t now);

/* The record the BSP writes for one slot, in precedence order:
 *   unsafe (the barrier excluded the driver) -> FAILED, BOOT_FATAL, whatever
 *     the slot holds, because a late worker must not contradict the exclusion;
 *   published                                -> the slot's own verdict;
 *   never published                          -> FAILED, BOOT_FATAL: nobody
 *     established the device is absent, so SKIPPED would be a false claim.
 * Pure; exposed for tests. */
void boot_probe_slot_verdict(const struct boot_probe_slot *slot, int unsafe,
                             uint8_t *state, uint16_t *err, uint32_t *dur);
