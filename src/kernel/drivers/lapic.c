/* ============================================================================
 * lapic.c — Local APIC driver
 *
 * Initializes the BSP's (and later AP's) Local APIC for interrupt delivery.
 * Replaces the legacy 8259 PIC for EOI and adds IPI support for SMP.
 *
 * The LAPIC base address is discovered from the ACPI MADT (default
 * 0xFEE00000). All registers are memory-mapped and identity-mapped in
 * our page tables (the first 4 GiB is identity-mapped at boot).
 *
 * Key operations:
 *   1. Enable LAPIC via Spurious Vector Register (SVR)
 *   2. Set Task Priority to 0 (accept all interrupts)
 *   3. Mask ALL LVT entries (following xv6 pattern)
 *   4. Provide EOI, IPI send, and LAPIC timer setup
 *
 * xv6 pattern: mask LINT0, LINT1, PCINT, Thermal; only enable Timer.
 * This avoids all PIC/ExtINT interference on VBox NEM and real hardware.
 * ============================================================================ */

#include "kernel/drivers/lapic.h"
#include "kernel/drivers/pit.h"
#include "kernel/idt.h"
#include "kernel/sched/task.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/barrier.h"

/* ---- State ---- */

static volatile uint32_t *lapic_base = (volatile uint32_t *)0;
static int lapic_ready = 0;

/* ---- LVT registers not in header ---- */
#define LAPIC_REG_LVT_THERMAL  0x330
#define LAPIC_REG_LVT_PERF     0x340

/* ---- Internal helpers ---- */

