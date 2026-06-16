/* ============================================================================
 * watchdog.c -- ACPI WDAT hardware watchdog (TODO-23)
 *
 * Discovers the ACPI WDAT via the validated acpi_get_raw_table("WDAT") path,
 * validates every field before any register access, and (WDAT-only,
 * I/O-space-only first cut) arms a generous boot-wide hardware watchdog. Petted
 * from boot_progress(); disarmed (readback-verified, fail-closed) at the
 * boot->userland handoff before the scheduler starts -- there is no runtime
 * petter yet, so the desktop must never run armed.
 *
 * STANDALONE: the LAPIC NMI software watchdog is deferred (blocked on nested-NMI
 * replay + per-CPU AP IST), so "no usable WDAT" means no reboot coverage yet.
 *
 * Safety / scope (first cut):
 *   - I/O-space GAS ONLY. A memory-space WDAT register is firmware-controlled
 *     and could point at RAM; rather than map+write arbitrary physical memory,
 *     a MEM-space WDAT is rejected (-> HW_WD_NONE). MEM-GAS is a tracked
 *     follow-up, alongside the deferred direct-iTCO path.
 *   - Arm only if a real boot-safe timeout can be programmed AND the arm
 *     actions actually executed; disarm is verified by readback and is
 *     fail-closed (the caller halts rather than enter the desktop armed).
 *   - On QEMU there is no WDAT, so init reports HW_WD_NONE and arms nothing.
 * ============================================================================ */

#include "kernel/drivers/watchdog.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"

/* A generous whole-boot budget; per-phase timeouts are a deferred refinement. */
#define HW_WD_BOOT_TIMEOUT_MS  120000u   /* 120 s requested */
/* Refuse to arm if the hardware cannot represent at least this long -- a
 * shorter watchdog would reboot mid-boot (Phase 2 alone can take ~30 s). */
#define HW_WD_MIN_SAFE_MS       60000u   /* 60 s floor */

/* WDAT signature "WDAT" as a little-endian uint32 for acpi_get_raw_table(). */
#define WDAT_SIGNATURE  0x54414457u      /* 'W' | 'D'<<8 | 'A'<<16 | 'T'<<24 */

/* I/O-space register access (file-local, mirrors the acpi.c pattern). */
static inline void wd_outb(uint16_t p, uint8_t v)
{ __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void wd_outw(uint16_t p, uint16_t v)
{ __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void wd_outl(uint16_t p, uint32_t v)
{ __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t wd_inb(uint16_t p)
{ uint8_t r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(p)); return r; }
static inline uint16_t wd_inw(uint16_t p)
{ uint16_t r; __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(p)); return r; }
static inline uint32_t wd_inl(uint16_t p)
{ uint32_t r; __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(p)); return r; }

/* --- module state (boot-path, single-threaded init; no concurrent access) --- */
static const struct acpi_wdat *g_wdat = (const struct acpi_wdat *)0;
static const struct acpi_wdat_entry *g_entries = (const struct acpi_wdat_entry *)0;
static uint32_t g_entry_count = 0;
static enum hw_watchdog_kind g_kind = HW_WD_NONE;
static int g_armed = 0;

/* A GAS register region we can safely access. I/O-SPACE ONLY in this first cut:
 * a memory-space region is firmware-controlled and could alias RAM, so we do
 * not map/write it (MEM-GAS support is a tracked follow-up). */
static int gas_usable(const struct acpi_gas *g)
{
    if (g->address == 0)
        return 0;
    if (g->address_space != ACPI_GAS_SPACE_IO)
        return 0;                        /* I/O-space only (no MEM map) */
    if (g->bit_width != 8 && g->bit_width != 16 && g->bit_width != 32)
        return 0;
    if (g->address > 0xFFFFu)
        return 0;                        /* I/O ports are 16-bit addressed */
    return 1;
}

static uint32_t gas_read(const struct acpi_gas *g)
{
    uint16_t port = (uint16_t)g->address;
    if (g->bit_width == 8)  return wd_inb(port);
    if (g->bit_width == 16) return wd_inw(port);
    return wd_inl(port);
}

static void gas_write(const struct acpi_gas *g, uint32_t v)
{
    uint16_t port = (uint16_t)g->address;
    if (g->bit_width == 8)       wd_outb(port, (uint8_t)v);
    else if (g->bit_width == 16) wd_outw(port, (uint16_t)v);
    else                          wd_outl(port, v);
}

