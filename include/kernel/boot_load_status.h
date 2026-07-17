/* Boot load/status log (ntbtlog parity).
 *
 * A durable, per-subsystem record of what loaded, was skipped, failed, or
 * came up degraded during boot, with elapsed-ms timing. Win11 writes the
 * equivalent to ntbtlog.txt; Linux exposes it via dmesg + systemd-analyze.
 * The 15 coarse named boot stages (section 2) only say "DRIVERS" progressed;
 * this log says "ahci LOADED in 12ms, nvme FAILED 0x5" -- the granularity
 * boot-regression triage actually needs.
 *
 * SMP-safe by construction: boot probes can run in parallel (boot_async_group
 * fans ATA/AHCI/NVMe/VirtIO across CPUs), so slot allocation is a lock-free
 * atomic fetch-add and each owner release-publishes its slot. The pool never
 * wraps; overflow is counted and surfaced in the dump header + summary so an
 * over-capacity boot can never read as a complete log.
 */
#pragma once

#include "kernel/types.h"

#define BOOT_LOAD_MAX        64u   /* fixed pool; no wrap */
#define BOOT_LOAD_NAME_MAX   32u   /* per-entry name, includes NUL */

/* Subsystem class -- coarse grouping for the log, independent of boot_stage_t. */
enum boot_load_class {
    BOOT_LOAD_CLASS_CORE = 0,   /* PMM/VMM/HEAP/sched/VFS/registry */
    BOOT_LOAD_CLASS_STORAGE,    /* ATA/AHCI/NVMe/VirtIO-blk */
    BOOT_LOAD_CLASS_INPUT,      /* PS2/USB HID */
    BOOT_LOAD_CLASS_NET,        /* e1000/VirtIO-net/stack */
    BOOT_LOAD_CLASS_ACPI,       /* ACPI tables / power */
    BOOT_LOAD_CLASS_GFX,        /* GOP / compositor */
    BOOT_LOAD_CLASS_COUNT
};

/* Load outcome for one subsystem. */
enum boot_load_state {
    BOOT_LOAD_ATTEMPTED = 0,    /* begin recorded, not yet finished */
    BOOT_LOAD_LOADED,           /* up and functional */
    BOOT_LOAD_SKIPPED,          /* absent device / disabled by config */
    BOOT_LOAD_FAILED,           /* init failed; subsystem unavailable */
    BOOT_LOAD_DEGRADED,         /* up but reduced (fallback path) */
    BOOT_LOAD_STATE_COUNT
};

struct boot_load_entry {
    char     name[BOOT_LOAD_NAME_MAX];  /* NUL-terminated, truncated to fit */
    uint8_t  cls;                       /* enum boot_load_class */
    uint8_t  state;                     /* enum boot_load_state */
    uint16_t err_code;                  /* driver/NTSTATUS-ish error, 0 if none */
    uint16_t post_code;                 /* POST16 marker, 0 if none */
    uint32_t start_ms;                  /* boot_get_elapsed_ms() at begin/record */
    uint32_t duration_ms;               /* measured span (begin..finish), 0 for point events */
    uint32_t published;                 /* release-published once the entry is valid */
};

/* Point event: append a finished record (duration 0). SMP-safe; silently
 * dropped (counted) if the pool is full. */
void boot_load_record(const char *name, uint8_t cls, uint8_t state,
                      uint16_t err_code, uint16_t post_code);

/* Spanned event: claim a slot + stamp start_ms; returns a token (>= 0) to pass
 * to boot_load_finish, or -1 if the pool is full. SMP-safe. */
int  boot_load_begin(const char *name, uint8_t cls);

/* Finalize a boot_load_begin token: set final state/err/post and the measured
 * duration (now - start_ms). No-op on a negative (pool-full) token. */
void boot_load_finish(int token, uint8_t state,
                      uint16_t err_code, uint16_t post_code);

/* Pure formatter: render header + one line per published entry into buf
 * (NUL-terminated); returns bytes written (excluding the NUL). No VFS, so unit
 * tests can assert the format. The header reports recorded/cap/dropped and a
 * TRUNCATED marker when the pool overflowed. */
uint32_t boot_load_status_format(char *buf, uint32_t cap);

/* Pure helper: build "N degraded: ahci(0x5), e1000(0x3)" (plus "+D dropped"
 * when the pool overflowed) into buf; returns the count of FAILED+DEGRADED
 * entries. Writes an empty string and returns 0 when nothing degraded. */
uint32_t boot_load_status_degraded_summary(char *buf, uint32_t cap);

/* Phase-3 sinks. dump writes X:\Diag\boot-load-status.txt (gated on
 * klog_using_blackbox; best-effort). report emits the degraded summary on
 * serial + boot_splash_diag and MUST be called before boot_splash_finish so
 * the visible line is not swallowed by an inactive splash. */
void boot_load_status_dump_to_blackbox(void);
void boot_load_status_report_summary(void);

/* Test seam: snapshot/restore the whole pool so a unit test can record a
 * synthetic mix without destroying the live boot log. Pure memory ops.
 * Guarded out of release builds (release test-surface exclusion). */
#ifdef KERNEL_TESTS
struct boot_load_test_state {
    struct boot_load_entry saved[BOOT_LOAD_MAX];
    int32_t                saved_claimed;
};
void boot_load_status_test_save(struct boot_load_test_state *st);
void boot_load_status_test_restore(const struct boot_load_test_state *st);
#endif