static inline __attribute__((unused)) void outb_lapic(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline __attribute__((unused)) uint8_t inb_lapic(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- Register access ---- */

uint32_t lapic_read(uint32_t reg)
{
    return lapic_base[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t val)
{
    lapic_base[reg / 4] = val;
}

/* ---- Public API ---- */

void lapic_init(void)
{
    uint32_t base_addr;
    uint32_t ver;
    uint32_t max_lvt;

    base_addr = acpi_get_lapic_base();
    if (base_addr == 0) {
        klog(LOG_WARN, "lapic", "No LAPIC base — staying with PIC");
        return;
    }

    lapic_base = (volatile uint32_t *)(uintptr_t)base_addr;


    /* Enable the APIC via the IA32_APIC_BASE MSR (set bit 11 = global enable)
     * This is required on some hardware before MMIO access works. */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
        lo |= (1 << 11);  /* global enable */
        __asm__ volatile("wrmsr" : : "c"(0x1B), "a"(lo), "d"(hi));
    }


    /* Set Spurious Vector Register: enable APIC + set spurious vector */
    lapic_write(LAPIC_REG_SVR,
                LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    /* Set Task Priority to 0 — accept all interrupt priorities */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Clear Error Status Register (write twice per Intel manual) */
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);

    /* ---- Mask ALL LVT entries (xv6 pattern) ----
     * This prevents any stray interrupts during the PIC→APIC transition.
     * Timer will be unmasked later by lapic_timer_init().
     * LINT0/LINT1/PCINT/Thermal must stay masked when using IOAPIC. */


    /* Mask Timer */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    /* Mask LINT0 — NO ExtINT, just masked. Prevents PIC interference. */
    lapic_write(LAPIC_REG_LVT_LINT0, LVT_MASKED);

    /* Mask LINT1 — NMI can be problematic; mask during transition */
    lapic_write(LAPIC_REG_LVT_LINT1, LVT_MASKED);

    /* Mask Error */
    lapic_write(LAPIC_REG_LVT_ERROR, LVT_MASKED);

    /* Read version to determine max LVT entries */
    ver = lapic_read(LAPIC_REG_VERSION);
    max_lvt = ((ver >> 16) & 0xFF) + 1;

    /* Mask Performance Counter and Thermal if they exist (maxLVT >= 5/6) */
    if (max_lvt >= 5) {
        lapic_write(LAPIC_REG_LVT_PERF, LVT_MASKED);
    }
    if (max_lvt >= 6) {
        lapic_write(LAPIC_REG_LVT_THERMAL, LVT_MASKED);
    }

    /* Send EOI to clear any pending interrupts from init */
    lapic_write(LAPIC_REG_EOI, 0);

    /* Enable error vector now (after masking everything else) */
    lapic_write(LAPIC_REG_LVT_ERROR, 0xFE); /* vector 0xFE for errors */

    /* Send Init Level De-Assert to synchronize arbitration IDs (xv6) */
    lapic_write(LAPIC_REG_ICR_HI, 0);
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_INIT | ICR_DEST_ALL |
                ICR_LEVEL_DEASSERT | ICR_TRIGGER_LEVEL);
    while (lapic_read(LAPIC_REG_ICR_LO) & (1 << 12))
        barrier();

    /* Final EOI */
    lapic_write(LAPIC_REG_EOI, 0);

    lapic_ready = 1;

    klog(LOG_INFO, "lapic",
         "LAPIC enabled: base=%x, ID=%u, ver=%x, maxLVT=%u",
         (uint64_t)base_addr,
         (uint64_t)((lapic_read(LAPIC_REG_ID) >> 24) & 0xFF),
         (uint64_t)(ver & 0xFF),
         (uint64_t)max_lvt);

}

void lapic_init_ap(void)
{
    if (!lapic_base)
        return;

    /* Enable LAPIC via SVR */
    lapic_write(LAPIC_REG_SVR,
                LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    /* Accept all interrupts */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Clear ESR */
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);

    /* Mask ALL LVT entries on AP (xv6 pattern) */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, LVT_MASKED);

    {
        uint32_t ver = lapic_read(LAPIC_REG_VERSION);
        uint32_t max_lvt = ((ver >> 16) & 0xFF) + 1;
        if (max_lvt >= 5)
            lapic_write(LAPIC_REG_LVT_PERF, LVT_MASKED);
        if (max_lvt >= 6)
            lapic_write(LAPIC_REG_LVT_THERMAL, LVT_MASKED);
    }

    /* Error vector */
    lapic_write(LAPIC_REG_LVT_ERROR, 0xFE);

    /* Clear pending */
    lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_eoi(void)
{
    if (lapic_base)
        lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_id(void)
{
    if (!lapic_base)
        return 0;
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFF;
}

int lapic_available(void)
{
    return lapic_ready;
}

/* ---- IPI ---- */

/* Wait for the ICR delivery status bit to clear */
static void ipi_wait_delivery(void)
{
    /* ICR low bit 12 = delivery status: 0=idle, 1=pending */
    while (lapic_read(LAPIC_REG_ICR_LO) & (1 << 12))
        barrier();
}

void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector)
{
    if (!lapic_base)
        return;

    ipi_wait_delivery();

    /* Set target APIC ID in ICR high (bits 24-31) */
    lapic_write(LAPIC_REG_ICR_HI,
                (uint32_t)target_apic_id << 24);

    /* Send: fixed delivery, edge-triggered, assert, dest field */
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_FIXED | ICR_DEST_FIELD |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                (uint32_t)vector);
}

void lapic_send_ipi_all_but_self(uint8_t vector)
{
    if (!lapic_base)
        return;

    ipi_wait_delivery();

    /* Shorthand: all-but-self, no need to set destination */
    lapic_write(LAPIC_REG_ICR_HI, 0);
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_FIXED | ICR_DEST_ALL_BUT_SELF |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                (uint32_t)vector);
}

