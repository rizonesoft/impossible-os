/* ============================================================================
 * watchdog.h -- Boot watchdog (TODO-23)
 *
 * ACPI WDAT (Watchdog Action Table) hardware watchdog. A hardware watchdog
 * reboots the board even on a total CPU lockup. This is STANDALONE -- the LAPIC
 * NMI software watchdog is deferred (blocked on nested-NMI replay + per-CPU AP
 * IST), so "no usable WDAT" means NO reboot coverage yet, not a fallback to the
 * NMI path.
 *
 * Discovery uses the validated acpi_get_raw_table("WDAT") path; every field of
 * the table is range/enum/GAS-validated before any register access. WDAT-only
 * for the first cut: direct Intel iTCO PCI is a tracked follow-up.
 *
 * ABI: the on-disk WDAT layout is fixed by the ACPI specification. The
 * _Static_asserts below pin every load-bearing offset + size; a firmware table
 * that disagrees is rejected at runtime by the validator, never trusted by
 * offset.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/acpi.h"   /* struct acpi_sdt_header (36 B), struct acpi_gas (12 B) */

/* --- ACPI WDAT on-disk layout (UEFI Watchdog Action Table) --- */

/* WDAT fixed header: the 36-byte ACPI SDT header followed by the watchdog
 * descriptor fields, then `entries` x struct acpi_wdat_entry. */
struct acpi_wdat {
    struct acpi_sdt_header header;   /* off 0  (36 B) signature "WDAT" */
    uint32_t header_length;          /* off 36 length of this fixed header (68) */
    uint16_t pci_segment;            /* off 40 */
    uint8_t  pci_bus;                /* off 42 */
    uint8_t  pci_device;             /* off 43 */
    uint8_t  pci_function;           /* off 44 */
    uint8_t  reserved1[3];           /* off 45 */
    uint32_t timer_period;           /* off 48 milliseconds per count */
    uint32_t max_count;              /* off 52 max counter value */
    uint32_t min_count;              /* off 56 min counter value */
    uint8_t  flags;                  /* off 60 */
    uint8_t  reserved2[3];           /* off 61 */
    uint32_t entries;                /* off 64 count of instruction entries */
    /* off 68: struct acpi_wdat_entry entry[entries]; */
} __attribute__((packed));

_Static_assert(sizeof(struct acpi_wdat) == 68,
    "WDAT fixed header must be 68 bytes (36 SDT + 32 descriptor)");
_Static_assert(__builtin_offsetof(struct acpi_wdat, timer_period) == 48,
    "WDAT timer_period at offset 48");
_Static_assert(__builtin_offsetof(struct acpi_wdat, entries) == 64,
    "WDAT entries count at offset 64");

/* One watchdog instruction entry: an action implemented by reading/writing a
 * register region. Multiple entries may share an action (executed in order). */
struct acpi_wdat_entry {
    uint8_t  action;                 /* off 0  enum acpi_wdat_action */
    uint8_t  instruction;            /* off 1  instruction code + PRESERVE flag */
    uint16_t reserved;               /* off 2  */
    struct acpi_gas register_region; /* off 4  (12 B) */
    uint32_t value;                  /* off 16 compare/write value */
    uint32_t mask;                   /* off 20 register mask */
} __attribute__((packed));

_Static_assert(sizeof(struct acpi_wdat_entry) == 24,
    "WDAT instruction entry must be 24 bytes");
_Static_assert(__builtin_offsetof(struct acpi_wdat_entry, register_region) == 4,
    "WDAT entry register_region (GAS) at offset 4");
_Static_assert(__builtin_offsetof(struct acpi_wdat_entry, value) == 16,
    "WDAT entry value at offset 16");

/* WDAT actions (ACPI specification enum acpi_wdat_actions). */
#define ACPI_WDAT_RESET                 1
#define ACPI_WDAT_GET_CURRENT_COUNTDOWN 4
#define ACPI_WDAT_GET_COUNTDOWN         5
#define ACPI_WDAT_SET_COUNTDOWN         6
#define ACPI_WDAT_GET_RUNNING_STATE     8
#define ACPI_WDAT_SET_RUNNING_STATE     9
#define ACPI_WDAT_GET_STOPPED_STATE     10
#define ACPI_WDAT_SET_STOPPED_STATE     11
#define ACPI_WDAT_GET_REBOOT            16
#define ACPI_WDAT_SET_REBOOT            17
#define ACPI_WDAT_GET_SHUTDOWN          18
#define ACPI_WDAT_SET_SHUTDOWN          19
#define ACPI_WDAT_GET_STATUS            32
#define ACPI_WDAT_SET_STATUS            33

/* WDAT instruction codes (low 7 bits) + flag (high bit). */
#define ACPI_WDAT_READ_VALUE            0
#define ACPI_WDAT_READ_COUNTDOWN        1
#define ACPI_WDAT_WRITE_VALUE           2
#define ACPI_WDAT_WRITE_COUNTDOWN       3
#define ACPI_WDAT_INSTRUCTION_MASK      0x7F
#define ACPI_WDAT_PRESERVE_REGISTER     0x80

/* GAS address spaces relevant to WDAT registers. */
#define ACPI_GAS_SPACE_MEMORY           0
#define ACPI_GAS_SPACE_IO               1

/* --- Public API (TODO-23 boot watchdog) --- */

enum hw_watchdog_kind {
    HW_WD_NONE = 0,   /* no usable hardware watchdog -- no reboot coverage */
    HW_WD_WDAT = 1,   /* ACPI WDAT */
    HW_WD_ITCO = 2,   /* direct Intel iTCO PCI (deferred follow-up) */
};

/* Discover + validate the ACPI WDAT and, if usable, arm it with a generous
 * boot-wide timeout. Safe no-op (kind = HW_WD_NONE) when no usable WDAT exists
 * (e.g. QEMU). Call once after acpi_init, in Phase 1/2. */
void hw_watchdog_init(void);

/* Reload the watchdog countdown (RESET action). No-op unless armed. Called
 * from boot_progress() so a stalled boot phase lets the timer expire -> reboot. */
void hw_watchdog_pet(void);

/* Disarm (SET_STOPPED_STATE), readback-verified. MUST be called after the final
 * boot-log flush and BEFORE task_create()/scheduler_enable() -- there is no
 * runtime petter yet, so the desktop must never run with the watchdog armed.
 * Returns 0 when disarmed (or nothing was armed) and -1 when disarm could NOT
 * be confirmed; on -1 the caller MUST NOT proceed into the scheduler (halt
 * instead -- entering the desktop armed would reboot the machine). */
int hw_watchdog_boot_handoff(void);

/* The detected hardware-watchdog kind (HW_WD_NONE if none / not yet init'd). */
enum hw_watchdog_kind hw_watchdog_kind(void);

/* Pure WDAT validator (no I/O) -- exposed for unit testing on crafted tables.
 * Returns 1 only when `w` (a `size`-byte buffer) is a usable WDAT: header
 * length + entry-count fit within size with no overflow, timer_period != 0,
 * min_count <= max_count, every entry has a known instruction + usable
 * (I/O-space) GAS, and the arm/pet/disarm/verify actions are each present with
 * the correct instruction class: SET_RUNNING_STATE / SET_STOPPED_STATE write
 * a value, RESET writes (value or countdown), SET_COUNTDOWN writes the
 * countdown, GET_RUNNING_STATE reads -- a mixed/wrong-class entry for any of
 * these rejects the whole table. */
int hw_watchdog_wdat_validate(const struct acpi_wdat *w, uint32_t size);
