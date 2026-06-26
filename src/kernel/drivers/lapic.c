/* ============================================================================
 * lapic.c -- Local APIC driver
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
#include "kernel/msr.h"
#include "kernel/timer.h"
#include "kernel/cpuid.h"
#include "kernel/time/mono_clock.h"
#include "kernel/smp.h"
#include "kernel/drivers/pit.h"
#include "kernel/idt.h"
#include "kernel/sched/task.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"
#include "kernel/barrier.h"
#include "kernel/boot_info.h"
#include "kernel/mm/vmm.h"

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
        klog(LOG_WARN, "lapic", "No LAPIC base -- staying with PIC");
        return;
    }

    /* LAPIC registers are device MMIO: UC mapping is mandatory (cached
     * access returns stale ISR/IRR state and breaks EOI on real HW) */
    lapic_base = (volatile uint32_t *)vmm_map_mmio_uc(base_addr, 4096);
    if (!lapic_base) {
        klog(LOG_ERROR, "lapic",
             "UC map of LAPIC at 0x%x failed -- staying with PIC",
             (uint64_t)base_addr);
        return;
    }
    klog(LOG_DEBUG, "lapic", "init: base=0x%x (UC-mapped)", (uint64_t)base_addr);

    /* Enable the APIC via the IA32_APIC_BASE MSR (set bit 11 = global enable)
     * This is required on some hardware before MMIO access works. */
    {
        uint64_t apic_base = msr_read(MSR_IA32_APIC_BASE);
        apic_base |= (1UL << 11);  /* global enable */
        msr_write(MSR_IA32_APIC_BASE, apic_base);
    }
    klog(LOG_DEBUG, "lapic", "init: MSR enabled");


    /* Set Spurious Vector Register: enable APIC + set spurious vector */
    lapic_write(LAPIC_REG_SVR,
                LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    /* Set Task Priority to 0 -- accept all interrupt priorities */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Clear Error Status Register (write twice per Intel manual) */
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);
    klog(LOG_DEBUG, "lapic", "init: SVR/TPR/ESR done");

    /* ---- Mask ALL LVT entries (xv6 pattern) ----
     * This prevents any stray interrupts during the PIC->APIC transition.
     * Timer will be unmasked later by lapic_timer_init().
     * LINT0/LINT1/PCINT/Thermal must stay masked when using IOAPIC. */


    /* Mask Timer */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    /* Mask LINT0 -- NO ExtINT, just masked. Prevents PIC interference. */
    lapic_write(LAPIC_REG_LVT_LINT0, LVT_MASKED);

    /* Mask LINT1 -- NMI can be problematic; mask during transition */
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
    klog(LOG_DEBUG, "lapic", "init: LVT masked (maxLVT=%u)", (uint64_t)max_lvt);

    /* Send EOI to clear any pending interrupts from init */
    lapic_write(LAPIC_REG_EOI, 0);

    /* Enable error vector now (after masking everything else). Central registry
     * vector (vectors.h) -- distinct from the 0xFE TLB-shootdown IPI it used to
     * alias as a raw literal. */
    lapic_write(LAPIC_REG_LVT_ERROR, VECTOR_LAPIC_ERROR);

    /* Final EOI */
    lapic_write(LAPIC_REG_EOI, 0);

    lapic_ready = 1;

    klog(LOG_INFO, "lapic",
         "LAPIC enabled: base=0x%x, ID=%u, ver=0x%x, maxLVT=%u",
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

    /* Error vector (central registry; was a raw 0xFE aliasing TLB shootdown) */
    lapic_write(LAPIC_REG_LVT_ERROR, VECTOR_LAPIC_ERROR);

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

/* Wait for the ICR delivery status bit to clear (with timeout for WHPX) */
static void ipi_wait_delivery(void)
{
    /* ICR low bit 12 = delivery status: 0=idle, 1=pending */
    uint32_t timeout = 1000000;
    while ((lapic_read(LAPIC_REG_ICR_LO) & (1 << 12)) && timeout--)
        barrier();
}

/* Serialize the ICR HI/LO programming pair against timer-ISR re-entry
 * (TODO-09-boot S10). The CR-pin verify-IPI broadcast fires from the timer ISR;
 * if it preempted a thread-context sender between its ICR_HI and ICR_LO writes
 * it would clobber ICR_HI and the resumed sender's ICR_LO would target the
 * wrong CPU. Disabling local interrupts across the wait + HI + LO write makes
 * the sequence atomic vs the ISR. (The ISR sender already runs with IF=0; the
 * save/restore is a no-op there.) */
static inline uint64_t lapic_irq_save(void)
{
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void lapic_irq_restore(uint64_t f)
{
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector)
{
    uint64_t flags;

    if (!lapic_base)
        return;

    flags = lapic_irq_save();
    ipi_wait_delivery();

    /* Set target APIC ID in ICR high (bits 24-31) */
    lapic_write(LAPIC_REG_ICR_HI,
                (uint32_t)target_apic_id << 24);

    /* Send: fixed delivery, edge-triggered, assert, dest field */
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_FIXED | ICR_DEST_FIELD |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                (uint32_t)vector);
    lapic_irq_restore(flags);
}

/* ISR-safe IPI send: a SINGLE delivery-status check, never the spin loop.
 * Returns 1 if the IPI was written, 0 if a prior IPI is still pending (the
 * caller may retry on a later tick). For periodic/best-effort broadcasters
 * (the CR-pin verify-IPI, TODO-09-boot S10) that run in the timer ISR and must
 * not busy-wait there. The IRQ-save makes the check+HI+LO atomic vs any other
 * ICR writer regardless of calling context. */
int lapic_send_ipi_nowait(uint8_t target_apic_id, uint8_t vector)
{
    uint64_t flags;

    if (!lapic_base)
        return 0;

    flags = lapic_irq_save();
    if (lapic_read(LAPIC_REG_ICR_LO) & (1 << 12)) {
        lapic_irq_restore(flags);
        return 0;  /* delivery pending -- skip this round rather than spin in the ISR */
    }

    lapic_write(LAPIC_REG_ICR_HI, (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_FIXED | ICR_DEST_FIELD |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                (uint32_t)vector);
    lapic_irq_restore(flags);
    return 1;
}

void lapic_send_ipi_all_but_self(uint8_t vector)
{
    uint64_t flags;

    if (!lapic_base)
        return;

    flags = lapic_irq_save();
    ipi_wait_delivery();

    /* Shorthand: all-but-self, no need to set destination */
    lapic_write(LAPIC_REG_ICR_HI, 0);
    lapic_write(LAPIC_REG_ICR_LO,
                ICR_FIXED | ICR_DEST_ALL_BUT_SELF |
                ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                (uint32_t)vector);
    lapic_irq_restore(flags);
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

/* One-shot timer event armed (tickless-idle enabler): the timer ISR
 * auto-restores periodic mode when the one-shot fires so the scheduler
 * heartbeat can never silently stop (the future idle governor owns true
 * tickless policy). ISR + arm path both touch it: atomic accesses. */
static volatile uint32_t s_oneshot_armed;

/* Calibration window in milliseconds -- shared by all tiers */
#define CAL_MS  10

/* Plausibility bounds for a LAPIC timer rate, applied to EVERY tier
 * before it can mark calibration successful: firmware/hypervisor leaves
 * are untrusted input, and a wild value (0, or one that overflows the
 * 32-bit initial count when scaled) skews or storms the scheduler tick.
 * 1 MHz floor (slowest plausible crystal) through 10 GHz ceiling. */
#define CAL_TICKS_PER_MS_MIN  1000u
#define CAL_TICKS_PER_MS_MAX  10000000u
static int cal_value_plausible(uint32_t ticks_per_ms)
{
    return ticks_per_ms >= CAL_TICKS_PER_MS_MIN &&
           ticks_per_ms <= CAL_TICKS_PER_MS_MAX;
}

/* ---- MSR / CPUID helpers for calibration ---- */

/* cal_rdmsr replaced by msr_read() from kernel/msr.h */
#define cal_rdmsr(idx) msr_read(idx)

static inline uint64_t cal_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Wall-clock deadline for the measurement loops below. A bare iteration
 * cap (200M reads) maps to wildly different wall time per platform: with
 * interrupts off in timer_hal_init, a dead-but-advertised HPET could burn
 * tens of seconds before the cap fires. When the bootloader measured the
 * TSC, bound each tier at 4x the 10ms window instead; returns 0 (no
 * deadline, iteration cap only) when no TSC frequency is known. */
static inline uint64_t cal_deadline(void)
{
    uint64_t f = g_boot_info.timing.tsc_freq;
    /* Plausibility gate: the handoff value comes from a 1ms UEFI Stall
     * measurement and is advisory. Outside 1 MHz through 10 GHz treat it
     * as garbage and return 0 (iteration cap only) -- a tiny value would
     * round the 40ms delta toward 0 and instantly time out healthy
     * timers; the range also keeps the multiply far from u64 wrap. */
    if (f < 1000000ULL || f > 10000000000ULL)
        return 0;
    return cal_rdtsc() + (f * (CAL_MS * 4) / 1000);
}

static inline int cal_deadline_hit(uint64_t deadline)
{
    return deadline != 0 && cal_rdtsc() > deadline;
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
 * Returns exact LAPIC frequency in Hz -- 0ns latency, no hardware probing.
 * Availability is advertised by CPUID 0x40000003 EAX bit 11
 * (AccessFrequencyMsrs); platform_detect() decodes that into
 * HV_FLAG_APIC_FREQ_MSR. A constrained Hyper-V partition can omit the MSR,
 * so we gate on the flag AND fall through msr_try_read() per the
 * kernel-code-quality Gate 6 rule. */
#define HV_MSR_APIC_FREQUENCY  0x40000023

#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/msr.h"

static int cal_try_hyperv_msr(void)
{
    uint64_t freq;

    if (platform_get() != PLATFORM_HYPERV)
        return 0;
    if (!(g_boot_info.hv_flags & HV_FLAG_APIC_FREQ_MSR))
        return 0;

    if (msr_try_read(HV_MSR_APIC_FREQUENCY, &freq) != 0)
        return 0;
    if (freq == 0 || freq > 0xFFFFFFFFULL)
        return 0;

    cal_ticks_per_ms = (uint32_t)(freq / 1000);
    if (!cal_value_plausible(cal_ticks_per_ms)) {
        cal_ticks_per_ms = 0;
        return 0;
    }
    klog(LOG_INFO, "lapic",
         "Tier 1: Hyper-V MSR 0x40000023 -> %u ticks/ms (%u MHz bus)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000));
    return 1;
}

/* VMware / KVM: CPUID leaf 0x40000010
 * EBX = virtual APIC bus frequency in kHz -- already ticks/ms! */
static int cal_try_vmware_cpuid(void)
{
    uint32_t eax, ebx, ecx, edx;
    platform_id_t plat = platform_get();

    if (plat != PLATFORM_VMWARE && plat != PLATFORM_QEMU_KVM)
        return 0;

    /* Capability gate: platform_detect() sets HV_FLAG_APIC_FREQ_MSR only
     * after probing max_leaf >= 0x40000010 with nonzero EBX. Reading the
     * leaf without that gate would trust garbage on hypervisors that cap
     * the leaf range below 0x40000010 (most KVM configs). */
    if (!(g_boot_info.hv_flags & HV_FLAG_APIC_FREQ_MSR))
        return 0;

    cal_cpuid(0x40000010, &eax, &ebx, &ecx, &edx);
    if (ebx == 0)
        return 0;

    cal_ticks_per_ms = ebx;  /* kHz = ticks per ms */
    if (!cal_value_plausible(cal_ticks_per_ms)) {
        cal_ticks_per_ms = 0;
        return 0;
    }
    klog(LOG_INFO, "lapic",
         "Tier 1: %s CPUID 0x40000010 -> %u ticks/ms (%u MHz bus)",
         platform_name(),
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000));
    return 1;
}

/* Intel CPUID leaf 0x15: Time Stamp Counter / Core Crystal Clock
 *   EAX = denominator (TSC / crystal ratio)
 *   EBX = numerator   (TSC / crystal ratio)
 *   ECX = crystal frequency in Hz (0 on some CPUs -> use lookup table)
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

    /* ECX == 0 on some CPUs: derive the crystal from CPUID 0x16 base
     * frequency and the TSC/crystal ratio (crystal = TSC * EAX/EBX,
     * with TSC approximated by the 0x16 base MHz). A static per-model
     * lookup table is NOT used -- a stale entry would silently become
     * an authoritative calibration; machines without 0x16 fall through
     * to the measured HPET/PM/PIT tiers instead. */
    if (crystal_hz == 0) {
        uint32_t base_mhz, ebx16, ecx16, edx16;

        if (max_leaf < 0x16)
            return 0;
        cal_cpuid(0x16, &base_mhz, &ebx16, &ecx16, &edx16);
        if (base_mhz == 0)
            return 0;
        crystal_hz = (uint64_t)base_mhz * 1000000ULL * eax / ebx;
        if (crystal_hz == 0)
            return 0;
    }

    /* SDM: the APIC timer is clocked at the CORE CRYSTAL frequency, not
     * the TSC frequency (TSC = crystal * EBX/EAX runs ~100x faster on
     * Skylake+). Storing the TSC rate here would program a wildly fast
     * scheduler tick on bare metal. The ratio is validated above only
     * to confirm the leaf is populated. */
    tsc_freq = crystal_hz * ebx / eax;
    if (tsc_freq == 0)
        return 0;

    cal_ticks_per_ms = (uint32_t)(crystal_hz / 1000);
    if (!cal_value_plausible(cal_ticks_per_ms)) {
        cal_ticks_per_ms = 0;
        return 0;
    }
    klog(LOG_INFO, "lapic",
         "Tier 1: CPUID 0x15 -> %u ticks/ms (crystal=%u Hz, TSC ratio=%u/%u)",
         (uint64_t)cal_ticks_per_ms,
         crystal_hz,
         (uint64_t)ebx, (uint64_t)eax);
    return 1;
}

/* ---- Tier 1b: TSC-referenced calibration ----
 * Uses the bootloader's measured TSC frequency to time a 10ms window.
 * No MMIO, no I/O ports -- just TSC reads + LAPIC register reads.
 * Works on any platform where TSC frequency is known. */
static int cal_try_tsc_reference(void) __attribute__((unused));
static int cal_try_tsc_reference(void)
{
    uint64_t tsc_freq = g_boot_info.timing.tsc_freq;
    uint64_t tsc_start, tsc_target, tsc_now;
    uint32_t lapic_remaining, lapic_elapsed;

    if (tsc_freq == 0)
        return 0;

    if (!lapic_base)
        return 0;

    /* Start LAPIC timer from max (one-shot, masked) */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT | 0xFF);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* Wait 10ms using TSC */
    tsc_start  = cal_rdtsc();
    tsc_target = tsc_start + (tsc_freq * CAL_MS / 1000);

    do {
        tsc_now = cal_rdtsc();
    } while (tsc_now < tsc_target);

    /* Read LAPIC remaining count */
    lapic_remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    lapic_elapsed = 0xFFFFFFFF - lapic_remaining;
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    if (lapic_elapsed < 1000)
        return 0;  /* Too few ticks -- unreliable */

    cal_ticks_per_ms = lapic_elapsed / CAL_MS;
    if (!cal_value_plausible(cal_ticks_per_ms)) {
        cal_ticks_per_ms = 0;
        return 0;
    }

    /* Sanity: bus frequency must be in a reasonable range (100-500 MHz).
     * Below 100 MHz: TSC is scaled down (VBox NEM with slow TSC).
     * Above 500 MHz: TSC/LAPIC mismatch (VBox NEM with fast TSC but
     * LAPIC running at host rate). Real hardware is 100-400 MHz.
     * Fall through to PM Timer or PIT for accurate calibration. */
    if (cal_ticks_per_ms < 100000 || cal_ticks_per_ms > 500000) {
        klog(LOG_INFO, "lapic",
             "Tier 1b: TSC-referenced -> %u ticks/ms (%u MHz bus) -- "
             "out of range, TSC/LAPIC mismatch (VM?), skipping",
             (uint64_t)cal_ticks_per_ms,
             (uint64_t)(cal_ticks_per_ms / 1000));
        cal_ticks_per_ms = 0;
        return 0;
    }

    klog(LOG_INFO, "lapic",
         "Tier 1b: TSC-referenced -> %u ticks/ms (%u MHz bus, TSC %u MHz)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000),
         (uint64_t)(tsc_freq / 1000000));
    return 1;
}

/* ---- Tier 2: Modern Hardware Timers (10ms Delay, No PIT) ---- */

#include "kernel/acpi.h"
#include "kernel/mm/vmm.h"
#include "kernel/boot_init.h"

/* HPET register offsets (MMIO) */
#define HPET_CAP_REG    0x000   /* General Capabilities -- bits 32-63: period (fs) */
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

/* HPET-based calibration: map HPET as UC, read counter, measure LAPIC ticks.
 * Requires vmm_map_mmio_uc() -- HPET MMIO through WB pages causes MCE. */
static int cal_try_hpet(void)
{
    uint64_t hpet_phys;
    void *hpet_uc;           /* UC-mapped HPET base */
    uint64_t cap;
    uint32_t period_fs;
    uint64_t hpet_freq;
    uint64_t start_hpet, target_hpet, cur_hpet;
    uint32_t lapic_remaining, lapic_elapsed;

    hpet_phys = acpi_get_hpet_base();
    if (hpet_phys == 0)
        return 0;

    /* Reject bogus addresses (below 1 MiB or misaligned) */
    if (hpet_phys < 0x100000 || (hpet_phys & 0xFFF) != 0)
        return 0;

    /* Map HPET MMIO as UC -- 4 KiB is enough for all HPET registers */
    POST16(0xD102);
    hpet_uc = vmm_map_mmio_uc(hpet_phys, VMM_PAGE_SIZE);
    if (!hpet_uc) {
        klog(LOG_WARN, "lapic", "Tier 2: HPET UC mapping failed");
        return 0;
    }

    /* Probe with a 32-bit read -- reject if non-functional */
    {
        uint32_t probe = hpet_read32((uint64_t)(uintptr_t)hpet_uc, HPET_CAP_REG);
        if (probe == 0xFFFFFFFF || probe == 0x00000000) {
            vmm_unmap_mmio(hpet_uc, VMM_PAGE_SIZE);
            return 0;
        }
    }
    POST16(0xD103);

    /* Read capabilities -- upper 32 bits = period in femtoseconds */
    cap = hpet_read64((uint64_t)(uintptr_t)hpet_uc, HPET_CAP_REG);
    period_fs = (uint32_t)(cap >> 32);
    if (period_fs == 0 || period_fs > 100000000) {
        vmm_unmap_mmio(hpet_uc, VMM_PAGE_SIZE);
        return 0;
    }

    /* freq = 10^15 / period_fs */
    hpet_freq = 1000000000000000ULL / period_fs;

    /* Enable HPET counter if not already running */
    hpet_write32((uint64_t)(uintptr_t)hpet_uc, HPET_CFG_REG,
                 hpet_read32((uint64_t)(uintptr_t)hpet_uc, HPET_CFG_REG) | 0x01);

    /* Start LAPIC timer from max (one-shot, masked) */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT | 0xFF);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* Wait 10ms worth of HPET ticks (with spin timeout) */
    start_hpet = hpet_read64((uint64_t)(uintptr_t)hpet_uc, HPET_COUNTER);
    target_hpet = start_hpet + (hpet_freq * CAL_MS / 1000);

    {
        uint32_t spin = 200000000;
        uint64_t deadline = cal_deadline();
        do {
            cur_hpet = hpet_read64((uint64_t)(uintptr_t)hpet_uc, HPET_COUNTER);
        } while (cur_hpet < target_hpet && --spin > 0 &&
                 !cal_deadline_hit(deadline));

        if (cur_hpet < target_hpet) {
            lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
            vmm_unmap_mmio(hpet_uc, VMM_PAGE_SIZE);
            klog(LOG_WARN, "lapic", "Tier 2: HPET calibration timeout");
            return 0;
        }
    }

    /* Read LAPIC remaining count */
    lapic_remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    lapic_elapsed = 0xFFFFFFFF - lapic_remaining;
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    if (lapic_elapsed < 1000) {
        vmm_unmap_mmio(hpet_uc, VMM_PAGE_SIZE);
        return 0;
    }

    cal_ticks_per_ms = lapic_elapsed / CAL_MS;
    if (!cal_value_plausible(cal_ticks_per_ms)) {
        cal_ticks_per_ms = 0;
        return 0;
    }
    klog(LOG_INFO, "lapic",
         "Tier 2: HPET calibration -> %u ticks/ms (%u MHz bus, HPET %u MHz)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000),
         (uint64_t)(hpet_freq / 1000000));
    POST16(0xD104);
    return 1;
}

/* ACPI PM Timer frequency + 24-bit mask are canonical in mono_clock.h
 * (PMTMR_FREQ_HZ / PMTMR_24BIT_MASK); do not redefine here. */
/* 10ms worth of PM Timer ticks */
#define PMTIMER_10MS     (PMTMR_FREQ_HZ / 100)   /* ~35795 */

/* Raw single PM Timer read for the calibration spin loop (the loop measures
 * elapsed ticks over a 10 ms window and self-corrects a transient glitch). The
 * one-shot start/end reference reads instead go through the centralized
 * glitch-filtered acpi_pmtimer_read_value() so a single bad latch cannot skew
 * the LAPIC rate on this no-TSC/no-HPET path. */
static inline uint32_t pmtimer_read(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* PM Timer-based calibration: pace a 10ms LAPIC window via the ACPI PM Timer.
 * The PM Timer is an I/O port, NOT PIT hardware -- it works even when
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
    mask = is_32bit ? 0xFFFFFFFF : PMTMR_24BIT_MASK;

    /* Start LAPIC timer from max (one-shot, masked) */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT | 0xFF);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* Wait 10ms worth of PM Timer ticks (with spin timeout). The start
     * reference goes through the glitch-filtered centralized read so a single
     * bad latch cannot offset the whole calibration window. */
    start_pm = acpi_pmtimer_read_value() & mask;
    {
        uint32_t spin = 200000000;
        uint64_t deadline = cal_deadline();
        do {
            cur_pm = pmtimer_read(port) & mask;
            elapsed_pm = (cur_pm - start_pm) & mask;
        } while (elapsed_pm < PMTIMER_10MS && --spin > 0 &&
                 !cal_deadline_hit(deadline));

        if (elapsed_pm < PMTIMER_10MS) {
            lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
            klog(LOG_WARN, "lapic", "Tier 2: PM Timer calibration timeout");
            return 0;
        }
    }

    /* Read LAPIC remaining count */
    lapic_remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    lapic_elapsed = 0xFFFFFFFF - lapic_remaining;
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    if (lapic_elapsed < 1000)
        return 0;  /* Too few ticks -- unreliable */

    cal_ticks_per_ms = lapic_elapsed / CAL_MS;
    if (!cal_value_plausible(cal_ticks_per_ms)) {
        cal_ticks_per_ms = 0;
        return 0;
    }
    klog(LOG_INFO, "lapic",
         "Tier 2: PM Timer calibration -> %u ticks/ms (%u MHz bus, port=0x%x %s)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000),
         (uint64_t)port,
         is_32bit ? "32-bit" : "24-bit");
    return 1;
}