void lapic_send_init(uint8_t target_apic_id)
{
    if (!lapic_base)
        return;

    ipi_wait_delivery();

    /* INIT IPI: level-triggered, assert */
    lapic_write(LAPIC_REG_ICR_HI,
                (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_INIT | ICR_DEST_FIELD |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);

    ipi_wait_delivery();

    /* De-assert INIT */
    lapic_write(LAPIC_REG_ICR_HI,
                (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_INIT | ICR_DEST_FIELD |
                ICR_LEVEL_DEASSERT | ICR_TRIGGER_LEVEL);
}

void lapic_send_sipi(uint8_t target_apic_id, uint8_t vector_page)
{
    if (!lapic_base)
        return;

    ipi_wait_delivery();

    /* SIPI: startup vector = page number (phys addr = vector_page * 0x1000) */
    lapic_write(LAPIC_REG_ICR_HI,
                (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_STARTUP | ICR_DEST_FIELD |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                (uint32_t)vector_page);
}

/* ---- LAPIC Timer ---- */

/* Calibrated ticks per millisecond (0 = uncalibrated / fallback) */
static uint32_t cal_ticks_per_ms = 0;

/* Calibration window in milliseconds — shared by all tiers */
#define CAL_MS  10

/* ---- MSR / CPUID helpers for calibration ---- */

static inline uint64_t cal_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cal_cpuid(uint32_t leaf,
                              uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf));
}

/* ---- Tier 1: Architectural Fast-Path (Zero Delay, No PIT) ---- */

#include "kernel/cpuid_platform.h"

/* Hyper-V: MSR 0x40000023 (HV_X64_MSR_APIC_FREQUENCY)
 * Returns exact LAPIC frequency in Hz — 0ns latency, no hardware probing. */
#define HV_MSR_APIC_FREQUENCY  0x40000023

static int cal_try_hyperv_msr(void)
{
    uint64_t freq;

    if (platform_get() != PLATFORM_HYPERV)
        return 0;

    freq = cal_rdmsr(HV_MSR_APIC_FREQUENCY);
    if (freq == 0 || freq > 0xFFFFFFFFULL)
        return 0;

    cal_ticks_per_ms = (uint32_t)(freq / 1000);
    klog(LOG_INFO, "lapic",
         "Tier 1: Hyper-V MSR 0x40000023 → %u ticks/ms (%u MHz bus)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000));
    return 1;
}

/* VMware / KVM: CPUID leaf 0x40000010
 * EBX = virtual APIC bus frequency in kHz — already ticks/ms! */
static int cal_try_vmware_cpuid(void)
{
    uint32_t eax, ebx, ecx, edx;
    platform_id_t plat = platform_get();

    if (plat != PLATFORM_VMWARE && plat != PLATFORM_QEMU_KVM)
        return 0;

    cal_cpuid(0x40000010, &eax, &ebx, &ecx, &edx);
    if (ebx == 0)
        return 0;

    cal_ticks_per_ms = ebx;  /* kHz = ticks per ms */
    klog(LOG_INFO, "lapic",
         "Tier 1: %s CPUID 0x40000010 → %u ticks/ms (%u MHz bus)",
         platform_name(),
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000));
    return 1;
}

/* Intel CPUID leaf 0x15: Time Stamp Counter / Core Crystal Clock
 *   EAX = denominator (TSC / crystal ratio)
 *   EBX = numerator   (TSC / crystal ratio)
 *   ECX = crystal frequency in Hz (0 on some CPUs → use lookup table)
 *
 * TSC freq = ECX * EBX / EAX.
 * On most Intel CPUs, LAPIC bus freq ≈ TSC freq (the LAPIC timer is
 * clocked from the core crystal, same as TSC). */
static int cal_try_cpuid_15h(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_leaf;
    uint64_t crystal_hz;
    uint64_t tsc_freq;

    /* Check max CPUID leaf */
    cal_cpuid(0x00, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf < 0x15)
        return 0;

    cal_cpuid(0x15, &eax, &ebx, &ecx, &edx);
    if (eax == 0 || ebx == 0)
        return 0;

    crystal_hz = ecx;

    /* ECX == 0 on some CPUs — use known crystal frequencies.
     * Check CPUID.01H model/family for identification. */
    if (crystal_hz == 0) {
        uint32_t eax1, ebx1, ecx1, edx1;
        uint32_t family, model;

        cal_cpuid(0x01, &eax1, &ebx1, &ecx1, &edx1);
        family = (eax1 >> 8) & 0xF;
        model  = (eax1 >> 4) & 0xF;
        if (family == 6)
            model |= ((eax1 >> 16) & 0xF) << 4;

        /* Known crystal frequencies by CPU model:
         * Skylake/Kaby Lake/Coffee Lake: 24 MHz
         * Atom Goldmont/Tremont: 19.2 MHz
         * Reference: Intel SDM Vol. 3 Table 18-85 */
        if (model == 0x55 || model == 0x4E || model == 0x5E ||
            model == 0x8E || model == 0x9E || model == 0xA5 ||
            model == 0xA6 || model == 0xA7) {
            crystal_hz = 24000000;   /* 24 MHz — Skylake+ */
        } else if (model == 0x5C || model == 0x5F || model == 0x7A ||
                   model == 0x86) {
            crystal_hz = 19200000;   /* 19.2 MHz — Atom */
        } else {
            return 0;  /* Unknown model — can't determine crystal */
        }
    }

    tsc_freq = crystal_hz * ebx / eax;
    if (tsc_freq == 0)
        return 0;

    cal_ticks_per_ms = (uint32_t)(tsc_freq / 1000);
    klog(LOG_INFO, "lapic",
         "Tier 1: CPUID 0x15 → %u ticks/ms (crystal=%u Hz, ratio=%u/%u)",
         (uint64_t)cal_ticks_per_ms,
         crystal_hz,
         (uint64_t)ebx, (uint64_t)eax);
    return 1;
}

/* ---- Tier 2: Modern Hardware Timers (10ms Delay, No PIT) ---- */

#include "kernel/acpi.h"

/* HPET register offsets (MMIO) */
#define HPET_CAP_REG    0x000   /* General Capabilities — bits 32-63: period (fs) */
#define HPET_CFG_REG    0x010   /* General Configuration */
#define HPET_COUNTER    0x0F0   /* Main Counter Value (64-bit) */

/* Read a 32-bit HPET register via MMIO */
static inline uint32_t hpet_read32(uint64_t base, uint32_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(base + offset);
    return *reg;
}

/* Read a 64-bit HPET register via MMIO */
static inline uint64_t hpet_read64(uint64_t base, uint32_t offset)
{
    volatile uint64_t *reg = (volatile uint64_t *)(uintptr_t)(base + offset);
    return *reg;
}

/* Write a 32-bit HPET register */
static inline void hpet_write32(uint64_t base, uint32_t offset, uint32_t val)
{
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(base + offset);
    *reg = val;
}

/* HPET-based calibration: read HPET counter, run LAPIC for 10ms, measure ticks.
 * No PIT hardware touched.  HPET is memory-mapped (identity-mapped in first 4 GiB). */
static int cal_try_hpet(void)
{
    uint64_t hpet_base;
    uint64_t cap;
    uint32_t period_fs;    /* HPET period in femtoseconds */
    uint64_t hpet_freq;
    uint64_t start_hpet, target_hpet, cur_hpet;
    uint32_t lapic_remaining, lapic_elapsed;

    hpet_base = acpi_get_hpet_base();
    if (hpet_base == 0)
        return 0;

    /* Read HPET capabilities — upper 32 bits = period in femtoseconds */
    cap = hpet_read64(hpet_base, HPET_CAP_REG);
    period_fs = (uint32_t)(cap >> 32);
    if (period_fs == 0 || period_fs > 100000000) {
        /* Invalid period (>100ns per tick is unreasonable) */
        return 0;
    }

    /* freq = 10^15 / period_fs */
    hpet_freq = 1000000000000000ULL / period_fs;

    /* Enable HPET counter if not already running */
    hpet_write32(hpet_base, HPET_CFG_REG,
                 hpet_read32(hpet_base, HPET_CFG_REG) | 0x01);

    /* Start LAPIC timer from max (one-shot, masked) */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT | 0xFF);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* Wait 10ms worth of HPET ticks */
    start_hpet = hpet_read64(hpet_base, HPET_COUNTER);
    target_hpet = start_hpet + (hpet_freq * CAL_MS / 1000);

    do {
        cur_hpet = hpet_read64(hpet_base, HPET_COUNTER);
    } while (cur_hpet < target_hpet);

    /* Read LAPIC remaining count */
    lapic_remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    lapic_elapsed = 0xFFFFFFFF - lapic_remaining;
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    if (lapic_elapsed < 1000)
        return 0;  /* Too few ticks — unreliable */

    cal_ticks_per_ms = lapic_elapsed / CAL_MS;
    klog(LOG_INFO, "lapic",
         "Tier 2: HPET calibration → %u ticks/ms (%u MHz bus, HPET %u MHz)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000),
         (uint64_t)(hpet_freq / 1000000));
    return 1;
}

