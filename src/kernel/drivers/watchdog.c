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
static uint32_t g_countdown = 0;     /* configured reload count (WRITE_COUNTDOWN) */
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

/* Execute the WDAT instruction entries for `action`, in table order, running
 * ONLY the entries whose direction matches `expect_write` (1 = SET/RESET write
 * actions, 0 = GET read actions). Skipping the wrong direction is the execution
 * guard: a stray write entry under a GET action must not poke a register during
 * a readback, and a stray read under a SET must not be mistaken for the write.
 *   - WRITE_VALUE writes the entry's own value; WRITE_COUNTDOWN writes the
 *     configured reload count g_countdown (NOT a per-call param -- a 0 param
 *     would zero the countdown and trip an immediate reset).
 *   - For a READ entry the WDAT result is a compare: the action "matches" iff
 *     (reg & mask) == (value & mask). *out_matched (if non-NULL) is set to 1
 *     when the LAST executed read entry matched, else 0.
 * Returns the number of entries that actually executed -- 0 means the action is
 * absent / unusable / wrong-direction, so the caller treats it as a failure,
 * not a no-op success. */
static uint32_t wdat_run_action(uint8_t action, int expect_write, int *out_matched)
{
    uint32_t i;
    uint32_t ran = 0;

    if (out_matched)
        *out_matched = 0;

    for (i = 0; i < g_entry_count; i++) {
        const struct acpi_wdat_entry *e = &g_entries[i];
        uint8_t instr;
        int is_write;
        if (e->action != action)
            continue;
        if (!gas_usable(&e->register_region))
            continue;                          /* validated at init; belt+braces */
        instr = e->instruction & ACPI_WDAT_INSTRUCTION_MASK;
        is_write = (instr == ACPI_WDAT_WRITE_VALUE ||
                    instr == ACPI_WDAT_WRITE_COUNTDOWN);
        if (is_write != (expect_write != 0))
            continue;                          /* skip wrong-direction entries */

        if (is_write) {
            uint32_t x = (instr == ACPI_WDAT_WRITE_COUNTDOWN) ? g_countdown : e->value;
            uint32_t out = x & e->mask;
            if (e->instruction & ACPI_WDAT_PRESERVE_REGISTER) {
                uint32_t cur = gas_read(&e->register_region);
                out |= (cur & ~e->mask);
            }
            gas_write(&e->register_region, out);
        } else { /* READ_VALUE / READ_COUNTDOWN: compare against the entry value */
            uint32_t r = gas_read(&e->register_region) & e->mask;
            if (out_matched)
                *out_matched = (r == (e->value & e->mask)) ? 1 : 0;
        }
        ran++;
    }
    return ran;
}

/* True if `action` is implemented with the correct instruction CLASS: at least
 * one usable entry has its instruction in [ilo, ihi] AND no usable entry for
 * that action falls OUTSIDE [ilo, ihi]. The "no out-of-class entry" half is
 * what makes execution safe: wdat_run_action runs every same-direction entry,
 * so a stray wrong-operand entry (e.g. a WRITE_COUNTDOWN under SET_RUNNING_STATE
 * that would clobber the state register with the reload count, or a WRITE_VALUE
 * under SET_COUNTDOWN that would overwrite the programmed countdown) must make
 * the whole table fail validation -> HW_WD_NONE rather than arm wrongly. */