/* Forward declaration -- defined below after PIT I/O helpers */
static int cal_try_pit(void);

/* ---- Calibration Waterfall ----
 * THE single calibration entry point.  Cascades through 3 tiers:
 *   Tier 1: MSR/CPUID  (instant, no PIT)
 *   Tier 2: HPET / PM Timer (~10ms, no PIT)
 *   Tier 3: PIT channel 2 (~10ms, legacy only)
 * Falls back to hardcoded estimate if ALL tiers fail.
 */
/* 1 = a real calibration tier measured the frequency; 0 = uncalibrated or
 * running on the hardcoded estimate. Consumed by timer_hal_init() so the
 * estimate can never silently become the system tick source. */
static int cal_succeeded = 0;

int lapic_timer_calibrated(void)
{
    return cal_succeeded;
}

void lapic_timer_calibrate(void)
{
    cal_succeeded = 0;

    if (!lapic_base)
        return;

    /* Tier 1: Instant frequency from MSR/CPUID */
    if (cal_try_hyperv_msr() || cal_try_vmware_cpuid() || cal_try_cpuid_15h()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 1 succeeded -- no PIT/HPET needed");
        cal_succeeded = 1;
        return;
    }

    /* Tier 1b: TSC-referenced calibration -- DISABLED.
     * TSC frequency != LAPIC timer frequency on many platforms (VMs, AMD,
     * turbo boost). Linux and Windows don't use TSC for LAPIC calibration.
     * PM Timer and HPET give reliable results on all hardware. */

    /* Tier 2: Modern hardware timers (HPET -> PM Timer) */
    if (cal_try_hpet()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 2 succeeded -- HPET (UC mapped)");
        cal_succeeded = 1;
        return;
    }
    if (cal_try_pmtimer()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 2 succeeded -- no PIT needed");
        cal_succeeded = 1;
        return;
    }

    /* Tier 3: Legacy PIT (only if PCAT_COMPAT=1 && !HW_REDUCED) */
    if (cal_try_pit()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 3 succeeded -- PIT channel 2");
        cal_succeeded = 1;
        return;
    }

    /* All tiers failed -- cal_ticks_per_ms stays 0 per the header
     * contract (consumers like mono_clock gate on ticks_per_ms > 0; a
     * stored estimate would masquerade as a real calibration). The
     * timer HAL halts or falls back to the PIT on !cal_succeeded. */
    cal_ticks_per_ms = 0;
    klog(LOG_WARN, "lapic",
         "All calibration tiers failed -- LAPIC timer rate unknown");
}