/* ACPI PM Timer frequency: exactly 3.579545 MHz (ACPI spec §4.8.3.3) */
#define PMTIMER_FREQ     3579545
/* 10ms worth of PM Timer ticks */
#define PMTIMER_10MS     (PMTIMER_FREQ / 100)   /* ≈ 35795 */
#define PMTIMER_24BIT_MASK  0x00FFFFFF

/* Read the ACPI PM Timer counter (32-bit I/O read) */
static inline uint32_t pmtimer_read(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* PM Timer-based calibration: pace a 10ms LAPIC window via the ACPI PM Timer.
 * The PM Timer is an I/O port, NOT PIT hardware — it works even when
 * the 8254 PIT is absent (Hyper-V Gen 2, HW-reduced ACPI). */
static int cal_try_pmtimer(void)
{
    uint16_t port;
    int is_32bit;
    uint32_t mask;
    uint32_t start_pm, cur_pm, elapsed_pm;
    uint32_t lapic_remaining, lapic_elapsed;

    port = acpi_get_pmtimer_port();
    if (port == 0)
        return 0;

    is_32bit = acpi_pmtimer_is_32bit();
    mask = is_32bit ? 0xFFFFFFFF : PMTIMER_24BIT_MASK;

    /* Start LAPIC timer from max (one-shot, masked) */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT | 0xFF);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* Wait 10ms worth of PM Timer ticks */
    start_pm = pmtimer_read(port) & mask;
    do {
        cur_pm = pmtimer_read(port) & mask;
        elapsed_pm = (cur_pm - start_pm) & mask;
    } while (elapsed_pm < PMTIMER_10MS);

    /* Read LAPIC remaining count */
    lapic_remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    lapic_elapsed = 0xFFFFFFFF - lapic_remaining;
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    if (lapic_elapsed < 1000)
        return 0;  /* Too few ticks — unreliable */

    cal_ticks_per_ms = lapic_elapsed / CAL_MS;
    klog(LOG_INFO, "lapic",
         "Tier 2: PM Timer calibration → %u ticks/ms (%u MHz bus, port=%x %s)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000),
         (uint64_t)port,
         is_32bit ? "32-bit" : "24-bit");
    return 1;
}