static int wdat_has_action_instr(const struct acpi_wdat_entry *ent, uint32_t n,
                                 uint8_t action, uint8_t ilo, uint8_t ihi)
{
    uint32_t i;
    int found = 0;
    for (i = 0; i < n; i++) {
        uint8_t instr;
        if (ent[i].action != action || !gas_usable(&ent[i].register_region))
            continue;
        instr = ent[i].instruction & ACPI_WDAT_INSTRUCTION_MASK;
        if (instr < ilo || instr > ihi)
            return 0;                          /* wrong-class entry -> reject table */
        found = 1;
    }
    return found;
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
    /* Every action init/pet/disarm/verify uses must be present, usable, AND
     * implemented with the right instruction class (every entry for the action,
     * not just one -- see wdat_has_action_instr):
     *   SET_RUNNING_STATE / SET_STOPPED_STATE -- WRITE_VALUE only (the state
     *     pattern lives in the entry value; WRITE_COUNTDOWN would write the
     *     reload count to the state register instead).
     *   RESET -- WRITE_VALUE or WRITE_COUNTDOWN (a reload writes either the
     *     entry value or the configured countdown).
     *   SET_COUNTDOWN -- WRITE_COUNTDOWN only (programs the reload count).
     *   GET_RUNNING_STATE -- READ_VALUE / READ_COUNTDOWN (a write would never
     *     produce a readback -> false disarm confirmation). */
    return wdat_has_action_instr(ent, w->entries, ACPI_WDAT_SET_RUNNING_STATE,
                                 ACPI_WDAT_WRITE_VALUE, ACPI_WDAT_WRITE_VALUE) &&
           wdat_has_action_instr(ent, w->entries, ACPI_WDAT_SET_STOPPED_STATE,
                                 ACPI_WDAT_WRITE_VALUE, ACPI_WDAT_WRITE_VALUE) &&
           wdat_has_action_instr(ent, w->entries, ACPI_WDAT_RESET,
                                 ACPI_WDAT_WRITE_VALUE, ACPI_WDAT_WRITE_COUNTDOWN) &&
           wdat_has_action_instr(ent, w->entries, ACPI_WDAT_SET_COUNTDOWN,
                                 ACPI_WDAT_WRITE_COUNTDOWN, ACPI_WDAT_WRITE_COUNTDOWN) &&
           wdat_has_action_instr(ent, w->entries, ACPI_WDAT_GET_RUNNING_STATE,
                                 ACPI_WDAT_READ_VALUE, ACPI_WDAT_READ_COUNTDOWN);
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

    g_countdown = (uint32_t)countdown;   /* WRITE_COUNTDOWN reload value */

    /* boot-status: was the previous reset watchdog-triggered? (diagnostic).
     * GET_STATUS/SET_STATUS are OPTIONAL and NOT covered by the required-action
     * class validation, so gate each on its own instruction class before
     * running it -- otherwise a SET_STATUS entry encoded as WRITE_COUNTDOWN
     * would write the reload count into the status register. GET_STATUS is a
     * compare-read: matched == the watchdog-reset condition. A class mismatch
     * (or no such entry) just skips the diagnostic, which is the safe direction. */
    if (wdat_has_action_instr(g_entries, g_entry_count, ACPI_WDAT_GET_STATUS,
                              ACPI_WDAT_READ_VALUE, ACPI_WDAT_READ_COUNTDOWN)) {
        int status_set = 0;
        if (wdat_run_action(ACPI_WDAT_GET_STATUS, 0, &status_set) && status_set)
            klog(LOG_WARN, "watchdog",
                 "previous boot was watchdog-reset (WDAT status condition met)");
    }
    if (wdat_has_action_instr(g_entries, g_entry_count, ACPI_WDAT_SET_STATUS,
                              ACPI_WDAT_WRITE_VALUE, ACPI_WDAT_WRITE_VALUE))
        wdat_run_action(ACPI_WDAT_SET_STATUS, 1, (int *)0);      /* clear (write) */

    /* Arm: program countdown, load it, start. Each action MUST execute (a
     * required action with no matching entry was rejected by validation, but
     * verify the run count so a silent no-op never reports "armed"). */
    if (wdat_run_action(ACPI_WDAT_SET_COUNTDOWN, 1, (int *)0) == 0 ||
        wdat_run_action(ACPI_WDAT_RESET, 1, (int *)0) == 0 ||
        wdat_run_action(ACPI_WDAT_SET_RUNNING_STATE, 1, (int *)0) == 0) {
        klog(LOG_WARN, "watchdog",
             "HW watchdog: none (WDAT arm action did not execute)");
        g_wdat = (const struct acpi_wdat *)0;
        g_entries = (const struct acpi_wdat_entry *)0;
        g_entry_count = 0;
        g_countdown = 0;
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
        wdat_run_action(ACPI_WDAT_RESET, 1, (int *)0);
}

int hw_watchdog_boot_handoff(void)
{
    int matched = 0;                           /* GET_RUNNING_STATE: running? */
    uint32_t ran;

    if (!g_armed || g_kind != HW_WD_WDAT)
        return 0;                              /* nothing armed */

    wdat_run_action(ACPI_WDAT_SET_STOPPED_STATE, 1, (int *)0);

    /* Readback-verify the stop actually took. Only trust the readback if a
     * GET_RUNNING_STATE entry executed (validation guarantees one exists);
     * matched == the read matched the running pattern -> still running. */
    ran = wdat_run_action(ACPI_WDAT_GET_RUNNING_STATE, 0, &matched);
    if (ran == 0 || matched) {
        klog(LOG_ERROR, "watchdog",
             "HW watchdog disarm not confirmed (ran=%u running=%d); retrying",
             (uint64_t)ran, matched);
        wdat_run_action(ACPI_WDAT_RESET, 1, (int *)0);       /* buy time */
        wdat_run_action(ACPI_WDAT_SET_STOPPED_STATE, 1, (int *)0);
        ran = wdat_run_action(ACPI_WDAT_GET_RUNNING_STATE, 0, &matched);
        if (ran == 0 || matched) {
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