/* PIT base frequency (Hz) -- the 8254 oscillator runs at this exact rate */
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
 *   4. Read remaining LAPIC count -> elapsed = 0xFFFFFFFF - remaining.
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
    cal_outb(0x61, gate | 0x01);   /* gate ON -> PIT starts counting */

    /* ---- 2. Start LAPIC timer from max value (one-shot, masked) ---- */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT | 0xFF);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* ---- 3. Wait for PIT output (bit 5 of port 0x61) ---- */
    timeout = 200000000;  /* iteration cap (no-TSC fallback) */
    {
        uint64_t deadline = cal_deadline();
        while (!(cal_inb(0x61) & 0x20) && --timeout > 0 &&
               !cal_deadline_hit(deadline))
            ;
        if (cal_deadline_hit(deadline))
            timeout = 0;   /* report as timeout below */
    }

    /* ---- 4. Read LAPIC timer current count ---- */
    lapic_remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    lapic_elapsed = 0xFFFFFFFF - lapic_remaining;

    /* Stop LAPIC timer */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    /* Disable PIT channel 2 gate */
    cal_outb(0x61, cal_inb(0x61) & ~0x01);

    /* ---- 5. Calculate ticks per ms ---- */
    if (timeout == 0 || lapic_elapsed < 1000) {
        /* Calibration failed -- timeout or too few ticks */
        klog(LOG_WARN, "lapic",
             "Tier 3: PIT calibration timeout or too few ticks");
        return 0;
    }

    cal_ticks_per_ms = lapic_elapsed / CAL_MS;
    if (!cal_value_plausible(cal_ticks_per_ms)) {
        cal_ticks_per_ms = 0;
        return 0;
    }
    klog(LOG_INFO, "lapic",
         "Tier 3: PIT ch2 calibration -> %u ticks/ms (%u MHz bus)",
         (uint64_t)cal_ticks_per_ms,
         (uint64_t)(cal_ticks_per_ms / 1000));
    return 1;
}