/* Execute every WDAT instruction entry for `action`, in table order. `param`
 * supplies the value for WRITE_COUNTDOWN instructions. *out_read (if non-NULL)
 * receives the masked value of the LAST matching read entry. Returns the number
 * of matching entries that actually executed -- 0 means the action is absent or
 * had no usable register, so the caller must treat it as a failure, not a no-op
 * success. */
static uint32_t wdat_run_action(uint8_t action, uint32_t param, uint32_t *out_read)
{
    uint32_t i;
    uint32_t ran = 0;

    if (out_read)
        *out_read = 0;

    for (i = 0; i < g_entry_count; i++) {
        const struct acpi_wdat_entry *e = &g_entries[i];
        uint8_t instr;
        if (e->action != action)
            continue;
        if (!gas_usable(&e->register_region))
            continue;                          /* validated at init; belt+braces */
        instr = e->instruction & ACPI_WDAT_INSTRUCTION_MASK;

        if (instr == ACPI_WDAT_WRITE_VALUE || instr == ACPI_WDAT_WRITE_COUNTDOWN) {
            uint32_t x = (instr == ACPI_WDAT_WRITE_COUNTDOWN) ? param : e->value;
            uint32_t out = x & e->mask;
            if (e->instruction & ACPI_WDAT_PRESERVE_REGISTER) {
                uint32_t cur = gas_read(&e->register_region);
                out |= (cur & ~e->mask);
            }
            gas_write(&e->register_region, out);
        } else { /* READ_VALUE / READ_COUNTDOWN */
            uint32_t r = gas_read(&e->register_region) & e->mask;
            if (out_read)
                *out_read = r;
        }
        ran++;
    }
    return ran;
}

/* True if at least one validated entry exists for `action`. */
static int wdat_has_action(const struct acpi_wdat_entry *ent, uint32_t n, uint8_t action)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (ent[i].action == action && gas_usable(&ent[i].register_region))
            return 1;
    return 0;
}

int hw_watchdog_wdat_validate(const struct acpi_wdat *w, uint32_t size)
{
    uint64_t need;
    uint32_t i;
    const struct acpi_wdat_entry *ent;

    if (!w)
        return 0;
    if (size < sizeof(struct acpi_wdat))
        return 0;
    if (w->header_length < sizeof(struct acpi_wdat) || w->header_length > size)
        return 0;
    if (w->timer_period == 0)
        return 0;                              /* avoid divide-by-zero on arm */
    if (w->max_count == 0 || w->min_count > w->max_count)
        return 0;

    /* entries * 24 must fit after the fixed header, no overflow. */
    need = (uint64_t)w->header_length +
           (uint64_t)w->entries * sizeof(struct acpi_wdat_entry);
    if (w->entries == 0 || need > size)
        return 0;

    ent = (const struct acpi_wdat_entry *)((const uint8_t *)w + w->header_length);
    for (i = 0; i < w->entries; i++) {
        uint8_t instr = ent[i].instruction & ACPI_WDAT_INSTRUCTION_MASK;
        if (instr > ACPI_WDAT_WRITE_COUNTDOWN)
            return 0;                          /* unknown instruction */
        if (!gas_usable(&ent[i].register_region))
            return 0;                          /* unusable (or non-I/O) region */
    }
    /* Every action init/pet/disarm/verify uses must be present + usable. */
    return wdat_has_action(ent, w->entries, ACPI_WDAT_SET_RUNNING_STATE) &&
           wdat_has_action(ent, w->entries, ACPI_WDAT_SET_STOPPED_STATE) &&
           wdat_has_action(ent, w->entries, ACPI_WDAT_GET_RUNNING_STATE) &&
           wdat_has_action(ent, w->entries, ACPI_WDAT_SET_COUNTDOWN) &&
           wdat_has_action(ent, w->entries, ACPI_WDAT_RESET);
}

/* --- public API --- */

