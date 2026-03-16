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
 *   3. Configure LVT entries (timer, LINT0, LINT1, error)
 *   4. Provide EOI, IPI send, and timer calibration
 * ============================================================================ */

#include "kernel/drivers/lapic.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/barrier.h"

/* ---- State ---- */

static volatile uint32_t *lapic_base = (volatile uint32_t *)0;
static int lapic_ready = 0;

/* ---- Internal helpers ---- */

/* PIT I/O for timer calibration only */
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

    /* Mask LVT Timer initially (will be configured in lapic_timer_init) */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);

    /* Configure LINT0: ExtINT (for virtual wire mode compatibility) */
    lapic_write(LAPIC_REG_LVT_LINT0, 0x00000700); /* ExtINT, masked */

    /* Configure LINT1: NMI */
    lapic_write(LAPIC_REG_LVT_LINT1, 0x00000400); /* NMI delivery */

    /* Configure LVT Error */
    lapic_write(LAPIC_REG_LVT_ERROR, 0xFE); /* vector 0xFE for errors */

    /* Send EOI to clear any pending interrupts from init */
    lapic_write(LAPIC_REG_EOI, 0);

    lapic_ready = 1;

    ver = lapic_read(LAPIC_REG_VERSION);

    klog(LOG_INFO, "lapic",
         "LAPIC enabled: base=%x, ID=%u, ver=%x, maxLVT=%u",
         (uint64_t)base_addr,
         (uint64_t)((lapic_read(LAPIC_REG_ID) >> 24) & 0xFF),
         (uint64_t)(ver & 0xFF),
         (uint64_t)((ver >> 16) & 0xFF));
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

    /* Mask timer and LINT0 */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT0, LVT_MASKED);

    /* NMI on LINT1 */
    lapic_write(LAPIC_REG_LVT_LINT1, 0x00000400);

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

/* Use PIT channel 2 for one-shot calibration of LAPIC timer frequency.
 * PIT runs at 1193182 Hz. We count how many LAPIC ticks occur in ~10 ms. */
void lapic_timer_init(uint32_t hz)
{
    uint32_t ticks_per_10ms;
    uint32_t ticks_per_second;

    if (!lapic_base || hz == 0)
        return;

    /* Set LAPIC timer divider to 16 */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_16);

    /* Start LAPIC timer at max count (one-shot) */
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_ONESHOT);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* Use PIT channel 2 for ~10 ms delay (11932 ticks at 1193182 Hz) */
    outb_lapic(0x61, (inb_lapic(0x61) & 0xFD) | 0x01); /* gate on */
    outb_lapic(0x43, 0xB0);  /* channel 2, mode 0, lobyte/hibyte */
    outb_lapic(0x42, 0x9C);  /* 11932 & 0xFF */
    outb_lapic(0x42, 0x2E);  /* 11932 >> 8 */

    /* Reset PIT gate to start countdown */
    {
        uint8_t tmp = inb_lapic(0x61);
        outb_lapic(0x61, tmp & 0xFE);   /* gate off */
        outb_lapic(0x61, tmp | 0x01);   /* gate on  */
    }

    /* Wait for PIT to finish (bit 5 of port 0x61 goes high) */
    while (!(inb_lapic(0x61) & 0x20))
        barrier();

    /* Read how many LAPIC ticks elapsed in ~10 ms */
    ticks_per_10ms = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CCR);
    ticks_per_second = ticks_per_10ms * 100;

    /* Configure periodic timer at the requested frequency */
    lapic_write(LAPIC_REG_TIMER_DCR, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER,
                LVT_TIMER_PERIODIC | 32); /* vector 32 = IRQ0 (PIT replacement) */
    lapic_write(LAPIC_REG_TIMER_ICR,
                ticks_per_second / hz);

    klog(LOG_INFO, "lapic",
         "LAPIC timer: %u ticks/10ms, %u Hz, ICR=%u",
         (uint64_t)ticks_per_10ms,
         (uint64_t)hz,
         (uint64_t)(ticks_per_second / hz));
}