/* Forward declaration — defined below after PIT I/O helpers */
static int cal_try_pit(void);

/* ---- Calibration Waterfall ----
 * THE single calibration entry point.  Cascades through 3 tiers:
 *   Tier 1: MSR/CPUID  (instant, no PIT)
 *   Tier 2: HPET / PM Timer (~10ms, no PIT)
 *   Tier 3: PIT channel 2 (~10ms, legacy only)
 * Falls back to hardcoded estimate if ALL tiers fail.
 */
void lapic_timer_calibrate(void)
{
    if (!lapic_base)
        return;

    /* Tier 1: Instant frequency from MSR/CPUID */
    if (cal_try_hyperv_msr() || cal_try_vmware_cpuid() || cal_try_cpuid_15h()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 1 succeeded — no PIT/HPET needed");
        return;
    }

    /* Tier 2: Modern hardware timers (HPET → PM Timer) */
    if (cal_try_hpet() || cal_try_pmtimer()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 2 succeeded — no PIT needed");
        return;
    }

    /* Tier 3: Legacy PIT (only if PCAT_COMPAT=1 && !HW_REDUCED) */
    if (cal_try_pit()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 3 succeeded — PIT channel 2");
        return;
    }

    /* All tiers failed — use conservative hardcoded estimate */
    cal_ticks_per_ms = 100;
    klog(LOG_WARN, "lapic",
         "All calibration tiers failed — using hardcoded %u ticks/ms",
         (uint64_t)cal_ticks_per_ms);
}

