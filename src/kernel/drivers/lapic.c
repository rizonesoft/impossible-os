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

/* ---- Calibration Waterfall Dispatcher ----
 * Called from timer_hal_init() (§6.4).  Tries in order:
 *   Tier 1: MSR/CPUID (instant, no PIT)
 *   Tier 2: HPET / PM Timer (future — not yet implemented)
 *   Tier 3: PIT channel 2 (existing lapic_timer_calibrate, legacy fallback)
 */
void lapic_timer_calibrate_waterfall(void)
{
    if (!lapic_base)
        return;

    /* Tier 1: Instant frequency from MSR/CPUID */
    if (cal_try_hyperv_msr() || cal_try_vmware_cpuid() || cal_try_cpuid_15h()) {
        klog(LOG_INFO, "lapic",
             "Calibration: Tier 1 succeeded — no PIT/HPET needed");
        return;
    }

    /* Tier 2: HPET / PM Timer — not yet implemented, fall through */

    /* Tier 3: PIT channel 2 (legacy, existing implementation) */
    klog(LOG_INFO, "lapic",
         "Calibration: Tier 1 failed — falling back to PIT channel 2");
    lapic_timer_calibrate();
}

/* PIT base frequency (Hz) — the 8254 oscillator runs at this exact rate */
#define PIT_OSC_FREQ  1193182

/* Calibration window: 10ms via PIT channel 2 one-shot */
#define CAL_MS        10
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

/* Calibrate LAPIC timer frequency using PIT channel 2 (speaker gate).
 *
 * How it works:
 *   1. Program PIT channel 2 in one-shot mode for CAL_MS milliseconds.
 *   2. Start the LAPIC timer counting down from 0xFFFFFFFF.
 *   3. Busy-wait for PIT output bit (port 0x61, bit 5) to go high.
 *   4. Read remaining LAPIC count → elapsed = 0xFFFFFFFF - remaining.
 *   5. ticks_per_ms = elapsed / CAL_MS.
 *
 * If PIT polling hangs (VBox NEM, some Hyper-V configs), a spin-counter
 * timeout fires and we fall back to the xv6 hardcoded ICR. */
void lapic_timer_calibrate(void)
{
    uint8_t gate;
    uint32_t lapic_remaining, lapic_elapsed;
    uint32_t timeout;

    if (!lapic_base)
        return;

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
        /* Calibration failed — use xv6 hardcoded fallback.
         * xv6 ICR=10000000, div=1 → works on QEMU and most hardware. */
        cal_ticks_per_ms = 0;
        klog(LOG_WARN, "lapic",
             "Timer calibration timeout — using hardcoded fallback");
    } else {
        cal_ticks_per_ms = lapic_elapsed / CAL_MS;
        klog(LOG_INFO, "lapic",
             "Timer calibrated: %u ticks in %ums → %u ticks/ms (%u MHz bus)",
             (uint64_t)lapic_elapsed, (uint64_t)CAL_MS,
             (uint64_t)cal_ticks_per_ms,
             (uint64_t)(cal_ticks_per_ms / 1000));
    }
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
