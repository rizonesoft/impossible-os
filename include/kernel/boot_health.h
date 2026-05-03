/* ============================================================================
 * boot_health.h -- consolidated boot-health audit JSON publisher
 *
 * Writes X:\Diag\boot-health.json once per boot, collapsing every
 * "what's wrong on this boot" signal currently scattered across serial:
 * degraded capabilities, degraded subsystems, missing hardware, perf
 * budget breaches, MAT W^X violations, active firmware quirks, recent
 * boot timestamps, and Secure Boot state.
 *
 * Schema: schema_version=1.  Pinned because the boot-trend rolling
 * regression file + future host decoder + CI regression gate all depend
 * on the wire shape.  Canonical wire format spec lives at
 * docs/boot/boot-health-schema.md -- bump schema_version when any field
 * is removed/typed-changed/renamed/semantically-redefined; adding new
 * optional fields is not a bump.
 *
 * Consumers: sysinfo CLI host decoder, CI regression gate, bare-metal
 * triage when serial is unavailable.
 * ============================================================================ */

#ifndef KERNEL_BOOT_HEALTH_H
#define KERNEL_BOOT_HEALTH_H

#include "kernel/types.h"

/* Secure Boot state classifier output. UNKNOWN must beat both ENABLED and
 * DISABLED so an unreadable state is never reported as DISABLED. */
enum boot_health_secureboot_state {
    BOOT_HEALTH_SB_UNKNOWN  = 0,
    BOOT_HEALTH_SB_DISABLED = 1,
    BOOT_HEALTH_SB_ENABLED  = 2,
    BOOT_HEALTH_SB_SETUP    = 3
};

/* Pure classifier -- exposed for unit tests so the priority ordering
 * (UNKNOWN > SETUP > ENABLED > DISABLED) is testable without invoking
 * uefi_runtime live state. Inputs are the three uefi_secureboot_*()
 * accessor results captured by the caller. */
enum boot_health_secureboot_state
boot_health_classify_secureboot(int state_valid, int setup_mode, int enabled);

/* Decoder helper for the JSON `secureboot_state` string field. */
const char *boot_health_secureboot_name(enum boot_health_secureboot_state s);

/* Publish X:\Diag\boot-health.json. Single-shot, BSP-only.  Idempotent
 * (overwrites prior contents via VFS_O_TRUNC).  Failure is LOG_WARN only --
 * never blocks userland entry. */
void boot_health_publish_json(void);

#endif /* KERNEL_BOOT_HEALTH_H */