/* PIT base frequency (Hz) — the 8254 oscillator runs at this exact rate */
#define PIT_OSC_FREQ  1193182
#define CAL_PIT_COUNT (PIT_OSC_FREQ * CAL_MS / 1000)  /* ~11932 */

/* Inline port I/O for calibration code */
static inline void cal_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t cal_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Tier 3: PIT channel 2 calibration (legacy fallback).
 *
 * SAFETY: Only touches PIT ports if BOTH:
 *   1. acpi_pcat_compat() == 1  (MADT says PIC/PIT/RTC exist)
 *   2. acpi_hw_reduced() == 0   (FADT does NOT set HW_REDUCED_ACPI)
 *
 * How it works:
 *   1. Program PIT channel 2 in one-shot mode for CAL_MS milliseconds.
 *   2. Start the LAPIC timer counting down from 0xFFFFFFFF.
 *   3. Busy-wait for PIT output bit (port 0x61, bit 5) to go high.
 *   4. Read remaining LAPIC count → elapsed = 0xFFFFFFFF - remaining.
 *   5. ticks_per_ms = elapsed / CAL_MS.
 *
 * If PIT polling hangs (VBox NEM, some Hyper-V configs), a spin-counter
 * timeout fires and we return 0 (failure). */
static int cal_try_pit(void)
{
    uint8_t gate;
    uint32_t lapic_remaining, lapic_elapsed;
    uint32_t timeout;

    /* Safety guard: never touch PIT ports on PIT-less platforms */
    if (!acpi_pcat_compat() || acpi_hw_reduced())
        return 0;

    if (!lapic_base)
        return 0;

    /* ---- 1. Prepare PIT channel 2 (speaker) ---- */

    /* Read current gate state; disable speaker output (bit 1), enable gate (bit 0) */
    gate = cal_inb(0x61);
    gate = (gate & 0xFC) | 0x01;     /* bit 0 = gate ON, bit 1 = speaker OFF */
    cal_outb(0x61, gate);

    /* Program PIT channel 2: mode 0 (one-shot), lobyte/hibyte, binary */
    cal_outb(0x43, 0xB0);  /* 10110000: ch2, lobyte/hibyte, mode 0, binary */
    cal_outb(0x42, (uint8_t)(CAL_PIT_COUNT & 0xFF));
    cal_outb(0x42, (uint8_t)((CAL_PIT_COUNT >> 8) & 0xFF));

    /* Re-arm gate: OFF then ON starts the countdown */
    gate = cal_inb(0x61);
    cal_outb(0x61, gate & ~0x01);  /* gate OFF */
    cal_outb(0x61, gate | 0x01);   /* gate ON → PIT starts counting */

    /* ---- 2. Start LAPIC timer from max value (one-shot, masked) ---- */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT | 0xFF);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* ---- 3. Wait for PIT output (bit 5 of port 0x61) ---- */
    timeout = 200000000;  /* generous spin timeout */
    while (!(cal_inb(0x61) & 0x20) && --timeout > 0)
        ;

    /* ---- 4. Read LAPIC timer current count ---- */
    lapic_remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    lapic_elapsed = 0xFFFFFFFF - lapic_remaining;

    /* Stop LAPIC timer */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    /* Disable PIT channel 2 gate */
    cal_outb(0x61, cal_inb(0x61) & ~0x01);

    /* ---- 5. Calculate ticks per ms ---- */
    if (timeout == 0 || lapic_elapsed < 1000) {
        /* Calibration failed — timeout or too few ticks */
        klog(LOG_WARN, "lapic",
             "Tier 3: PIT calibration timeout or too few ticks");
        return 0;
    }

    cal_ticks_per_ms = lapic_elapsed / CAL_MS;
    klog(LOG_INFO, "lapic",
         "Tier 3: PIT ch2 calibration → %u ticks/ms (%u MHz bus)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000));
    return 1;
}