uint32_t lapic_timer_ticks_per_ms(void)
{
    return cal_ticks_per_ms;
}

/* ---- LAPIC timer driver (UTS backend) ---- */

static volatile uint64_t lapic_tick_count = 0;
static uint32_t          lapic_timer_hz   = 0;

static uint64_t lapic_get_ticks(void)
{
    return lapic_tick_count;
}

static void lapic_sleep_ms(uint32_t ms)
{
    uint64_t ticks;

    if (lapic_timer_hz == 0)
        return;
    /* Ceiling conversion with a 1-tick floor: at 100 Hz a 1-9 ms sleep
     * would otherwise truncate to ZERO ticks and return immediately,
     * collapsing driver polling loops (NVMe waits sleep_ms(1) per
     * iteration) into tight spins that exhaust their timeouts early. */
    ticks = ((uint64_t)ms * lapic_timer_hz + 999) / 1000;
    if (ticks == 0)
        ticks = 1;
    {
        uint64_t target = lapic_tick_count + ticks;
        while (lapic_tick_count < target)
            __asm__ volatile("hlt");
    }
}

static uint32_t lapic_get_freq(void)
{
    return lapic_timer_hz;
}

static void lapic_init_wrapper(uint32_t hz)
{
    lapic_timer_init(hz);
}

