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

static inline void outb_lapic(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_lapic(uint16_t port)
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

/* xv6-style LAPIC timer initialization.
 *
 * Instead of calibrating with the PIT (which hangs on VBox NEM due to
 * extremely slow port I/O under Hyper-V paravirtualization), we use a
 * hardcoded initial count based on typical LAPIC timer frequencies.
 *
 * The LAPIC timer counts down from ICR at (bus_clock / divider) Hz.
 * With divider=1:
 *   - QEMU TCG:   ~26 MHz bus → ICR = 260,000 for 100 Hz
 *   - VBox NEM:    ~1 GHz bus  → ICR = 10,000,000 for 100 Hz
 *   - Real Intel:  ~100-400 MHz → ICR varies
 *
 * xv6 uses ICR=10000000 with divider X1 and it works everywhere.
 * We do the same. The tick rate won't be exactly 100 Hz, but the
 * timer WILL fire, and that's what matters for boot progress. */

void lapic_timer_init(uint32_t hz)
{
    /* xv6-style: hardcoded initial count, divider 1 */
    uint32_t ticr = 10000000;
    (void)hz; /* we use xv6's fixed count instead of computing from hz */

    if (!lapic_base)
        return;


    /* Divide configuration = 1 (no division) */
    lapic_write(LAPIC_REG_TIMER_DCR, 0x0B); /* divide by 1 = 0b1011 */

    /* Configure periodic timer on vector 32 (same as PIT IRQ0) */
    lapic_write(LAPIC_REG_LVT_TIMER,
                LVT_TIMER_PERIODIC | 32);

    /* Set initial count — starts the timer immediately */
    lapic_write(LAPIC_REG_TIMER_ICR, ticr);

    klog(LOG_INFO, "lapic",
         "LAPIC timer: periodic, vec=32, ICR=%u, div=1 (xv6-style)",
         (uint64_t)ticr);

}