uint32_t lapic_timer_ticks_per_ms(void)
{
    return cal_ticks_per_ms;
}

/* ---- LAPIC timer IRQ handler ----
 * Always handles preemptive scheduling.  Tick counting depends on
 * whether a PIT exists:
 *
 *   PCAT_COMPAT=1 (VBox, QEMU, most boards):
 *     PIT drives tick_count via pit_irq_handler (uses real host
 *     timers on QEMU TCG = wall-clock accurate).  LAPIC timer
 *     only does schedule().  This avoids a 10x boot regression
 *     on QEMU TCG where LAPIC timer runs from QEMU_CLOCK_VIRTUAL.
 *
 *   PCAT_COMPAT=0 (Hyper-V Gen 2, modern APIC-only boards):
 *     No PIT hardware — LAPIC timer drives BOTH tick_count and
 *     schedule().  No TCG concern because these platforms always
 *     have hardware virtualization (VT-x).
 *
 * Flag is set by lapic_timer_set_tick_source(). */
static uint8_t lapic_is_tick_source;

void lapic_timer_set_tick_source(int enable)
{
    lapic_is_tick_source = enable ? 1 : 0;
}

static uint64_t lapic_timer_handler(struct interrupt_frame *frame)
{
    if (lapic_is_tick_source)
        pit_tick_increment();
    lapic_eoi();
    return schedule(frame);
}

void lapic_timer_init(uint32_t hz)
{
    uint32_t icr;

    if (!lapic_base)
        return;

    /* Calculate ICR from calibrated frequency, or use xv6 fallback */
    if (cal_ticks_per_ms > 0) {
        icr = cal_ticks_per_ms * 1000 / hz;
    } else {
        /* xv6 hardcoded: ICR=10000000 with div=1 works on QEMU + most HW */
        icr = 10000000;
    }

    /* Register handler on dedicated LAPIC timer vector */
    idt_register_handler(LAPIC_TIMER_VECTOR, lapic_timer_handler);

    /* ---- Drain stale ISR bits ----
     * During early boot, hardware interrupts may fire before handlers are
     * registered (between IOAPIC route setup and sti).  If an interrupt
     * completes without proper EOI, its ISR bit stays set and blocks ALL
     * interrupts in the same priority class (same 16-vector group).
     *
     * The LAPIC timer fires on LAPIC_TIMER_VECTOR (priority class 2 = vec
     * 32-47).  A stuck ISR bit anywhere in 32-47 (e.g., mouse vec 44)
     * prevents the engine from delivering our timer interrupt.
     *
     * Fix: send multiple EOIs to drain any stale ISR bits before starting
     * the timer.  lapic_eoi() clears the highest-priority ISR bit each
     * time; 16 iterations clears the entire 32-47 class. */
    {
        uint32_t drain;
        uint32_t isr_before = lapic_read(0x110);  /* ISR bits 32-63 */
        for (drain = 0; drain < 16; drain++)
            lapic_eoi();
        if (isr_before) {
            klog(LOG_WARN, "lapic",
                 "Drained stale ISR bits: 0x%x (cleared %u EOIs)",
                 (uint64_t)isr_before, (uint64_t)drain);
        }
    }

    /* Configure LAPIC timer: divide by 1, periodic mode, dedicated vector */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_ICR, icr);

    klog(LOG_INFO, "lapic",
         "LAPIC timer: periodic, vec=%u, ICR=%u, div=1 (%s, %u Hz target)",
         (uint64_t)LAPIC_TIMER_VECTOR, (uint64_t)icr,
         cal_ticks_per_ms > 0 ? "calibrated" : "fallback",
         (uint64_t)hz);
}
