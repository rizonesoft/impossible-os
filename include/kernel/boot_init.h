/* ============================================================================
 * boot_init.h — Kernel boot infrastructure: result types, subsystem readiness
 *               oracle, POST code constants, and phase macros.
 *
 * Included by every boot phase file and any driver init function that
 * participates in the formal phased init model.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* --- Boolean (freestanding — no stdbool.h) -------------------------------- */
#ifndef __BOOT_INIT_BOOL_DEFINED
#define __BOOT_INIT_BOOL_DEFINED
typedef uint8_t bool;
#define true  ((bool)1)
#define false ((bool)0)
#endif

/* --- Boot result type ----------------------------------------------------- */

typedef enum {
    BOOT_OK       = 0,   /* Subsystem initialised successfully */
    BOOT_DEGRADED = 1,   /* Initialised with reduced capability; log + continue */
    BOOT_FATAL    = 2,   /* Cannot proceed; caller must halt or BSOD */
} boot_result_t;

/* --- Subsystem identifiers ------------------------------------------------ */
/*
 * One entry per logical subsystem. Ordered roughly by init sequence so
 * early-phase subsystems have low numbers — makes the readiness dump easy
 * to read.  Add new entries *before* SUBSYS_COUNT; never reorder existing
 * entries (stable ABI for crash-dump analysis).
 */
typedef enum {
    SUBSYS_SERIAL   =  0,
    SUBSYS_PMM      =  1,
    SUBSYS_VMM      =  2,
    SUBSYS_HEAP     =  3,
    SUBSYS_KLOG     =  4,
    SUBSYS_GDT      =  5,
    SUBSYS_IDT      =  6,
    SUBSYS_ACPI     =  7,
    SUBSYS_LAPIC    =  8,
    SUBSYS_IOAPIC   =  9,
    SUBSYS_TIMER    = 10,
    SUBSYS_RTC      = 11,
    SUBSYS_FB       = 12,
    SUBSYS_VFS      = 13,
    SUBSYS_REGISTRY = 14,
    SUBSYS_SCHED    = 15,
    SUBSYS_IPC      = 16,
    SUBSYS_SMP      = 17,
    SUBSYS_EXEC     = 18,
    SUBSYS_DESKTOP  = 19,
    SUBSYS_COUNT    = 20,  /* sentinel — keep last */
} kernel_subsys_t;

/* --- POST code constants --------------------------------------------------
 *
 * One code per major init step. Values match common BIOS diagnostic LED
 * conventions so physical POST-code cards can decode them on real hardware.
 *
 * Phase 0 (0x10–0x2F): critical pre-interrupt init
 * Phase 1 (0x30–0x4F): platform services
 * Phase 2 (0x50–0x5F): system services
 * Phase 3 (0x60–0x6F): user platform
 */
#define POSTCODE_SERIAL_INIT    0x10
#define POSTCODE_PMM_INIT       0x20
#define POSTCODE_VMM_INIT       0x21
#define POSTCODE_HEAP_INIT      0x22
#define POSTCODE_KLOG_INIT      0x23
#define POSTCODE_CPUID_INIT     0x24
#define POSTCODE_SIMD_INIT      0x25
#define POSTCODE_BOOT_CFG       0x26
#define POSTCODE_GDT_INIT       0x30
#define POSTCODE_IDT_INIT       0x31
#define POSTCODE_ACPI_INIT      0x32
#define POSTCODE_LAPIC_INIT     0x33
#define POSTCODE_IOAPIC_INIT    0x34
#define POSTCODE_TIMER_INIT     0x35
#define POSTCODE_DPC_INIT       0x36
#define POSTCODE_RTC_INIT       0x37
#define POSTCODE_KBD_INIT       0x38
#define POSTCODE_SMBIOS_INIT    0x40
#define POSTCODE_FB_INIT        0x41
#define POSTCODE_PCI_INIT       0x50
#define POSTCODE_STORAGE_INIT   0x51
#define POSTCODE_VFS_INIT       0x52
#define POSTCODE_REGISTRY_INIT  0x53
#define POSTCODE_SMP_INIT       0x54
#define POSTCODE_NET_INIT       0x55
#define POSTCODE_SCHED_INIT     0x60
#define POSTCODE_IPC_INIT       0x61
#define POSTCODE_EXEC_INIT      0x62
#define POSTCODE_DESKTOP_INIT   0x63

/* --- Subsystem readiness oracle ------------------------------------------ */

/* Returns true if the subsystem completed init without BOOT_FATAL. */
bool kernel_subsystem_ready(kernel_subsys_t subsys);

/* Record the readiness state of a subsystem.  Call once per subsystem. */
void kernel_subsystem_set_ready(kernel_subsys_t subsys, bool ok);

/* Dump all subsystem states to klog(LOG_INFO).
 * Call before any halt so the serial log captures full state. */
void kernel_subsystem_dump(void);

/* --- Boot progress tracker ----------------------------------------------- */

/* Emit "[PHASEn] step (0xNN)\n" to serial and record a TSC timestamp.
 * Safe to call from any phase; serial must be initialised (SUBSYS_SERIAL). */
void boot_progress(uint8_t phase, const char *step, uint8_t postcode);

/* --- Macros --------------------------------------------------------------- */

/*
 * BOOT_REQUIRE(subsys) — prerequisite guard
 *
 * Placed at the top of an init function body.  If the required subsystem is
 * not ready, logs the failure and returns BOOT_FATAL to the caller.
 * Only use after SUBSYS_KLOG is ready (klog_init called).
 */
#define BOOT_REQUIRE(subsys) \
    do { \
        if (!kernel_subsystem_ready(subsys)) { \
            _boot_require_failed(#subsys); \
            return BOOT_FATAL; \
        } \
    } while (0)

/*
 * BOOT_STEP(subsys, fn) — call an init function and record its result
 *
 * fn must have signature: boot_result_t fn(void)
 * Sets the subsystem ready if fn returns BOOT_OK or BOOT_DEGRADED.
 * Does NOT call boot_progress — the caller is responsible for that.
 */
#define BOOT_STEP(subsys, fn) \
    do { \
        boot_result_t _boot_step_r = (fn)(); \
        kernel_subsystem_set_ready((subsys), _boot_step_r != BOOT_FATAL); \
    } while (0)

/* Internal helper used by BOOT_REQUIRE — logs via serial (klog optional). */
void _boot_require_failed(const char *subsys_name);