void hw_watchdog_init(void)
{
    const uint8_t *addr = (const uint8_t *)0;
    uint32_t size = 0;
    const struct acpi_wdat *w;
    uint64_t countdown, actual_ms;

    g_kind = HW_WD_NONE;
    g_armed = 0;

    if (!acpi_is_ready() ||
        !acpi_get_raw_table(WDAT_SIGNATURE, &addr, &size) || !addr) {
        klog(LOG_INFO, "watchdog", "HW watchdog: none (no ACPI WDAT)");
        return;
    }

    w = (const struct acpi_wdat *)addr;
    if (!hw_watchdog_wdat_validate(w, size)) {
        klog(LOG_WARN, "watchdog",
             "HW watchdog: none (WDAT present but invalid/unusable or not I/O-space)");
        return;
    }

    g_wdat = w;
    g_entries = (const struct acpi_wdat_entry *)
                ((const uint8_t *)w + w->header_length);
    g_entry_count = w->entries;

    /* countdown = boot timeout / period, clamped into [min_count, max_count].
     * Then check the ACTUAL representable timeout: if the hardware cannot hold
     * at least the safety floor, leave it UNARMED rather than reboot mid-boot. */
    countdown = (uint64_t)HW_WD_BOOT_TIMEOUT_MS / w->timer_period;
    if (countdown < w->min_count) countdown = w->min_count;
    if (countdown > w->max_count) countdown = w->max_count;
    actual_ms = countdown * (uint64_t)w->timer_period;
    if (actual_ms < HW_WD_MIN_SAFE_MS) {
        klog(LOG_WARN, "watchdog",
             "HW watchdog: none (WDAT max timeout %u ms < %u ms safe floor)",
             (uint64_t)actual_ms, (uint64_t)HW_WD_MIN_SAFE_MS);
        return;
    }

    /* boot-status: was the previous reset watchdog-triggered? (diagnostic). */
    {
        uint32_t status = 0;
        if (wdat_run_action(ACPI_WDAT_GET_STATUS, 0, &status) && status)
            klog(LOG_WARN, "watchdog",
                 "previous boot was watchdog-reset (WDAT status=0x%x)",
                 (uint64_t)status);
        wdat_run_action(ACPI_WDAT_SET_STATUS, 0, (uint32_t *)0);  /* clear */
    }

    /* Arm: program countdown, load it, start. Each action MUST execute (a
     * required action with no matching entry was rejected by validation, but
     * verify the run count so a silent no-op never reports "armed"). */
    if (wdat_run_action(ACPI_WDAT_SET_COUNTDOWN, (uint32_t)countdown, (uint32_t *)0) == 0 ||
        wdat_run_action(ACPI_WDAT_RESET, 0, (uint32_t *)0) == 0 ||
        wdat_run_action(ACPI_WDAT_SET_RUNNING_STATE, 0, (uint32_t *)0) == 0) {
        klog(LOG_WARN, "watchdog",
             "HW watchdog: none (WDAT arm action did not execute)");
        g_wdat = (const struct acpi_wdat *)0;
        g_entries = (const struct acpi_wdat_entry *)0;
        g_entry_count = 0;
        return;
    }

    g_kind = HW_WD_WDAT;
    g_armed = 1;
    klog(LOG_INFO, "watchdog",
         "HW watchdog: WDAT armed (%u ms; %u counts x %u ms)",
         (uint64_t)actual_ms, (uint64_t)countdown, (uint64_t)w->timer_period);
}

void hw_watchdog_pet(void)
{
    if (g_armed && g_kind == HW_WD_WDAT)
        wdat_run_action(ACPI_WDAT_RESET, 0, (uint32_t *)0);
}

int hw_watchdog_boot_handoff(void)
{
    uint32_t running = 0;
    uint32_t ran;

    if (!g_armed || g_kind != HW_WD_WDAT)
        return 0;                              /* nothing armed */

    wdat_run_action(ACPI_WDAT_SET_STOPPED_STATE, 0, (uint32_t *)0);

    /* Readback-verify the stop actually took. Only trust the readback if a
     * GET_RUNNING_STATE entry executed (validation guarantees one exists). */
    ran = wdat_run_action(ACPI_WDAT_GET_RUNNING_STATE, 0, &running);
    if (ran == 0 || running) {
        klog(LOG_ERROR, "watchdog",
             "HW watchdog disarm not confirmed (ran=%u running=0x%x); retrying",
             (uint64_t)ran, (uint64_t)running);
        wdat_run_action(ACPI_WDAT_RESET, 0, (uint32_t *)0);  /* buy time */
        wdat_run_action(ACPI_WDAT_SET_STOPPED_STATE, 0, (uint32_t *)0);
        ran = wdat_run_action(ACPI_WDAT_GET_RUNNING_STATE, 0, &running);
        if (ran == 0 || running) {
            klog(LOG_FATAL, "watchdog",
                 "HW watchdog DISARM FAILED -- refusing to enter the desktop "
                 "armed (it would reboot with no runtime petter)");
            return -1;                         /* caller MUST NOT proceed */
        }
    }
    g_armed = 0;
    klog(LOG_INFO, "watchdog", "HW watchdog disarmed at boot handoff");
    return 0;
}

enum hw_watchdog_kind hw_watchdog_kind(void)
{
    return g_kind;
}