static inline uint64_t lapic_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Pure conversion helpers (testable without touching hardware) */
uint64_t lapic_oneshot_ns_to_tsc(uint64_t delta_ns, uint64_t freq_hz)
{
    /* Split multiply-divide: delta * freq overflows u64 after a few
     * seconds at GHz rates, and 128-bit division has no freestanding
     * runtime. (delta % 1e9) < 1e9 keeps the partial product in range. */
    return (delta_ns / 1000000000ULL) * freq_hz
         + ((delta_ns % 1000000000ULL) * freq_hz) / 1000000000ULL;
}

uint64_t lapic_oneshot_ns_to_ticks(uint64_t delta_ns, uint32_t ticks_per_ms)
{
    return (delta_ns / 1000000ULL) * ticks_per_ms
         + ((delta_ns % 1000000ULL) * ticks_per_ms) / 1000000ULL;
}

static int lapic_timer_arm_oneshot(uint64_t deadline_mono_ns)
{
    uint64_t now, delta;

    if (!lapic_base || lapic_timer_hz == 0)
        return -1;

    /* BSP-only: the periodic heartbeat is a BSP-global design (AP LVT
     * timers are masked) and arming an AP's local timer would create
     * cross-CPU delivery/restore races. Per-CPU one-shot arrives with
     * the per-CPU run-queue work. */
    {
        struct per_cpu_data *me = smp_this_cpu();
        if (me && me->cpu_id != 0)
            return -1;
    }

    now = mono_ns();
    /* Past deadlines fire as soon as possible */
    delta = (deadline_mono_ns > now) ? (deadline_mono_ns - now) : 0;

    /* Preferred: TSC-deadline mode (CPUID-gated; needs the invariant-TSC
     * frequency for the ns -> TSC conversion). 128-bit multiply: a u64
     * delta_ns * 4 GHz overflows 64 bits after ~4.6 seconds. */
    if (cpu_has(CPU_FEATURE_TSC_DL) && cpu_has(CPU_FEATURE_TSC_INV)) {
        extern uint64_t boot_timing_tsc_freq(void);
        uint64_t freq = boot_timing_tsc_freq();
        if (freq > 0) {
            uint64_t delta_tsc = lapic_oneshot_ns_to_tsc(delta, freq);
            uint64_t target = lapic_rdtsc() + (delta_tsc ? delta_tsc : 1);
            __atomic_store_n(&s_oneshot_armed, 1, __ATOMIC_RELEASE);
            lapic_write(LAPIC_REG_LVT_TIMER,
                        LVT_TIMER_TSC_DEADLINE | LAPIC_TIMER_VECTOR);
            /* SDM: serialize between the LVT mode change and the
             * deadline MSR write, or the wrmsr can be ignored */
            __asm__ volatile("mfence" ::: "memory");
            msr_write(MSR_IA32_TSC_DEADLINE, target);
            return 0;
        }
    }

    /* Fallback: LVT one-shot mode from the calibrated bus frequency */
    if (cal_ticks_per_ms == 0)
        return -1;
    {
        uint64_t ticks = lapic_oneshot_ns_to_ticks(delta, cal_ticks_per_ms);
        if (ticks == 0)
            ticks = 1;
        if (ticks > 0xFFFFFFFFULL)
            return -1;   /* beyond the 32-bit initial-count range */
        __atomic_store_n(&s_oneshot_armed, 1, __ATOMIC_RELEASE);
        lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
        lapic_write(LAPIC_REG_LVT_TIMER,
                    LVT_TIMER_ONESHOT | LAPIC_TIMER_VECTOR);
        lapic_write(LAPIC_REG_TIMER_ICR, (uint32_t)ticks);
        return 0;
    }
}

