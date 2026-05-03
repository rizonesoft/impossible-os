/* ============================================================================
 * firmware_advisor.h -- read-only firmware-update advisor public API
 *
 * Surfaces what firmware updates exist for the host and tells the operator
 * how to apply them via the VENDOR's update path (Lenovo Vantage, Dell
 * Command Update, fwupd from a Linux live USB, BIOS Setup, etc.).
 *
 * Impossible OS does NOT call UpdateCapsule(), does NOT write the
 * OsIndications capsule bit, does NOT stage capsule images on the ESP, and
 * does NOT trigger reboot-and-flash.  This is a deliberate "advisory only"
 * scope decision: write enforcement (compile-time _Static_assert sentinel
 * + linker-time symbol refusal) lives in src/kernel/firmware_capsule_refused.c.
 *
 * Init runs BSP-only in Phase 3 after VFS / ESRT / registry are ready so the
 * registry surface (HKLM\SOFTWARE\Impossible\FirmwareAdvisor\*) is populated
 * before any desktop notification daemon starts.  Eager init beats lazy
 * because the boot-time cost is bounded by ESRT count (typically <8
 * components) and the registry surface is the canonical consumer.
 * ============================================================================ */

#ifndef KERNEL_FIRMWARE_ADVISOR_H
#define KERNEL_FIRMWARE_ADVISOR_H

#include "kernel/types.h"

/* Per-component advisor verdict.  Read-only after firmware_advisor_init(). */
enum firmware_advisor_status {
    FW_ADVISOR_STATUS_UNKNOWN          = 0, /* no cache entry for this FwClass */
    FW_ADVISOR_STATUS_UP_TO_DATE       = 1, /* current >= latest */
    FW_ADVISOR_STATUS_UPDATE_AVAILABLE = 2  /* current < latest */
};

enum firmware_advisor_severity {
    FW_ADVISOR_SEVERITY_NONE        = 0, /* status=unknown */
    FW_ADVISOR_SEVERITY_RECOMMENDED = 1, /* update available, no CVE */
    FW_ADVISOR_SEVERITY_CRITICAL    = 2  /* update available, CVE_ID present */
};

/* Cache file load state (mirrored to HKLM\SOFTWARE\Impossible\FirmwareAdvisor
 * \_Header\CacheState as a REG_SZ for the sysinfo CLI to render). */
enum firmware_advisor_cache_state {
    FW_ADVISOR_CACHE_MISSING   = 0, /* X:\Diag\lvfs-metadata.json absent */
    FW_ADVISOR_CACHE_MALFORMED = 1, /* JSON parse failure or schema mismatch */
    FW_ADVISOR_CACHE_LOADED    = 2  /* loaded successfully */
};

/* Initialize advisor: load cache, join with ESRT, populate registry mirror.
 * Idempotent (RegDeleteTree on the parent key first).  Logs one LOG_INFO
 * line summarizing how many components were classified.  Safe to call when
 * ESRT is empty (registry parent key stays empty, no per-component subkeys). */
void firmware_advisor_init(void);

/* Cache state oracle for the sysinfo CLI + tests.  Returns the cache state
 * latched at firmware_advisor_init() time. */
enum firmware_advisor_cache_state firmware_advisor_cache_state(void);

/* Counts -- used by tests + telemetry.  All return 0 before init. */
uint32_t firmware_advisor_component_count(void);  /* == esrt_count() */
uint32_t firmware_advisor_count_with_status(enum firmware_advisor_status s);

/* Status oracle by ESRT index.  Returns FW_ADVISOR_STATUS_UNKNOWN for OOR. */
enum firmware_advisor_status firmware_advisor_status_by_index(uint32_t idx);
enum firmware_advisor_severity firmware_advisor_severity_by_index(uint32_t idx);

/* Decoder helpers -- static const char *, safe to print directly. */
const char *firmware_advisor_status_name(enum firmware_advisor_status s);
const char *firmware_advisor_severity_name(enum firmware_advisor_severity s);
const char *firmware_advisor_cache_state_name(enum firmware_advisor_cache_state s);

#endif /* KERNEL_FIRMWARE_ADVISOR_H */
