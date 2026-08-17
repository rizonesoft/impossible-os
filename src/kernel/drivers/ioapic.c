/* ============================================================================
 * ioapic.c -- I/O APIC driver
 *
 * Routes hardware interrupts through the I/O APIC instead of the legacy PIC.
 * The I/O APIC provides per-IRQ routing to any Local APIC (CPU) with
 * configurable vector, delivery mode, polarity, and trigger mode.
 *
 * Initialization:
 *   1. Read I/O APIC base address from ACPI MADT
 *   2. Read max redirection entries from IOAPICVER register
 *   3. Mask all entries (safe default)
 *   4. Apply MADT Interrupt Source Override entries
 *   5. Route ISA IRQs to BSP's LAPIC with sequential vectors (32+N)
 *
 * ISA IRQ override example (extremely common):
 *   MADT says: IRQ 0 (PIT) → GSI 2
 *   Without this, the PIT interrupt would be lost because the I/O APIC
 *   pin 0 is connected to the PIT on legacy hardware, but on APIC hardware
 *   pin 2 is the PIT. The override table corrects this.
 * ============================================================================ */

#include "kernel/drivers/ioapic.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/pic.h"
#include "kernel/vectors.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"
#include "kernel/mm/vmm.h"
#include "kernel/sched/spinlock.h"
/* ---- State ---- */

static volatile uint32_t *ioapic_base = (volatile uint32_t *)0;
static uint32_t max_redir_entries = 0;
static uint32_t ioapic_gsi_base = 0;   /* first GSI handled by this IOAPIC */
static int ioapic_ready = 0;

/* The IOREGSEL/IOWIN pair is a shared two-step register window: any
 * interleaved select from another CPU lands the data phase on the wrong
 * register. Held across every select+data sequence AND across the 64-bit
 * redirection-entry read/modify/write pairs. */
static spinlock_t ioapic_lock = SPINLOCK_INIT;

/* ---- MMIO register access (callers hold ioapic_lock) ---- */

/* I/O APIC uses indirect register access:
 * Write register index to IOREGSEL (offset 0x00),
 * then read/write value at IOWIN (offset 0x10). */

static uint32_t ioapic_read(uint32_t reg)
{
    ioapic_base[0] = reg;          /* IOREGSEL */
    return ioapic_base[4];         /* IOWIN = offset 0x10 / 4 */
}

static void ioapic_write(uint32_t reg, uint32_t val)
{
    ioapic_base[0] = reg;          /* IOREGSEL */
    ioapic_base[4] = val;          /* IOWIN */
}

/* Translate an absolute GSI to this IOAPIC's redirection-table pin.
 * Returns the pin index, or -1 when the GSI is outside this IOAPIC's
 * GSI range (gsi_base for max_redir_entries pins). */
static int ioapic_gsi_to_pin(uint32_t gsi)
{
    if (gsi < ioapic_gsi_base)
        return -1;
    if (gsi - ioapic_gsi_base >= max_redir_entries)
        return -1;
    return (int)(gsi - ioapic_gsi_base);
}

/* ---- Redirection table access ---- */

/* Each redirection entry is 64 bits, split across two 32-bit registers:
 *   Low  (0x10 + 2*irq): vector, delivery mode, mask, trigger, polarity
 *   High (0x11 + 2*irq): destination APIC ID (bits 24-27) */

static void ioapic_set_entry(uint8_t irq, uint64_t entry)
{
    uint32_t reg_lo = 0x10 + (uint32_t)irq * 2;
    uint32_t reg_hi = reg_lo + 1;

    ioapic_write(reg_hi, (uint32_t)(entry >> 32));
    ioapic_write(reg_lo, (uint32_t)(entry & 0xFFFFFFFF));
}