/* Exported vtable for timer HAL selection */
timer_driver_t lapic_driver = {
    .name        = "LAPIC",
    .init        = lapic_init_wrapper,
    .get_ticks   = lapic_get_ticks,
    .sleep_ms    = lapic_sleep_ms,
    .get_freq    = lapic_get_freq,
    .read_ns     = mono_ns_coarse,
    .arm_oneshot = lapic_timer_arm_oneshot,
};

static uint64_t lapic_timer_handler(struct interrupt_frame *frame)
{
    lapic_tick_count++;
    timer_tick_callback_fire();
    lapic_eoi();

    /* Advance the PMTMR 64-bit epoch before any time reader runs (no-op
     * unless PMTMR is the active monotonic source). Keeps the wrap-extend
     * fresh far faster than the 24-bit ~4.69 s wrap. */
    {
        extern void mono_clock_pmtmr_advance(void);
        mono_clock_pmtmr_advance();
    }

    /* Update KUSER_SHARED_DATA time fields for user-mode readers */
    {
        extern void kusd_update_time(void);
        kusd_update_time();
    }

    /* NT timer queue scan: fire/rearm any armed timers whose due_ns
     * has been reached. Safe from ISR -- uses irqsave spinlock and
     * event_set() (documented IRQ-safe). */
    {
        extern void nt_timer_tick(void);
        nt_timer_tick();
    }

    /* Fire expired kernel_timer_t timers (tick-based, DPC-associated) BEFORE
     * the DPC drain so a fired timer's DPC drains in the same tick. */
    {
        extern uint32_t ktimer_expire_current_cpu(void);
        ktimer_expire_current_cpu();
    }

    /* Drain pending DPCs after EOI (LAPIC can accept new interrupts)
     * but before schedule (DPC work completes before thread dispatch).
     * Uses lightweight drain -- no IRQL raise since we're in ISR context. */
    {
        extern void dpc_watchdog_tick(void);
        extern uint32_t dpc_drain_current_cpu(void);
        dpc_watchdog_tick();   /* per-tick budget refill + sustained-depth check */
        dpc_drain_current_cpu();
    }

    /* One-shot fired: restore periodic mode IMMEDIATELY -- the periodic
     * heartbeat (scheduler, NT timers, DPC drain) must never silently
     * stop. Minimal reprogram, no logging in the ISR. The tickless-idle
     * governor will own true one-shot residency when it lands. */
    if (__atomic_exchange_n(&s_oneshot_armed, 0, __ATOMIC_ACQ_REL)) {
        /* Preserve a concurrent quiesce's mask bit: an rt_call on
         * another CPU may have masked the LVT after this interrupt was
         * accepted, and the restore must not unmask it into firmware */
        uint32_t cur = lapic_read(LAPIC_REG_LVT_TIMER);
        uint64_t icr64 = (cal_ticks_per_ms > 0 && lapic_timer_hz > 0)
                           ? (uint64_t)cal_ticks_per_ms * 1000ULL
                                 / lapic_timer_hz
                           : 10000000ULL;
        uint32_t icr = (icr64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFu
                                               : (uint32_t)icr64;
        lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
        lapic_write(LAPIC_REG_LVT_TIMER,
                    LVT_TIMER_PERIODIC | LAPIC_TIMER_VECTOR
                        | (cur & LVT_MASKED));
        lapic_write(LAPIC_REG_TIMER_ICR, icr ? icr : 1);
    }

    return schedule(frame);
}

/* Quiesce/resume fixup: an armed one-shot whose deadline expired while
 * the LVT was masked never reaches the ISR auto-restore -- the heartbeat
 * would stay silently stopped after a UEFI runtime call. Conservatively
 * drop the pending one-shot and force periodic mode (the consumer
 * re-arms; losing a one-shot event beats losing the scheduler tick). */
void lapic_timer_resume_fixup(void)
{
    /* BSP-only: s_oneshot_armed is BSP heartbeat state. An AP resuming
     * from a UEFI runtime call must restore only its own saved LVT --
     * consuming the flag here would lose the BSP's pending one-shot and
     * program the AP's local timer as periodic/unmasked. */
    {
        struct per_cpu_data *me = smp_this_cpu();
        if (me && me->cpu_id != 0)
            return;
    }
    if (__atomic_exchange_n(&s_oneshot_armed, 0, __ATOMIC_ACQ_REL)) {
        uint64_t icr64 = (cal_ticks_per_ms > 0 && lapic_timer_hz > 0)
                           ? (uint64_t)cal_ticks_per_ms * 1000ULL
                                 / lapic_timer_hz
                           : 10000000ULL;
        uint32_t icr = (icr64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFu
                                               : (uint32_t)icr64;
        lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_1);
        lapic_write(LAPIC_REG_LVT_TIMER,
                    LVT_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
        lapic_write(LAPIC_REG_TIMER_ICR, icr ? icr : 1);
    }
}

void lapic_timer_init(uint32_t hz)
{
    uint32_t icr;

    if (!lapic_base)
        return;

    lapic_timer_hz = hz;

    /* Calculate ICR from calibrated frequency, or use xv6 fallback.
     * 64-bit math: ticks/ms * 1000 wraps u32 above ~4.29 GHz rates. */
    if (cal_ticks_per_ms > 0) {
        uint64_t icr64 = (uint64_t)cal_ticks_per_ms * 1000ULL / hz;
        icr = (icr64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)icr64;
        if (icr == 0)
            icr = 1;
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

int lapic_timer_set_hz(uint32_t new_hz)
{
    uint32_t icr;

    if (!lapic_base || cal_ticks_per_ms == 0 || new_hz == 0)
        return -1;

    /* BSP-only: the heartbeat is the BSP's LAPIC timer (AP LVTs are
     * masked); reprogramming an AP's local timer would publish a new
     * global rate without changing the actual tick source. AP-side
     * routing arrives with the per-CPU timer bring-up. */
    {
        struct per_cpu_data *me = smp_this_cpu();
        if (me && me->cpu_id != 0)
            return -1;   /* caller logs after releasing its locks */
    }

    /* Bank tick time at the OLD rate before the frequency changes --
     * otherwise the mono_clock tick fallback rewinds (lifetime ticks
     * must never be rescaled by a new frequency) */
    mono_clock_tick_rebase(new_hz);

    {
        uint64_t icr64 = (uint64_t)cal_ticks_per_ms * 1000ULL / new_hz;
        icr = (icr64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)icr64;
    }
    if (icr == 0) icr = 1;

    lapic_write(LAPIC_REG_TIMER_ICR, icr);
    lapic_timer_hz = new_hz;

    /* No logging here: timer_set_tick_hz holds the mode lock across this
     * call and klog can flush to disk -- the syscall layer logs after
     * releasing every lock */
    return 0;
}