static uint64_t ioapic_get_entry(uint8_t irq)
{
    uint32_t reg_lo = 0x10 + (uint32_t)irq * 2;
    uint32_t reg_hi = reg_lo + 1;

    uint32_t lo = ioapic_read(reg_lo);
    uint32_t hi = ioapic_read(reg_hi);
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Public API ---- */

uint32_t ioapic_isa_to_gsi(uint8_t isa_irq)
{
    uint32_t i;
    uint32_t count = acpi_get_override_count();

    for (i = 0; i < count; i++) {
        const struct madt_int_override *ovr = acpi_get_override(i);
        if (ovr && ovr->source == isa_irq)
            return ovr->gsi;
    }

    /* No override -- GSI = ISA IRQ number (identity mapping) */
    return (uint32_t)isa_irq;
}

void ioapic_init(void)
{
    uint32_t base_addr;
    uint32_t ver;
    uint32_t i;
    uint32_t bsp_lapic_id;

    base_addr = acpi_get_ioapic_base();
    if (base_addr == 0) {
        klog(LOG_WARN, "ioapic", "No I/O APIC found -- staying with PIC");
        return;
    }
    if (base_addr & 0xFFF) {
        klog(LOG_ERROR, "ioapic",
             "I/O APIC base 0x%x not page-aligned -- staying with PIC",
             (uint64_t)base_addr);
        return;
    }

    /* IOREGSEL/IOWIN are device registers: UC mapping is mandatory
     * (cached/reordered access returns stale interrupt state on real HW) */
    ioapic_base = (volatile uint32_t *)vmm_map_mmio_uc(base_addr, 4096);
    if (!ioapic_base) {
        klog(LOG_ERROR, "ioapic",
             "UC map of I/O APIC at 0x%x failed -- staying with PIC",
             (uint64_t)base_addr);
        return;
    }

    /* Read version register: bits 16-23 = max redirection entry.
     * 0 / all-ones means no device decodes the window -- fail closed
     * instead of programming 256 phantom entries. */
    {
        uint64_t irqf;
        spin_lock_irqsave(&ioapic_lock, &irqf);
        ver = ioapic_read(0x01);
        spin_unlock_irqrestore(&ioapic_lock, irqf);
    }
    if (ver == 0 || ver == 0xFFFFFFFF) {
        klog(LOG_ERROR, "ioapic",
             "I/O APIC version register invalid (0x%x) -- device absent, staying with PIC",
             (uint64_t)ver);
        ioapic_base = (volatile uint32_t *)0;
        return;
    }
    max_redir_entries = ((ver >> 16) & 0xFF) + 1;
    ioapic_gsi_base = acpi_madt_info()->ioapic_gsi_base;

    /* Get BSP LAPIC ID for routing */
    bsp_lapic_id = lapic_id();

    /* Mask ALL entries first (safe default).
     * ioapic_route_irq() also routes masked; each driver unmasks after init. */
    {
        uint64_t irqf;
        spin_lock_irqsave(&ioapic_lock, &irqf);
        for (i = 0; i < max_redir_entries; i++) {
            /* Set mask bit (bit 16), vector 0, destination 0 */
            ioapic_set_entry((uint8_t)i, (uint64_t)1 << 16);
        }
        spin_unlock_irqrestore(&ioapic_lock, irqf);
    }

    /* Route standard ISA IRQs to BSP with vectors 32-47.
     * Apply MADT interrupt source overrides for correct pin mapping.
     *
     * CRITICAL: Skip IRQ 2 (8259 cascade) -- it doesn't exist on APIC
     * systems. With the standard IRQ 0→GSI 2 override, IRQ 2 would also
     * map to GSI 2 and overwrite the PIT routing, killing the timer. */
    {
        int pit_routed = 0;       /* the PIT route is mandatory: without it
                                   * the PIC gets disabled with no timer */
        uint8_t gsi_routed[24];   /* track which GSIs are already routed */
        for (i = 0; i < 24; i++)
            gsi_routed[i] = 0;

        for (i = 0; i < 16; i++) {
            uint32_t gsi;
            uint16_t flags = 0;
            uint32_t j;

            /* Skip cascade IRQ -- doesn't exist on APIC */
            if (i == 2)
                continue;

            gsi = ioapic_isa_to_gsi((uint8_t)i);

            /* Skip if this GSI was already routed (prevents collision) */
            if (gsi < 24 && gsi_routed[gsi])
                continue;

            /* Check for override flags */
            for (j = 0; j < acpi_get_override_count(); j++) {
                const struct madt_int_override *ovr = acpi_get_override(j);
                if (ovr && ovr->source == (uint8_t)i) {
                    flags = ovr->flags;
                    break;
                }
            }

            /* ISA IRQ 0-7 -> 0x20+, IRQ 8-15 -> VECTOR_ISA_IRQ8_BASE+
             * (matches the PIC remap; keeps IRQ14 off the INT 0x2E gate) */
            uint8_t vec = (i < 8) ? (uint8_t)(PIC1_OFFSET + i)
                                  : (uint8_t)(VECTOR_ISA_IRQ8_BASE + (i - 8));
            if (ioapic_route_irq(gsi, vec,
                                 (uint8_t)bsp_lapic_id, flags) == 0) {
                klog(LOG_DEBUG, "ioapic",
                     "  Route: ISA IRQ %u -> GSI %u, vec 0x%x, dest LAPIC %u, flags=0x%x",
                     (uint64_t)i, (uint64_t)gsi, (uint64_t)vec,
                     (uint64_t)bsp_lapic_id, (uint64_t)flags);
                if (gsi < 24)
                    gsi_routed[gsi] = 1;
                if (i == 0)
                    pit_routed = 1;
            } else {
                klog(LOG_WARN, "ioapic",
                     "  ISA IRQ %u -> GSI %u outside this IOAPIC's range -- not routed",
                     (uint64_t)i, (uint64_t)gsi);
            }
        }

        if (!pit_routed) {
            /* Without the timer route, advertising IOAPIC readiness would
             * let boot disable the PIC and lose all timer interrupts.
             * Fail closed and keep the PIC path alive. */
            klog(LOG_ERROR, "ioapic",
                 "PIT route (ISA IRQ 0) not coverable by this IOAPIC -- "
                 "staying with PIC");
            ioapic_base = (volatile uint32_t *)0;
            max_redir_entries = 0;
            return;
        }
    }

    ioapic_ready = 1;

    klog(LOG_INFO, "ioapic",
         "I/O APIC at 0x%x: %u entries, ISA IRQs routed to BSP (LAPIC %u)",
         (uint64_t)base_addr, (uint64_t)max_redir_entries,
         (uint64_t)bsp_lapic_id);
}

int ioapic_route_irq(uint32_t gsi, uint8_t vector,
                     uint8_t dest_lapic, uint16_t flags)
{
    uint64_t entry = 0;
    uint64_t irqf;
    int pin;

    if (!ioapic_base)
        return -1;
    pin = ioapic_gsi_to_pin(gsi);
    if (pin < 0)
        return -1;

    /* Vector (bits 0-7) */
    entry = (uint64_t)vector;

    /* Delivery mode: fixed (000) -- bits 8-10 */
    /* entry |= 0; */

    /* Polarity (bit 13): from MADT flags bits 0-1
     *   00 = bus default (active high for ISA)
     *   01 = active high
     *   11 = active low */
    if ((flags & 0x03) == 0x03) {
        entry |= (1ULL << 13);  /* active low */
    }

    /* Trigger mode (bit 15): from MADT flags bits 2-3
     *   00 = bus default (edge for ISA)
     *   01 = edge
     *   11 = level */
    if (((flags >> 2) & 0x03) == 0x03) {
        entry |= (1ULL << 15);  /* level-triggered */
    }

    /* Destination LAPIC ID (bits 56-59 in the high dword) */
    entry |= ((uint64_t)dest_lapic << 56);

    /* Start MASKED (bit 16 = 1).
     * Each driver must call ioapic_unmask_irq() after registering its
     * handler so that no IRQ fires into the IDT before a handler exists. */
    entry |= (1ULL << 16);

    spin_lock_irqsave(&ioapic_lock, &irqf);
    ioapic_set_entry((uint8_t)pin, entry);
    spin_unlock_irqrestore(&ioapic_lock, irqf);
    return 0;
}

int ioapic_mask_irq(uint32_t gsi)
{
    uint64_t entry;
    uint64_t irqf;
    int pin;

    if (!ioapic_base)
        return -1;
    pin = ioapic_gsi_to_pin(gsi);
    if (pin < 0)
        return -1;

    spin_lock_irqsave(&ioapic_lock, &irqf);
    entry = ioapic_get_entry((uint8_t)pin);
    entry |= (1ULL << 16);  /* set mask bit */
    ioapic_set_entry((uint8_t)pin, entry);
    spin_unlock_irqrestore(&ioapic_lock, irqf);
    return 0;
}

int ioapic_unmask_irq(uint32_t gsi)
{
    uint64_t entry;
    uint64_t irqf;
    int pin;

    if (!ioapic_base)
        return -1;
    pin = ioapic_gsi_to_pin(gsi);
    if (pin < 0)
        return -1;

    spin_lock_irqsave(&ioapic_lock, &irqf);
    entry = ioapic_get_entry((uint8_t)pin);
    entry &= ~(1ULL << 16);  /* clear mask bit */
    ioapic_set_entry((uint8_t)pin, entry);
    spin_unlock_irqrestore(&ioapic_lock, irqf);
    return 0;
}

int ioapic_irq_masked(uint32_t gsi)
{
    uint64_t entry;
    uint64_t irqf;
    int pin;

    if (!ioapic_base)
        return -1;
    pin = ioapic_gsi_to_pin(gsi);
    if (pin < 0)
        return -1;

    /* Read-back accessor for save/restore callers. A caller that masks a
     * line temporarily cannot restore it correctly by unmasking blindly:
     * the line may have been masked before it ever ran, and unmasking it
     * would enable an interrupt whose handler is not registered. */
    spin_lock_irqsave(&ioapic_lock, &irqf);
    entry = ioapic_get_entry((uint8_t)pin);
    spin_unlock_irqrestore(&ioapic_lock, irqf);
    return (entry & (1ULL << 16)) ? 1 : 0;
}

int ioapic_set_destination(uint32_t gsi, uint8_t dest_lapic)
{
    uint64_t entry;
    uint64_t irqf;
    int pin;

    if (!ioapic_base)
        return -1;
    pin = ioapic_gsi_to_pin(gsi);
    if (pin < 0)
        return -1;

    spin_lock_irqsave(&ioapic_lock, &irqf);
    entry = ioapic_get_entry((uint8_t)pin);
    entry &= ~(0xFFULL << 56);              /* clear destination field */
    entry |= ((uint64_t)dest_lapic << 56);
    ioapic_set_entry((uint8_t)pin, entry);
    spin_unlock_irqrestore(&ioapic_lock, irqf);
    return 0;
}

int ioapic_available(void)
{
    return ioapic_ready;
}
